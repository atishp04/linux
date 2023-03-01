// SPDX-License-Identifier: GPL-2.0
/*
 * Confidential Computing Platform Capability checks
 *
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#include <linux/export.h>
#include <linux/cc_platform.h>
#include <asm/sbi.h>
#include <asm/cove.h>

static bool is_tvm;

bool is_cove_guest(void)
{
	return is_tvm;
}
EXPORT_SYMBOL_GPL(is_cove_guest);

void riscv_cove_sbi_init(void)
{
	if (sbi_probe_extension(SBI_EXT_COVG) > 0)
		is_tvm = true;
}
