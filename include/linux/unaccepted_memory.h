/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_UNACCEPTED_MEMORY_H
#define _LINUX_UNACCEPTED_MEMORY_H

#include <linux/efi.h>

/*
 * Physical address of the unaccepted memory table, or PHYS_ADDR_MAX if none.
 * Set by EFI config table parsing or setup_data discovery.
 */
extern phys_addr_t unaccepted_table_phys;

static inline struct efi_unaccepted_memory *get_unaccepted_table(void)
{
	if (unaccepted_table_phys == PHYS_ADDR_MAX)
		return NULL;
	return __va(unaccepted_table_phys);
}

#endif /* _LINUX_UNACCEPTED_MEMORY_H */
