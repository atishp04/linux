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


/* Check for valid access_width, otherwise, fallback to using bit_width */
#define GET_BIT_WIDTH(reg) ((reg)->access_size ? (8 << ((reg)->access_size - 1)) : (reg)->bit_width)

/* Index of ACPI buffer in a CFGX package */
enum {
	SMMC_MSI_ADDR_LOW_INDEX = 0,
	SMMC_MSI_ADDR_HIGH_INDEX,
	SMMC_MSI_DATA_INDEX,
	SMMC_MSI_ENABLE_INDEX,
	SMMC_MSI_INDEX_MAX,
};

struct smmc_data {
	struct device	*dev;
	acpi_handle acpi_dev_handle;
	u32             gsi_base;
	u32             nr_irqs;
	u32             id;
	struct list_head dev_res_list;
};

/* SMMC ACPI Generic Register Descriptor format */
struct smmc_reg {
	u8 descriptor;
	u16 length;
	u8 space_id;
	u8 bit_width;
	u8 bit_offset;
	u8 access_size;
	u64 address;
} __packed;

struct smmc_register_resource {
	acpi_object_type type;
	union {
		u32 __iomem *sysmem_va;
		u64 addr_reg;
	};
};

struct smmc_dev_res {
	struct list_head list;
	u64 gsino;
	struct smmc_register_resource regs[SMMC_MSI_INDEX_MAX];
};

static struct smmc_dev_res *smmc_find_devres_for_gsi(struct smmc_data *data, int hwirq)
{
	struct smmc_dev_res *devres = NULL;
	int gsi = data->gsi_base + hwirq;

	list_for_each_entry(devres, &data->dev_res_list, list) {
		if (gsi == devres->gsino)
			return devres;
	}

	if (!devres)
		pr_err("can't find a dev resource for gsi %d\n", hwirq);

	return NULL;
}

static void smmc_free_devres_list(struct smmc_data *data)
{
	struct smmc_dev_res *devres, *temp;
	int i;

	list_for_each_entry_safe(devres, temp, &data->dev_res_list, list) {
		list_del(&devres->list);
		for(i = 0; i < SMMC_MSI_INDEX_MAX; i++) {
			if (devres->regs[i].type == ACPI_ADR_SPACE_SYSTEM_MEMORY &&
			    devres->regs[i].sysmem_va)
				iounmap(devres->regs[i].sysmem_va);

		}
		kfree(devres);
	}
}

static void smmc_msi_irq_unmask(struct irq_data *d)
{
	struct smmc_data *data = irq_data_get_irq_chip_data(d);
	struct smmc_dev_res *devres = smmc_find_devres_for_gsi(data, d->hwirq);

	if (!devres)
		return;

	writel(1, devres->regs[SMMC_MSI_ENABLE_INDEX].sysmem_va);
	irq_chip_unmask_parent(d);
}

static void smmc_msi_irq_mask(struct irq_data *d)
{
	struct smmc_data *data = irq_data_get_irq_chip_data(d);
	struct smmc_dev_res *devres = smmc_find_devres_for_gsi(data, d->hwirq);

	if (!devres)
		return;

	writel(0, devres->regs[SMMC_MSI_ENABLE_INDEX].sysmem_va);
	irq_chip_mask_parent(d);
}

static int smmc_msi_irq_set_type(struct irq_data *d, unsigned int type)
{
	/* Nothing do right now. We can return failure for level if we restrict supported type to EDGE */

	return 0;
}

static void smmc_msi_write_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct smmc_data *data = irq_data_get_irq_chip_data(d);
	struct smmc_dev_res *devres = smmc_find_devres_for_gsi(data, d->hwirq);

	if (!devres)
		return;

	writel(msg->address_lo, devres->regs[SMMC_MSI_ADDR_LOW_INDEX].sysmem_va);
	writel(msg->address_hi, devres->regs[SMMC_MSI_ADDR_HIGH_INDEX].sysmem_va);
	writel(msg->data, devres->regs[SMMC_MSI_DATA_INDEX].sysmem_va);
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

static int smmc_parse_package_resource(struct platform_device *pdev, struct smmc_data *data)
{
	struct device *dev = &pdev->dev;
	int result = -EFAULT;
	acpi_status status = AE_OK;
	struct acpi_buffer buffer = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *smmc_obj = NULL, *tmp_obj, *gsi_num_obj, *smmc_reg_obj;
	struct smmc_dev_res *res;
	struct smmc_reg *gas_t;
	int i,j;

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

	for (i = 0; i < smmc_obj->package.count; i++) {
		tmp_obj = &smmc_obj->package.elements[i];
		if (tmp_obj->type != ACPI_TYPE_PACKAGE) {
			pr_err("Unsupported CFGN object found at %i index of type %d\n", i, tmp_obj->type);
			goto end;
		}
		if(tmp_obj->package.count != 5) {
			pr_err("Unsupported CFGN object found at %i with package elements %d\n",
				i, tmp_obj->package.count);
			goto end;
		}

		res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);
		if (!res)
			goto end_nomem;

		gsi_num_obj = &tmp_obj->package.elements[0];
		if (gsi_num_obj->type != ACPI_TYPE_INTEGER) {
			pr_err("Unsupported CFGN object found at %i with invalid first element type %d\n",
				i, gsi_num_obj->type);
			goto end;
		}

		res->gsino = gsi_num_obj->integer.value;
		for(j = 0; j < SMMC_MSI_INDEX_MAX; j++) {
			smmc_reg_obj = &tmp_obj->package.elements[j+1];
			if (smmc_reg_obj->type != ACPI_TYPE_BUFFER) {
				pr_err("Unsupported CFGN object found at %i with invalid element type %d\n",
					i, smmc_reg_obj->type);
				goto free_list;
			}
			gas_t = (struct smmc_reg *)smmc_reg_obj->buffer.pointer;
			if (gas_t->space_id == ACPI_ADR_SPACE_SYSTEM_MEMORY) {
				if (gas_t->address) {
					void __iomem *addr;
					size_t access_width;
					access_width = GET_BIT_WIDTH(gas_t) / 8;
					addr = ioremap(gas_t->address, access_width);
					if (!addr)
						goto free_list;
					res->regs[j].type = ACPI_ADR_SPACE_SYSTEM_MEMORY;
					res->regs[j].sysmem_va = addr;
				}
			} else {
				pr_err("Unsupported register type(%d) in _SMC object at index(%d)\n", gas_t->space_id, j);
				goto end;
			}
		}

		list_add(&res->list, &data->dev_res_list);
	}
	kfree(buffer.pointer);
	return 0;

end_nomem:
	result = -ENOMEM;
free_list:
	smmc_free_devres_list(data);
end:
	kfree(buffer.pointer);
	return result;
}

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

	smmc_parse_package_resource(pdev, data);
	//TODO: Setup any initial state of the device and enable MSI delivery
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