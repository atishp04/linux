// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/kvm_vcpu_sbi.h>
#include <asm/kvm_cove.h>
#include <asm/kvm_cove_sbi.h>

static int cove_share_converted_page(struct kvm_vcpu *vcpu, gpa_t gpa,
				     struct kvm_riscv_cove_page *tpage)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_cove_tvm_context *tvmc = kvm->arch.tvmc;
	int rc;

	rc = sbi_covh_tvm_invalidate_pages(tvmc->tvm_guest_id, gpa, PAGE_SIZE);
	if (rc)
		return rc;

	rc = kvm_riscv_cove_tvm_fence(vcpu);
	if (rc)
		goto err;

	rc = sbi_covh_tvm_remove_pages(tvmc->tvm_guest_id, gpa, PAGE_SIZE);
	if (rc)
		goto err;

	rc = sbi_covh_tsm_reclaim_page(page_to_phys(tpage->page));
	if (rc)
		return rc;

	spin_lock(&kvm->mmu_lock);
	list_del(&tpage->link);
	list_add(&tpage->link, &tvmc->shared_pages);
	spin_unlock(&kvm->mmu_lock);

	return 0;

err:
	sbi_covh_tvm_validate_pages(tvmc->tvm_guest_id, gpa, PAGE_SIZE);

	return rc;
}

static int cove_share_page(struct kvm_vcpu *vcpu, gpa_t gpa,
			   unsigned long *sbi_err)
{
	unsigned long hva = gfn_to_hva(vcpu->kvm, gpa >> PAGE_SHIFT);
	struct kvm_cove_tvm_context *tvmc = vcpu->kvm->arch.tvmc;
	struct mm_struct *mm = current->mm;
	struct kvm_riscv_cove_page *tpage;
	struct page *page;
	int rc;

	if (kvm_is_error_hva(hva)) {
		/* Address is out of the guest ram memory region. */
		*sbi_err = SBI_ERR_INVALID_PARAM;
		return 0;
	}

	tpage = kmalloc(sizeof(*tpage), GFP_KERNEL_ACCOUNT);
	if (!tpage)
		return -ENOMEM;

	mmap_read_lock(mm);
	rc = pin_user_pages(hva, 1, FOLL_LONGTERM | FOLL_WRITE, &page);
	mmap_read_unlock(mm);

	if (rc != 1) {
		rc = -EINVAL;
		goto free_tpage;
	} else if (!folio_test_swapbacked(page_folio(page))) {
		rc = -EIO;
		goto free_tpage;
	}

	tpage->page = page;
	tpage->gpa = gpa;
	tpage->hva = hva;
	INIT_LIST_HEAD(&tpage->link);

	spin_lock(&vcpu->kvm->mmu_lock);
	list_add(&tpage->link, &tvmc->shared_pages);
	spin_unlock(&vcpu->kvm->mmu_lock);

	return 0;

free_tpage:
	kfree(tpage);

	return rc;
}

static int kvm_riscv_cove_share_page(struct kvm_vcpu *vcpu, gpa_t gpa,
				     unsigned long *sbi_err)
{
	struct kvm_cove_tvm_context *tvmc = vcpu->kvm->arch.tvmc;
	struct kvm_riscv_cove_page *tpage, *next;
	bool converted = false;

	/*
	 * Check if the shared memory is part of the pages already assigned
	 * to the TVM.
	 *
	 * TODO: Implement a better approach to track regions to avoid
	 * traversing the whole list.
	 */
	spin_lock(&vcpu->kvm->mmu_lock);
	list_for_each_entry_safe(tpage, next, &tvmc->zero_pages, link) {
		if (tpage->gpa == gpa) {
			converted = true;
			break;
		}
	}
	spin_unlock(&vcpu->kvm->mmu_lock);

	if (converted)
		return cove_share_converted_page(vcpu, gpa, tpage);

	return cove_share_page(vcpu, gpa, sbi_err);
}

static int kvm_riscv_cove_unshare_page(struct kvm_vcpu *vcpu, gpa_t gpa)
{
	struct kvm_riscv_cove_page *tpage, *next;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_cove_tvm_context *tvmc = kvm->arch.tvmc;
	struct page *page = NULL;
	int rc;

	spin_lock(&kvm->mmu_lock);
	list_for_each_entry_safe(tpage, next, &tvmc->shared_pages, link) {
		if (tpage->gpa == gpa) {
			page = tpage->page;
			break;
		}
	}
	spin_unlock(&kvm->mmu_lock);

	if (unlikely(!page))
		return -EINVAL;

	rc = sbi_covh_tvm_invalidate_pages(tvmc->tvm_guest_id, gpa, PAGE_SIZE);
	if (rc)
		return rc;

	rc = kvm_riscv_cove_tvm_fence(vcpu);
	if (rc)
		return rc;

	rc = sbi_covh_tvm_remove_pages(tvmc->tvm_guest_id, gpa, PAGE_SIZE);
	if (rc)
		return rc;

	unpin_user_pages_dirty_lock(&page, 1, true);

	spin_lock(&kvm->mmu_lock);
	list_del(&tpage->link);
	spin_unlock(&kvm->mmu_lock);

	kfree(tpage);

	return 0;
}

static int kvm_sbi_ext_covg_handler(struct kvm_vcpu *vcpu, struct kvm_run *run,
				    struct kvm_vcpu_sbi_return *retdata)
{
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;
	uint32_t num_pages = cp->a1 / PAGE_SIZE;
	unsigned long funcid = cp->a6;
	unsigned long *err_val = &retdata->err_val;
	uint32_t i;
	int ret;

	switch (funcid) {
	case SBI_EXT_COVG_SHARE_MEMORY:
		for (i = 0; i < num_pages; i++) {
			ret = kvm_riscv_cove_share_page(
				vcpu, cp->a0 + i * PAGE_SIZE, err_val);
			if (ret || *err_val != SBI_SUCCESS)
				return ret;
		}
		return 0;

	case SBI_EXT_COVG_UNSHARE_MEMORY:
		for (i = 0; i < num_pages; i++) {
			ret = kvm_riscv_cove_unshare_page(
				vcpu, cp->a0 + i * PAGE_SIZE);
			if (ret)
				return ret;
		}
		return 0;

	case SBI_EXT_COVG_ADD_MMIO_REGION:
	case SBI_EXT_COVG_REMOVE_MMIO_REGION:
	case SBI_EXT_COVG_ALLOW_EXT_INTERRUPT:
	case SBI_EXT_COVG_DENY_EXT_INTERRUPT:
		/* We don't really need to do anything here for now. */
		return 0;

	default:
		kvm_err("%s: Unsupported guest SBI %ld.\n", __func__, funcid);
		retdata->err_val = SBI_ERR_NOT_SUPPORTED;
		return -EOPNOTSUPP;
	}
}

static unsigned long kvm_sbi_ext_covg_probe(struct kvm_vcpu *vcpu)
{
	/* KVM COVG SBI handler is only meant for handling calls from TSM */
	return 1;
}

const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_covg = {
	.extid_start = SBI_EXT_COVG,
	.extid_end = SBI_EXT_COVG,
	.handler = kvm_sbi_ext_covg_handler,
	.probe = kvm_sbi_ext_covg_probe,
};
