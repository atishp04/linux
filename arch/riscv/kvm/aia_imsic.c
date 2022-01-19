// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Western Digital Corporation or its affiliates.
 * Copyright (C) 2022 Ventana Micro Systems Inc.
 *
 * Authors:
 *	Anup Patel <anup@brainfault.org>
 */

#include <linux/bitmap.h>
#include <linux/irqchip/riscv-imsic.h>
#include <linux/kvm_host.h>
#include <linux/math.h>
#include <linux/spinlock.h>
#include <linux/swab.h>
#include <kvm/iodev.h>
#include <asm/csr.h>

#define IMSIC_MAX_LONGS	(IMSIC_EIP63 - IMSIC_EIP0 + 1)

struct imsic_regs {
	unsigned long eidelivery;
	unsigned long eithreshold;
	unsigned long eip[IMSIC_MAX_LONGS];
	unsigned long eie[IMSIC_MAX_LONGS];
};

struct imsic {
	struct kvm_io_device iodev;

	u32 nr_msis;
	u32 nr_longs;
	u32 nr_hw_longs;

	raw_spinlock_t lock;

	/*
	 * At any point in time, the register state is in
	 * one of the following places:
	 *
	 * 1) Hardware: IMSIC VS-file (hgei_cpu >= 0)
	 * 2) Software: IMSIC SW-file (hgei_cpu < 0)
	 */

	/* IMSIC VS-file */
	int hgei;
	int hgei_cpu;
	void __iomem *hgei_va;
	phys_addr_t hgei_pa;

	/* IMSIC SW-file */
	struct imsic_regs swfile;
};

#define imsic_vs_csr_swap(__c, __v)		\
({						\
	unsigned long __r;			\
	csr_write(CSR_VSISELECT, __c);		\
	__r = csr_swap(CSR_VSIREG, __v);	\
	__r;					\
})

#define imsic_swap_switchcase(__ireg, __v)		\
	case __ireg:					\
		return imsic_vs_csr_swap(__ireg, __v);
#define imsic_swap_switchcase_2(__ireg, __v)		\
	imsic_swap_switchcase(__ireg + 0, __v)		\
	imsic_swap_switchcase(__ireg + 1, __v)
#define imsic_swap_switchcase_4(__ireg, __v)		\
	imsic_swap_switchcase_2(__ireg + 0, __v)	\
	imsic_swap_switchcase_2(__ireg + 2, __v)
#define imsic_swap_switchcase_8(__ireg, __v)		\
	imsic_swap_switchcase_4(__ireg + 0, __v)	\
	imsic_swap_switchcase_4(__ireg + 4, __v)
#define imsic_swap_switchcase_16(__ireg, __v)		\
	imsic_swap_switchcase_8(__ireg + 0, __v)	\
	imsic_swap_switchcase_8(__ireg + 8, __v)
#define imsic_swap_switchcase_32(__ireg, __v)		\
	imsic_swap_switchcase_16(__ireg + 0, __v)	\
	imsic_swap_switchcase_16(__ireg + 16, __v)
#define imsic_swap_switchcase_64(__ireg, __v)		\
	imsic_swap_switchcase_32(__ireg + 0, __v)	\
	imsic_swap_switchcase_32(__ireg + 32, __v)

static unsigned long imsic_eix_swap(int ireg, unsigned long val)
{
	switch (ireg) {
	imsic_swap_switchcase_64(IMSIC_EIP0, val)
	imsic_swap_switchcase_64(IMSIC_EIE0, val)
	};

	return 0;
}

#define imsic_vs_csr_write(__c, __v)		\
do {						\
	csr_write(CSR_VSISELECT, __c);		\
	csr_write(CSR_VSIREG, __v);		\
} while (0)

#define imsic_write_switchcase(__ireg, __v)		\
	case __ireg:					\
		imsic_vs_csr_write(__ireg, __v);	\
		break;
#define imsic_write_switchcase_2(__ireg, __v)		\
	imsic_write_switchcase(__ireg + 0, __v)		\
	imsic_write_switchcase(__ireg + 1, __v)
#define imsic_write_switchcase_4(__ireg, __v)		\
	imsic_write_switchcase_2(__ireg + 0, __v)	\
	imsic_write_switchcase_2(__ireg + 2, __v)
#define imsic_write_switchcase_8(__ireg, __v)		\
	imsic_write_switchcase_4(__ireg + 0, __v)	\
	imsic_write_switchcase_4(__ireg + 4, __v)
#define imsic_write_switchcase_16(__ireg, __v)		\
	imsic_write_switchcase_8(__ireg + 0, __v)	\
	imsic_write_switchcase_8(__ireg + 8, __v)
#define imsic_write_switchcase_32(__ireg, __v)		\
	imsic_write_switchcase_16(__ireg + 0, __v)	\
	imsic_write_switchcase_16(__ireg + 16, __v)
#define imsic_write_switchcase_64(__ireg, __v)		\
	imsic_write_switchcase_32(__ireg + 0, __v)	\
	imsic_write_switchcase_32(__ireg + 32, __v)

static void imsic_eix_write(int ireg, unsigned long val)
{
	switch (ireg) {
	imsic_write_switchcase_64(IMSIC_EIP0, val)
	imsic_write_switchcase_64(IMSIC_EIE0, val)
	};
}

#define imsic_vs_csr_set(__c, __v)		\
do {						\
	csr_write(CSR_VSISELECT, __c);		\
	csr_set(CSR_VSIREG, __v);		\
} while (0)

#define imsic_set_switchcase(__ireg, __v)		\
	case __ireg:					\
		imsic_vs_csr_set(__ireg, __v);		\
		break;
#define imsic_set_switchcase_2(__ireg, __v)		\
	imsic_set_switchcase(__ireg + 0, __v)		\
	imsic_set_switchcase(__ireg + 1, __v)
#define imsic_set_switchcase_4(__ireg, __v)		\
	imsic_set_switchcase_2(__ireg + 0, __v)		\
	imsic_set_switchcase_2(__ireg + 2, __v)
#define imsic_set_switchcase_8(__ireg, __v)		\
	imsic_set_switchcase_4(__ireg + 0, __v)		\
	imsic_set_switchcase_4(__ireg + 4, __v)
#define imsic_set_switchcase_16(__ireg, __v)		\
	imsic_set_switchcase_8(__ireg + 0, __v)		\
	imsic_set_switchcase_8(__ireg + 8, __v)
#define imsic_set_switchcase_32(__ireg, __v)		\
	imsic_set_switchcase_16(__ireg + 0, __v)	\
	imsic_set_switchcase_16(__ireg + 16, __v)
#define imsic_set_switchcase_64(__ireg, __v)		\
	imsic_set_switchcase_32(__ireg + 0, __v)	\
	imsic_set_switchcase_32(__ireg + 32, __v)

static void imsic_eix_set(int ireg, unsigned long val)
{
	switch (ireg) {
	imsic_set_switchcase_64(IMSIC_EIP0, val)
	imsic_set_switchcase_64(IMSIC_EIE0, val)
	};
}

struct imsic_vsfile_read_clear_data {
	int hgei;
	u32 nr_longs;
	struct imsic_regs *regs;
};

static void imsic_vsfile_local_read_clear(void *data)
{
	u32 i;
	struct imsic_vsfile_read_clear_data *idata = data;
	struct imsic_regs *regs = idata->regs;
	unsigned long new_hstatus, old_hstatus, old_vsiselect;

	old_vsiselect = csr_read(CSR_VSISELECT);
	old_hstatus = csr_read(CSR_HSTATUS);
	new_hstatus = old_hstatus & ~HSTATUS_VGEIN;
	new_hstatus |= ((unsigned long)idata->hgei) << HSTATUS_VGEIN_SHIFT;
	csr_write(CSR_HSTATUS, new_hstatus);

	regs->eidelivery = imsic_vs_csr_swap(IMSIC_EIDELIVERY, 0);
	regs->eithreshold = imsic_vs_csr_swap(IMSIC_EITHRESHOLD, 0);
	for (i = 0; i < idata->nr_longs; i++) {
#ifdef CONFIG_64BIT
		regs->eip[i] = imsic_eix_swap(IMSIC_EIP0 + i * 2, 0);
		regs->eie[i] = imsic_eix_swap(IMSIC_EIE0 + i * 2, 0);
#else
		regs->eip[i] = imsic_eix_swap(IMSIC_EIP0 + i, 0);
		regs->eie[i] = imsic_eix_swap(IMSIC_EIE0 + i, 0);
#endif
	}

	csr_write(CSR_HSTATUS, old_hstatus);
	csr_write(CSR_VSISELECT, old_vsiselect);
}

static void imsic_vsfile_read_clear(int hgei, int hgei_cpu, u32 nr_longs,
				    struct imsic_regs *regs)
{
	struct imsic_vsfile_read_clear_data idata;

	/* We can only read clear if we have a IMSIC VS-file */
	if (hgei_cpu < 0 || hgei <= 0)
		return;

	/* We can only read clear on local CPU */
	idata.hgei = hgei;
	idata.nr_longs = nr_longs;
	idata.regs = regs;
	on_each_cpu_mask(cpumask_of(hgei_cpu),
			 imsic_vsfile_local_read_clear, &idata, 1);
}

static void imsic_vsfile_local_clear(int hgei, u32 nr_longs)
{
	u32 i;
	unsigned long new_hstatus, old_hstatus, old_vsiselect;

	/* We can only zero-out if we have a IMSIC VS-file */
	if (hgei <= 0)
		return;

	old_vsiselect = csr_read(CSR_VSISELECT);
	old_hstatus = csr_read(CSR_HSTATUS);
	new_hstatus = old_hstatus & ~HSTATUS_VGEIN;
	new_hstatus |= ((unsigned long)hgei) << HSTATUS_VGEIN_SHIFT;
	csr_write(CSR_HSTATUS, new_hstatus);

	imsic_vs_csr_write(IMSIC_EIDELIVERY, 0);
	imsic_vs_csr_write(IMSIC_EITHRESHOLD, 0);
	for (i = 0; i < nr_longs; i++) {
#ifdef CONFIG_64BIT
		 imsic_eix_write(IMSIC_EIP0 + i * 2, 0);
		 imsic_eix_write(IMSIC_EIE0 + i * 2, 0);
#else
		 imsic_eix_write(IMSIC_EIP0 + i, 0);
		 imsic_eix_write(IMSIC_EIE0 + i, 0);
#endif
	}

	csr_write(CSR_HSTATUS, old_hstatus);
	csr_write(CSR_VSISELECT, old_vsiselect);
}

static void imsic_vsfile_local_update(int hgei, u32 nr_longs,
				      struct imsic_regs *regs)
{
	u32 i;
	unsigned long new_hstatus, old_hstatus, old_vsiselect;

	/* We can only update if we have a HW IMSIC context */
	if (hgei <= 0)
		return;

	old_vsiselect = csr_read(CSR_VSISELECT);
	old_hstatus = csr_read(CSR_HSTATUS);
	new_hstatus = old_hstatus & ~HSTATUS_VGEIN;
	new_hstatus |= ((unsigned long)hgei) << HSTATUS_VGEIN_SHIFT;
	csr_write(CSR_HSTATUS, new_hstatus);

	for (i = 0; i < nr_longs; i++) {
#ifdef CONFIG_64BIT
		imsic_eix_set(IMSIC_EIP0 + i * 2, regs->eip[i]);
		imsic_eix_set(IMSIC_EIE0 + i * 2, regs->eie[i]);
#else
		imsic_eix_set(IMSIC_EIP0 + i, regs->eip[i]);
		imsic_eix_set(IMSIC_EIE0 + i, regs->eie[i]);
#endif
	}
	imsic_vs_csr_write(IMSIC_EITHRESHOLD, regs->eithreshold);
	imsic_vs_csr_write(IMSIC_EIDELIVERY, regs->eidelivery);

	csr_write(CSR_HSTATUS, old_hstatus);
	csr_write(CSR_VSISELECT, old_vsiselect);
}

static void imsic_vsfile_cleanup(struct imsic *imsic)
{
	int old_hgei, old_hgei_cpu;
	unsigned long flags;

	raw_spin_lock_irqsave(&imsic->lock, flags);
	old_hgei = imsic->hgei;
	old_hgei_cpu = imsic->hgei_cpu;
	imsic->hgei_cpu = imsic->hgei = -1;
	imsic->hgei_va = NULL;
	imsic->hgei_pa = 0;
	memset(&imsic->swfile, 0, sizeof(imsic->swfile));
	raw_spin_unlock_irqrestore(&imsic->lock, flags);

	if (old_hgei_cpu >= 0)
		kvm_riscv_aia_free_hgei(old_hgei_cpu, old_hgei);
}

/* This function must be called with imsic->lock held */
static u32 __imsic_swfile_topei(struct imsic *imsic)
{
	u32 i, max_msi;
	struct imsic_regs *swfile = &imsic->swfile;

	max_msi = (swfile->eithreshold &&
		   (swfile->eithreshold <= imsic->nr_msis)) ?
		    swfile->eithreshold : imsic->nr_msis;
	for (i = 1; i < max_msi; i++) {
		if (test_bit(i, swfile->eie) && test_bit(i, swfile->eip))
			return (i << TOPEI_ID_SHIFT) | i;
	}

	return 0;
}

/* This function must be called with imsic->lock held */
static void __imsic_swfile_extirq_update(struct kvm_vcpu *vcpu)
{
	struct imsic *imsic = vcpu->arch.aia.imsic_state;

	if (imsic->swfile.eidelivery && __imsic_swfile_topei(imsic))
		kvm_riscv_vcpu_set_interrupt(vcpu, IRQ_VS_EXT);
	else
		kvm_riscv_vcpu_unset_interrupt(vcpu, IRQ_VS_EXT);
}

static void imsic_swfile_read_clear(struct kvm_vcpu *vcpu,
				    struct imsic_regs *regs)
{
	unsigned long flags;
	struct imsic *imsic = vcpu->arch.aia.imsic_state;

	raw_spin_lock_irqsave(&imsic->lock, flags);
	memcpy(regs, &imsic->swfile, sizeof(*regs));
	memset(&imsic->swfile, 0, sizeof(imsic->swfile));
	__imsic_swfile_extirq_update(vcpu);
	raw_spin_unlock_irqrestore(&imsic->lock, flags);
}

static void imsic_swfile_update(struct kvm_vcpu *vcpu,
				struct imsic_regs *regs)
{
	u32 i;
	unsigned long flags;
	struct imsic *imsic = vcpu->arch.aia.imsic_state;

	raw_spin_lock_irqsave(&imsic->lock, flags);

	imsic->swfile.eidelivery = regs->eidelivery;
	imsic->swfile.eithreshold = regs->eithreshold;
	for (i = 0; i < imsic->nr_hw_longs; i++) {
		imsic->swfile.eip[i] |= regs->eip[i];
		imsic->swfile.eie[i] |= regs->eie[i];
	}

	__imsic_swfile_extirq_update(vcpu);
	raw_spin_unlock_irqrestore(&imsic->lock, flags);
}

static int imsic_swfile_eidelivery_rmw(struct kvm_vcpu *vcpu,
				       unsigned long *val,
				       unsigned long new_val,
				       unsigned long wr_mask)
{
	unsigned long old_val, flags;
	struct imsic *imsic = vcpu->arch.aia.imsic_state;

	raw_spin_lock_irqsave(&imsic->lock, flags);

	old_val = imsic->swfile.eidelivery;
	if (val)
		*val = old_val;

	wr_mask &= 0x1;
	imsic->swfile.eidelivery = (old_val & ~wr_mask) |
				   (new_val & wr_mask);

	__imsic_swfile_extirq_update(vcpu);
	raw_spin_unlock_irqrestore(&imsic->lock, flags);

	return 1;
}

static int imsic_swfile_eithreshold_rmw(struct kvm_vcpu *vcpu,
					unsigned long *val,
					unsigned long new_val,
					unsigned long wr_mask)
{
	unsigned long old_val, flags;
	struct imsic *imsic = vcpu->arch.aia.imsic_state;

	raw_spin_lock_irqsave(&imsic->lock, flags);

	old_val = imsic->swfile.eithreshold;
	if (val)
		*val = old_val;

	wr_mask &= (IMSIC_MAX_ID - 1);
	imsic->swfile.eithreshold = (old_val & ~wr_mask) |
				    (new_val & wr_mask);

	__imsic_swfile_extirq_update(vcpu);
	raw_spin_unlock_irqrestore(&imsic->lock, flags);

	return 1;
}

static int imsic_swfile_topei_rmw(struct kvm_vcpu *vcpu, unsigned long *val,
				  unsigned long new_val,
				  unsigned long wr_mask)
{
	u32 topei;
	unsigned long flags;
	struct imsic *imsic = vcpu->arch.aia.imsic_state;

	raw_spin_lock_irqsave(&imsic->lock, flags);

	/* Read pending and enabled interrupt with highest priority */
	topei = __imsic_swfile_topei(imsic);
	if (val)
		*val = topei;

	/* Writes ignore value and clear top pending interrupt */
	if (topei && wr_mask) {
		topei >>= TOPEI_ID_SHIFT;
		if (topei) {
			clear_bit(topei, imsic->swfile.eip);
			__imsic_swfile_extirq_update(vcpu);
		}
	}

	raw_spin_unlock_irqrestore(&imsic->lock, flags);

	return 1;
}

static int imsic_swfile_eix_rmw(struct kvm_vcpu *vcpu, u32 num, bool pend,
				unsigned long *val, unsigned long new_val,
				unsigned long wr_mask)
{
	unsigned long flags;
	struct imsic *imsic = vcpu->arch.aia.imsic_state;
	unsigned long *eix = (pend) ? imsic->swfile.eip : imsic->swfile.eie;

#ifdef CONFIG_64BIT
	if (num & 0x1)
		return 2;
	num >>= 1;
#endif
	if (num >= imsic->nr_longs)
		return 2;

	raw_spin_lock_irqsave(&imsic->lock, flags);

	if (val)
		*val = eix[num];

	if (wr_mask) {
		/* Bit0 of EIP0 or EIE0 is read-only */
		if (!num)
			wr_mask &= ~BIT(0);

		eix[num] = (eix[num] & ~wr_mask) | (new_val & wr_mask);
		__imsic_swfile_extirq_update(vcpu);
	}

	raw_spin_unlock_irqrestore(&imsic->lock, flags);

	return 1;
}

void kvm_riscv_vcpu_aia_imsic_release(struct kvm_vcpu *vcpu)
{
	unsigned long flags;
	struct imsic_regs tregs;
	int old_hgei, old_hgei_cpu;
	struct imsic *imsic = vcpu->arch.aia.imsic_state;

	/* Read IMSIC VS-file details */
	raw_spin_lock_irqsave(&imsic->lock, flags);
	old_hgei = imsic->hgei;
	old_hgei_cpu = imsic->hgei_cpu;
	raw_spin_unlock_irqrestore(&imsic->lock, flags);

	/* Do nothing, if no IMSIC VS-file to release */
	if (old_hgei_cpu < 0)
		return;

	/*
	 * At this point, all interrupt producers are still using the
	 * the old IMSIC VS-file so we first re-direct all interrupt
	 * producers.
	 */

	/* Purge the G-stage mapping */
	kvm_riscv_stage2_iounmap(vcpu->kvm,
				 vcpu->arch.aia.imsic_addr,
				 IMSIC_MMIO_PAGE_SZ);

	/* TODO: Purge the IOMMU mapping ??? */

	/* Clear IMSIC VS-file details in the IMSIC context */
	raw_spin_lock_irqsave(&imsic->lock, flags);
	imsic->hgei_cpu = imsic->hgei = -1;
	imsic->hgei_va = NULL;
	imsic->hgei_pa = 0;
	raw_spin_unlock_irqrestore(&imsic->lock, flags);

	/*
	 * At this point, all interrupt producers have been re-directed
	 * to somewhere else so we move register state from the old IMSIC
	 * VS-file to the IMSIC SW-file.
	 */

	/* Read and clear register state from old IMSIC VS-file */
	memset(&tregs, 0, sizeof(tregs));
	imsic_vsfile_read_clear(old_hgei, old_hgei_cpu,
				imsic->nr_hw_longs, &tregs);

	/* Update register state in IMSIC SW-file */
	imsic_swfile_update(vcpu, &tregs);

	/* Free-up old IMSIC VS-file */
	kvm_riscv_aia_free_hgei(old_hgei_cpu, old_hgei);
}

int kvm_riscv_vcpu_aia_imsic_update(struct kvm_vcpu *vcpu)
{
	unsigned long flags;
	phys_addr_t new_hgei_pa;
	struct imsic_regs tregs;
	void __iomem *new_hgei_va;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_run *run = vcpu->run;
	struct kvm_vcpu_aia *vaia = &vcpu->arch.aia;
	struct imsic *imsic = vaia->imsic_state;
	int ret = 0, new_hgei = -1, old_hgei, old_hgei_cpu;

	/* Do nothing for emulation mode */
	if (kvm->arch.aia.mode == KVM_DEV_RISCV_AIA_MODE_EMUL)
		return 1;

	/* Read old IMSIC VS-file details */
	raw_spin_lock_irqsave(&imsic->lock, flags);
	old_hgei = imsic->hgei;
	old_hgei_cpu = imsic->hgei_cpu;
	raw_spin_unlock_irqrestore(&imsic->lock, flags);

	/* Do nothing if we are continuing on same CPU */
	if (old_hgei_cpu == vcpu->cpu)
		return 1;

	/* Allocate new IMSIC VS-file */
	ret = kvm_riscv_aia_alloc_hgei(vcpu->cpu, vcpu,
				       &new_hgei_va, &new_hgei_pa);
	if (ret <= 0) {
		/* For HW acceleration mode, we can't continue */
		if (kvm->arch.aia.mode == KVM_DEV_RISCV_AIA_MODE_HWACCEL) {
			run->fail_entry.hardware_entry_failure_reason =
								CSR_HSTATUS;
			run->fail_entry.cpu = vcpu->cpu;
			run->exit_reason = KVM_EXIT_FAIL_ENTRY;
			return 0;
		}

		/* Release old IMSIC VS-file */
		if (old_hgei_cpu >= 0)
			kvm_riscv_vcpu_aia_imsic_release(vcpu);

		/* For automatic mode, we continue */
		goto done;
	}
	new_hgei = ret;

	/*
	 * At this point, all interrupt producers are still using
	 * to the old IMSIC VS-file so we first move all interrupt
	 * producers to the new IMSIC VS-file.
	 */

	/* Zero-out new IMSIC VS-file */
	imsic_vsfile_local_clear(new_hgei, imsic->nr_hw_longs);

	/* Update G-stage mapping for the new IMSIC VS-file */
	ret = kvm_riscv_stage2_ioremap(kvm, vcpu->arch.aia.imsic_addr,
				       new_hgei_pa, IMSIC_MMIO_PAGE_SZ,
				       true, true);
	if (ret)
		goto fail_free_hgei;

	/* TODO: Update the IOMMU mapping ??? */

	/* Update new IMSIC VS-file details in IMSIC context */
	raw_spin_lock_irqsave(&imsic->lock, flags);
	imsic->hgei = new_hgei;
	imsic->hgei_cpu = vcpu->cpu;
	imsic->hgei_va = new_hgei_va;
	imsic->hgei_pa = new_hgei_pa;
	raw_spin_unlock_irqrestore(&imsic->lock, flags);

	/*
	 * At this point, all interrupt producers have been moved
	 * to the new IMSIC VS-file so we move register state from
	 * the old IMSIC VS/SW-file to the new IMSIC VS-file.
	 */

	memset(&tregs, 0, sizeof(tregs));
	if (old_hgei_cpu >= 0) {
		/* Read and clear register state from old IMSIC VS-file */
		imsic_vsfile_read_clear(old_hgei, old_hgei_cpu,
					imsic->nr_hw_longs, &tregs);

		/* Free-up old IMSIC VS-file */
		kvm_riscv_aia_free_hgei(old_hgei_cpu, old_hgei);
	} else {
		/* Read and clear register state from IMSIC SW-file */
		imsic_swfile_read_clear(vcpu, &tregs);
	}

	/* Restore register state in the new IMSIC VS-file */
	imsic_vsfile_local_update(new_hgei, imsic->nr_hw_longs, &tregs);

done:
	/* Set VCPU HSTATUS.VGEIN to new IMSIC VS-file */
	vcpu->arch.guest_context.hstatus &= ~HSTATUS_VGEIN;
	if (new_hgei > 0)
		vcpu->arch.guest_context.hstatus |=
			((unsigned long)new_hgei) << HSTATUS_VGEIN_SHIFT;

	/* Continue run-loop */
	return 1;

fail_free_hgei:
	kvm_riscv_aia_free_hgei(vcpu->cpu, new_hgei);
	return ret;
}

int kvm_riscv_vcpu_aia_imsic_rmw(struct kvm_vcpu *vcpu, unsigned long isel,
				 unsigned long *val, unsigned long new_val,
				 unsigned long wr_mask)
{
	/* Select appropriate IMSIC emulation function */
	switch (isel) {
	case IMSIC_EIDELIVERY:
		return imsic_swfile_eidelivery_rmw(vcpu, val,
						   new_val, wr_mask);
	case IMSIC_EITHRESHOLD:
		return imsic_swfile_eithreshold_rmw(vcpu, val,
						    new_val, wr_mask);
	case KVM_RISCV_AIA_IMSIC_TOPEI:
		return imsic_swfile_topei_rmw(vcpu, val, new_val, wr_mask);
	case IMSIC_EIP0 ... IMSIC_EIP63:
		return imsic_swfile_eix_rmw(vcpu, isel - IMSIC_EIP0, true,
					    val, new_val, wr_mask);
	case IMSIC_EIE0 ... IMSIC_EIE63:
		return imsic_swfile_eix_rmw(vcpu, isel - IMSIC_EIE0, false,
					    val, new_val, wr_mask);
	default:
		break;
	};

	/* Forward unknown IMSIC register to user-space */
	return 0;
}

void kvm_riscv_vcpu_aia_imsic_reset(struct kvm_vcpu *vcpu)
{
	unsigned long flags;
	struct imsic *imsic = vcpu->arch.aia.imsic_state;

	if (!imsic)
		return;

	kvm_riscv_vcpu_aia_imsic_release(vcpu);

	raw_spin_lock_irqsave(&imsic->lock, flags);
	memset(&imsic->swfile, 0, sizeof(imsic->swfile));
	raw_spin_unlock_irqrestore(&imsic->lock, flags);
}

int kvm_riscv_vcpu_aia_imsic_inject(struct kvm_vcpu *vcpu,
				    u32 guest_index, u32 offset, u32 iid)
{
	unsigned long flags;
	struct imsic *imsic = vcpu->arch.aia.imsic_state;

	/* We only emulate one IMSIC MMIO page for each Guest VCPU */
	if (!imsic || !iid || guest_index ||
	    (offset != IMSIC_MMIO_SETIPNUM_LE &&
	     offset != IMSIC_MMIO_SETIPNUM_BE))
		return -ENODEV;

	iid = (offset == IMSIC_MMIO_SETIPNUM_BE) ? __swab32(iid) : iid;
	if (imsic->nr_msis <= iid)
		return -EINVAL;

	raw_spin_lock_irqsave(&imsic->lock, flags);

	if (imsic->hgei_cpu >= 0) {
		writel(iid, imsic->hgei_va + IMSIC_MMIO_SETIPNUM_LE);
		kvm_vcpu_kick(vcpu);
	} else {
		set_bit(iid, imsic->swfile.eip);
		__imsic_swfile_extirq_update(vcpu);
	}

	raw_spin_unlock_irqrestore(&imsic->lock, flags);

	return 0;
}

static int imsic_mmio_read(struct kvm_vcpu *vcpu, struct kvm_io_device *dev,
			   gpa_t addr, int len, void *val)
{
	if (len != 4 || (addr & 0x3) != 0)
		return -EOPNOTSUPP;

	*((u32 *)val) = 0;

	return 0;
}

static int imsic_mmio_write(struct kvm_vcpu *vcpu, struct kvm_io_device *dev,
			    gpa_t addr, int len, const void *val)
{
	struct kvm_msi msi = { 0 };

	if (len != 4 || (addr & 0x3) != 0)
		return -EOPNOTSUPP;

	msi.address_hi = addr >> 32;
	msi.address_lo = (u32)addr;
	msi.data = *((const u32 *)val);
	kvm_riscv_aia_inject_msi(vcpu->kvm, &msi);

	return 0;
};

static struct kvm_io_device_ops imsic_iodoev_ops = {
	.read = imsic_mmio_read,
	.write = imsic_mmio_write,
};

int kvm_riscv_vcpu_aia_imsic_init(struct kvm_vcpu *vcpu)
{
	int ret = 0;
	struct imsic *imsic;
	struct kvm *kvm = vcpu->kvm;

	/* Fail if we have zero IDs */
	if (!kvm->arch.aia.nr_ids)
		return -EINVAL;

	/* Allocate IMSIC context */
	imsic = kzalloc(sizeof(*imsic), GFP_KERNEL);
	if (!imsic)
		return -ENOMEM;
	vcpu->arch.aia.imsic_state = imsic;

	/* Setup IMSIC context  */
	imsic->nr_msis = kvm->arch.aia.nr_ids + 1;
	raw_spin_lock_init(&imsic->lock);
	imsic->nr_longs = BITS_TO_LONGS(imsic->nr_msis);
	imsic->nr_hw_longs = BITS_TO_LONGS(kvm_riscv_aia_max_ids);
	imsic->hgei = imsic->hgei_cpu = -1;

	/* Setup IO device */
	kvm_iodevice_init(&imsic->iodev, &imsic_iodoev_ops);
	mutex_lock(&kvm->slots_lock);
	ret = kvm_io_bus_register_dev(kvm, KVM_MMIO_BUS,
				      vcpu->arch.aia.imsic_addr,
				      KVM_DEV_RISCV_IMSIC_SIZE,
				      &imsic->iodev);
	mutex_unlock(&kvm->slots_lock);
	if (ret)
		goto fail_free_imsic;

	return 0;

fail_free_imsic:
	vcpu->arch.aia.imsic_state = NULL;
	kfree(imsic);
	return ret;
}

void kvm_riscv_vcpu_aia_imsic_cleanup(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	struct imsic *imsic = vcpu->arch.aia.imsic_state;

	if (!imsic)
		return;

	imsic_vsfile_cleanup(imsic);

	mutex_lock(&kvm->slots_lock);
	kvm_io_bus_unregister_dev(kvm, KVM_MMIO_BUS, &imsic->iodev);
	mutex_unlock(&kvm->slots_lock);

	vcpu->arch.aia.imsic_state = NULL;
	kfree(imsic);
}
