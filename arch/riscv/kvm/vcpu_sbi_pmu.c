// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Rivos Inc
 *
 * Authors:
 *     Atish Patra <atishp@rivosinc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/kvm_vcpu_sbi.h>

static int kvm_sbi_ext_pmu_handler(struct kvm_vcpu *vcpu, struct kvm_run *run,
				   unsigned long *out_val,
				   struct kvm_cpu_trap *utrap,
				   bool *exit)
{
	int ret = -EOPNOTSUPP;
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;
	unsigned long funcid = cp->a6;
	uint64_t temp;

	switch (funcid) {
	case SBI_EXT_PMU_NUM_COUNTERS:
		ret = kvm_riscv_vcpu_pmu_num_ctrs(vcpu, out_val);
		break;
	case SBI_EXT_PMU_COUNTER_GET_INFO:
		ret = kvm_riscv_vcpu_pmu_ctr_info(vcpu, cp->a0, out_val);
		break;
	case SBI_EXT_PMU_COUNTER_CFG_MATCH:
#if defined(CONFIG_32BIT)
		temp = ((uint64_t)cp->a5 << 32) | cp->a4;
#else
		temp = cp->a4;
#endif
		ret = kvm_riscv_vcpu_pmu_ctr_cfg_match(vcpu, cp->a0, cp->a1, cp->a2, cp->a3, temp);
		if (ret >= 0) {
			*out_val = ret;
			ret = 0;
		}
		break;
	case SBI_EXT_PMU_COUNTER_START:
#if defined(CONFIG_32BIT)
		temp = ((uint64_t)cp->a4 << 32) | cp->a3;
#else
		temp = cp->a3;
#endif
		ret = kvm_riscv_vcpu_pmu_ctr_start(vcpu, cp->a0, cp->a1, cp->a2, temp);
		break;
	case SBI_EXT_PMU_COUNTER_STOP:
		ret = kvm_riscv_vcpu_pmu_ctr_stop(vcpu, cp->a0, cp->a1, cp->a2);
		break;
	case SBI_EXT_PMU_COUNTER_FW_READ:
		ret = kvm_riscv_vcpu_pmu_ctr_read(vcpu, cp->a0, out_val);
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	return ret;
}

unsigned long kvm_sbi_ext_pmu_probe(unsigned long extid)
{
	/*
	 * PMU Extension is only available to guests if privilege mode filtering
	 * is available. Otherwise, guest will always count events while the
	 * execution is in hypervisor mode.
	 */
	return riscv_isa_extension_available(NULL, SSCOFPMF);
}

const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_pmu = {
	.extid_start = SBI_EXT_PMU,
	.extid_end = SBI_EXT_PMU,
	.handler = kvm_sbi_ext_pmu_handler,
	.probe = kvm_sbi_ext_pmu_probe,
};
