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
#include <linux/cpumask.h>
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

static void kvm_tee_imsic_clone(void *info)
{
	int rc;
	struct kvm_vcpu *vcpu = info;
	struct kvm *kvm = vcpu->kvm;

	pr_err("%s: In pcpu %d vcpu %d\n", __func__, smp_processor_id(), vcpu->vcpu_idx);

	rc = sbi_teei_rebind_vcpu_imsic_clone(kvm->arch.tvmc->tvm_guest_id, vcpu->vcpu_idx);
	if (rc)
		kvm_err("imsic clone failed on cpu %d rc %d\n", rc, smp_processor_id());
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

int kvm_riscv_tee_vcpu_imsic_addr(struct kvm_vcpu *vcpu)
{
	struct kvm_tee_tvm_context *tvmc;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_vcpu_aia *vaia = &vcpu->arch.aia;
	int ret;

	if (!kvm->arch.tvmc)
		return -EINVAL;

	tvmc = kvm->arch.tvmc;

	ret = sbi_teei_set_vcpu_imsic_addr(tvmc->tvm_guest_id, vcpu->vcpu_idx, vaia->imsic_addr);
	if (ret)
		return -EPERM;

	return 0;
}

int kvm_riscv_tee_aia_convert_imsic(struct kvm_vcpu *vcpu, phys_addr_t imsic_pa)
{
	struct kvm *kvm = vcpu->kvm;
	int ret;

	if (!kvm->arch.tvmc)
		return -EINVAL;

	//TODO: Check bind sanity check - imsic file must not be bound
	ret = sbi_teei_convert_imsic(imsic_pa);
	if (ret)
		return -EPERM;

	ret = kvm_riscv_tee_hfence();
	if (ret)
		return ret;

	return 0;
}

int kvm_riscv_tee_aia_claim_imsic(struct kvm_vcpu *vcpu, phys_addr_t imsic_pa)
{
	int ret;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_tee_tvm_vcpu_context *tvcpu = vcpu->arch.tc;

	if (!kvm->arch.tvmc)
		return -EINVAL;

	if (tvcpu->imsic.bound) {
		kvm_err("Imisc claim can not proceed as it is still bound on vcpu %d\n",
			vcpu->vcpu_idx);
		return -EINVAL;
	}

	ret = sbi_teei_reclaim_imsic(imsic_pa);
	if (ret)
		return -EPERM;

	//TODO: Do we need a hfence here ?

	return 0;
}

int kvm_riscv_tee_vcpu_imsic_rebind(struct kvm_vcpu *vcpu, int old_pcpu)
{
	struct kvm_tee_tvm_context *tvmc;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_tee_tvm_vcpu_context *tvcpu = vcpu->arch.tc;
	int ret;
	unsigned long gid, vcpuid;
	cpumask_var_t tmpmask;

	if (!kvm->arch.tvmc)
		return -EINVAL;

	tvmc = kvm->arch.tvmc;
	gid  = tvmc->tvm_guest_id;
	vcpuid = vcpu->vcpu_idx;

	ret = sbi_teei_rebind_vcpu_imsic_begin(gid, vcpuid, BIT(tvcpu->imsic.vsfile_hgei));
	if (ret) {
		kvm_err("unable to rebind vcpu %d", ret);
		return ret;
	}

	ret = sbi_teeh_tvm_initiate_fence(gid);
	if (ret)
		return ret;

	__cpumask_set_cpu(old_pcpu, tmpmask);
	on_each_cpu_mask(tmpmask, kvm_tee_imsic_clone, vcpu, 1);

	ret = sbi_teei_rebind_vcpu_imsic_end(gid, vcpuid);
	if (ret) {
		pr_err("%s: rebind failed %d\n", __func__, ret);
		return ret;
	}

	tvcpu->imsic.bound = true;

	return 0;
}

int kvm_riscv_tee_vcpu_imsic_bind(struct kvm_vcpu *vcpu, unsigned long imsic_mask)
{
	struct kvm_tee_tvm_context *tvmc;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_tee_tvm_vcpu_context *tvcpu = vcpu->arch.tc;
	int ret;

	if (!kvm->arch.tvmc)
		return -EINVAL;

	tvmc = kvm->arch.tvmc;

	ret = sbi_teei_bind_vcpu_imsic(tvmc->tvm_guest_id, vcpu->vcpu_idx, imsic_mask);
	if (ret) {
		kvm_err("unable to bind vcpu %d", ret);
		return ret;
	}
	tvcpu->imsic.bound = true;

	return 0;
}

int kvm_riscv_tee_vcpu_imsic_unbind(struct kvm_vcpu *vcpu)
{
	struct kvm_tee_tvm_context *tvmc;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_tee_tvm_vcpu_context *tvcpu = vcpu->arch.tc;
	int ret;

	if (!kvm->arch.tvmc)
		return -EINVAL;

	tvmc = kvm->arch.tvmc;

	ret = sbi_teei_unbind_vcpu_imsic_begin(tvmc->tvm_guest_id, vcpu->vcpu_idx);
	if (ret)
		return ret;

	ret = sbi_teeh_tvm_initiate_fence(tvmc->tvm_guest_id);
	if (ret)
		return ret;

	ret = sbi_teei_unbind_vcpu_imsic_end(tvmc->tvm_guest_id, vcpu->vcpu_idx);
	if (ret)
		return ret;
	tvcpu->imsic.bound = false;

	return 0;
}

int kvm_riscv_tee_vcpu_inject_interrupt(struct kvm_vcpu *vcpu, unsigned long iid)
{
	struct kvm_tee_tvm_context *tvmc;
	struct kvm *kvm = vcpu->kvm;
	int ret;

	if (!kvm->arch.tvmc)
		return -EINVAL;

	tvmc = kvm->arch.tvmc;

	ret = sbi_teei_inject_external_interrupt(tvmc->tvm_guest_id, vcpu->vcpu_idx, iid);
	if (ret)
		return ret;

	return 0;
}

int kvm_riscv_tee_aia_init(struct kvm *kvm)
{
	struct kvm_aia *aia = &kvm->arch.aia;
	struct sbi_tee_tvm_aia_params *tvm_aia;
	struct kvm_vcpu *vcpu;
	struct kvm_tee_tvm_context *tvmc;
	int ret;

	if (!kvm->arch.tvmc)
		return -EINVAL;

	tvmc = kvm->arch.tvmc;

	/* Sanity Check */
	if (aia->aplic_addr != KVM_RISCV_AIA_UNDEF_ADDR)
		return -EINVAL;

	/* TVMs must have a physical guest interrut file */
	if (aia->mode != KVM_DEV_RISCV_AIA_MODE_HWACCEL)
		return -ENODEV;

	tvm_aia = kzalloc(sizeof(*tvm_aia), GFP_KERNEL);
	if (!tvm_aia)
		return -ENOMEM;

	 /*TODO: KVM only knows the IMSIC address of vcpus. Can the base address
	 * be different than that ?
	 */
	/* Address of the IMSIC group ID, hart ID & guest ID of 0 */
	vcpu = kvm_get_vcpu_by_id(kvm, 0);
	tvm_aia->imsic_base_addr = vcpu->arch.aia.imsic_addr;

	tvm_aia->group_index_bits = aia->nr_group_bits;
	tvm_aia->group_index_shift = aia->nr_group_shift;
	tvm_aia->hart_index_bits = aia->nr_hart_bits;
	tvm_aia->guest_index_bits = aia->nr_guest_bits;
	/* Nested TVMs are not supported yet */
	tvm_aia->guests_per_hart = 0;


	ret = sbi_teei_tvm_aia_init(tvmc->tvm_guest_id, tvm_aia);
	if (ret)
		kvm_err("TVM AIA init failed with rc %d\n", ret);

	return ret;
}

void kvm_riscv_tee_vcpu_load(struct kvm_vcpu *vcpu)
{
	/* TODO */
}

void kvm_riscv_tee_vcpu_put(struct kvm_vcpu *vcpu)
{
	/* TODO */
}

/* Inspired from pkvm_mem_abort */
int kvm_riscv_tee_gstage_map(struct kvm_vcpu *vcpu, gpa_t gpa, unsigned long hva)
{
	struct kvm_riscv_tee_page *tpage;
	struct mm_struct *mm = current->mm;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_riscv_tee_page_range nc_pr;
	//TODO: Do need a FOLL_HWPOISION like pkvm ?
	unsigned int flags = FOLL_LONGTERM | FOLL_WRITE;
	struct page *page;
	int rc;
	struct kvm_tee_tvm_context *tvmc = kvm->arch.tvmc;

	tpage = kmalloc(sizeof(*tpage), GFP_KERNEL_ACCOUNT);
	if (!tpage)
		return -ENOMEM;

	mmap_read_lock(mm);
	rc = pin_user_pages(hva, 1, flags, &page, NULL);
	mmap_read_unlock(mm);

	//TODO: Do we need to handle -EHWPOISON here as well?
	if (rc != 1) {
		rc = -EFAULT;
		goto free_tpage;
	} else if (!PageSwapBacked(page)) {
		rc = -EIO;
		goto free_tpage;
	}

	nc_pr.num_pages = 1;
	nc_pr.pgr_phys = page_to_phys(page);
	nc_pr.pgr = page_to_virt(page);
	nc_pr.ptype = kvm_tee_map_ptype(PAGE_SIZE);

	//TODO: We can't do spin lock here it sends IPI to each vcpu to initiate local fence. 
	rc = tee_convert_pages(&nc_pr, true);
	if(rc)
		goto unpin_page;

	spin_lock(&kvm->mmu_lock);
	rc = sbi_teeh_add_zero_pages(tvmc->tvm_guest_id, nc_pr.pgr_phys,
				     nc_pr.ptype, 1, gpa);
	if (rc) {
		pr_err("%s: Adding zero pages failed %d\n", __func__, rc);
		goto zero_page_failed;
	}
	tpage->page = page;
	INIT_LIST_HEAD(&tpage->link);
	list_add(&tpage->link, &kvm->arch.tvmc->zero_pages);

	spin_unlock(&kvm->mmu_lock);

	return 0;

zero_page_failed:
	spin_unlock(&kvm->mmu_lock);

unpin_page:
	unpin_user_pages(&page, 1);

free_tpage:
	kfree(tpage);

	return rc;
}

void noinstr kvm_riscv_tee_vcpu_switchto(struct kvm_vcpu *vcpu, struct kvm_cpu_trap *trap)
{
	int rc;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_tee_tvm_context *tvmc;
	struct kvm_cpu_context *cntx = &vcpu->arch.guest_context;
	void *nshmem;
	struct kvm_tee_tvm_vcpu_context *tvcpu = vcpu->arch.tc;

	if (!kvm->arch.tvmc)
		return;

	tvmc = kvm->arch.tvmc;

	nshmem = nacl_shmem();
	/* Invoke finalize to mark TVM is ready run for the first time */
	if (unlikely(!tvmc->finalized_done)) {

		rc = sbi_teeh_tsm_finalize_tvm(tvmc->tvm_guest_id, cntx->sepc, cntx->a1);
		if (rc) {
			kvm_err("TVM Finalized failed with %d\n", rc);
			return;
		}
		tvmc->finalized_done = true;
	}

	/*
	 * TODO: Ideally, the bind should happen in imsic during new vsfile allocation.
	 * However, the TEEH BIND call requires the TVM to be in finalized state.
	 * Check with Salus implementation if the bind can happen before finalization.
	 */
	if (tvcpu->imsic.bind_required) {
		tvcpu->imsic.bind_required = false;
		rc = kvm_riscv_tee_vcpu_imsic_bind(vcpu, BIT(tvcpu->imsic.vsfile_hgei));
		if (rc) {
			kvm_err("bind failed with rc %d\n", rc);
			return;
		}
	}

	rc = sbi_teeh_run_tvm_vcpu(tvmc->tvm_guest_id, vcpu->vcpu_idx);
	if (rc) {
		//TODO: Should we try return to the user space or panic ?
		kvm_err("TVM run failed vcpu id %d with rc %d\n", vcpu->vcpu_idx, rc);
		return;
	}

	trap->htinst = nacl_shmem_csr_read(nshmem, CSR_HTINST);
	trap->htval = nacl_shmem_csr_read(nshmem, CSR_HTVAL);
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

int kvm_riscv_tee_vm_measure_pages(struct kvm *kvm, struct kvm_riscv_tee_measure_region *mr)
{
	struct kvm_tee_tvm_context *tvmc = kvm->arch.tvmc;
	struct kvm_riscv_tee_page_range measured_pr;
	int rc, idx, num_pages;
	struct kvm_riscv_tee_mem_region *conf;
	struct page *pinned_page, *conf_page;
	struct kvm_riscv_tee_page *cpage;

	if (!tvmc)
		return -EFAULT;

	if (tvmc->finalized_done) {
		kvm_err("measured_mr pages can not be added after finalize\n");
		return -EINVAL;
	}

	num_pages = bytes_to_pages(mr->size);

	kvm_info("%s: In user_addr %lx gpa %lx size %lx num_pages %d...\n", __func__,
		mr->userspace_addr, mr->gpa, mr->size, num_pages);
	conf = &tvmc->confidential_region;

	if (!IS_ALIGNED(mr->userspace_addr, PAGE_SIZE) ||
	    !IS_ALIGNED(mr->gpa, PAGE_SIZE) || !mr->size ||
	    !((conf->gpa <= mr->gpa) && ((conf->gpa + (conf->npages << PAGE_SHIFT)) >=
	    				 mr->gpa + mr->size)))
		return -EINVAL;

	idx = srcu_read_lock(&kvm->srcu);

	/*TODO: Iterate one page at a time as pinning multiple pages fail with unmapped panic
	 * with a virtual address range belonging to vmalloc region for some reason.
	 */
	while(num_pages) {
		if (signal_pending(current)) {
			rc = -ERESTARTSYS;
			break;
		}

		if (need_resched())
			cond_resched();

		rc = get_user_pages_fast(mr->userspace_addr, 1, 0, &pinned_page);
		if (rc < 0) {
			kvm_err("Pinning the userpsace addr %lx failed\n", mr->userspace_addr);
			break;
		}

		/* Enough pages are not available to be pinned */
		if (rc != 1) {
			rc = -ENOMEM;
			break;
		}
		conf_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!conf_page) {
			rc = -ENOMEM;
			break;
		}

		measured_pr.num_pages = 1;
		measured_pr.pgr_phys = page_to_phys(conf_page);
		measured_pr.pgr = page_to_virt(conf_page);
		measured_pr.ptype = kvm_tee_map_ptype(PAGE_SIZE);

		/*TODO: Can we measure it without issueing fence ?*/
		rc = tee_convert_pages(&measured_pr, true);
		if (rc)
			goto conf_error;

		rc = sbi_teeh_add_measured_pages(tvmc->tvm_guest_id, page_to_phys(pinned_page),
					 	page_to_phys(conf_page), kvm_tee_map_ptype(PAGE_SIZE),
					 	1, mr->gpa);

		/* Unpin the page now */
		put_page(pinned_page);
		if (rc)
			goto measure_error;

		cpage = kmalloc(sizeof(*cpage), GFP_KERNEL_ACCOUNT);
		if (!cpage) {
			rc = -ENOMEM;
			goto measure_error;
		}

		cpage->page = conf_page;
		INIT_LIST_HEAD(&cpage->link);
		list_add(&cpage->link, &tvmc->measured_pages);

		mr->userspace_addr += PAGE_SIZE;
		mr->gpa += PAGE_SIZE;
		num_pages--;

		continue;
conf_error:
		kvm_err("Converting page failed\n");
measure_error:
		kvm_err("Adding measured page at %lx failed\n", mr->gpa);
		break;
	}

	srcu_read_unlock(&kvm->srcu, idx);
	//TODO: Iterate over the converted page list and free them
	//If one conversion/add failed: We need to delete everything and return error
	if (num_pages)
		kvm_err("Adding measured pages or conversion failed %d\n", num_pages);

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
