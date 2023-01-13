/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RISCV Memory Encryption Support.
 *
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#ifndef __RISCV_MEM_ENCRYPT_H__
#define __RISCV_MEM_ENCRYPT_H__

#include <linux/init.h>

struct device;

bool force_dma_unencrypted(struct device *dev);

/* Architecture __weak replacement functions */
void __init mem_encrypt_init(void);

int set_memory_encrypted(unsigned long addr, int numpages);
int set_memory_decrypted(unsigned long addr, int numpages);

#endif /* __RISCV_MEM_ENCRYPT_H__ */
