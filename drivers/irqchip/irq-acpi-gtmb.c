// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Rivos Inc.
 */


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
#define ACPI_GTMB_VERSION 0

/* Index of ACPI buffer in a CFGX package */
enum {
	GTMB_MSI_ADDR_LOW_INDEX = 0,
	GTMB_MSI_ADDR_HIGH_INDEX,
	GTMB_MSI_DATA_INDEX,
	GTMB_MSI_ENABLE_INDEX,
	GTMB_MSI_RETRIGGER_VAL_INDEX,
	GTMB_MSI_RETRIGGER_REG_INDEX,
	GTMB_MSI_TRIG_TYPE_EDGE_RISING_INDEX,
	GTMB_MSI_TRIG_TYPE_EDGE_FALLING_INDEX,
	GTMB_MSI_TRIG_TYPE_LEVEL_HIGH_INDEX,
	GTMB_MSI_TRIG_TYPE_LEVEL_LOW_INDEX,
	GTMB_MSI_TRIGGER_TYPE_REG_INDEX,
	GTMB_MSI_INDEX_MAX,
};

struct gtmb_data {
	struct device	*dev;
	acpi_handle acpi_dev_handle;
	u32             gsi_base;
	u32             nr_irqs;
	u32             id;
	struct list_head dev_res_list;
};

/* GTMB ACPI Generic Register Descriptor format */
struct gtmb_reg {
	u8 descriptor;
	u16 length;
	u8 space_id;
	u8 bit_width;
	u8 bit_offset;
	u8 access_size;
	u64 address;
} __packed;

struct gtmb_register_resource {
	acpi_object_type type;
	union {
		u32 __iomem *sysmem_va;
		u64 addr_reg;
		u64 int_val;
	};
};

struct gtmb_dev_res {
	struct list_head list;
	u64 gsino;
	struct gtmb_register_resource regs[GTMB_MSI_INDEX_MAX];
};

static struct gtmb_dev_res *gtmb_find_devres_for_gsi(struct gtmb_data *data, int hwirq)
{
	struct gtmb_dev_res *devres = NULL;
	int gsi = data->gsi_base + hwirq;

	list_for_each_entry(devres, &data->dev_res_list, list) {
		if (gsi == devres->gsino)
			return devres;
	}

	if (!devres)
		pr_err("can't find a dev resource for gsi %d\n", hwirq);

	return NULL;
}

static void gtmb_free_devres_list(struct gtmb_data *data)
{
	struct gtmb_dev_res *devres, *temp;
	int i;

	list_for_each_entry_safe(devres, temp, &data->dev_res_list, list) {
		list_del(&devres->list);
		for(i = 0; i < GTMB_MSI_INDEX_MAX; i++) {
			if (devres->regs[i].type == ACPI_ADR_SPACE_SYSTEM_MEMORY &&
			    devres->regs[i].sysmem_va)
				iounmap(devres->regs[i].sysmem_va);

		}
		kfree(devres);
	}
}

static void gtmb_msi_irq_unmask(struct irq_data *d)
{
	struct gtmb_data *data = irq_data_get_irq_chip_data(d);
	struct gtmb_dev_res *devres = gtmb_find_devres_for_gsi(data, d->hwirq);

	if (!devres)
		return;

	writel(1, devres->regs[GTMB_MSI_ENABLE_INDEX].sysmem_va);
	irq_chip_unmask_parent(d);
}

static void gtmb_msi_irq_mask(struct irq_data *d)
{
	struct gtmb_data *data = irq_data_get_irq_chip_data(d);
	struct gtmb_dev_res *devres = gtmb_find_devres_for_gsi(data, d->hwirq);

	if (!devres)
		return;

	writel(0, devres->regs[GTMB_MSI_ENABLE_INDEX].sysmem_va);
	irq_chip_mask_parent(d);
}

static void gtmb_msi_irq_eoi(struct irq_data *d){
	struct gtmb_data *data = irq_data_get_irq_chip_data(d);
	struct gtmb_dev_res *devres = gtmb_find_devres_for_gsi(data, d->hwirq);
	u32 trig_type  = irqd_get_trigger_type(d);
	struct gtmb_register_resource retrig_val_res = devres->regs[GTMB_MSI_RETRIGGER_VAL_INDEX];
	u32 msi_retrig_magic;

	if (trig_type == IRQ_TYPE_LEVEL_LOW || trig_type == IRQ_TYPE_LEVEL_HIGH) {
		if (!devres->regs[GTMB_MSI_RETRIGGER_REG_INDEX].sysmem_va)
		/* No need to do anything as platform did not specify a MSI retrigger register */
			return;
		if (retrig_val_res.type == ACPI_TYPE_INTEGER) {
			msi_retrig_magic = retrig_val_res.int_val;
		} else if(retrig_val_res.type == ACPI_TYPE_BUFFER) {
			msi_retrig_magic = readl(retrig_val_res.sysmem_va);
		} else {
			return;
		}
		writel(msi_retrig_magic, devres->regs[GTMB_MSI_RETRIGGER_REG_INDEX].sysmem_va);
	}
}

static int gtmb_msi_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gtmb_data *data = irq_data_get_irq_chip_data(d);
	struct gtmb_dev_res *devres = gtmb_find_devres_for_gsi(data, d->hwirq);
	u32 trig_type_val;
	struct gtmb_register_resource gtmb_res;

	if (!devres)
		return -ENODEV;

	/* No need to do anything as platform did not specify a MSI trigger type register */
	if (!devres->regs[GTMB_MSI_TRIGGER_TYPE_REG_INDEX].sysmem_va)
		return 0;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		gtmb_res = devres->regs[GTMB_MSI_TRIG_TYPE_EDGE_RISING_INDEX];
		break;
	case IRQ_TYPE_EDGE_FALLING:
		gtmb_res = devres->regs[GTMB_MSI_TRIG_TYPE_EDGE_FALLING_INDEX];
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		gtmb_res = devres->regs[GTMB_MSI_TRIG_TYPE_LEVEL_HIGH_INDEX];
		break;
	case IRQ_TYPE_LEVEL_LOW:
		gtmb_res = devres->regs[GTMB_MSI_TRIG_TYPE_LEVEL_LOW_INDEX];
		break;
	default:
		return -ENODEV;
	}

	if (gtmb_res.type == ACPI_TYPE_INTEGER)
		trig_type_val = gtmb_res.int_val;
	else if (gtmb_res.type == ACPI_TYPE_BUFFER)
		trig_type_val = readl(gtmb_res.sysmem_va);
	else
	 	return 0; /* Trigger Type is not defined by the platform */

	writel(trig_type_val, devres->regs[GTMB_MSI_TRIGGER_TYPE_REG_INDEX].sysmem_va);

	return 0;
}

static void gtmb_msi_write_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct gtmb_data *data = irq_data_get_irq_chip_data(d);
	struct gtmb_dev_res *devres = gtmb_find_devres_for_gsi(data, d->hwirq);

	if (!devres)
		return;

	writel(msg->address_lo, devres->regs[GTMB_MSI_ADDR_LOW_INDEX].sysmem_va);
	writel(msg->address_hi, devres->regs[GTMB_MSI_ADDR_HIGH_INDEX].sysmem_va);
	writel(msg->data, devres->regs[GTMB_MSI_DATA_INDEX].sysmem_va);
}

static void gtmb_msi_set_desc(msi_alloc_info_t *arg, struct msi_desc *desc)
{
	arg->desc = desc;
	arg->hwirq = (u32)desc->data.icookie.value;
}

static int gtmb_msi_irqdomain_translate(struct irq_domain *d, struct irq_fwspec *fwspec,
			       unsigned long *hwirq, unsigned int *type)
{
	struct msi_domain_info *info = d->host_data;
	struct gtmb_data *data = info->data;

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

static const struct msi_domain_template gtmb_msi_template = {
	.chip = {
		.name			= "GTMB-MSI",
		.irq_mask		= gtmb_msi_irq_mask,
		.irq_unmask		= gtmb_msi_irq_unmask,
		.irq_set_type		= gtmb_msi_irq_set_type,
		.irq_eoi		= gtmb_msi_irq_eoi,
#ifdef CONFIG_SMP
		.irq_set_affinity	= irq_chip_set_affinity_parent,
#endif
		.irq_write_msi_msg	= gtmb_msi_write_msg,
		.flags			= IRQCHIP_SET_TYPE_MASKED |
					  IRQCHIP_SKIP_SET_WAKE |
					  IRQCHIP_MASK_ON_SUSPEND,
	},

	.ops = {
		.set_desc		= gtmb_msi_set_desc,
		.msi_translate		= gtmb_msi_irqdomain_translate,
	},

	.info = {
		.bus_token		= DOMAIN_BUS_WIRED_TO_MSI, //TODO: Check
		.flags			= MSI_FLAG_USE_DEV_FWNODE,
		.handler		= handle_fasteoi_irq,
		.handler_name		= "fasteoi",
	},
};

static int gtmb_parse_package_resource(struct platform_device *pdev, struct gtmb_data *data)
{
	struct device *dev = &pdev->dev;
	int result = -EFAULT;
	acpi_status status = AE_OK;
	struct acpi_buffer buffer = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *gtmb_obj = NULL, *tmp_obj, *gsi_num_obj, *gtmb_reg_obj;
	struct gtmb_dev_res *res;
	struct gtmb_reg *gas_t;
	int i,j;

	struct acpi_handle *handle = ACPI_HANDLE_FWNODE(dev->fwnode);
	status = acpi_evaluate_object_typed(handle, "_GMA", NULL,
					    &buffer, ACPI_TYPE_PACKAGE);
	if (status == AE_NOT_FOUND)
		return 0;
	if (ACPI_FAILURE(status))
		return -ENODEV;

	gtmb_obj = buffer.pointer;
	if (!gtmb_obj) {
		pr_err("Invalid GTMB data\n");
		goto end;
	}

	/* Validate the version */
	if (gtmb_obj->package.elements[0].integer.value != ACPI_GTMB_VERSION){
		pr_err("Invalid GTMB Version\n");
		goto end;
	}

	for (i = 1; i < gtmb_obj->package.count; i++) {
		tmp_obj = &gtmb_obj->package.elements[i];
		if (tmp_obj->type != ACPI_TYPE_PACKAGE) {
			pr_err("Unsupported _GMA object found at %i index of type %d\n", i, tmp_obj->type);
			goto end;
		}
		if(tmp_obj->package.count != (GTMB_MSI_INDEX_MAX + 1)) {
			pr_err("Unsupported _GMA object found at %i with package elements %d\n",
				i, tmp_obj->package.count);
			goto end;
		}

		res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);
		if (!res)
			goto end_nomem;

		gsi_num_obj = &tmp_obj->package.elements[0];
		if (gsi_num_obj->type != ACPI_TYPE_INTEGER) {
			pr_err("Unsupported _GMA object found at %i with invalid first element type %d\n",
				i, gsi_num_obj->type);
			goto end;
		}

		res->gsino = gsi_num_obj->integer.value;
		for(j = 0; j < GTMB_MSI_INDEX_MAX; j++) {
			gtmb_reg_obj = &tmp_obj->package.elements[j+1];
			if (gtmb_reg_obj->type == ACPI_TYPE_INTEGER) {
				if ((j == GTMB_MSI_RETRIGGER_REG_INDEX) ||
				    (j >= GTMB_MSI_TRIG_TYPE_EDGE_RISING_INDEX &&
				    j <= GTMB_MSI_TRIG_TYPE_LEVEL_LOW_INDEX)){
				    	res->regs[j].type = ACPI_TYPE_INTEGER;
					res->regs[j].int_val = gtmb_reg_obj->integer.value;
				    } else {
					pr_err("Invalid _GMA object at %i type %d\n",
						j, gtmb_reg_obj->type);
					goto free_list;
				    }
			}
			else if (gtmb_reg_obj->type == ACPI_TYPE_BUFFER) {
				gas_t = (struct gtmb_reg *)gtmb_reg_obj->buffer.pointer;
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
			} else {
				pr_err("Unsupported _GMA object found at %i with invalid element type %d\n",
					i, gtmb_reg_obj->type);
				goto free_list;
			}
		}
		pr_info("Registered GSI %d\n", res->gsino);
		list_add(&res->list, &data->dev_res_list);
	}
	kfree(buffer.pointer);
	return 0;

end_nomem:
	result = -ENOMEM;
free_list:
	gtmb_free_devres_list(data);
end:
	kfree(buffer.pointer);
	return result;
}

static acpi_status gtmb_parse_gsi_base_range(acpi_handle handle,
					     u64 *gbase, u64 *num_gsis)
{
	acpi_status status;

	if (!acpi_has_method(handle, "_GMA")) {
		acpi_handle_err(handle, "_GMA method not found\n");
		return AE_ERROR;
	}

	if (!acpi_has_method(handle, "_GSB")) {
		acpi_handle_err(handle, "_GSB method not found\n");
		return AE_ERROR;
	}

	status = acpi_evaluate_integer(handle, "_GSB", NULL, gbase);
	if (ACPI_FAILURE(status)) {
		acpi_handle_err(handle, "failed to evaluate _GSB method\n");
		return status;
	}

	status = acpi_evaluate_integer(handle, "_NGI", NULL, num_gsis);
	if (ACPI_FAILURE(status) || num_gsis <= 0) {
		acpi_handle_err(handle, "Number of GSIs is not valid\n");
		return status;
	}

	return AE_OK;
}

static int gtmb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc = 0;
	acpi_status status;
	const struct imsic_global_config *imsic_global;
	struct irq_domain *msi_domain;
	struct gtmb_data *data;

	/* GTMB device is only valid for ACPI supported platforms */
	if (acpi_disabled)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	data->acpi_dev_handle = ACPI_HANDLE_FWNODE(dev->fwnode);
	INIT_LIST_HEAD(&data->dev_res_list);

	status = gtmb_parse_gsi_base_range(data->acpi_dev_handle,
				(u64 *)&data->gsi_base, (u64 *)&data->nr_irqs);
	if (status != AE_OK) {
		dev_err(dev, "failed to find GSI mapping\n");
		return rc;
	}

	pr_info("GTMB device found with GSI base [%d] for [%d] GSIs\n",data->gsi_base,data->nr_irqs);
	/* TODO: Do we need to validate that irqchip is registered for fwnode */
	gtmb_parse_package_resource(pdev, data);
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

	if (!msi_create_device_irq_domain(dev, MSI_DEFAULT_DOMAIN, &gtmb_msi_template,
						data->nr_irqs + 1, data, data)) {
		dev_err(dev, "failed to create MSI irq domain\n");
		return -ENOMEM;
	}

	acpi_dev_clear_dependencies(ACPI_COMPANION(dev));

	return rc;
}

static const struct acpi_device_id gtmb_acpi_match[] = {
	{ "ACPI0019", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, gtmb_acpi_match);

static struct platform_driver gtmb_driver = {
	.driver = {
		.name		= "riscv-gtmb",
		.acpi_match_table = ACPI_PTR(gtmb_acpi_match),
	},
	.probe = gtmb_probe,
};
builtin_platform_driver(gtmb_driver);