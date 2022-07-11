/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022 Rivos Inc
 *
 * Authors:
 *     Atish Patra <atishp@rivosinc.com>
 */

#ifndef _KVM_VCPU_RISCV_PMU_H
#define _KVM_VCPU_RISCV_PMU_H

#include <linux/perf/riscv_pmu.h>
#include <asm/sbi.h>

#ifdef CONFIG_RISCV_PMU_SBI
#define RISCV_KVM_MAX_FW_CTRS 32

/* Per virtual pmu counter data */
struct kvm_pmc {
	u8 idx;
	struct kvm_vcpu *vcpu;
	struct perf_event *perf_event;
	uint64_t counter_val;
	union sbi_pmu_ctr_info cinfo;
};

/* PMU data structure per vcpu */
struct kvm_pmu {
	struct kvm_pmc pmc[RISCV_MAX_COUNTERS];
	/* Number of the virtual firmware counters available */
	int num_fw_ctrs;
	/* Number of the virtual hardware counters available */
	int num_hw_ctrs;
	/* Bit map of all the virtual counter used */
	DECLARE_BITMAP(used_pmc, RISCV_MAX_COUNTERS);
};

#define vcpu_to_pmu(vcpu) (&(vcpu)->arch.pmu)
#define pmu_to_vcpu(pmu)  (container_of((pmu), struct kvm_vcpu, arch.pmu))
#define pmc_to_pmu(pmc)   (&(pmc)->vcpu->arch.pmu)

int kvm_riscv_vcpu_pmu_num_ctrs(struct kvm_vcpu *vcpu, unsigned long *out_val);
int kvm_riscv_vcpu_pmu_ctr_info(struct kvm_vcpu *vcpu, unsigned long cidx,
				unsigned long *ctr_info);

int kvm_riscv_vcpu_pmu_ctr_start(struct kvm_vcpu *vcpu, unsigned long ctr_base,
				 unsigned long ctr_mask, unsigned long flag, uint64_t ival);
int kvm_riscv_vcpu_pmu_ctr_stop(struct kvm_vcpu *vcpu, unsigned long ctr_base,
				 unsigned long ctr_mask, unsigned long flag);
int kvm_riscv_vcpu_pmu_ctr_cfg_match(struct kvm_vcpu *vcpu, unsigned long ctr_base,
				     unsigned long ctr_mask, unsigned long flag,
				     unsigned long eidx, uint64_t edata);
int kvm_riscv_vcpu_pmu_ctr_read(struct kvm_vcpu *vcpu, unsigned long cidx,
				unsigned long *out_val);
int kvm_riscv_vcpu_pmu_init(struct kvm_vcpu *vcpu);
void kvm_riscv_vcpu_pmu_deinit(struct kvm_vcpu *vcpu);
void kvm_riscv_vcpu_pmu_reset(struct kvm_vcpu *vcpu);

#else
struct kvm_pmu {
};

static inline int kvm_riscv_vcpu_pmu_init(struct kvm_vcpu *vcpu)
{
	return 0;
}
static inline void kvm_riscv_vcpu_pmu_deinit(struct kvm_vcpu *vcpu) {}
static inline void kvm_riscv_vcpu_pmu_reset(struct kvm_vcpu *vcpu) {}
#endif
#endif
