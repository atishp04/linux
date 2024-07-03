// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Rivos Inc.
 */

#include "asm-generic/errno-base.h"
#include "linux/list.h"
#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/irqchip/riscv-imsic.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <acpi/actypes.h>

//#define RISCV_SMMC_DSM_APPROCH 1

struct smmc_data {
	struct device	*dev;
	acpi_handle acpi_dev_handle;
	u32             gsi_base;
	u32             nr_irqs;
	u32             id;
	struct list_head dev_res_list;
};

#ifdef RISCV_SMMC_DSM_APPROCH
static const guid_t smmc_guid = GUID_INIT(0xF0EAA91D,0x3F8E,0x4D2B,0x8C,0x74,0xD4,0xBA,0x77,0x92,0xF3,0xA4);

#else
struct smmc_dev_cfg {
	u64 resoure_index;
	u64 gsino;
	u64 addr_low;
	u64 addr_hi;
	u64 msidata;
	u64 enable;
};
struct smmc_dev_res {
	struct list_head list;
	struct smmc_dev_cfg *cfg;
	void __iomem	*regs;
};

static struct smmc_dev_res *smmc_find_devres_for_gsi(struct smmc_data *data, int hwirq)
{
	struct smmc_dev_res *devres = NULL;
	int gsi = data->gsi_base + hwirq;

	pr_err("data->gsi_base %d hwirq %d gsi %d\n", data->gsi_base, hwirq, gsi);
	list_for_each_entry(devres, &data->dev_res_list, list) {
		pr_err("gsino %d regs %lx ah %lx\n", devres->cfg->gsino, devres->regs, devres->cfg->addr_hi);
		if ((gsi == devres->cfg->gsino) && devres->regs)
			return devres;
	}

	if (!devres)
		pr_err("can't find a dev resource for gsi %d\n", hwirq);

	return NULL;
}

static struct smmc_dev_res *smmc_find_devres_for_index(struct smmc_data *data, int index)
{
	struct smmc_dev_res *devres = NULL;

	list_for_each_entry(devres, &data->dev_res_list, list) {
		if ((index == devres->cfg->resoure_index) && devres->regs)
			return devres;
	}

	return NULL;
}

static void smmc_free_devres_list(struct smmc_data *data)
{
	struct smmc_dev_res *devres, *temp;

	list_for_each_entry_safe(devres, temp, &data->dev_res_list, list) {
		list_del(&devres->list);
		kfree(devres->cfg);
		kfree(devres);
	}
}

#endif

static void smmc_msi_irq_unmask(struct irq_data *d)
{
	struct smmc_data *data = irq_data_get_irq_chip_data(d);

#ifdef RISCV_SMMC_DSM_APPROCH
	union acpi_object *acpi_obj, tmp[2];
	union acpi_object argv = ACPI_INIT_DSM_ARGV4(2, tmp);
	int ret;

	tmp[0].type = ACPI_TYPE_INTEGER;
	tmp[0].integer.value = d->hwirq;
	tmp[1].type = ACPI_TYPE_INTEGER;
	tmp[1].integer.value = 1;

	acpi_obj = acpi_evaluate_dsm_typed(data->acpi_dev_handle, &smmc_guid, 0, 0, &argv, ACPI_TYPE_INTEGER);
	if (!acpi_obj) {
		pr_err("evaluating DSM function %d failed\n", 1);
		return;
	} else {
		ret = acpi_obj->integer.value;
		ACPI_FREE(acpi_obj);
	}

	if (ret)
		pr_err("%s msi write message failed %d \n", __func__, ret);
#else
	struct smmc_dev_res *devres = smmc_find_devres_for_gsi(data, d->hwirq);

	if (!devres)
		return;

	writel(1, devres->regs + devres->cfg->enable);
#endif
	irq_chip_unmask_parent(d);
}

static void smmc_msi_irq_mask(struct irq_data *d)
{
	struct smmc_data *data = irq_data_get_irq_chip_data(d);

#ifdef RISCV_SMMC_DSM_APPROCH
	union acpi_object *acpi_obj, tmp[2];
	union acpi_object argv = ACPI_INIT_DSM_ARGV4(2, tmp);
	int ret;

	tmp[0].type = ACPI_TYPE_INTEGER;
	tmp[0].integer.value = d->hwirq;
	tmp[1].type = ACPI_TYPE_INTEGER;
	tmp[1].integer.value = 0;

	acpi_obj = acpi_evaluate_dsm_typed(data->acpi_dev_handle, &smmc_guid, 0, 1, &argv, ACPI_TYPE_INTEGER);
	if (!acpi_obj) {
		pr_err("evaluating DSM function %d failed\n", 1);
		return;
	} else {
		ret = acpi_obj->integer.value;
		ACPI_FREE(acpi_obj);
	}

	if (ret)
		pr_err("irq masking failed\n");
#else
	struct smmc_dev_res *devres = smmc_find_devres_for_gsi(data, d->hwirq);

	if (!devres)
		return;

	writel(0, devres->regs + devres->cfg->enable);
#endif
	irq_chip_mask_parent(d);
}

static int smmc_msi_irq_set_type(struct irq_data *d, unsigned int type)
{
	pr_err("%s: In type %d \n", __func__, type);

	return 0;
}

static void smmc_msi_write_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct smmc_data *data = irq_data_get_irq_chip_data(d);

#ifdef RISCV_SMMC_DSM_APPROCH
	union acpi_object *acpi_obj, tmp[4];
	union acpi_object argv = ACPI_INIT_DSM_ARGV4(4, tmp);
	int ret;

	pr_err("%s : irq %d: hwirq: %d\n", __func__, d->irq, d->hwirq);
	tmp[0].type = ACPI_TYPE_INTEGER;
	tmp[0].integer.value = d->hwirq;
	tmp[1].type = ACPI_TYPE_INTEGER;
	tmp[1].integer.value = msg->address_lo;
	tmp[2].type = ACPI_TYPE_INTEGER;
	tmp[2].integer.value = msg->address_hi;
	tmp[3].type = ACPI_TYPE_INTEGER;
	tmp[3].integer.value = msg->data;

	acpi_obj = acpi_evaluate_dsm_typed(data->acpi_dev_handle, &smmc_guid, 0, 1, &argv, ACPI_TYPE_INTEGER);
	if (!acpi_obj) {
		pr_err("%s :evaluating DSM function %d failed\n", __func__, 1);
		return;
	} else {
		ret = acpi_obj->integer.value;
		ACPI_FREE(acpi_obj);
	}

	if (ret)
		pr_err("%s msi write message failed %d \n", __func__, ret);
#else
	struct smmc_dev_res *devres = smmc_find_devres_for_gsi(data, d->hwirq);

	if (!devres)
		return;

	pr_err("%s: In d->irq %d hwirq %d msg_hi %lx msg_low %lx data %lx\n",
	   __func__, d->irq, d->hwirq, msg->address_hi, msg->address_lo, msg->data);

	writel(msg->address_lo, devres->regs + devres->cfg->addr_low);
	writel(msg->address_hi, devres->regs + devres->cfg->addr_hi);
	writel(msg->data, devres->regs + devres->cfg->msidata);
	pr_err("%s: regs.0 %x\n", __func__, readl(devres->regs + devres->cfg->addr_low));
	pr_err("%s: regs.1 %x\n", __func__, readl(devres->regs + devres->cfg->addr_hi));
	pr_err("%s: regs.2 %x\n", __func__, readl(devres->regs + devres->cfg->msidata));
	pr_err("%s: regs.3 %x\n", __func__, readl(devres->regs + devres->cfg->enable));
#endif
}

static void smmc_msi_set_desc(msi_alloc_info_t *arg, struct msi_desc *desc)
{
	arg->desc = desc;
	arg->hwirq = (u32)desc->data.icookie.value;
}

static int smmc_msi_irqdomain_translate(struct irq_domain *d, struct irq_fwspec *fwspec,
			       unsigned long *hwirq, unsigned int *type)
{
	struct msi_domain_info *info = d->host_data;
	struct smmc_data *data = info->data;

	if (WARN_ON(fwspec->param_count < 2))
		return -EINVAL;
	if (WARN_ON(!fwspec->param[0]))
		return -EINVAL;

	/* For DT, gsi_base is always zero. */
	*hwirq = fwspec->param[0] - data->gsi_base;
	*type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;

	WARN_ON(*type == IRQ_TYPE_NONE);

	return 0;
}

static const struct msi_domain_template smmc_msi_template = {
	.chip = {
		.name			= "SMMC-MSI",
		.irq_mask		= smmc_msi_irq_mask,
		.irq_unmask		= smmc_msi_irq_unmask,
		.irq_set_type	= smmc_msi_irq_set_type,
		//.irq_eoi		= aplic_msi_irq_eoi,
#ifdef CONFIG_SMP
		.irq_set_affinity	= irq_chip_set_affinity_parent,
#endif
		.irq_write_msi_msg	= smmc_msi_write_msg,
		.flags			= IRQCHIP_SET_TYPE_MASKED |
					  IRQCHIP_SKIP_SET_WAKE |
					  IRQCHIP_MASK_ON_SUSPEND,
	},

	.ops = {
		.set_desc		= smmc_msi_set_desc,
		.msi_translate		= smmc_msi_irqdomain_translate,
	},

	.info = {
		.bus_token		= DOMAIN_BUS_WIRED_TO_MSI, //TODO: Check
		.flags			= MSI_FLAG_USE_DEV_FWNODE,
		//.handler		= handle_fasteoi_irq,
		//.handler_name		= "fasteoi",
	},
};
#ifndef RISCV_SMMC_DSM_APPROCH

static int smmc_parse_package_resource(struct platform_device *pdev, struct smmc_data *data)
{
	struct device *dev = &pdev->dev;
	int result = -EFAULT;
	acpi_status status = AE_OK;
	struct acpi_buffer buffer = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *smmc_obj = NULL;
	struct acpi_buffer format = {sizeof("NNNNNN"), "NNNNNN"};
	struct acpi_buffer state = {0, NULL};
	struct smmc_dev_res *res, *temp;
	int i;

	struct acpi_handle *handle = ACPI_HANDLE_FWNODE(dev->fwnode);

	status = acpi_evaluate_object_typed(handle, "CFGN", NULL,
					    &buffer, ACPI_TYPE_PACKAGE);
	if (status == AE_NOT_FOUND)
		return 0;
	if (ACPI_FAILURE(status))
		return -ENODEV;

	smmc_obj = buffer.pointer;
	if (!smmc_obj) {
		pr_err("Invalid SMMC data\n");
		goto end;
	}

	pr_err("%s: package count %d\n", __func__, smmc_obj->package.count);

	for (i = 0; i < smmc_obj->package.count; i++) {
		res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);
		if (!res)
			goto end_nomem;
		res->cfg = devm_kzalloc(dev, sizeof(struct smmc_dev_cfg), GFP_KERNEL);
		if (!res->cfg)
			goto end_nomem;

		state.length = sizeof(struct smmc_dev_cfg);
		state.pointer = res->cfg;

		status = acpi_extract_package(&(smmc_obj->package.elements[i]),
				      &format, &state);
		if (ACPI_FAILURE(status)) {
			pr_err("Invalid SMMC CFG %d\n", status);
			goto end;
		}
		/* if resource is already mapped, no need to invoke ioremap again */
		temp = smmc_find_devres_for_index(data, res->cfg->resoure_index);
		if (temp) {
			res->regs = temp->regs;
			goto skip_ioremap;
		}
		res->regs = devm_platform_ioremap_resource(pdev, res->cfg->resoure_index);
		if (IS_ERR(res->regs)) {
			result = PTR_ERR(res->regs);
			smmc_free_devres_list(data);
			goto end;
		}
skip_ioremap:
		list_add(&res->list, &data->dev_res_list);
	}
	result = 0;

end_nomem:
	result = -ENOMEM;
end:
	kfree(buffer.pointer);
	return result;
}
#endif

static int smmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc = 0;
	const struct imsic_global_config *imsic_global;
	struct irq_domain *msi_domain;
	struct smmc_data *data;

	/* SMMC device is only valid for ACPI supported platforms */
	if (acpi_disabled)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	data->acpi_dev_handle = ACPI_HANDLE_FWNODE(dev->fwnode);
	INIT_LIST_HEAD(&data->dev_res_list);

	rc = riscv_acpi_get_gsi_info(dev->fwnode, &data->gsi_base, &data->id,
					     &data->nr_irqs, NULL);
	if (rc) {
		dev_err(dev, "failed to find GSI mapping\n");
		return rc;
	}

#ifndef RISCV_SMMC_DSM_APPROCH

	smmc_parse_package_resource(pdev, data);
	//TODO: Setup any initial state of the device and enable MSI delivery
#endif
	imsic_global = imsic_get_global_config();
	if (!imsic_global) {
		dev_err(dev, "IMSIC global config not found\n");
		return -ENODEV;
	}

	if (!dev_get_msi_domain(dev)) {
		msi_domain = irq_find_matching_fwnode(imsic_acpi_get_fwnode(dev),
							DOMAIN_BUS_PLATFORM_MSI);
		if (msi_domain)
			dev_set_msi_domain(dev, msi_domain);
	}

	if (!msi_create_device_irq_domain(dev, MSI_DEFAULT_DOMAIN, &smmc_msi_template,
						data->nr_irqs + 1, data, data)) {
		dev_err(dev, "failed to create MSI irq domain\n");
		return -ENOMEM;
	}

	acpi_dev_clear_dependencies(ACPI_COMPANION(dev));

	return rc;
}

static const struct acpi_device_id smmc_acpi_match[] = {
	{ "RSCV0005", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, smmc_acpi_match);

static struct platform_driver smmc_driver = {
	.driver = {
		.name		= "riscv-smmc",
		.acpi_match_table = ACPI_PTR(smmc_acpi_match),
	},
	.probe = smmc_probe,
};
builtin_platform_driver(smmc_driver);