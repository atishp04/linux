// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#include <linux/export.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#include <asm/covg_sbi.h>
#include <asm/cove.h>
#include <asm-generic/io.h>

void ioremap_phys_range_hook(phys_addr_t addr, size_t size, pgprot_t prot)
{
	unsigned long offset;

	if (!is_cove_guest())
		return;

	/* Page-align address and size. */
	offset = addr & (~PAGE_MASK);
	addr -= offset;
	size = PAGE_ALIGN(size + offset);

	sbi_covg_add_mmio_region(addr, size);
}

void iounmap_phys_range_hook(phys_addr_t addr, size_t size)
{
	unsigned long offset;

	if (!is_cove_guest())
		return;

	/* Page-align address and size. */
	offset = addr & (~PAGE_MASK);
	addr -= offset;
	size = PAGE_ALIGN(size + offset);

	sbi_covg_remove_mmio_region(addr, size);
}
