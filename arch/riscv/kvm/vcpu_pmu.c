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
#include <linux/perf/riscv_pmu.h>
#include <asm/csr.h>
#include <asm/bitops.h>
#include <asm/kvm_vcpu_pmu.h>
#include <linux/kvm_host.h>

#define get_event_type(x) ((x & SBI_PMU_EVENT_IDX_TYPE_MASK) >> 16)
#define get_event_code(x) (x & SBI_PMU_EVENT_IDX_CODE_MASK)

static inline u64 pmu_get_sample_period(struct kvm_pmc *pmc)
{
	u64 counter_val_mask = GENMASK(pmc->cinfo.width, 0);
	u64 sample_period;

	if (!pmc->counter_val)
		sample_period = counter_val_mask;
	else
		sample_period = pmc->counter_val & counter_val_mask;

	return sample_period;
}

static u32 pmu_get_perf_event_type(unsigned long eidx)
{
	enum sbi_pmu_event_type etype = get_event_type(eidx);
	u32 type;

	if (etype == SBI_PMU_EVENT_TYPE_HW)
		type = PERF_TYPE_HARDWARE;
	else if (etype == SBI_PMU_EVENT_TYPE_CACHE)
		type = PERF_TYPE_HW_CACHE;
	else if (etype == SBI_PMU_EVENT_TYPE_RAW || etype == SBI_PMU_EVENT_TYPE_FW)
		type = PERF_TYPE_RAW;
	else
		type = PERF_TYPE_MAX;

	return type;
}

static inline bool pmu_is_fw_event(unsigned long eidx)
{
	enum sbi_pmu_event_type etype = get_event_type(eidx);

	return (etype == SBI_PMU_EVENT_TYPE_FW) ? true : false;
}

static void pmu_release_perf_event(struct kvm_pmc *pmc)
{
	if (pmc->perf_event) {
		perf_event_disable(pmc->perf_event);
		perf_event_release_kernel(pmc->perf_event);
		pmc->perf_event = NULL;
	}
}

static u64 pmu_get_perf_event_hw_config(u32 sbi_event_code)
{
	/* SBI PMU HW event code is offset by 1 from perf hw event codes */
	return (u64)sbi_event_code - 1;
}

static u64 pmu_get_perf_event_cache_config(u32 sbi_event_code)
{
	u64 config = U64_MAX;
	unsigned int cache_type, cache_op, cache_result;

	/* All the cache event masks lie within 0xFF. No separate masking is necesssary */
	cache_type = (sbi_event_code & SBI_PMU_EVENT_CACHE_ID_CODE_MASK) >> 3;
	cache_op = (sbi_event_code & SBI_PMU_EVENT_CACHE_OP_ID_CODE_MASK) >> 1;
	cache_result = sbi_event_code & SBI_PMU_EVENT_CACHE_RESULT_ID_CODE_MASK;

	if (cache_type >= PERF_COUNT_HW_CACHE_MAX ||
	    cache_op >= PERF_COUNT_HW_CACHE_OP_MAX ||
	    cache_result >= PERF_COUNT_HW_CACHE_RESULT_MAX)
		goto out;
	config = cache_type | (cache_op << 8) | (cache_result << 16);
out:
	return config;
}

static u64 pmu_get_perf_event_config(unsigned long eidx, uint64_t edata)
{
	enum sbi_pmu_event_type etype = get_event_type(eidx);
	u32 ecode = get_event_code(eidx);
	u64 config = U64_MAX;

	if (etype == SBI_PMU_EVENT_TYPE_HW)
		config = pmu_get_perf_event_hw_config(ecode);
	else if (etype == SBI_PMU_EVENT_TYPE_CACHE)
		config = pmu_get_perf_event_cache_config(ecode);
	else if (etype == SBI_PMU_EVENT_TYPE_RAW)
		config = edata & RISCV_PMU_RAW_EVENT_MASK;
	else if ((etype == SBI_PMU_EVENT_TYPE_FW) && (ecode < SBI_PMU_FW_MAX))
		config = (1ULL << 63) | ecode;

	return config;
}

static int pmu_get_fixed_pmc_index(unsigned long eidx)
{
	u32 etype = pmu_get_perf_event_type(eidx);
	u32 ecode = get_event_code(eidx);
	int ctr_idx;

	if (etype != SBI_PMU_EVENT_TYPE_HW)
		return -EINVAL;

	if (ecode == SBI_PMU_HW_CPU_CYCLES)
		ctr_idx = 0;
	else if (ecode == SBI_PMU_HW_INSTRUCTIONS)
		ctr_idx = 2;
	else
		return -EINVAL;

	return ctr_idx;
}

static int pmu_get_programmable_pmc_index(struct kvm_pmu *kvpmu, unsigned long eidx,
					  unsigned long cbase, unsigned long cmask)
{
	int ctr_idx = -1;
	int i, pmc_idx;
	int min, max;

	if (pmu_is_fw_event(eidx)) {
		/* Firmware counters are mapped 1:1 starting from num_hw_ctrs for simplicity */
		min = kvpmu->num_hw_ctrs;
		max = min + kvpmu->num_fw_ctrs;
	} else {
		/* First 3 counters are reserved for fixed counters */
		min = 3;
		max = kvpmu->num_hw_ctrs;
	}

	for_each_set_bit(i, &cmask, BITS_PER_LONG) {
		pmc_idx = i + cbase;
		if ((pmc_idx >= min && pmc_idx < max) &&
		    !test_bit(pmc_idx, kvpmu->used_pmc)) {
			ctr_idx = pmc_idx;
			break;
		}
	}

	return ctr_idx;
}

static int pmu_get_pmc_index(struct kvm_pmu *pmu, unsigned long eidx,
			     unsigned long cbase, unsigned long cmask)
{
	int ret;

	/* Fixed counters need to be have fixed mapping as they have different width */
	ret = pmu_get_fixed_pmc_index(eidx);
	if (ret >= 0)
		return ret;

	return pmu_get_programmable_pmc_index(pmu, eidx, cbase, cmask);
}

int kvm_riscv_vcpu_pmu_incr_fw(struct kvm_vcpu *vcpu, unsigned long fid)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);
	struct kvm_fw_event *fevent;

	if (!kvpmu || fid >= SBI_PMU_FW_MAX)
		return -EINVAL;

	fevent = &kvpmu->fw_event[fid];
	if (fevent->started)
		fevent->value++;

	return 0;
}

int kvm_riscv_vcpu_pmu_ctr_read(struct kvm_vcpu *vcpu, unsigned long cidx,
				unsigned long *out_val)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc;
	u64 enabled, running;
	int fevent_code;

	if (!kvpmu)
		return -EINVAL;

	pmc = &kvpmu->pmc[cidx];

	if (pmc->cinfo.type == SBI_PMU_CTR_TYPE_FW) {
		fevent_code = get_event_code(pmc->event_idx);
		pmc->counter_val = kvpmu->fw_event[fevent_code].value;
	} else if (pmc->perf_event)
		pmc->counter_val += perf_event_read_value(pmc->perf_event, &enabled, &running);
	*out_val = pmc->counter_val;

	return 0;
}

int kvm_riscv_vcpu_pmu_read_hpm(struct kvm_vcpu *vcpu, unsigned int csr_num,
				unsigned long *val, unsigned long new_val,
				unsigned long wr_mask)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);
	int cidx, ret = KVM_INSN_CONTINUE_NEXT_SEPC;

	if (!kvpmu)
		return KVM_INSN_EXIT_TO_USER_SPACE;
	if (wr_mask)
		return KVM_INSN_ILLEGAL_TRAP;
	cidx = csr_num - CSR_CYCLE;

	if (kvm_riscv_vcpu_pmu_ctr_read(vcpu, cidx, val) < 0)
		return KVM_INSN_EXIT_TO_USER_SPACE;

	return ret;
}

int kvm_riscv_vcpu_pmu_num_ctrs(struct kvm_vcpu *vcpu, unsigned long *out_val)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);

	if (!kvpmu)
		return -EINVAL;

	*out_val = kvpmu->num_fw_ctrs + kvpmu->num_hw_ctrs;
	return 0;
}

int kvm_riscv_vcpu_pmu_ctr_info(struct kvm_vcpu *vcpu, unsigned long cidx,
				unsigned long *ctr_info)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);

	if (!kvpmu || (cidx > RISCV_MAX_COUNTERS) || (cidx == 1))
		return -EINVAL;

	*ctr_info = kvpmu->pmc[cidx].cinfo.value;

	return 0;
}

int kvm_riscv_vcpu_pmu_ctr_start(struct kvm_vcpu *vcpu, unsigned long ctr_base,
				 unsigned long ctr_mask, unsigned long flag, uint64_t ival)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);
	int i, num_ctrs, pmc_index;
	struct kvm_pmc *pmc;
	int fevent_code;

	num_ctrs = kvpmu->num_fw_ctrs + kvpmu->num_hw_ctrs;
	if (ctr_base + __fls(ctr_mask) >= num_ctrs)
		return -EINVAL;

	/* Start the counters that have been configured and requested by the guest */
	for_each_set_bit(i, &ctr_mask, RISCV_MAX_COUNTERS) {
		pmc_index = i + ctr_base;
		if (!test_bit(pmc_index, kvpmu->used_pmc))
			continue;
		pmc = &kvpmu->pmc[pmc_index];
		if (flag & SBI_PMU_START_FLAG_SET_INIT_VALUE)
			pmc->counter_val = ival;
		if (pmc->cinfo.type == SBI_PMU_CTR_TYPE_FW) {
			fevent_code = get_event_code(pmc->event_idx);
			if (fevent_code >= SBI_PMU_FW_MAX)
				return -EINVAL;

			kvpmu->fw_event[fevent_code].started = true;
			kvpmu->fw_event[fevent_code].value = pmc->counter_val;
		} else if (pmc->perf_event) {
			perf_event_period(pmc->perf_event, pmu_get_sample_period(pmc));
			perf_event_enable(pmc->perf_event);
		}
	}

	return 0;
}

int kvm_riscv_vcpu_pmu_ctr_stop(struct kvm_vcpu *vcpu, unsigned long ctr_base,
				 unsigned long ctr_mask, unsigned long flag)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);
	int i, num_ctrs, pmc_index;
	u64 enabled, running;
	struct kvm_pmc *pmc;
	int fevent_code;

	num_ctrs = kvpmu->num_fw_ctrs + kvpmu->num_hw_ctrs;
	if ((ctr_base + __fls(ctr_mask)) >= num_ctrs)
		return -EINVAL;

	/* Stop the counters that have been configured and requested by the guest */
	for_each_set_bit(i, &ctr_mask, RISCV_MAX_COUNTERS) {
		pmc_index = i + ctr_base;
		if (!test_bit(pmc_index, kvpmu->used_pmc))
			continue;
		pmc = &kvpmu->pmc[pmc_index];
		if (pmc->cinfo.type == SBI_PMU_CTR_TYPE_FW) {
			fevent_code = get_event_code(pmc->event_idx);
			if (fevent_code >= SBI_PMU_FW_MAX)
				return -EINVAL;
			kvpmu->fw_event[fevent_code].started = false;
		} else if (pmc->perf_event) {
			/* Stop counting the counter */
			perf_event_disable(pmc->perf_event);
			if (flag & SBI_PMU_STOP_FLAG_RESET) {
				/* Relase the counter if this is a reset request */
				pmc->counter_val += perf_event_read_value(pmc->perf_event,
									  &enabled, &running);
				pmu_release_perf_event(pmc);
			}
		}
		if (flag & SBI_PMU_STOP_FLAG_RESET) {
			pmc->event_idx = SBI_PMU_EVENT_IDX_INVALID;
			clear_bit(pmc_index, kvpmu->used_pmc);
		}
	}

	return 0;
}

int kvm_riscv_vcpu_pmu_ctr_cfg_match(struct kvm_vcpu *vcpu, unsigned long ctr_base,
				     unsigned long ctr_mask, unsigned long flag,
				     unsigned long eidx, uint64_t edata)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);
	struct perf_event *event;
	struct perf_event_attr attr;
	int num_ctrs, ctr_idx;
	u32 etype = pmu_get_perf_event_type(eidx);
	u64 config;
	struct kvm_pmc *pmc = NULL;
	bool is_fevent;
	unsigned long event_code;

	num_ctrs = kvpmu->num_fw_ctrs + kvpmu->num_hw_ctrs;
	if ((etype == PERF_TYPE_MAX) || ((ctr_base + __fls(ctr_mask)) >= num_ctrs))
		return -EINVAL;

	event_code = get_event_code(eidx);
	is_fevent = pmu_is_fw_event(eidx);
	if (is_fevent && event_code >= SBI_PMU_FW_MAX)
		return -EOPNOTSUPP;

	/*
	 * SKIP_MATCH flag indicates the caller is aware of the assigned counter
	 * for this event. Just do a sanity check if it already marked used.
	 */
	if (flag & SBI_PMU_CFG_FLAG_SKIP_MATCH) {
		if (!test_bit(ctr_base, kvpmu->used_pmc))
			return -EINVAL;
		ctr_idx = ctr_base;
		if (is_fevent)
			goto perf_event_done;
		else
			goto match_done;
	}

	ctr_idx = pmu_get_pmc_index(kvpmu, eidx, ctr_base, ctr_mask);
	if (ctr_idx < 0)
		return -EOPNOTSUPP;

	/*
	 * No need to create perf events for firmware events as the firmware counter
	 * is supposed to return the measurement of VS->HS mode invocations.
	 */
	if (is_fevent)
		goto perf_event_done;

match_done:
	pmc = &kvpmu->pmc[ctr_idx];
	pmu_release_perf_event(pmc);
	pmc->idx = ctr_idx;

	config = pmu_get_perf_event_config(eidx, edata);
	memset(&attr, 0, sizeof(struct perf_event_attr));
	attr.type = etype;
	attr.size = sizeof(attr);
	attr.pinned = true;

	/*
	 * It should never reach here if the platform doesn't support sscofpmf extensio
	 * as mode filtering won't work without it.
	 */
	attr.exclude_host = true;
	attr.exclude_hv = true;
	attr.exclude_user = flag & SBI_PMU_CFG_FLAG_SET_UINH ? 1 : 0;
	attr.exclude_kernel = flag & SBI_PMU_CFG_FLAG_SET_SINH ? 1 : 0;
	attr.config = config;
	attr.config1 = RISCV_KVM_PMU_CONFIG1_GUEST_EVENTS;
	if (flag & SBI_PMU_CFG_FLAG_CLEAR_VALUE) {
		//TODO: Do we really want to clear the value in hardware counter
		pmc->counter_val = 0;
	}
	/*
	 * Set the default sample_period for now. The guest specified value
	 * will be updated in the start call.
	 */
	attr.sample_period = pmu_get_sample_period(pmc);

	event = perf_event_create_kernel_counter(&attr, -1, current, NULL, pmc);
	if (IS_ERR(event)) {
		pr_err("kvm pmu event creation failed event %pe for eidx %lx\n", event, eidx);
		return -EOPNOTSUPP;
	}

	pmc->perf_event = event;
perf_event_done:
	if (flag & SBI_PMU_CFG_FLAG_AUTO_START) {
		if (is_fevent)
			kvpmu->fw_event[event_code].started = true;
		else
			perf_event_enable(pmc->perf_event);
	}
	/* This should be only true for firmware events */
	if (!pmc)
		pmc = &kvpmu->pmc[ctr_idx];
	pmc->event_idx = eidx;
	set_bit(ctr_idx, kvpmu->used_pmc);

	return ctr_idx;
}

int kvm_riscv_vcpu_pmu_init(struct kvm_vcpu *vcpu)
{
	int i, num_hw_ctrs, num_fw_ctrs, hpm_width;
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc;

	if (!kvpmu)
		return -EINVAL;

	num_hw_ctrs = riscv_pmu_sbi_get_num_hw_ctrs();
	if ((num_hw_ctrs + RISCV_KVM_MAX_FW_CTRS) > RISCV_MAX_COUNTERS)
		num_fw_ctrs = RISCV_MAX_COUNTERS - num_hw_ctrs;
	else
		num_fw_ctrs = RISCV_KVM_MAX_FW_CTRS;

	hpm_width = riscv_pmu_sbi_hpmc_width();
	if (hpm_width <= 0) {
		pr_err("Can not initialize PMU for vcpu as hpmcounter width is not available\n");
		return -EINVAL;
	}

	bitmap_zero(kvpmu->used_pmc, RISCV_MAX_COUNTERS);
	kvpmu->num_hw_ctrs = num_hw_ctrs;
	kvpmu->num_fw_ctrs = num_fw_ctrs;
	memset(&kvpmu->fw_event, 0, SBI_PMU_FW_MAX * sizeof(struct kvm_fw_event));
	/*
	 * There is no corelation betwen the logical hardware counter and virtual counters.
	 * However, we need to encode a hpmcounter CSR in the counter info field so that
	 * KVM can trap n emulate the read. This works well in the migraiton usecase as well
	 * KVM doesn't care if the actual hpmcounter is available in the hardware or not.
	 */
	for (i = 0; i < num_hw_ctrs + num_fw_ctrs; i++) {
		/* TIME CSR shouldn't be read from perf interface */
		if (i == 1)
			continue;
		pmc = &kvpmu->pmc[i];
		pmc->idx = i;
		pmc->counter_val = 0;
		pmc->vcpu = vcpu;
		pmc->event_idx = SBI_PMU_EVENT_IDX_INVALID;
		if (i < kvpmu->num_hw_ctrs) {
			kvpmu->pmc[i].cinfo.type = SBI_PMU_CTR_TYPE_HW;
			if (i < 3)
				/* CY, IR counters */
				pmc->cinfo.width = 63;
			else
				pmc->cinfo.width = hpm_width;
			/*
			 * The CSR number doesn't have any relation with the logical
			 * hardware counters. The CSR numbers are encoded sequentially
			 * to avoid maintaining a map between the virtual counter
			 * and CSR number.
			 */
			pmc->cinfo.csr = CSR_CYCLE + i;
		} else {
			pmc->cinfo.type = SBI_PMU_CTR_TYPE_FW;
			pmc->cinfo.width = BITS_PER_LONG - 1;
		}
	}

	return 0;
}

void kvm_riscv_vcpu_pmu_deinit(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *kvpmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc;
	int i;

	if (!kvpmu)
		return;

	for_each_set_bit(i, kvpmu->used_pmc, RISCV_MAX_COUNTERS) {
		pmc = &kvpmu->pmc[i];
		pmu_release_perf_event(pmc);
		pmc->counter_val = 0;
		pmc->event_idx = SBI_PMU_EVENT_IDX_INVALID;
	}
	memset(&kvpmu->fw_event, 0, SBI_PMU_FW_MAX * sizeof(struct kvm_fw_event));
}

void kvm_riscv_vcpu_pmu_reset(struct kvm_vcpu *vcpu)
{
	kvm_riscv_vcpu_pmu_deinit(vcpu);
}
