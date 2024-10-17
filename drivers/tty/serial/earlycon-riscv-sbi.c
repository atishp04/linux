// SPDX-License-Identifier: GPL-2.0
/*
 * RISC-V SBI based earlycon
 *
 * Copyright (C) 2018 Anup Patel <anup@brainfault.org>
 */
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/serial_core.h>
#include <asm/sbi.h>
#include <asm/cove.h>
#include <asm/covg_sbi.h>
#include <linux/memblock.h>

#ifdef CONFIG_RISCV_COVE_GUEST
#define DBCN_BOUNCE_BUF_SIZE (PAGE_SIZE)
static char dbcn_buf[DBCN_BOUNCE_BUF_SIZE] __aligned(PAGE_SIZE);
#endif

static void sbi_putc(struct uart_port *port, unsigned char c)
{
	sbi_console_putchar(c);
}

static void sbi_0_1_console_write(struct console *con,
				  const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;
	uart_console_write(&dev->port, s, n, sbi_putc);
}

#ifdef CONFIG_RISCV_COVE_GUEST
static void sbi_dbcn_console_write_cove(struct console *con, const char *s,
					unsigned int n)
{
	phys_addr_t pa = __pa(dbcn_buf);
	unsigned int off = 0;

	while (off < n) {
		const unsigned int rem = n - off;
		const unsigned int size =
			rem > DBCN_BOUNCE_BUF_SIZE ? DBCN_BOUNCE_BUF_SIZE : rem;

		memcpy(dbcn_buf, &s[off], size);
		sbi_ecall(SBI_EXT_DBCN, SBI_EXT_DBCN_CONSOLE_WRITE,
#ifdef CONFIG_32BIT
			  size, pa, (u64)pa >> 32,
#else
			  size, pa, 0,
#endif
			  0, 0, 0);

		off += size;
	}
}
#endif

static void sbi_dbcn_console_write(struct console *con,
				   const char *s, unsigned int n)
{
	int ret;

	while (n) {
		ret = sbi_debug_console_write(s, n);
		if (ret < 0)
			break;

		s += ret;
		n -= ret;
	}
}

static int __init early_sbi_setup(struct earlycon_device *device,
				  const char *opt)
{
	int ret;

	if ((sbi_spec_version >= sbi_mk_version(1, 0)) &&
	    (sbi_probe_extension(SBI_EXT_DBCN) > 0)) {
		#ifdef CONFIG_RISCV_COVE_GUEST
		if (is_cove_guest()) {
			ret = sbi_covg_share_memory(__pa(dbcn_buf),
						    DBCN_BOUNCE_BUF_SIZE);
			if (ret)
				return ret;

			device->con->write = sbi_dbcn_console_write_cove;
			return 0;
		}
		#endif
		device->con->write = sbi_dbcn_console_write;
	}
	else if (IS_ENABLED(CONFIG_RISCV_SBI_V01))
		device->con->write = sbi_0_1_console_write;
	else
		return -ENODEV;

	return 0;
}
EARLYCON_DECLARE(sbi, early_sbi_setup);
