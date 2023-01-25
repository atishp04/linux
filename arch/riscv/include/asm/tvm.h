// SPDX-License-Identifier: GPL-2.0
/*
 * TVM helper functions
 *
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#ifndef __RISCV_TVM_H__
#define __RISCV_TVM_H__

#ifdef CONFIG_RISCV_TEE_VM

#include <asm/sbi.h>

static inline bool is_secure_guest(void)
{
	/* TODO: Cache this in a global variable. */
	return sbi_probe_extension(SBI_EXT_TEEG) > 0;
}

#else /* CONFIG_RISCV_TEE_VM */

static inline bool is_secure_guest(void)
{
	return false;
}

#endif /* CONFIG_RISCV_TEE_VM */

#endif /* __RISCV_TVM_H__ */
