/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_UNACCEPTED_MEMORY_H
#define _LINUX_UNACCEPTED_MEMORY_H

#include <linux/types.h>

struct unaccepted_memory {
	u32 version;
	u32 unit_size;
	u64 phys_base;
	u64 size;
	unsigned long bitmap[];
};

/*
 * Physical address of the unaccepted memory table, or PHYS_ADDR_MAX if none.
 * Set by EFI config table parsing or setup_data discovery.
 */
extern phys_addr_t unaccepted_table_phys;

static inline struct unaccepted_memory *get_unaccepted_table(void)
{
	if (unaccepted_table_phys == PHYS_ADDR_MAX)
		return NULL;
	return __va(unaccepted_table_phys);
}

#endif /* _LINUX_UNACCEPTED_MEMORY_H */
