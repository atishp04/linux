// SPDX-License-Identifier: GPL-2.0
/*
 * TEEG SBI extensions related helper functions.
 *
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#include <linux/errno.h>
#include <asm/sbi.h>

int sbi_teeg_add_mmio_region(unsigned long addr, unsigned long len)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_TEEG, SBI_EXT_TEEG_ADD_MMIO_REGION, addr, len,
			0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_teeg_remove_mmio_region(unsigned long addr, unsigned long len)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_TEEG, SBI_EXT_TEEG_REMOVE_MMIO_REGION, addr,
			len, 0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_teeg_share_memory(unsigned long addr, unsigned long len)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_TEEG, SBI_EXT_TEEG_SHARE_MEMORY, addr, len, 0,
			0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_teeg_unshare_memory(unsigned long addr, unsigned long len)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_TEEG, SBI_EXT_TEEG_UNSHARE_MEMORY, addr, len, 0,
			0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_teeg_allow_external_interrupt(unsigned long id)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_TEEG, SBI_EXT_TEEG_ALLOW_EXT_INTERRUPT, id, 0,
			0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_teeg_allow_all_external_interrupt(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_TEEG, SBI_EXT_TEEG_ALLOW_EXT_INTERRUPT, -1, 0,
			0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_teeg_deny_external_interrupt(unsigned long id)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_TEEG, SBI_EXT_TEEG_DENY_EXT_INTERRUPT, id, 0, 0,
			0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_teeg_deny_all_external_interrupt(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_TEEG, SBI_EXT_TEEG_DENY_EXT_INTERRUPT, -1, 0, 0,
			0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}
