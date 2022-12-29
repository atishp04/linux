/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * COVE SBI extension related header file.
 *
 * Copyright (c) 2023 RivosInc
 *
 * Authors:
 *     Atish Patra <atishp@rivosinc.com>
 */

#ifndef __KVM_COVE_SBI_H
#define __KVM_COVE_SBI_H

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <asm/csr.h>
#include <asm/sbi.h>

int sbi_covh_tsm_get_info(struct sbi_cove_tsm_info *tinfo_addr);
int sbi_covh_tvm_initiate_fence(unsigned long tvmid);
int sbi_covh_tsm_initiate_fence(void);
int sbi_covh_tsm_local_fence(void);
int sbi_covh_tsm_create_tvm(struct sbi_cove_tvm_create_params *tparam, unsigned long *tvmid);
int sbi_covh_tsm_finalize_tvm(unsigned long tvmid, unsigned long sepc, unsigned long entry_arg);
int sbi_covh_tsm_destroy_tvm(unsigned long tvmid);
int sbi_covh_add_memory_region(unsigned long tvmid, enum sbi_cove_mem_type,
			       unsigned long tgpadr, unsigned long rlen);

int sbi_covh_tsm_reclaim_pages(unsigned long phys_addr, unsigned long npages);
int sbi_covh_tsm_convert_pages(unsigned long phys_addr, unsigned long npages);
int sbi_covh_tsm_reclaim_page(unsigned long page_addr_phys);
int sbi_covh_add_pgt_pages(unsigned long tvmid, unsigned long page_addr_phys, unsigned long npages);

int sbi_covh_add_measured_pages(unsigned long tvmid, unsigned long src_addr,
				unsigned long dest_addr, enum sbi_cove_page_type ptype,
				unsigned long npages, unsigned long tgpa);
int sbi_covh_add_zero_pages(unsigned long tvmid, unsigned long page_addr_phys,
			    enum sbi_cove_page_type ptype, unsigned long npages,
			    unsigned long tvm_base_page_addr);

int sbi_covh_create_tvm_vcpu(unsigned long tvmid, unsigned long tvm_vcpuid,
			     unsigned long vpus_page_addr);

int sbi_covh_run_tvm_vcpu(unsigned long tvmid, unsigned long tvm_vcpuid);

#endif
