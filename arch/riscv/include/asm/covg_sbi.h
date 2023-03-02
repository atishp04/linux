/* SPDX-License-Identifier: GPL-2.0 */
/*
 * COVG SBI extension related header file.
 *
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#ifndef __RISCV_COVG_SBI_H__
#define __RISCV_COVG_SBI_H__

#ifdef CONFIG_RISCV_COVE_GUEST

int sbi_covg_add_mmio_region(unsigned long addr, unsigned long len);
int sbi_covg_remove_mmio_region(unsigned long addr, unsigned long len);
int sbi_covg_share_memory(unsigned long addr, unsigned long len);
int sbi_covg_unshare_memory(unsigned long addr, unsigned long len);
int sbi_covg_allow_external_interrupt(unsigned long id);
int sbi_covg_allow_all_external_interrupt(void);
int sbi_covg_deny_external_interrupt(unsigned long id);
int sbi_covg_deny_all_external_interrupt(void);

#else

static inline int sbi_covg_add_mmio_region(unsigned long addr, unsigned long len) { return 0; }
static inline int sbi_covg_remove_mmio_region(unsigned long addr, unsigned long len) { return 0; }
static inline int sbi_covg_share_memory(unsigned long addr, unsigned long len) { return 0; }
static inline int sbi_covg_unshare_memory(unsigned long addr, unsigned long len) { return 0; }
static inline int sbi_covg_allow_external_interrupt(unsigned long id) { return 0; }
static inline int sbi_covg_allow_all_external_interrupt(void) { return 0; }
static inline int sbi_covg_deny_external_interrupt(unsigned long id) { return 0; }
static inline int sbi_covg_deny_all_external_interrupt(void) { return 0; }

#endif

#endif /* __RISCV_COVG_SBI_H__ */
