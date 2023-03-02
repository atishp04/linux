// SPDX-License-Identifier: GPL-2.0
/*
 * COVG SBI extensions related helper functions.
 *
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#include <linux/errno.h>
#include <asm/sbi.h>

int sbi_covg_add_mmio_region(unsigned long addr, unsigned long len)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_ADD_MMIO_REGION, addr, len,
			0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_covg_remove_mmio_region(unsigned long addr, unsigned long len)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_REMOVE_MMIO_REGION, addr,
			len, 0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_covg_share_memory(unsigned long addr, unsigned long len)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_SHARE_MEMORY, addr, len, 0,
			0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_covg_unshare_memory(unsigned long addr, unsigned long len)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_UNSHARE_MEMORY, addr, len, 0,
			0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_covg_allow_external_interrupt(unsigned long id)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_ALLOW_EXT_INTERRUPT, id, 0,
			0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_covg_allow_all_external_interrupt(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_ALLOW_EXT_INTERRUPT, -1, 0,
			0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_covg_deny_external_interrupt(unsigned long id)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_DENY_EXT_INTERRUPT, id, 0, 0,
			0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}

int sbi_covg_deny_all_external_interrupt(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_DENY_EXT_INTERRUPT, -1, 0, 0,
			0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return 0;
}
