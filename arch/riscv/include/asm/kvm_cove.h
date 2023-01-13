/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * COVE related header file.
 *
 * Copyright (c) 2023 RivosInc
 *
 * Authors:
 *     Atish Patra <atishp@rivosinc.com>
 */

#ifndef __KVM_RISCV_COVE_H
#define __KVM_RISCV_COVE_H

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <linux/kvm.h>
#include <linux/list.h>
#include <asm/csr.h>
#include <asm/sbi.h>

#define KVM_COVE_PAGE_SIZE_4K	(1UL << 12)
#define KVM_COVE_PAGE_SIZE_2MB	(1UL << 21)
#define KVM_COVE_PAGE_SIZE_1GB	(1UL << 30)
#define KVM_COVE_PAGE_SIZE_512GB (1UL << 39)

#define bytes_to_pages(n) ((n + PAGE_SIZE - 1) >> PAGE_SHIFT)

/* Allocate 2MB(i.e. 512 pages) for the page table pool */
#define KVM_COVE_PGTABLE_SIZE_MAX ((1UL << 10) * PAGE_SIZE)

#define get_order_num_pages(n) (get_order(n << PAGE_SHIFT))

#define get_gpr_index(goffset) \
	((goffset - KVM_ARCH_GUEST_ZERO) / (__riscv_xlen / 8))

/* Describe a confidential or shared memory region */
struct kvm_riscv_cove_mem_region {
	unsigned long hva;
	unsigned long gpa;
	unsigned long npages;
};

/* Page management structure for the host */
struct kvm_riscv_cove_page {
	struct list_head link;

	/* Pointer to page allocated */
	struct page *page;

	/* number of pages allocated for page */
	unsigned long npages;

	/* Described the page type */
	unsigned long ptype;

	/* set if the page is mapped in guest physical address */
	bool is_mapped;

	/* The below two fileds are only valid if is_mapped is true */
	/* host virtual address for the mapping */
	unsigned long hva;
	/* guest physical address for the mapping */
	unsigned long gpa;
};

struct imsic_cove_state {
	bool bind_required;
	bool bound;
	int vsfile_hgei;
};

struct kvm_cove_tvm_vcpu_context {
	struct kvm_vcpu *vcpu;
	/* Pages storing each vcpu state of the TVM in TSM */
	struct kvm_riscv_cove_page vcpu_state;

	/* Per VCPU imsic state */
	struct imsic_cove_state imsic;
};

struct kvm_cove_tvm_context {
	struct kvm *kvm;

	/* TODO: This is not really a VMID as TSM returns the page owner ID instead of VMID */
	unsigned long tvm_guest_id;

	/* Pages where TVM page table is stored */
	struct kvm_riscv_cove_page pgtable;

	/* Pages storing the TVM state in TSM */
	struct kvm_riscv_cove_page tvm_state;

	/* Keep track of zero pages */
	struct list_head zero_pages;

	/* Pages where TVM image is measured & loaded */
	struct list_head measured_pages;

	/* keep track of shared pages */
	struct list_head shared_pages;

	/* keep track of pending reclaim confidential pages */
	struct list_head reclaim_pending_pages;

	struct kvm_riscv_cove_mem_region shared_region;
	struct kvm_riscv_cove_mem_region confidential_region;

	/* spinlock to protect the tvm fence sequence */
	spinlock_t tvm_fence_lock;

	/* Track TVM state */
	bool finalized_done;
};

static inline bool is_cove_vm(struct kvm *kvm)
{
	return kvm->arch.vm_type == KVM_VM_TYPE_RISCV_COVE;
}

static inline bool is_cove_vcpu(struct kvm_vcpu *vcpu)
{
	return is_cove_vm(vcpu->kvm);
}

#ifdef CONFIG_RISCV_COVE_HOST

bool kvm_riscv_cove_enabled(void);
int kvm_riscv_cove_init(void);

/* TVM related functions */
void kvm_riscv_cove_vm_destroy(struct kvm *kvm);
int kvm_riscv_cove_vm_init(struct kvm *kvm);

/* TVM VCPU related functions */
void kvm_riscv_cove_vcpu_destroy(struct kvm_vcpu *vcpu);
int kvm_riscv_cove_vcpu_init(struct kvm_vcpu *vcpu);
void kvm_riscv_cove_vcpu_load(struct kvm_vcpu *vcpu);
void kvm_riscv_cove_vcpu_put(struct kvm_vcpu *vcpu);
void kvm_riscv_cove_vcpu_switchto(struct kvm_vcpu *vcpu, struct kvm_cpu_trap *trap);
int kvm_riscv_cove_vcpu_sbi_ecall(struct kvm_vcpu *vcpu, struct kvm_run *run);

int kvm_riscv_cove_vm_measure_pages(struct kvm *kvm, struct kvm_riscv_cove_measure_region *mr);
int kvm_riscv_cove_vm_add_memreg(struct kvm *kvm, unsigned long gpa, unsigned long size);
int kvm_riscv_cove_handle_pagefault(struct kvm_vcpu *vcpu, gpa_t gpa,
				    unsigned long hva);
/* Fence related function */
int kvm_riscv_cove_tvm_fence(struct kvm_vcpu *vcpu);

/* AIA related CoVE functions */
int kvm_riscv_cove_aia_init(struct kvm *kvm);
int kvm_riscv_cove_vcpu_inject_interrupt(struct kvm_vcpu *vcpu, unsigned long iid);
int kvm_riscv_cove_vcpu_imsic_unbind(struct kvm_vcpu *vcpu, int old_cpu);
int kvm_riscv_cove_vcpu_imsic_bind(struct kvm_vcpu *vcpu, unsigned long imsic_mask);
int kvm_riscv_cove_vcpu_imsic_rebind(struct kvm_vcpu *vcpu, int old_pcpu);
int kvm_riscv_cove_aia_claim_imsic(struct kvm_vcpu *vcpu, phys_addr_t imsic_pa);
int kvm_riscv_cove_aia_convert_imsic(struct kvm_vcpu *vcpu, phys_addr_t imsic_pa);
int kvm_riscv_cove_vcpu_imsic_addr(struct kvm_vcpu *vcpu);
#else
static inline bool kvm_riscv_cove_enabled(void) {return false; };
static inline int kvm_riscv_cove_init(void) { return -1; }
static inline void kvm_riscv_cove_hardware_disable(void) {}
static inline int kvm_riscv_cove_hardware_enable(void) {return 0; }

/* TVM related functions */
static inline void kvm_riscv_cove_vm_destroy(struct kvm *kvm) {}
static inline int kvm_riscv_cove_vm_init(struct kvm *kvm) {return -1; }

/* TVM VCPU related functions */
static inline void kvm_riscv_cove_vcpu_destroy(struct kvm_vcpu *vcpu) {}
static inline int kvm_riscv_cove_vcpu_init(struct kvm_vcpu *vcpu) {return -1; }
static inline void kvm_riscv_cove_vcpu_load(struct kvm_vcpu *vcpu) {}
static inline void kvm_riscv_cove_vcpu_put(struct kvm_vcpu *vcpu) {}
static inline void kvm_riscv_cove_vcpu_switchto(struct kvm_vcpu *vcpu, struct kvm_cpu_trap *trap) {}
static inline int kvm_riscv_cove_vcpu_sbi_ecall(struct kvm_vcpu *vcpu, struct kvm_run *run) {return -1; }
static inline int kvm_riscv_cove_vm_add_memreg(struct kvm *kvm, unsigned long gpa,
					       unsigned long size) {return -1; }
static inline int kvm_riscv_cove_vm_measure_pages(struct kvm *kvm,
						  struct kvm_riscv_cove_measure_region *mr)
{
	return -1;
}
static inline int kvm_riscv_cove_handle_pagefault(struct kvm_vcpu *vcpu,
						  gpa_t gpa, unsigned long hva) { return -1; }

/* TVM interrupt managenet via AIA functions */
static inline int kvm_riscv_cove_aia_init(struct kvm *kvm) { return -1; }
static inline int kvm_riscv_cove_vcpu_inject_interrupt(struct kvm_vcpu *vcpu,
						       unsigned long iid) { return -1; }
static inline int kvm_riscv_cove_vcpu_imsic_unbind(struct kvm_vcpu *vcpu,
						   int old_cpu) { return -1; }
static inline int kvm_riscv_cove_vcpu_imsic_bind(struct kvm_vcpu *vcpu,
						 unsigned long imsic_mask) { return -1; }
static inline int kvm_riscv_cove_aia_claim_imsic(struct kvm_vcpu *vcpu,
						 phys_addr_t imsic_pa) { return -1; }
static inline int kvm_riscv_cove_aia_convert_imsic(struct kvm_vcpu *vcpu,
						 phys_addr_t imsic_pa) { return -1; }
static inline int kvm_riscv_cove_vcpu_imsic_addr(struct kvm_vcpu *vcpu) { return -1; }
static inline int kvm_riscv_cove_vcpu_imsic_rebind(struct kvm_vcpu *vcpu,
						   int old_pcpu) { return -1; }
#endif /* CONFIG_RISCV_COVE_HOST */

#endif /* __KVM_RISCV_COVE_H */
