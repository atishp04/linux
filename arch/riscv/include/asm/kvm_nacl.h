/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */

#ifndef __KVM_NACL_H
#define __KVM_NACL_H

#include <linux/jump_label.h>
#include <linux/percpu.h>
#include <asm/byteorder.h>
#include <asm/csr.h>
#include <asm/sbi.h>

struct kvm_vcpu_arch;

DECLARE_STATIC_KEY_FALSE(kvm_riscv_nacl_available);
#define kvm_riscv_nacl_available() \
	static_branch_unlikely(&kvm_riscv_nacl_available)

struct kvm_riscv_nacl {
	void *shmem;
	phys_addr_t shmem_phys;
};
DECLARE_PER_CPU(struct kvm_riscv_nacl, kvm_riscv_nacl);

void __kvm_riscv_nacl_hfence(void *shmem,
			     unsigned long control,
			     unsigned long page_addr,
			     unsigned long page_count);

void __kvm_riscv_nacl_switch_to(struct kvm_vcpu_arch *vcpu_arch,
				unsigned long sbi_ext_id,
				unsigned long sbi_func_id,
				unsigned long sync_sret_flags);

int kvm_riscv_nacl_enable(void);

void kvm_riscv_nacl_disable(void);

void kvm_riscv_nacl_exit(void);

int kvm_riscv_nacl_init(void);

#ifdef CONFIG_32BIT
#define lelong_to_cpu(__x)	le32_to_cpu(__x)
#define cpu_to_lelong(__x)	cpu_to_le32(__x)
#else
#define lelong_to_cpu(__x)	le64_to_cpu(__x)
#define cpu_to_lelong(__x)	cpu_to_le64(__x)
#endif

#define nacl_shmem()						\
	this_cpu_ptr(&kvm_riscv_nacl)->shmem
#define nacl_shmem_fast()					\
	(kvm_riscv_nacl_available() ? nacl_shmem() : NULL)

#define nacl_shmem_scratch_read_long(__s, __o)			\
({								\
	unsigned long *__p = (__s) +				\
			     SBI_NACL_SHMEM_SCRATCH_OFFSET +	\
			     (__o);				\
	lelong_to_cpu(*__p);					\
})

#define nacl_shmem_scratch_write_long(__s, __o, __v)		\
do {								\
	unsigned long *__p = (__s) +				\
			     SBI_NACL_SHMEM_SCRATCH_OFFSET +	\
			     (__o);				\
	*__p = cpu_to_lelong(__v);				\
} while (0)

#define nacl_shmem_scratch_write_longs(__s, __o, __a, __c)	\
do {								\
	unsigned int __i;					\
	unsigned long *__p = (__s) +				\
			     SBI_NACL_SHMEM_SCRATCH_OFFSET +	\
			     (__o);				\
	for (__i = 0; __i < (__c); __i++)			\
		__p[__i] = cpu_to_lelong((__a)[__i]);		\
} while (0)

#define nacl_shmem_sync_hfence(__e)				\
do {								\
	sbi_ecall(SBI_EXT_NACL, SBI_EXT_NACL_SYNC_HFENCE,	\
		  (__e), 0, 0, 0, 0, 0);			\
} while (0)

#define nacl_hfence_mkctrl(__t, __o, __v, __a)			\
({								\
	unsigned long __c = SBI_NACL_SHMEM_HFENCE_CTRL_PEND;	\
	__c |= ((__t) & SBI_NACL_SHMEM_HFENCE_CTRL_TYPE_MASK)	\
		<< SBI_NACL_SHMEM_HFENCE_CTRL_TYPE_SHIFT;	\
	__c |= ((__o) & SBI_NACL_SHMEM_HFENCE_CTRL_PSIZE_MASK)	\
		<< SBI_NACL_SHMEM_HFENCE_CTRL_PSIZE_SHIFT;	\
	__c |= ((__v) & SBI_NACL_SHMEM_HFENCE_CTRL_VMID_MASK)	\
		<< SBI_NACL_SHMEM_HFENCE_CTRL_VMID_SHIFT;	\
	__c |= ((__a) & SBI_NACL_SHMEM_HFENCE_CTRL_ASID_MASK);	\
	__c;							\
})

#define nacl_hfence_mkpaddr(__o, __addr)			\
	((__addr) >> (__o))

#define nacl_hfence_mkpcount(__o, __size)			\
	((__size) >> (__o))

#define nacl_shmem_hfence_gvma(__s, __gpa, __gpsz, __o)		\
__kvm_riscv_nacl_hfence(__s,					\
	nacl_hfence_mkctrl(SBI_NACL_SHMEM_HFENCE_TYPE_GVMA,	\
			   __o, 0, 0),				\
	nacl_hfence_mkpaddr(__o, __gpa),			\
	nacl_hfence_mkpcount(__o, __gpsz))

#define nacl_shmem_hfence_gvma_all(__s)				\
__kvm_riscv_nacl_hfence(__s,					\
	nacl_hfence_mkctrl(SBI_NACL_SHMEM_HFENCE_TYPE_GVMA_ALL,	\
			   0, 0, 0), 0, 0)

#define nacl_shmem_hfence_gvma_vmid(__s, __v, __gpa, __gpsz, __o)\
__kvm_riscv_nacl_hfence(__s,					\
	nacl_hfence_mkctrl(SBI_NACL_SHMEM_HFENCE_TYPE_GVMA_VMID,\
			   __o, __v, 0),			\
	nacl_hfence_mkpaddr(__o, __gpa),			\
	nacl_hfence_mkpcount(__o, __gpsz))

#define nacl_shmem_hfence_gvma_vmid_all(__s, __v)		\
__kvm_riscv_nacl_hfence(__s,					\
	nacl_hfence_mkctrl(SBI_NACL_SHMEM_HFENCE_TYPE_GVMA_VMID_ALL,\
			   0, __v, 0), 0, 0)

#define nacl_shmem_hfence_vvma(__s, __v, __gva, __gvsz, __o)	\
__kvm_riscv_nacl_hfence(__s,					\
	nacl_hfence_mkctrl(SBI_NACL_SHMEM_HFENCE_TYPE_VVMA,	\
			   __o, __v, 0),			\
	nacl_hfence_mkpaddr(__o, __gva),			\
	nacl_hfence_mkpcount(__o, __gvsz))

#define nacl_shmem_hfence_vvma_all(__s, __v)			\
__kvm_riscv_nacl_hfence(__s,					\
	nacl_hfence_mkctrl(SBI_NACL_SHMEM_HFENCE_TYPE_VVMA_ALL,	\
			   0, __v, 0), 0, 0)

#define nacl_shmem_hfence_vvma_asid(__s, __v, __a, __gva, __gvsz, __o)\
__kvm_riscv_nacl_hfence(__s,					\
	nacl_hfence_mkctrl(SBI_NACL_SHMEM_HFENCE_TYPE_VVMA_ASID,\
			   __o, __v, __a),			\
	nacl_hfence_mkpaddr(__o, __gva),			\
	nacl_hfence_mkpcount(__o, __gvsz))

#define nacl_shmem_hfence_vvma_asid_all(__s, __v, __a)		\
__kvm_riscv_nacl_hfence(__s,					\
	nacl_hfence_mkctrl(SBI_NACL_SHMEM_HFENCE_TYPE_VVMA_ASID_ALL,\
			   0, __v, __a), 0, 0)

#define nacl_shmem_csr_read(__s, __c)				\
({								\
	unsigned long *__a = (__s) + 0;	\
	lelong_to_cpu(__a[SBI_NACL_SHMEM_CSR_INDEX(__c)]);	\
})

#define nacl_shmem_csr_write(__s, __c, __v)			\
do {								\
	unsigned int __i = SBI_NACL_SHMEM_CSR_INDEX(__c);	\
	unsigned long *__a = (__s) + SBI_NACL_SHMEM_CSR_OFFSET;	\
	u8 *__b = (__s) + SBI_NACL_SHMEM_DBITMAP_OFFSET;	\
	__a[__i] = cpu_to_lelong(__v);				\
	__b[__i >> 3] |= 1U << (__i & 0x7);			\
} while (0)

#define nacl_shmem_csr_swap(__s, __c, __v)			\
({								\
	unsigned int __i = SBI_NACL_SHMEM_CSR_INDEX(__c);	\
	unsigned long *__a = (__s) + SBI_NACL_SHMEM_CSR_OFFSET;	\
	u8 *__b = (__s) + SBI_NACL_SHMEM_DBITMAP_OFFSET;	\
	unsigned long __r = lelong_to_cpu(__a[__i]);		\
	__a[__i] = cpu_to_lelong(__v);				\
	__b[__i >> 3] |= 1U << (__i & 0x7);			\
	__r;							\
})

#define nacl_shmem_sync_csr(__c)				\
do {								\
	sbi_ecall(SBI_EXT_NACL, SBI_EXT_NACL_SYNC_CSR,		\
		  (__c), 0, 0, 0, 0, 0);			\
} while (0)

#define nacl_csr_read(__c)					\
({								\
	unsigned long __r;					\
	if (kvm_riscv_nacl_available())				\
		__r = nacl_shmem_csr_read(nacl_shmem(), __c);	\
	else							\
		__r = csr_read(__c);				\
	__r;							\
})

#define nacl_csr_write(__c, __v)				\
do {								\
	if (kvm_riscv_nacl_available())				\
		nacl_shmem_csr_write(nacl_shmem(), __c, __v);	\
	else							\
		csr_write(__c, __v);				\
} while (0)

#define nacl_csr_swap(__c, __v)					\
({								\
	unsigned long __r;					\
	if (kvm_riscv_nacl_available())				\
		__r = nacl_shmem_csr_swap(nacl_shmem(), __c, __v);\
	else							\
		__r = csr_swap(__c, __v);			\
	__r;							\
})

#define nacl_sync_csr(__c)					\
do {								\
	if (kvm_riscv_nacl_available())				\
		nacl_shmem_sync_csr(__c);			\
} while (0)

#endif
