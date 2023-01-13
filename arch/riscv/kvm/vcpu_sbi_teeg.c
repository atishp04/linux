// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/kvm_vcpu_sbi.h>
#include <asm/kvm_tee.h>

static int kvm_sbi_ext_teeg_handler(struct kvm_vcpu *vcpu, struct kvm_run *run,
				    unsigned long *out_val,
				    struct kvm_cpu_trap *utrap, bool *exit)
{
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;
	uint32_t num_pages = cp->a1 / PAGE_SIZE;
	unsigned long funcid = cp->a6;
	uint32_t i;

	switch (funcid) {
	case SBI_EXT_TEEG_SHARE_MEMORY:
		for (i = 0; i < num_pages; i++) {
			int ret = kvm_riscv_tee_share_page(
				vcpu, cp->a0 + i * PAGE_SIZE);
			if (ret)
				return ret;
		}
		return 0;

	case SBI_EXT_TEEG_UNSHARE_MEMORY:
		for (i = 0; i < num_pages; i++) {
			int ret = kvm_riscv_tee_unshare_page(
				vcpu, cp->a0 + i * PAGE_SIZE);
			if (ret)
				return ret;
		}
		return 0;

	case SBI_EXT_TEEG_ADD_MMIO_REGION:
	case SBI_EXT_TEEG_REMOVE_MMIO_REGION:
	case SBI_EXT_TEEG_ALLOW_EXT_INTERRUPT:
	case SBI_EXT_TEEG_DENY_EXT_INTERRUPT:
		/* We don't really need to do anything here for now. */
		return 0;

	default:
		kvm_err("%s: Unsupported guest SBI %ld.\n", __func__, funcid);
		return -EOPNOTSUPP;
	}
}

const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_teeg = {
	.extid_start = SBI_EXT_TEEG,
	.extid_end = SBI_EXT_TEEG,
	.handler = kvm_sbi_ext_teeg_handler,
};
