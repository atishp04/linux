/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * KVM MMU related header file.
 *
 * Copyright (c) 2022 RivosInc
 *
 * Authors:
 *     Atish Patra <atishp@rivosinc.com>
 */


#ifndef __RISCV_KVM_MMU_H__
#define __RISCV_KVM_MMU_H__

#include <asm/csr.h>

#ifdef CONFIG_64BIT
#define gstage_index_bits	9
static unsigned long gstage_mode = (HGATP_MODE_SV39X4 << HGATP_MODE_SHIFT);
static unsigned long gstage_pgd_levels = 3;
#else
#define gstage_index_bits	10
static unsigned long gstage_mode = (HGATP_MODE_SV32X4 << HGATP_MODE_SHIFT);
static unsigned long gstage_pgd_levels = 2;
#endif

#define gstage_pgd_xbits	2
#define gstage_pgd_size	(1UL << (HGATP_PAGE_SHIFT + gstage_pgd_xbits))
#define gstage_gpa_bits	(HGATP_PAGE_SHIFT + \
			 (gstage_pgd_levels * gstage_index_bits) + \
			 gstage_pgd_xbits)
#define gstage_gpa_size	((gpa_t)(1ULL << gstage_gpa_bits))

#endif
