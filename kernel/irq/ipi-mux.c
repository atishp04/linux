// SPDX-License-Identifier: GPL-2.0-only
/*
 * Multiplex several IPIs over a single HW IPI.
 *
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */

#define pr_fmt(fmt) "ipi-mux: " fmt
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/smp.h>

static unsigned int ipi_mux_parent_virq;
static struct irq_domain *ipi_mux_domain;
static const struct  ipi_mux_ops *ipi_mux_ops;
static DEFINE_PER_CPU(unsigned long, ipi_mux_bits);

static void ipi_mux_dummy(struct irq_data *d)
{
}

static void ipi_mux_send_mask(struct irq_data *d, const struct cpumask *mask)
{
	int cpu;

	/* Barrier before doing atomic bit update to IPI bits */
	smp_mb__before_atomic();

	for_each_cpu(cpu, mask)
		set_bit(d->hwirq, per_cpu_ptr(&ipi_mux_bits, cpu));

	/* Barrier after doing atomic bit update to IPI bits */
	smp_mb__after_atomic();

	/* Trigger the parent IPI */
	ipi_mux_ops->ipi_mux_send(ipi_mux_parent_virq, mask);
}

static struct irq_chip ipi_mux_chip = {
	.name		= "RISC-V IPI Mux",
	.irq_mask	= ipi_mux_dummy,
	.irq_unmask	= ipi_mux_dummy,
	.ipi_send_mask	= ipi_mux_send_mask,
};

static int ipi_mux_domain_map(struct irq_domain *d, unsigned int irq,
			      irq_hw_number_t hwirq)
{
	irq_set_percpu_devid(irq);
	irq_domain_set_info(d, irq, hwirq, &ipi_mux_chip, d->host_data,
			    handle_percpu_devid_irq, NULL, NULL);

	return 0;
}

static int ipi_mux_domain_alloc(struct irq_domain *d, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	int i, ret;
	irq_hw_number_t hwirq;
	unsigned int type = IRQ_TYPE_NONE;
	struct irq_fwspec *fwspec = arg;

	ret = irq_domain_translate_onecell(d, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++) {
		ret = ipi_mux_domain_map(d, virq + i, hwirq + i);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct irq_domain_ops ipi_mux_domain_ops = {
	.translate	= irq_domain_translate_onecell,
	.alloc		= ipi_mux_domain_alloc,
	.free		= irq_domain_free_irqs_top,
};

void ipi_mux_process(void)
{
	int err;
	unsigned long irqs, *bits = this_cpu_ptr(&ipi_mux_bits);
	irq_hw_number_t hwirq;

	while (true) {
		/* Clear the parent IPI */
		ipi_mux_ops->ipi_mux_clear(ipi_mux_parent_virq);

		/* Order bit clearing and data access. */
		mb();

		irqs = xchg(bits, 0);
		if (!irqs)
			break;

		for_each_set_bit(hwirq, &irqs, IPI_MUX_NR_IRQS) {
			err = generic_handle_domain_irq(ipi_mux_domain,
							hwirq);
			if (unlikely(err))
				pr_warn_ratelimited(
					"can't find mapping for hwirq %lu\n",
					hwirq);
		}
	}
}

static void ipi_mux_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);

	chained_irq_enter(chip, desc);
	ipi_mux_process();
	chained_irq_exit(chip, desc);
}

static int ipi_mux_dying_cpu(unsigned int cpu)
{
	if (ipi_mux_parent_virq > 0)
		disable_percpu_irq(ipi_mux_parent_virq);
	return 0;
}

static int ipi_mux_starting_cpu(unsigned int cpu)
{
	if (ipi_mux_parent_virq > 0)
		enable_percpu_irq(ipi_mux_parent_virq,
			irq_get_trigger_type(ipi_mux_parent_virq));
	return 0;
}

int ipi_mux_create(unsigned int parent_virq, const struct ipi_mux_ops *ops)
{
	int virq;
	struct irq_fwspec ipi;
	struct irq_domain *domain;

	if (ipi_mux_domain ||
	    !ops || !ops->ipi_mux_send || !ops->ipi_mux_clear)
		return 0;

	domain = irq_domain_add_linear(NULL, IPI_MUX_NR_IRQS,
				       &ipi_mux_domain_ops, NULL);
	if (!domain) {
		pr_err("unable to add IPI Mux domain\n");
		return 0;
	}

	ipi.fwnode = domain->fwnode;
	ipi.param_count = 1;
	ipi.param[0] = 0;
	virq = __irq_domain_alloc_irqs(domain, -1, IPI_MUX_NR_IRQS,
				       NUMA_NO_NODE, &ipi, false, NULL);
	if (virq <= 0) {
		pr_err("unable to alloc IRQs from IPI Mux domain\n");
		irq_domain_remove(domain);
		return virq;
	}

	ipi_mux_domain = domain;
	ipi_mux_parent_virq = parent_virq;
	ipi_mux_ops = ops;

	if (parent_virq > 0) {
		irq_set_chained_handler(parent_virq, ipi_mux_handler);

		cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
				  "irqchip/ipi-mux:starting",
				  ipi_mux_starting_cpu, ipi_mux_dying_cpu);
	}

	return virq;
}

void ipi_mux_destroy(void)
{
	if (!ipi_mux_domain)
		return;

	irq_domain_remove(ipi_mux_domain);
	ipi_mux_domain = NULL;
	ipi_mux_parent_virq = 0;
}
