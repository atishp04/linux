// SPDX-License-Identifier: GPL-2.0-only

#include "error.h"
#include "misc.h"
#include "tdx.h"
#include "sev.h"
#include <asm/shared/tdx.h>

/* Unaccepted memory bitmap — set by EFI stub or setup_data discovery */
struct unaccepted_memory *unaccepted_table;

/*
 * accept_memory() and process_unaccepted_memory() called from EFI stub which
 * runs before decompressor and its early_tdx_detect().
 *
 * Enumerate TDX directly from the early users.
 */
static bool early_is_tdx_guest(void)
{
	static bool once;
	static bool is_tdx;

	if (!IS_ENABLED(CONFIG_INTEL_TDX_GUEST))
		return false;

	if (!once) {
		u32 eax, sig[3];

		cpuid_count(TDX_CPUID_LEAF_ID, 0, &eax,
			    &sig[0], &sig[2],  &sig[1]);
		is_tdx = !memcmp(TDX_IDENT, sig, sizeof(sig));
		once = true;
	}

	return is_tdx;
}

void arch_accept_memory(phys_addr_t start, phys_addr_t end)
{
	/* Platform-specific memory-acceptance call goes here */
	if (early_is_tdx_guest()) {
		if (!tdx_accept_memory(start, end))
			panic("TDX: Failed to accept memory\n");
	} else if (early_is_sevsnp_guest()) {
		snp_accept_memory(start, end);
	} else {
		error("Cannot accept memory: unknown platform\n");
	}
}

/*
 * accept_memory() -- Accept memory during early boot (decompressor).
 *
 * Single-threaded context, no locking needed. Walks the unaccepted bitmap
 * and calls arch_accept_memory() for each set range.
 */
void accept_memory(phys_addr_t start, unsigned long size)
{
	unsigned long range_start, range_end;
	phys_addr_t end = start + size;
	unsigned long bitmap_size;
	u64 unit_size;

	if (!unaccepted_table)
		return;

	unit_size = unaccepted_table->unit_size;

	/*
	 * Only care for the part of the range that is represented
	 * in the bitmap.
	 */
	if (start < unaccepted_table->phys_base)
		start = unaccepted_table->phys_base;
	if (end < unaccepted_table->phys_base)
		return;

	/* Translate to offsets from the beginning of the bitmap */
	start -= unaccepted_table->phys_base;
	end -= unaccepted_table->phys_base;

	/* Make sure not to overrun the bitmap */
	if (end > unaccepted_table->size * unit_size * BITS_PER_BYTE)
		end = unaccepted_table->size * unit_size * BITS_PER_BYTE;

	range_start = start / unit_size;
	bitmap_size = DIV_ROUND_UP(end, unit_size);

	for_each_set_bitrange_from(range_start, range_end,
				   unaccepted_table->bitmap, bitmap_size) {
		unsigned long phys_start, phys_end;

		phys_start = range_start * unit_size + unaccepted_table->phys_base;
		phys_end = range_end * unit_size + unaccepted_table->phys_base;

		arch_accept_memory(phys_start, phys_end);
		bitmap_clear(unaccepted_table->bitmap,
			     range_start, range_end - range_start);
	}
}

#ifdef CONFIG_EFI_STUB
static bool init_unaccepted_memory_efi(void)
{
	guid_t guid = LINUX_EFI_UNACCEPTED_MEM_TABLE_GUID;
	struct unaccepted_memory *table;
	unsigned long cfg_table_pa;
	unsigned int cfg_table_len;
	enum efi_type et;
	int ret;

	et = efi_get_type(boot_params_ptr);
	if (et == EFI_TYPE_NONE)
		return false;

	ret = efi_get_conf_table(boot_params_ptr, &cfg_table_pa, &cfg_table_len);
	if (ret) {
		warn("EFI config table not found.");
		return false;
	}

	table = (void *)efi_find_vendor_table(boot_params_ptr, cfg_table_pa,
					      cfg_table_len, guid);
	if (!table)
		return false;

	if (table->version != 1)
		error("Unknown version of unaccepted memory table\n");

	/*
	 * In many cases unaccepted_table is already set by EFI stub, but it
	 * has to be initialized again to cover cases when the table is not
	 * allocated by EFI stub or EFI stub copied the kernel image with
	 * efi_relocate_kernel() before the variable is set.
	 *
	 * It must be initialized before the first usage of accept_memory().
	 */
	unaccepted_table = table;

	return true;
}
#else
static bool init_unaccepted_memory_efi(void) { return false; }
#endif

static bool init_unaccepted_memory_setup_data(void)
{
	struct unaccepted_memory *table;
	struct setup_data *sd;
	u64 pa_sd;

	/*
	 * The decompressor runs identity-mapped, so setup_data physical
	 * addresses are directly dereferenceable.
	 */
	pa_sd = boot_params_ptr->hdr.setup_data;
	while (pa_sd) {
		sd = (struct setup_data *)pa_sd;
		if (sd->type == SETUP_UNACCEPTED_MEM) {
			if (sd->len < sizeof(*table)) {
				warn("SETUP_UNACCEPTED_MEM payload too small");
				return false;
			}

			table = (struct unaccepted_memory *)sd->data;
			if (table->version != 1) {
				warn("Unknown version of unaccepted memory table");
				return false;
			}

			if (sizeof(*table) + table->size > sd->len) {
				warn("SETUP_UNACCEPTED_MEM bitmap exceeds payload");
				return false;
			}

			unaccepted_table = table;
			return true;
		}
		pa_sd = sd->next;
	}

	return false;
}

bool init_unaccepted_memory(void)
{
	if (init_unaccepted_memory_efi())
		return true;

	return init_unaccepted_memory_setup_data();
}
