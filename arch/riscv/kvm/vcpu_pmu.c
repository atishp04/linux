// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Rivos Inc
 *
 * Authors:
 *     Atish Patra <atishp@rivosinc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <linux/perf/riscv_pmu.h>
#include <asm/csr.h>
#include <asm/kvm_vcpu_pmu.h>
#include <linux/kvm_host.h>

int kvm_riscv_vcpu_pmu_num_ctrs(struct kvm_vcpu *vcpu, unsigned long *out_val)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);

	if (!kvpmu)
		return -EINVAL;

	*out_val = kvpmu->num_fw_ctrs + kvpmu->num_hw_ctrs;
	return 0;
}

int kvm_riscv_vcpu_pmu_ctr_info(struct kvm_vcpu *vcpu, unsigned long cidx,
				unsigned long *ctr_info)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);

	if (!kvpmu || (cidx > RISCV_MAX_COUNTERS) || (cidx == 1))
		return -EINVAL;

	*ctr_info = kvpmu->pmc[cidx].cinfo.value;

	return 0;
}

int kvm_riscv_vcpu_pmu_ctr_start(struct kvm_vcpu *vcpu, unsigned long ctr_base,
				 unsigned long ctr_mask, unsigned long flag, uint64_t ival)
{
	/* TODO */
	return 0;
}

int kvm_riscv_vcpu_pmu_ctr_stop(struct kvm_vcpu *vcpu, unsigned long ctr_base,
				 unsigned long ctr_mask, unsigned long flag)
{
	/* TODO */
	return 0;
}

int kvm_riscv_vcpu_pmu_ctr_cfg_match(struct kvm_vcpu *vcpu, unsigned long ctr_base,
				     unsigned long ctr_mask, unsigned long flag,
				     unsigned long eidx, uint64_t edata)
{
	/* TODO */
	return 0;
}

int kvm_riscv_vcpu_pmu_ctr_read(struct kvm_vcpu *vcpu, unsigned long cidx,
				unsigned long *out_val)
{
	/* TODO */
	return 0;
}

int kvm_riscv_vcpu_pmu_init(struct kvm_vcpu *vcpu)
{
	int i = 0, num_hw_ctrs, num_fw_ctrs, hpm_width;
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);

	if (!kvpmu)
		return -EINVAL;

	num_hw_ctrs = riscv_pmu_sbi_get_num_hw_ctrs();
	if ((num_hw_ctrs + RISCV_KVM_MAX_FW_CTRS) > RISCV_MAX_COUNTERS)
		num_fw_ctrs = RISCV_MAX_COUNTERS - num_hw_ctrs;
	else
		num_fw_ctrs = RISCV_KVM_MAX_FW_CTRS;

	hpm_width = riscv_pmu_sbi_hpmc_width();
	if (hpm_width <= 0) {
		pr_err("Can not initialize PMU for vcpu as hpmcounter width is not available\n");
		return -EINVAL;
	}

	kvpmu->num_hw_ctrs = num_hw_ctrs;
	kvpmu->num_fw_ctrs = num_fw_ctrs;
	/*
	 * There is no corelation betwen the logical hardware counter and virtual counters.
	 * However, we need to encode a hpmcounter CSR in the counter info field so that
	 * KVM can trap n emulate the read. This works well in the migraiton usecase as well
	 * KVM doesn't care if the actual hpmcounter is available in the hardware or not.
	 */
	for (i = 0; i < num_hw_ctrs + num_fw_ctrs; i++) {
		/* TIME CSR shouldn't be read from perf interface */
		if (i == 1)
			continue;
		kvpmu->pmc[i].idx = i;
		kvpmu->pmc[i].vcpu = vcpu;
		if (i < kvpmu->num_hw_ctrs) {
			kvpmu->pmc[i].cinfo.type = SBI_PMU_CTR_TYPE_HW;
			if (i < 3)
				/* CY, IR counters */
				kvpmu->pmc[i].cinfo.width = 63;
			else
				kvpmu->pmc[i].cinfo.width = hpm_width;
			/*
			 * The CSR number doesn't have any relation with the logical
			 * hardware counters. The CSR numbers are encoded sequentially
			 * to avoid maintaining a map between the virtual counter
			 * and CSR number.
			 */
			kvpmu->pmc[i].cinfo.csr = CSR_CYCLE + i;
		} else {
			kvpmu->pmc[i].cinfo.type = SBI_PMU_CTR_TYPE_FW;
			kvpmu->pmc[i].cinfo.width = BITS_PER_LONG - 1;
		}
	}

	return 0;
}

void kvm_riscv_vcpu_pmu_reset(struct kvm_vcpu *vcpu)
{
	/* TODO */
}

void kvm_riscv_vcpu_pmu_deinit(struct kvm_vcpu *vcpu)
{
	/* TODO */
}

