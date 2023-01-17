// SPDX-License-Identifier: GPL-2.0
/*
 * COVE related helper functions.
 *
 * Copyright (c) 2023 RivosInc
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
#include <asm/kvm_nacl.h>
#include <asm/kvm_cove.h>
#include <asm/kvm_cove_sbi.h>
#include <asm/asm-offsets.h>

static struct sbi_cove_tsm_info tinfo;
struct sbi_cove_tvm_create_params params;

/* We need a global lock as initiate fence can be invoked once per host */
static DEFINE_SPINLOCK(cove_fence_lock);

static bool riscv_cove_enabled;

static inline bool cove_is_within_region(unsigned long addr1, unsigned long size1,
				       unsigned long addr2, unsigned long size2)
{
	return ((addr1 <= addr2) && ((addr1 + size1) >= (addr2 + size2)));
}

static void kvm_cove_local_fence(void *info)
{
	int rc;

	rc = sbi_covh_tsm_local_fence();

	if (rc)
		kvm_err("local fence for TSM failed %d on cpu %d\n", rc, smp_processor_id());
}

static void cove_delete_page_list(struct kvm *kvm, struct list_head *tpages, bool unpin)
{
	struct kvm_riscv_cove_page *tpage, *temp;
	int rc;

	list_for_each_entry_safe(tpage, temp, tpages, link) {
		rc = sbi_covh_tsm_reclaim_pages(page_to_phys(tpage->page), tpage->npages);
		if (rc)
			kvm_err("Reclaiming page %llx failed\n", page_to_phys(tpage->page));
		if (unpin)
			unpin_user_pages_dirty_lock(&tpage->page, 1, true);
		list_del(&tpage->link);
		kfree(tpage);
	}
}

static int kvm_riscv_cove_fence(void)
{
	int rc;

	spin_lock(&cove_fence_lock);

	rc = sbi_covh_tsm_initiate_fence();
	if (rc) {
		kvm_err("initiate fence for tsm failed %d\n", rc);
		goto done;
	}

	/* initiate local fence on each online hart */
	on_each_cpu(kvm_cove_local_fence, NULL, 1);
done:
	spin_unlock(&cove_fence_lock);
	return rc;
}

int kvm_riscv_cove_tvm_fence(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_cove_tvm_context *tvmc = kvm->arch.tvmc;
	DECLARE_BITMAP(vcpu_mask, KVM_MAX_VCPUS);
	unsigned long i;
	struct kvm_vcpu *temp_vcpu;
	int ret;

	if (!tvmc)
		return -EINVAL;

	spin_lock(&tvmc->tvm_fence_lock);
	ret = sbi_covh_tvm_initiate_fence(tvmc->tvm_guest_id);
	if (ret) {
		spin_unlock(&tvmc->tvm_fence_lock);
		return ret;
	}

	bitmap_clear(vcpu_mask, 0, KVM_MAX_VCPUS);
	kvm_for_each_vcpu(i, temp_vcpu, kvm) {
		if (temp_vcpu != vcpu)
			bitmap_set(vcpu_mask, i, 1);
	}

	/*
	 * The host just needs to make sure that the running vcpus exit the
	 * guest mode and traps into TSM so that it can issue hfence.
	 */
	kvm_make_vcpus_request_mask(kvm, KVM_REQ_OUTSIDE_GUEST_MODE, vcpu_mask);
	spin_unlock(&tvmc->tvm_fence_lock);

	return 0;
}


static int cove_convert_pages(unsigned long phys_addr, unsigned long npages, bool fence)
{
	int rc;

	if (!IS_ALIGNED(phys_addr, PAGE_SIZE))
		return -EINVAL;

	rc = sbi_covh_tsm_convert_pages(phys_addr, npages);
	if (rc)
		return rc;

	/* Conversion was successful. Flush the TLB if caller requested */
	if (fence)
		rc = kvm_riscv_cove_fence();

	return rc;
}

__always_inline bool kvm_riscv_cove_enabled(void)
{
	return riscv_cove_enabled;
}

void kvm_riscv_cove_vcpu_load(struct kvm_vcpu *vcpu)
{
	/* TODO */
}

void kvm_riscv_cove_vcpu_put(struct kvm_vcpu *vcpu)
{
	/* TODO */
}

int kvm_riscv_cove_vcpu_sbi_ecall(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	void *nsh;
	const struct kvm_vcpu_sbi_extension *sbi_ext;
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;
	struct kvm_cpu_trap utrap = { 0 };
	struct kvm_vcpu_sbi_return sbi_ret = {
		.out_val = 0,
		.err_val = 0,
		.utrap = &utrap,
	};
	bool ext_is_01 = false;
	int ret = 1;

	nsh = nacl_shmem();
	cp->a0 = nacl_gpr_read_cove(nsh, KVM_ARCH_GUEST_A0);
	cp->a1 = nacl_gpr_read_cove(nsh, KVM_ARCH_GUEST_A1);
	cp->a6 = nacl_gpr_read_cove(nsh, KVM_ARCH_GUEST_A6);
	cp->a7 = nacl_gpr_read_cove(nsh, KVM_ARCH_GUEST_A7);

	/* TSM will only forward legacy console to the host */
#ifdef CONFIG_RISCV_SBI_V01
	if (cp->a7 == SBI_EXT_0_1_CONSOLE_PUTCHAR)
		ext_is_01 = true;
#endif

	sbi_ext = kvm_vcpu_sbi_find_ext(vcpu, cp->a7);
	if ((sbi_ext && sbi_ext->handler) && ((cp->a7 == SBI_EXT_DBCN) ||
	    (cp->a7 == SBI_EXT_HSM) || (cp->a7 == SBI_EXT_SRST) || ext_is_01)) {
		ret = sbi_ext->handler(vcpu, run, &sbi_ret);
	} else {
		kvm_err("%s: SBI EXT %lx not supported for TVM\n", __func__, cp->a7);
		/* Return error for unsupported SBI calls */
		sbi_ret.err_val = SBI_ERR_NOT_SUPPORTED;
		goto ecall_done;
	}

	if (ret < 0)
		goto ecall_done;

	ret = (sbi_ret.uexit) ? 0 : 1;

ecall_done:
	/*
	 * No need to update the sepc as TSM will take care of SEPC increment
	 * for ECALLS that won't be forwarded to the user space (e.g. console)
	 */
	nacl_gpr_write_cove(nsh, KVM_ARCH_GUEST_A0, sbi_ret.err_val);
	if (!ext_is_01)
		nacl_gpr_write_cove(nsh, KVM_ARCH_GUEST_A1, sbi_ret.out_val);

	return ret;
}

int kvm_riscv_cove_gstage_map(struct kvm_vcpu *vcpu, gpa_t gpa, unsigned long hva)
{
	struct kvm_riscv_cove_page *tpage;
	struct mm_struct *mm = current->mm;
	struct kvm *kvm = vcpu->kvm;
	unsigned int flags = FOLL_LONGTERM | FOLL_WRITE | FOLL_HWPOISON;
	struct page *page;
	int rc;
	struct kvm_cove_tvm_context *tvmc = kvm->arch.tvmc;

	tpage = kmalloc(sizeof(*tpage), GFP_KERNEL_ACCOUNT);
	if (!tpage)
		return -ENOMEM;

	mmap_read_lock(mm);
	rc = pin_user_pages(hva, 1, flags, &page);
	mmap_read_unlock(mm);

	if (rc == -EHWPOISON) {
		send_sig_mceerr(BUS_MCEERR_AR, (void __user *)hva,
				PAGE_SHIFT, current);
		rc = 0;
		goto free_tpage;
	} else if (rc != 1) {
		rc = -EFAULT;
		goto free_tpage;
	} else if (!folio_test_swapbacked(page_folio(page))) {
		rc = -EIO;
		goto free_tpage;
	}

	rc = cove_convert_pages(page_to_phys(page), 1, true);
	if (rc)
		goto unpin_page;

	rc = sbi_covh_add_zero_pages(tvmc->tvm_guest_id, page_to_phys(page),
				     SBI_COVE_PAGE_4K, 1, gpa);
	if (rc) {
		pr_err("%s: Adding zero pages failed %d\n", __func__, rc);
		goto zero_page_failed;
	}
	tpage->page = page;
	tpage->npages = 1;
	tpage->is_mapped = true;
	tpage->gpa = gpa;
	tpage->hva = hva;
	INIT_LIST_HEAD(&tpage->link);

	spin_lock(&kvm->mmu_lock);
	list_add(&tpage->link, &kvm->arch.tvmc->zero_pages);
	spin_unlock(&kvm->mmu_lock);

	return 0;

zero_page_failed:
	//TODO: Do we need to reclaim the page now or VM gets destroyed ?

unpin_page:
	unpin_user_pages(&page, 1);

free_tpage:
	kfree(tpage);

	return rc;
}

void kvm_riscv_cove_vcpu_switchto(struct kvm_vcpu *vcpu, struct kvm_cpu_trap *trap)
{
	/* TODO */
}

void kvm_riscv_cove_vcpu_destroy(struct kvm_vcpu *vcpu)
{
	struct kvm_cove_tvm_vcpu_context *tvcpuc = vcpu->arch.tc;
	struct kvm *kvm = vcpu->kvm;

	/*
	 * Just add the vcpu state pages to a list at this point as these can not
	 * be claimed until tvm is destroyed. *
	 */
	list_add(&tvcpuc->vcpu_state.link, &kvm->arch.tvmc->reclaim_pending_pages);
}

int kvm_riscv_cove_vcpu_init(struct kvm_vcpu *vcpu)
{
	int rc;
	struct kvm *kvm;
	struct kvm_cove_tvm_vcpu_context *tvcpuc;
	struct kvm_cove_tvm_context *tvmc;
	struct page *vcpus_page;
	unsigned long vcpus_phys_addr;

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
		goto alloc_page_failed;
	}

	tvcpuc->vcpu = vcpu;
	tvcpuc->vcpu_state.npages = tinfo.tvcpu_pages_needed;
	tvcpuc->vcpu_state.page = vcpus_page;
	vcpus_phys_addr = page_to_phys(vcpus_page);

	rc = cove_convert_pages(vcpus_phys_addr, tvcpuc->vcpu_state.npages, true);
	if (rc)
		goto convert_failed;

	rc = sbi_covh_create_tvm_vcpu(tvmc->tvm_guest_id, vcpu->vcpu_idx, vcpus_phys_addr);
	if (rc)
		goto vcpu_create_failed;

	vcpu->arch.tc = tvcpuc;

	return 0;

vcpu_create_failed:
	/* Reclaim all the pages or return to the confidential page pool */
	sbi_covh_tsm_reclaim_pages(vcpus_phys_addr, tvcpuc->vcpu_state.npages);

convert_failed:
	__free_pages(vcpus_page, get_order_num_pages(tinfo.tvcpu_pages_needed));

alloc_page_failed:
	kfree(tvcpuc);
	return rc;
}

int kvm_riscv_cove_vm_measure_pages(struct kvm *kvm, struct kvm_riscv_cove_measure_region *mr)
{
	struct kvm_cove_tvm_context *tvmc = kvm->arch.tvmc;
	int rc = 0, idx, num_pages;
	struct kvm_riscv_cove_mem_region *conf;
	struct page *pinned_page, *conf_page;
	struct kvm_riscv_cove_page *cpage;

	if (!tvmc)
		return -EFAULT;

	if (tvmc->finalized_done) {
		kvm_err("measured_mr pages can not be added after finalize\n");
		return -EINVAL;
	}

	num_pages = bytes_to_pages(mr->size);
	conf = &tvmc->confidential_region;

	if (!IS_ALIGNED(mr->userspace_addr, PAGE_SIZE) ||
	    !IS_ALIGNED(mr->gpa, PAGE_SIZE) || !mr->size ||
	    !cove_is_within_region(conf->gpa, conf->npages << PAGE_SHIFT, mr->gpa, mr->size))
		return -EINVAL;

	idx = srcu_read_lock(&kvm->srcu);

	/*TODO: Iterate one page at a time as pinning multiple pages fail with unmapped panic
	 * with a virtual address range belonging to vmalloc region for some reason.
	 */
	while (num_pages) {
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

		rc = cove_convert_pages(page_to_phys(conf_page), 1, true);
		if (rc)
			break;

		/*TODO: Support other pages sizes */
		rc = sbi_covh_add_measured_pages(tvmc->tvm_guest_id, page_to_phys(pinned_page),
						 page_to_phys(conf_page), SBI_COVE_PAGE_4K,
						 1, mr->gpa);
		if (rc)
			break;

		/* Unpin the page now */
		put_page(pinned_page);

		cpage = kmalloc(sizeof(*cpage), GFP_KERNEL_ACCOUNT);
		if (!cpage) {
			rc = -ENOMEM;
			break;
		}

		cpage->page = conf_page;
		cpage->npages = 1;
		cpage->gpa = mr->gpa;
		cpage->hva = mr->userspace_addr;
		cpage->is_mapped = true;
		INIT_LIST_HEAD(&cpage->link);
		list_add(&cpage->link, &tvmc->measured_pages);

		mr->userspace_addr += PAGE_SIZE;
		mr->gpa += PAGE_SIZE;
		num_pages--;
		conf_page = NULL;

		continue;
	}
	srcu_read_unlock(&kvm->srcu, idx);

	if (rc < 0) {
		/* We don't to need unpin pages as it is allocated by the hypervisor itself */
		cove_delete_page_list(kvm, &tvmc->measured_pages, false);
		/* Free the last allocated page for which conversion/measurement failed */
		kfree(conf_page);
		kvm_err("Adding/Converting measured pages failed %d\n", num_pages);
	}

	return rc;
}

int kvm_riscv_cove_vm_add_memreg(struct kvm *kvm, unsigned long gpa, unsigned long size)
{
	int rc;
	struct kvm_cove_tvm_context *tvmc = kvm->arch.tvmc;

	if (!tvmc)
		return -EFAULT;

	if (tvmc->finalized_done) {
		kvm_err("Memory region can not be added after finalize\n");
		return -EINVAL;
	}

	tvmc->confidential_region.gpa = gpa;
	tvmc->confidential_region.npages = bytes_to_pages(size);

	rc = sbi_covh_add_memory_region(tvmc->tvm_guest_id, gpa, size);
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
void kvm_riscv_cove_vm_destroy(struct kvm *kvm)
{
	int rc;
	struct kvm_cove_tvm_context *tvmc = kvm->arch.tvmc;
	unsigned long pgd_npages;

	if (!tvmc)
		return;

	/* Release all the confidential pages using COVH SBI call */
	rc = sbi_covh_tsm_destroy_tvm(tvmc->tvm_guest_id);
	if (rc) {
		kvm_err("TVM %ld destruction failed with rc = %d\n", tvmc->tvm_guest_id, rc);
		return;
	}

	cove_delete_page_list(kvm, &tvmc->reclaim_pending_pages, false);
	cove_delete_page_list(kvm, &tvmc->measured_pages, false);
	cove_delete_page_list(kvm, &tvmc->zero_pages, true);

	/* Reclaim and Free the pages for tvm state management */
	rc = sbi_covh_tsm_reclaim_pages(page_to_phys(tvmc->tvm_state.page), tvmc->tvm_state.npages);
	if (rc)
		goto reclaim_failed;

	__free_pages(tvmc->tvm_state.page, get_order_num_pages(tvmc->tvm_state.npages));

	/* Reclaim and Free the pages for gstage page table management */
	rc = sbi_covh_tsm_reclaim_pages(page_to_phys(tvmc->pgtable.page), tvmc->pgtable.npages);
	if (rc)
		goto reclaim_failed;

	__free_pages(tvmc->pgtable.page, get_order_num_pages(tvmc->pgtable.npages));

	/* Reclaim the confidential page for pgd */
	pgd_npages = kvm_riscv_gstage_pgd_size() >> PAGE_SHIFT;
	rc = sbi_covh_tsm_reclaim_pages(kvm->arch.pgd_phys, pgd_npages);
	if (rc)
		goto reclaim_failed;

	kfree(tvmc);

	return;

reclaim_failed:
	kvm_err("Memory reclaim failed with rc %d\n", rc);
}

int kvm_riscv_cove_vm_init(struct kvm *kvm)
{
	struct kvm_cove_tvm_context *tvmc;
	struct page *tvms_page, *pgt_page;
	unsigned long tvm_gid, pgt_phys_addr, tvms_phys_addr;
	unsigned long gstage_pgd_size = kvm_riscv_gstage_pgd_size();
	int rc = 0;

	tvmc = kzalloc(sizeof(*tvmc), GFP_KERNEL);
	if (!tvmc)
		return -ENOMEM;

	/* Allocate the pages required for gstage page table management */
	/* TODO: Just give enough pages for page table pool for now */
	pgt_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, get_order(KVM_COVE_PGTABLE_SIZE_MAX));
	if (!pgt_page)
		return -ENOMEM;

	/* pgd is always 16KB aligned */
	rc = cove_convert_pages(kvm->arch.pgd_phys, gstage_pgd_size >> PAGE_SHIFT, false);
	if (rc)
		goto done;

	/* Convert the gstage page table pages */
	tvmc->pgtable.page = pgt_page;
	tvmc->pgtable.npages = KVM_COVE_PGTABLE_SIZE_MAX >> PAGE_SHIFT;
	pgt_phys_addr = page_to_phys(pgt_page);

	rc = cove_convert_pages(pgt_phys_addr, tvmc->pgtable.npages, false);
	if (rc) {
		kvm_err("%s: page table pool conversion failed rc %d\n", __func__, rc);
		goto pgt_convert_failed;
	}

	/* Allocate and convert the pages required for TVM state management */
	tvms_page = alloc_pages(GFP_KERNEL | __GFP_ZERO,
				get_order_num_pages(tinfo.tvm_pages_needed));
	if (!tvms_page) {
		rc = -ENOMEM;
		goto tvms_alloc_failed;
	}

	tvmc->tvm_state.page = tvms_page;
	tvmc->tvm_state.npages = tinfo.tvm_pages_needed;
	tvms_phys_addr = page_to_phys(tvms_page);

	rc = cove_convert_pages(tvms_phys_addr, tinfo.tvm_pages_needed, false);
	if (rc) {
		kvm_err("%s: tvm state page conversion failed rc %d\n", __func__, rc);
		goto tvms_convert_failed;
	}

	rc = kvm_riscv_cove_fence();
	if (rc)
		goto tvm_init_failed;

	INIT_LIST_HEAD(&tvmc->measured_pages);
	INIT_LIST_HEAD(&tvmc->zero_pages);
	INIT_LIST_HEAD(&tvmc->shared_pages);
	INIT_LIST_HEAD(&tvmc->reclaim_pending_pages);

	/* The required pages have been converted to confidential memory. Create the TVM now */
	params.tvm_page_directory_addr = kvm->arch.pgd_phys;
	params.tvm_state_addr = tvms_phys_addr;

	rc = sbi_covh_tsm_create_tvm(&params, &tvm_gid);
	if (rc)
		goto tvm_init_failed;

	tvmc->tvm_guest_id = tvm_gid;
	spin_lock_init(&tvmc->tvm_fence_lock);
	kvm->arch.tvmc = tvmc;

	rc = sbi_covh_add_pgt_pages(tvm_gid, pgt_phys_addr, tvmc->pgtable.npages);
	if (rc)
		goto tvm_init_failed;

	tvmc->kvm = kvm;
	kvm_info("Guest VM creation successful with guest id %lx\n", tvm_gid);

	return 0;

tvm_init_failed:
	/* Reclaim tvm state pages */
	sbi_covh_tsm_reclaim_pages(tvms_phys_addr, tvmc->tvm_state.npages);

tvms_convert_failed:
	__free_pages(tvms_page, get_order_num_pages(tinfo.tvm_pages_needed));

tvms_alloc_failed:
	/* Reclaim pgtable pages */
	sbi_covh_tsm_reclaim_pages(pgt_phys_addr, tvmc->pgtable.npages);

pgt_convert_failed:
	__free_pages(pgt_page, get_order(KVM_COVE_PGTABLE_SIZE_MAX));
	/* Reclaim pgd pages */
	sbi_covh_tsm_reclaim_pages(kvm->arch.pgd_phys, gstage_pgd_size >> PAGE_SHIFT);

done:
	kfree(tvmc);
	return rc;
}

int kvm_riscv_cove_init(void)
{
	int rc;

	/* We currently support host in VS mode. Thus, NACL is mandatory */
	if (sbi_probe_extension(SBI_EXT_COVH) <= 0 || !kvm_riscv_nacl_available())
		return -EOPNOTSUPP;

	rc = sbi_covh_tsm_get_info(&tinfo);
	if (rc < 0)
		return -EINVAL;

	if (tinfo.tstate != TSM_READY) {
		kvm_err("TSM is not ready yet. Can't run TVMs\n");
		return -EAGAIN;
	}

	riscv_cove_enabled = true;
	kvm_info("The platform has confidential computing feature enabled\n");
	kvm_info("TSM version %d is loaded and ready to run\n", tinfo.version);

	return 0;
}
