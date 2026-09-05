// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2013 Broadcom
 * Copyright (C) 2020 Rafał Miłecki <rafal@milecki.pl>
 */

#include <dt-bindings/soc/bcm-pmb.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/reset/bcm63xx_pmb.h>

#define BPCM_ID_REG					0x00
#define BPCM_CAPABILITIES				0x04
#define  BPCM_CAP_NUM_ZONES				0x000000ff
#define  BPCM_CAP_SR_REG_BITS				0x0000ff00
#define  BPCM_CAP_PLLTYPE				0x00030000
#define  BPCM_CAP_UBUS					0x00080000
#define BPCM_CONTROL					0x08
#define BPCM_STATUS					0x0c
#define BPCM_ROSC_CONTROL				0x10
#define BPCM_ROSC_THRESH_H				0x14
#define BPCM_ROSC_THRESHOLD_BCM6838			0x14
#define BPCM_ROSC_THRESH_S				0x18
#define BPCM_ROSC_COUNT_BCM6838				0x18
#define BPCM_ROSC_COUNT					0x1c
#define BPCM_PWD_CONTROL_BCM6838			0x1c
#define BPCM_PWD_CONTROL				0x20
#define BPCM_SR_CONTROL_BCM6838				0x20
#define BPCM_PWD_ACCUM_CONTROL				0x24
#define BPCM_SR_CONTROL					0x28
#define BPCM_GLOBAL_CONTROL				0x2c
#define  BPCM_GLOBAL_CONTROL_RGMII_TX_CLK_ZONE1		BIT(18)
#define  BPCM_GLOBAL_CONTROL_RGMII_TX_CLK_ZONE2		BIT(22)
#define BPCM_MISC_CONTROL				0x30
#define BPCM_MISC_CONTROL2				0x34
#define BPCM_SGPHY_CNTL					0x38
#define BPCM_SGPHY_STATUS				0x3c
#define PLLBPCM_RESET					0x10
#define  PLLBPCM_RESET_RESETB				BIT(0)
#define  PLLBPCM_RESET_POST_RESETB			BIT(1)
#define PLLBPCM_STATUS					0x3c
#define  PLLBPCM_STATUS_LOCKED				BIT(31)
#define BPCM_ZONE0					0x40
#define  BPCM_ZONE_CONTROL				0x00
#define   BPCM_ZONE_CONTROL_MANUAL_CLK_EN		0x00000001
#define   BPCM_ZONE_CONTROL_MANUAL_RESET_CTL		0x00000002
#define   BPCM_ZONE_CONTROL_FREQ_SCALE_USED		0x00000004	/* R/O */
#define   BPCM_ZONE_CONTROL_DPG_CAPABLE			0x00000008	/* R/O */
#define   BPCM_ZONE_CONTROL_MANUAL_MEM_PWR		0x00000030
#define   BPCM_ZONE_CONTROL_MANUAL_ISO_CTL		0x00000040
#define   BPCM_ZONE_CONTROL_MANUAL_CTL			0x00000080
#define   BPCM_ZONE_CONTROL_DPG_CTL_EN			0x00000100
#define   BPCM_ZONE_CONTROL_PWR_DN_REQ			0x00000200
#define   BPCM_ZONE_CONTROL_PWR_UP_REQ			0x00000400
#define   BPCM_ZONE_CONTROL_MEM_PWR_CTL_EN		0x00000800
#define   BPCM_ZONE_CONTROL_BLK_RESET_ASSERT		0x00001000
#define   BPCM_ZONE_CONTROL_MEM_STBY			0x00002000
#define   BPCM_ZONE_CONTROL_RESERVED			0x0007c000
#define   BPCM_ZONE_CONTROL_PWR_CNTL_STATE		0x00f80000
#define   BPCM_ZONE_CONTROL_FREQ_SCALAR_DYN_SEL		0x01000000	/* R/O */
#define   BPCM_ZONE_CONTROL_PWR_OFF_STATE		0x02000000	/* R/O */
#define   BPCM_ZONE_CONTROL_PWR_ON_STATE		0x04000000	/* R/O */
#define   BPCM_ZONE_CONTROL_PWR_GOOD			0x08000000	/* R/O */
#define   BPCM_ZONE_CONTROL_DPG_PWR_STATE		0x10000000	/* R/O */
#define   BPCM_ZONE_CONTROL_MEM_PWR_STATE		0x20000000	/* R/O */
#define   BPCM_ZONE_CONTROL_ISO_STATE			0x40000000	/* R/O */
#define   BPCM_ZONE_CONTROL_RESET_STATE			0x80000000	/* R/O */
#define  BPCM_ZONE_CONFIG1				0x04
#define  BPCM_ZONE_CONFIG2				0x08
#define  BPCM_ZONE_FREQ_SCALAR_CONTROL			0x0c
#define  BPCM_ZONE_SIZE					0x10

struct bcm_pmb {
	struct device *dev;
	void __iomem *base;
	spinlock_t lock;
	bool little_endian;
	struct genpd_onecell_data genpd_onecell_data;
};

struct bcm_pmb_pd_data {
	const char * const name;
	int id;
	u8 bus;
	u8 device;
	unsigned int flags;
};

struct bcm_pmb_pm_domain {
	struct bcm_pmb *pmb;
	const struct bcm_pmb_pd_data *data;
	struct generic_pm_domain genpd;
};

static int bcm_pmb_bpcm_read(struct bcm_pmb *pmb, int bus, u8 device,
			     int offset, u32 *val)
{
	void __iomem *base = pmb->base + bus * 0x20;
	unsigned long flags;
	int err;

	spin_lock_irqsave(&pmb->lock, flags);
	err = bpcm_rd(base, device, offset, val);
	spin_unlock_irqrestore(&pmb->lock, flags);

	if (!err)
		*val = pmb->little_endian ? le32_to_cpu(*val) : be32_to_cpu(*val);

	return err;
}

static int bcm_pmb_bpcm_write(struct bcm_pmb *pmb, int bus, u8 device,
			      int offset, u32 val)
{
	void __iomem *base = pmb->base + bus * 0x20;
	unsigned long flags;
	int err;

	val = pmb->little_endian ? cpu_to_le32(val) : cpu_to_be32(val);

	spin_lock_irqsave(&pmb->lock, flags);
	err = bpcm_wr(base, device, offset, val);
	spin_unlock_irqrestore(&pmb->lock, flags);

	return err;
}

static int bcm_pmb_power_off_zone(struct bcm_pmb *pmb, int bus, u8 device,
				  int zone)
{
	int offset;
	u32 val;
	int err;

	offset = BPCM_ZONE0 + zone * BPCM_ZONE_SIZE + BPCM_ZONE_CONTROL;

	err = bcm_pmb_bpcm_read(pmb, bus, device, offset, &val);
	if (err)
		return err;

	val |= BPCM_ZONE_CONTROL_PWR_DN_REQ;
	val &= ~BPCM_ZONE_CONTROL_PWR_UP_REQ;

	err = bcm_pmb_bpcm_write(pmb, bus, device, offset, val);

	return err;
}

static int bcm_pmb_power_on_zone(struct bcm_pmb *pmb, int bus, u8 device,
				 int zone)
{
	int offset;
	u32 val;
	int err = 0;

	offset = BPCM_ZONE0 + zone * BPCM_ZONE_SIZE + BPCM_ZONE_CONTROL;

	err = bcm_pmb_bpcm_read(pmb, bus, device, offset, &val);
	if (err)
		return err;

	if (!(val & BPCM_ZONE_CONTROL_PWR_ON_STATE)) {
		val &= ~BPCM_ZONE_CONTROL_PWR_DN_REQ;
		val |= BPCM_ZONE_CONTROL_DPG_CTL_EN;
		val |= BPCM_ZONE_CONTROL_PWR_UP_REQ;
		val |= BPCM_ZONE_CONTROL_MEM_PWR_CTL_EN;
		val |= BPCM_ZONE_CONTROL_BLK_RESET_ASSERT;

		err = bcm_pmb_bpcm_write(pmb, bus, device, offset, val);
	}

	return err;
}

static int bcm_pmb_power_off_device(struct bcm_pmb *pmb, int bus, u8 device)
{
	int offset;
	u32 val;
	int err;

	/* Entire device can be powered off by powering off the 0th zone */
	offset = BPCM_ZONE0 + BPCM_ZONE_CONTROL;

	err = bcm_pmb_bpcm_read(pmb, bus, device, offset, &val);
	if (err)
		return err;

	if (!(val & BPCM_ZONE_CONTROL_PWR_OFF_STATE)) {
		val |= BPCM_ZONE_CONTROL_PWR_DN_REQ;
		val &= ~BPCM_ZONE_CONTROL_PWR_UP_REQ;

		err = bcm_pmb_bpcm_write(pmb, bus, device, offset, val);
	}

	return err;
}

static int bcm_pmb_power_on_device(struct bcm_pmb *pmb, int bus, u8 device)
{
	u32 val;
	int err;
	int i;

	err = bcm_pmb_bpcm_read(pmb, bus, device, BPCM_CAPABILITIES, &val);
	if (err)
		return err;

	for (i = 0; i < (val & BPCM_CAP_NUM_ZONES); i++) {
		err = bcm_pmb_power_on_zone(pmb, bus, device, i);
		if (err)
			return err;
	}

	return err;
}

static int bcm_pmb_power_on_pcie(struct bcm_pmb *pmb, int bus, u8 device)
{
	int err;

	err = bcm_pmb_power_on_zone(pmb, bus, device, 0);
	if (err)
		return err;

	/* BCM63138 PCIe requires the IDDQ soft reset to be released. */
	msleep(10);

	return bcm_pmb_bpcm_write(pmb, bus, device, BPCM_SR_CONTROL, 0);
}

static int bcm_pmb_power_on_sata(struct bcm_pmb *pmb, int bus, u8 device)
{
	int err;

	err = bcm_pmb_power_on_zone(pmb, bus, device, 0);
	if (err)
		return err;

	/* Does not apply to the BCM963158 */
	err = bcm_pmb_bpcm_write(pmb, bus, device, BPCM_MISC_CONTROL, 0);
	if (err)
		return err;

	err = bcm_pmb_bpcm_write(pmb, bus, device, BPCM_SR_CONTROL, 0xffffffff);
	if (err)
		return err;

	err = bcm_pmb_bpcm_write(pmb, bus, device, BPCM_SR_CONTROL, 0);

	return err;
}

static int bcm_pmb_power_on_rdp(struct bcm_pmb *pmb, int bus, u8 device)
{
	u32 val;
	int err;
	int i;

	/* BCM63138 uses a dedicated PLL at PMB bus 1, device 11. */
	err = bcm_pmb_bpcm_write(pmb, 1, 11, PLLBPCM_RESET, 0);
	if (err)
		return err;

	err = bcm_pmb_bpcm_read(pmb, 1, 11, PLLBPCM_RESET, &val);
	if (err)
		return err;

	err = bcm_pmb_bpcm_write(pmb, 1, 11, PLLBPCM_RESET,
				 val | PLLBPCM_RESET_RESETB);
	if (err)
		return err;

	for (i = 0; i < 1000; i++) {
		err = bcm_pmb_bpcm_read(pmb, 1, 11, PLLBPCM_STATUS, &val);
		if (err)
			return err;
		if (val & PLLBPCM_STATUS_LOCKED)
			break;
		udelay(10);
	}
	if (i == 1000)
		return -ETIMEDOUT;

	err = bcm_pmb_bpcm_read(pmb, 1, 11, PLLBPCM_RESET, &val);
	if (err)
		return err;

	err = bcm_pmb_bpcm_write(pmb, 1, 11, PLLBPCM_RESET,
				 val | PLLBPCM_RESET_POST_RESETB);
	if (err)
		return err;

	err = bcm_pmb_bpcm_write(pmb, bus, device, BPCM_SR_CONTROL, 0);
	if (err)
		return err;

	err = bcm_pmb_power_off_device(pmb, bus, device);
	if (err)
		return err;

	err = bcm_pmb_power_on_device(pmb, bus, device);
	if (err)
		return err;

	return bcm_pmb_bpcm_write(pmb, bus, device, BPCM_SR_CONTROL,
				  0xffffffff);
}

static int bcm_pmb_power_on_switch(struct bcm_pmb *pmb, int bus, u8 device)
{
	u32 val;
	int err;

	err = bcm_pmb_bpcm_read(pmb, bus, device, BPCM_GLOBAL_CONTROL, &val);
	if (err)
		return err;

	val &= ~(BPCM_GLOBAL_CONTROL_RGMII_TX_CLK_ZONE1 |
		 BPCM_GLOBAL_CONTROL_RGMII_TX_CLK_ZONE2);
	err = bcm_pmb_bpcm_write(pmb, bus, device, BPCM_GLOBAL_CONTROL, val);
	if (err)
		return err;

	return bcm_pmb_power_on_device(pmb, bus, device);
}

static int bcm_pmb_power_on(struct generic_pm_domain *genpd)
{
	struct bcm_pmb_pm_domain *pd = container_of(genpd, struct bcm_pmb_pm_domain, genpd);
	const struct bcm_pmb_pd_data *data = pd->data;
	struct bcm_pmb *pmb = pd->pmb;

	switch (data->id) {
	case BCM_PMB_PCIE0:
	case BCM_PMB_PCIE1:
	case BCM_PMB_PCIE2:
		return bcm_pmb_power_on_pcie(pmb, data->bus, data->device);
	case BCM_PMB_HOST_USB:
		return bcm_pmb_power_on_device(pmb, data->bus, data->device);
	case BCM_PMB_SATA:
		return bcm_pmb_power_on_sata(pmb, data->bus, data->device);
	case BCM_PMB_RDP:
		return bcm_pmb_power_on_rdp(pmb, data->bus, data->device);
	case BCM_PMB_SWITCH:
		return bcm_pmb_power_on_switch(pmb, data->bus, data->device);
	default:
		dev_err(pmb->dev, "unsupported device id: %d\n", data->id);
		return -EINVAL;
	}
}

static int bcm_pmb_power_off(struct generic_pm_domain *genpd)
{
	struct bcm_pmb_pm_domain *pd = container_of(genpd, struct bcm_pmb_pm_domain, genpd);
	const struct bcm_pmb_pd_data *data = pd->data;
	struct bcm_pmb *pmb = pd->pmb;

	switch (data->id) {
	case BCM_PMB_PCIE0:
	case BCM_PMB_PCIE1:
	case BCM_PMB_PCIE2:
		return bcm_pmb_power_off_zone(pmb, data->bus, data->device, 0);
	case BCM_PMB_HOST_USB:
		return bcm_pmb_power_off_device(pmb, data->bus, data->device);
	case BCM_PMB_RDP:
	case BCM_PMB_SWITCH:
		return 0;
	default:
		dev_err(pmb->dev, "unsupported device id: %d\n", data->id);
		return -EINVAL;
	}
}

/**
 * bcm_pmb_reset_rdp() - reset an attached BCM PMB RDP domain
 * @consumer: quiesced device attached to the RDP power domain
 *
 * The caller must stop all RDP execution and DMA activity before this reset.
 */
int bcm_pmb_reset_rdp(struct device *consumer)
{
	struct generic_pm_domain *genpd;
	struct bcm_pmb_pm_domain *pd;

	if (!consumer || !consumer->pm_domain)
		return -ENODEV;

	genpd = pd_to_genpd(consumer->pm_domain);
	if (genpd->power_on != bcm_pmb_power_on ||
	    genpd->power_off != bcm_pmb_power_off)
		return -EINVAL;

	pd = container_of(genpd, struct bcm_pmb_pm_domain, genpd);
	if (pd->data->id != BCM_PMB_RDP)
		return -EINVAL;

	return bcm_pmb_power_on_rdp(pd->pmb, pd->data->bus, pd->data->device);
}
EXPORT_SYMBOL_GPL(bcm_pmb_reset_rdp);

static int bcm_pmb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct bcm_pmb_pd_data *table;
	const struct bcm_pmb_pd_data *e;
	struct bcm_pmb *pmb;
	int max_id;
	int err;

	pmb = devm_kzalloc(dev, sizeof(*pmb), GFP_KERNEL);
	if (!pmb)
		return -ENOMEM;

	pmb->dev = dev;

	pmb->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pmb->base))
		return PTR_ERR(pmb->base);

	spin_lock_init(&pmb->lock);

	pmb->little_endian = !of_device_is_big_endian(dev->of_node);

	table = of_device_get_match_data(dev);
	if (!table)
		return -EINVAL;

	max_id = 0;
	for (e = table; e->name; e++)
		max_id = max(max_id, e->id);

	pmb->genpd_onecell_data.num_domains = max_id + 1;
	pmb->genpd_onecell_data.domains =
		devm_kcalloc(dev, pmb->genpd_onecell_data.num_domains,
			     sizeof(struct generic_pm_domain *), GFP_KERNEL);
	if (!pmb->genpd_onecell_data.domains)
		return -ENOMEM;

	for (e = table; e->name; e++) {
		struct bcm_pmb_pm_domain *pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);

		if (!pd)
			return -ENOMEM;

		pd->pmb = pmb;
		pd->data = e;
		pd->genpd.name = e->name;
		pd->genpd.power_on = bcm_pmb_power_on;
		pd->genpd.power_off = bcm_pmb_power_off;

		err = pm_genpd_init(&pd->genpd, NULL, true);
		if (err)
			return dev_err_probe(dev, err,
					     "failed to initialize %s domain\n",
					     e->name);

		/*
		 * The RDP starts off, but must remain powered after its first
		 * consumer attaches. Set ALWAYS_ON only after initializing the
		 * domain in its real initial state.
		 */
		pd->genpd.flags |= e->flags;
		pmb->genpd_onecell_data.domains[e->id] = &pd->genpd;
	}

	err = of_genpd_add_provider_onecell(dev->of_node, &pmb->genpd_onecell_data);
	if (err) {
		dev_err(dev, "failed to add genpd provider: %d\n", err);
		return err;
	}

	return 0;
}

static const struct bcm_pmb_pd_data bcm_pmb_bcm4908_data[] = {
	{ .name = "pcie2", .id = BCM_PMB_PCIE2, .bus = 0, .device = 2, },
	{ .name = "pcie0", .id = BCM_PMB_PCIE0, .bus = 1, .device = 14, },
	{ .name = "pcie1", .id = BCM_PMB_PCIE1, .bus = 1, .device = 15, },
	{ .name = "usb", .id = BCM_PMB_HOST_USB, .bus = 1, .device = 17, },
	{ },
};

static const struct bcm_pmb_pd_data bcm_pmb_bcm63138_data[] = {
	{ .name = "pcie0", .id = BCM_PMB_PCIE0, .bus = 0, .device = 15, },
	{ .name = "pcie1", .id = BCM_PMB_PCIE1, .bus = 0, .device = 16, },
	{ .name = "usb", .id = BCM_PMB_HOST_USB, .bus = 1, .device = 17, },
	{ .name = "sata", .id = BCM_PMB_SATA, .bus = 0, .device = 3, },
	{
		.name = "rdp",
		.id = BCM_PMB_RDP,
		.bus = 1,
		.device = 7,
		.flags = GENPD_FLAG_ALWAYS_ON,
	},
	{
		.name = "switch",
		.id = BCM_PMB_SWITCH,
		.bus = 1,
		.device = 1,
		.flags = GENPD_FLAG_ALWAYS_ON,
	},
	{ },
};

static const struct of_device_id bcm_pmb_of_match[] = {
	{ .compatible = "brcm,bcm4908-pmb", .data = &bcm_pmb_bcm4908_data, },
	{ .compatible = "brcm,bcm63138-pmb", .data = &bcm_pmb_bcm63138_data, },
	{ },
};

static struct platform_driver bcm_pmb_driver = {
	.driver = {
		.name = "bcm-pmb",
		.of_match_table = bcm_pmb_of_match,
	},
	.probe  = bcm_pmb_probe,
};

builtin_platform_driver(bcm_pmb_driver);
