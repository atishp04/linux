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

bool cc_platform_has(enum cc_attr attr)
{
	switch (attr) {
	case CC_ATTR_GUEST_MEM_ENCRYPT:
	case CC_ATTR_MEM_ENCRYPT:
		return true;
	default:
		return false;
	}
}
EXPORT_SYMBOL_GPL(cc_platform_has);
