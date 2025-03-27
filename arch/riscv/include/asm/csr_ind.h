/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Rivos Inc.
 */

#ifndef _ASM_RISCV_CSR_IND_H
#define _ASM_RISCV_CSR_IND_H

#include <linux/irqflags.h>

#include <asm/csr.h>

#define csr_ind_read(iregcsr, iselbase, iseloff) ({		\
	unsigned long __value = 0;				\
	unsigned long __flags;					\
	local_irq_save(__flags);				\
	csr_write(CSR_ISELECT, (iselbase) + (iseloff));		\
	__value = csr_read(iregcsr);				\
	local_irq_restore(__flags);				\
	__value;						\
})

#define csr_ind_write(iregcsr, iselbase, iseloff, value) ({	\
	unsigned long __flags;					\
	local_irq_save(__flags);				\
	csr_write(CSR_ISELECT, (iselbase) + (iseloff));		\
	csr_write(iregcsr, (value));				\
	local_irq_restore(__flags);				\
})

#define csr_ind_warl(iregcsr, iselbase, iseloff, warl_val) ({	\
	unsigned long __old_val = 0, __value = 0;		\
	unsigned long __flags;					\
	local_irq_save(__flags);				\
	csr_write(CSR_ISELECT, (iselbase) + (iseloff));		\
	__old_val = csr_read(iregcsr);				\
	csr_write(iregcsr, (warl_val));				\
	__value = csr_read(iregcsr);				\
	csr_write(iregcsr, __old_val);				\
	local_irq_restore(__flags);				\
	__value;						\
})

#endif
