/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TVM helper functions
 *
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#ifndef __RISCV_COVE_H__
#define __RISCV_COVE_H__

#ifdef CONFIG_RISCV_COVE_GUEST
void riscv_cove_sbi_init(void);
bool is_cove_guest(void);
#else /* CONFIG_RISCV_COVE_GUEST */
static inline bool is_cove_guest(void)
{
	return false;
}
static inline void riscv_cove_sbi_init(void)
{
}
#endif /* CONFIG_RISCV_COVE_GUEST */

#endif /* __RISCV_COVE_H__ */
