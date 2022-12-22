// SPDX-License-Identifier: GPL-2.0
/*
 * TEE related helper functions.
 *
 * Copyright (c) 2022 RivosInc
 *
 * Authors:
 *     Atish Patra <atishp@rivosinc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <linux/smp.h>
#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_nacl.h>
#include <asm/kvm_tee.h>
#include <asm/kvm_tee_sbi.h>
#include <asm/asm-offsets.h>

static struct sbi_tee_tsm_info tinfo;
struct sbi_tee_tvm_create_params params;

DEFINE_STATIC_KEY_FALSE(riscv_tee_enabled);

static void kvm_tee_local_fence(void *info)
{
	int rc;

	rc = sbi_teeh_tsm_local_fence();

	if (rc)
		kvm_err("local fence for TSM failed %d on cpu %d\n", rc, smp_processor_id());
}

static enum sbi_tee_page_type kvm_tee_map_ptype(unsigned long psize)
{
	switch(psize)
	{
		case KVM_TEE_PAGE_SIZE_4K:
			return SBI_TEE_PAGE_4K;
		case KVM_TEE_PAGE_SIZE_2MB:
			return SBI_TEE_PAGE_2MB;
		case KVM_TEE_PAGE_SIZE_1GB:
			return SBI_TEE_PAGE_1GB;
		case KVM_TEE_PAGE_SIZE_512GB:
			return SBI_TEE_PAGE_512GB;
		default:
			return -1;
	}
}

static void tee_delete_pinned_page_list(struct list_head *tpages)
{
	struct kvm_riscv_tee_page *tpage, *temp;
	int rc;

	list_for_each_entry_safe(tpage, temp, tpages, link) {
		rc = sbi_teeh_tsm_reclaim_page(page_to_phys(tpage->page));
		if (rc)
			kvm_err("Reclaiming page %llx failed\n", page_to_phys(tpage->page));
		unpin_user_pages_dirty_lock(&tpage->page, 1 , true);
		list_del(&tpage->link);
		kfree(tpage);
	}
}

__always_inline bool kvm_riscv_tee_enabled(void)
{
	return static_branch_unlikely(&riscv_tee_enabled);
}

int kvm_riscv_tee_hfence(void)
{
	int rc = sbi_teeh_tsm_global_fence();

	if (rc > 0) {
		kvm_err("global fence for TSM failed %d\n", rc);
		return rc;
	}

	/* Initiate local fence on each online hart */
	on_each_cpu(kvm_tee_local_fence, NULL, 1);

	return rc;
}

static int tee_convert_pages(struct kvm_riscv_tee_page_range *pgr, bool fence)
{
	int rc;

	if (!IS_ALIGNED(pgr->pgr_phys, PAGE_SIZE))
		return -EINVAL;

	rc = sbi_teeh_tsm_convert_pages(pgr->pgr_phys, pgr->num_pages);
	if (rc)
		return rc;

	/* Conversion was succesful. Flush the TLB if caller requested */
	if (fence)
		rc = kvm_riscv_tee_hfence();

	return rc;
}

void kvm_riscv_tee_vcpu_load(struct kvm_vcpu *vcpu)
{
	/* TODO */
}

void kvm_riscv_tee_vcpu_put(struct kvm_vcpu *vcpu)
{
	/* TODO */
}

int kvm_riscv_tee_gstage_map(struct kvm_vcpu *vcpu, gpa_t gpa, unsigned long hva)
{
	/* TODO */
	return 0;
}

void kvm_riscv_tee_vcpu_switchto(struct kvm_vcpu *vcpu, struct kvm_cpu_trap *trap)
{
	/* TODO */
}

void kvm_riscv_tee_vcpu_destroy(struct kvm_vcpu *vcpu)
{
	int rc;
	struct kvm_tee_tvm_vcpu_context *tvcpuc = vcpu->arch.tc;

	/**
	 * TODO: The per vcpu pages need to be claimed or returned a pool.
	 * Claiming is tricky at this point as vm_destroy has not invoked
	 * called yet.
	 * Once this function is returned, we lose the pointer to vcpu
	 * while the pages become dangling.
	 */

	rc = sbi_teeh_tsm_reclaim_pages(tvcpuc->vcpu_state.pgr_phys, tvcpuc->vcpu_state.num_pages);
	if (rc)
		kvm_err("Memory reclaim failed with rc %d\n", rc);

	/* Free the allocated pages now */
	free_pages((unsigned long)tvcpuc->vcpu_state.pgr, get_order_num_pages(tvcpuc->vcpu_state.num_pages));
	free_pages((unsigned long)tvcpuc->reg_shmem.pgr, get_order_num_pages(tvcpuc->reg_shmem.num_pages));
}

int kvm_riscv_tee_vcpu_init(struct kvm_vcpu *vcpu)
{
	int rc;
	struct kvm *kvm;
	struct kvm_tee_tvm_vcpu_context *tvcpuc;
	struct kvm_tee_tvm_context *tvmc;
	struct page *vcpus_page;

	if (!vcpu)
		return -EINVAL;

	kvm = vcpu->kvm;

	if (!kvm->arch.tvmc)
		return -EINVAL;

	tvmc = kvm->arch.tvmc;

	if (tvmc->finalized_done) {
		kvm_err("vcpu init must not happen after finalize\n");
		return -EINVAL;
	}

	tvcpuc = kzalloc(sizeof(*tvcpuc), GFP_KERNEL);
	if (!tvcpuc)
		return -ENOMEM;

	vcpus_page = alloc_pages(GFP_KERNEL | __GFP_ZERO,
				 get_order_num_pages(tinfo.tvcpu_pages_needed));
	if (!vcpus_page) {
		rc = -ENOMEM;
		goto tvcpuc_error;
	}

	tvcpuc->vcpu = vcpu;
	tvcpuc->vcpu_state.ptype = kvm_tee_map_ptype(PAGE_SIZE);
	tvcpuc->vcpu_state.num_pages = tinfo.tvcpu_pages_needed;
	tvcpuc->vcpu_state.pgr = page_to_virt(vcpus_page);
	tvcpuc->vcpu_state.pgr_phys = page_to_phys(vcpus_page);

	rc = tee_convert_pages(&tvcpuc->vcpu_state, true);
	if (rc)
		goto vcpus_error;

	rc = sbi_teeh_create_tvm_vcpu(tvmc->tvm_guest_id, vcpu->vcpu_idx,
				      tvcpuc->vcpu_state.pgr_phys);
	if (rc)
		goto vcpus_error;

	vcpu->arch.tc = tvcpuc;

	return 0;

vcpus_error:
	//TODO: We need to reclaim all the pages in error conditions ??
	__free_pages(vcpus_page, get_order_num_pages(tinfo.tvcpu_pages_needed));

tvcpuc_error:
	kfree(tvcpuc);
	return rc;
}

int kvm_riscv_tee_vm_add_memreg(struct kvm *kvm, unsigned long gpa, unsigned long size)
{
	int rc;
	struct kvm_tee_tvm_context *tvmc = kvm->arch.tvmc;

	if (!tvmc)
		return -EFAULT;

	if (tvmc->finalized_done) {
		kvm_err("Memory region can not be added after finalize\n");
		return -EINVAL;
	}

	tvmc->confidential_region.gpa = gpa;
	tvmc->confidential_region.npages = bytes_to_pages(size);

	rc = sbi_teeh_add_memory_region(tvmc->tvm_guest_id, TVM_MEM_REGION_TYPE_CONFIDENTIAL, gpa, size);
	if (rc) {
		kvm_err("Registering confidential memory region failed with rc %d\n", rc);
		return rc;
	}

	kvm_info("%s: Success with gpa %lx size %lx\n", __func__, gpa, size);

	return 0;
}

/*
 * Destroying A TVM is expensive because we need to reclaim all the pages by iterating over it.
 * Few ideas to improve:
 * 1. At least do the reclaim part in a worker thread in the background
 * 2. Define a page pool which can contain a pre-allocated/converted pages.
 *    In this step, we just return to the confidential page pool. Thus, some other TVM
 *    can use it.
 */
void kvm_riscv_tee_vm_destroy(struct kvm *kvm)
{
	int rc;
	struct kvm_tee_tvm_context *tvmc = kvm->arch.tvmc;
	struct kvm_riscv_tee_page_range pgdr;

	if (!tvmc)
		return;
	/* Release all the confidential pages using TEEH SBI call */
	rc = sbi_teeh_tsm_destroy_tvm(tvmc->tvm_guest_id);
	if (rc) {
		kvm_err("TVM %ld destruction failed with rc = %d\n", tvmc->tvm_guest_id, rc);
		return;
	}
	/* Reclaim all pages first */
	pgdr.num_pages = gstage_gpa_size >> PAGE_SHIFT;
	pgdr.pgr_phys = kvm->arch.pgd_phys;
	pgdr.ptype = kvm_tee_map_ptype(PAGE_SIZE);

	rc = sbi_teeh_tsm_reclaim_pages(pgdr.pgr_phys, pgdr.num_pages);
	if (rc)
		goto reclaim_failed;

	rc = sbi_teeh_tsm_reclaim_pages(tvmc->pgtable.pgr_phys, tvmc->pgtable.num_pages);
	if (rc)
		goto reclaim_failed;

	rc = sbi_teeh_tsm_reclaim_pages(tvmc->tvm_state.pgr_phys, tvmc->tvm_state.num_pages);
	if (rc)
		goto reclaim_failed;

	tee_delete_pinned_page_list(&tvmc->measured_pages);
	tee_delete_pinned_page_list(&tvmc->zero_pages);
	tee_delete_pinned_page_list(&tvmc->shared_pages);

	/* Free all the pages allocated due to TEE*/
	free_pages((unsigned long)tvmc->pgtable.pgr, get_order_num_pages(tvmc->pgtable.num_pages));
	free_pages((unsigned long)tvmc->tvm_state.pgr, get_order_num_pages(tvmc->pgtable.num_pages));

	kfree(tvmc);

	/* Now free the pgd */

	return;

reclaim_failed:
	kvm_err("Memory reclaim failed with rc %d\n", rc);
}


int kvm_riscv_tee_vm_init(struct kvm *kvm)
{
	struct kvm_tee_tvm_context *tvmc;
	struct page *tvms_page, *pgt_page;
	unsigned long tvm_gid, ptype;
	struct kvm_riscv_tee_page_range pgdr;
	int rc = 0;

	tvmc = kzalloc(sizeof(*tvmc), GFP_KERNEL);
	if (!tvmc)
		return -ENOMEM;

	/* pgd is always 16KB aligned */
	ptype = kvm_tee_map_ptype(PAGE_SIZE);
	pgdr.num_pages = gstage_pgd_size >> PAGE_SHIFT;
	pgdr.pgr_phys = kvm->arch.pgd_phys;
	pgdr.ptype = ptype;
	kvm_info("%s: gstage_pgd_size %lx %ld\n", __func__, gstage_pgd_size, pgdr.num_pages);

	rc = tee_convert_pages(&pgdr, false);
	if (rc)
		goto done;

	kvm_info("%s: In tvm pages needed %ld vcpu pages %ld max_vcpus %ld\n", __func__, 
		tinfo.tvm_pages_needed, tinfo.tvcpu_pages_needed, tinfo.tvm_max_vcpus);
	tvms_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, get_order_num_pages(tinfo.tvm_pages_needed));
	if (!tvms_page) {
		rc = -ENOMEM;
		goto tc_error;
	}

	kvm_info("tvms state page allocation successful\n");
	tvmc->kvm = kvm;

	//TODO: Should we use page_address instead of page_to_virt ?
	tvmc->tvm_state.pgr = page_to_virt(tvms_page);
	tvmc->tvm_state.pgr_phys = page_to_phys(tvms_page);
	tvmc->tvm_state.num_pages = tinfo.tvm_pages_needed; 
	tvmc->tvm_state.ptype = ptype; 

	rc = tee_convert_pages(&tvmc->tvm_state, false);
	if (rc) {
		kvm_err("%s: tvm state page conversion failed rc %d\n", __func__, rc);
		goto tstate_error;
	}

	INIT_LIST_HEAD(&tvmc->measured_pages);
	INIT_LIST_HEAD(&tvmc->zero_pages);
	INIT_LIST_HEAD(&tvmc->shared_pages);

	kvm_info("tvms convert page successful\n");
	/* TODO: Just give enough pages for page table pool for now */
	pgt_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, get_order(KVM_TEE_PGTABLE_SIZE_MAX));
	if (!pgt_page) {
		rc = -ENOMEM;
		goto tstate_error;
	}

	tvmc->pgtable.num_pages = KVM_TEE_PGTABLE_SIZE_MAX >> PAGE_SHIFT;
	//TODO: Should we use page_address instead of page_to_virt ?
	tvmc->pgtable.pgr = page_to_virt(pgt_page);
	tvmc->pgtable.pgr_phys = page_to_phys(pgt_page);
	tvmc->pgtable.ptype = ptype;

	kvm_info("Creating page table pages at %llx num_pages %lx\n", tvmc->pgtable.pgr_phys, tvmc->pgtable.num_pages);
	rc = tee_convert_pages(&tvmc->pgtable, false);
	if (rc) {
		kvm_err("%s: page table pool conversion failed rc %d\n", __func__, rc);
		goto pgt_error;
	}

	rc = kvm_riscv_tee_hfence();
	if (rc)
		goto pgt_error;

	/* The requires pages have been converted to confidential memory. Create the TVM now */
	params.tvm_page_directory_addr = kvm->arch.pgd_phys;
	params.tvm_state_addr = tvmc->tvm_state.pgr_phys;

	rc = sbi_teeh_tsm_create_tvm(&params, &tvm_gid);
	if(rc)
		goto pgt_error;

	tvmc->tvm_guest_id = tvm_gid;
	kvm->arch.tvmc = tvmc;

	rc = sbi_teeh_add_pgt_pages(tvm_gid, tvmc->pgtable.pgr_phys, tvmc->pgtable.num_pages);
	if (rc)
		goto pgt_error;

	kvm_info("Guest VM creation successful with guest id %lx\n", tvm_gid);

	return 0;

pgt_error:
	__free_pages(pgt_page, get_order(KVM_TEE_PGTABLE_SIZE_MAX));

tstate_error:
	__free_pages(tvms_page, get_order_num_pages(tinfo.tvm_pages_needed));

tc_error:
	kfree(tvmc);

done:
	return rc;
}

int kvm_riscv_tee_init(void)
{
	int rc;

	/* We currently support host in VS mode. Thus, NACL is mandatory */
	if (sbi_probe_extension(SBI_EXT_TEEH) <= 0 || !kvm_riscv_nacl_available())
		return -EOPNOTSUPP;

	kvm_info("The platform has confidential computing feature enabled\n");
	static_branch_enable(&riscv_tee_enabled);

	rc = sbi_teeh_tsm_get_info(&tinfo);
	if (rc < 0)
		return -EINVAL;

	if (tinfo.tstate != TSM_READY) {
		kvm_err("TSM is not ready yet. Can't run TVMs\n");
		return -EAGAIN;
	}

	kvm_info("TSM version %d is loaded and ready to run\n", tinfo.version);

	return 0;
}
