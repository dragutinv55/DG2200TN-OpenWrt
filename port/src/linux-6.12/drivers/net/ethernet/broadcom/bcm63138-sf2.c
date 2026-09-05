// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom BCM63138 SF2 switch read-only discovery driver
 */

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_mdio.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/platform_device.h>

#define SF2_PORT_CONTROL			0x00000
#define SF2_SWITCH_MODE				0x0000b
#define SF2_MII_PORT_STATE_OVERRIDE		0x0000e
#define SF2_PORT_FORWARD			0x00021
#define SF2_SWITCH_CTRL				0x00022
#define SF2_SWITCH_CTRL_MII_DUMP_FORWARDING_EN	BIT(6)
#define SF2_PORT_STATE				0x00058
#define SF2_SWITCH_CONTROL			0x40000
#define SF2_SWITCH_CONTROL_MDIO_MASTER		BIT(0)
#define SF2_CROSSBAR_SWITCH_CONTROL		0x400ac
#define SF2_CROSSBAR_PORT_MASK			GENMASK(2, 0)
#define SF2_CROSSBAR_PORT_SHIFT			3
#define SF2_CROSSBAR_WAN_INTERNAL_PORT		2
#define SF2_CROSSBAR_GPHY4_EXTERNAL_PORT		1
#define SF2_QUAD_PHY_CONTROL			0x40024
#define SF2_SINGLE_PHY_CONTROL			0x4002c
#define SF2_QUAD_PHY_ADDRESS_SHIFT		12
#define SF2_QUAD_PHY_ADDRESS			8
#define SF2_QUAD_PHY_RESET			BIT(8)
#define SF2_SINGLE_PHY_ADDRESS_SHIFT		8
#define SF2_SINGLE_PHY_ADDRESS			12
#define SF2_SINGLE_PHY_RESET			BIT(5)
#define SF2_MDIO_COMMAND			0x403c0
#define SF2_PORT_VLAN_CONTROL			(0x03100 << 2)
#define SF2_PORT_VLAN_STRIDE			0x08
#define SF2_CFE_PORT_VLAN_MAP			0x01ff
#define SF2_CFE_SWITCH_MODE			0x06
#define SF2_CFE_MII_OVERRIDE			0xbb
#define SF2_CFE_PORT_FORWARD			0x01
#define SF2_CPU_PORT				8
#define SF2_MDIO_BUSY				BIT(29)
#define SF2_MDIO_FAIL				BIT(28)
#define SF2_MDIO_C22_WRITE			(1 << 26)
#define SF2_MDIO_C22_READ			(2 << 26)
#define SF2_MDIO_PHY_SHIFT			21
#define SF2_MDIO_REG_SHIFT			16
#define SF2_PORT_COUNT				9

struct bcm63138_sf2 {
	void __iomem *base;
	struct mutex mdio_lock;
	struct mii_bus *mii_bus;
	bool cpu_port_configured;
	u8 saved_switch_mode;
	u8 saved_mii_override;
	u8 saved_port_forward;
	u8 saved_switch_control;
	u8 saved_port_control[4];
	u32 saved_port_vlan[4];
	u32 saved_cpu_port_vlan;
};

static int bcm63138_sf2_mdio_read(struct bcm63138_sf2 *sf2,
				  unsigned int phy, unsigned int reg);
static int bcm63138_sf2_mdio_write(struct bcm63138_sf2 *sf2,
				   unsigned int phy, unsigned int reg,
				   u16 data);

static u8 bcm63138_sf2_read8(struct bcm63138_sf2 *sf2,
			     unsigned int page, unsigned int reg)
{
	unsigned int offset = ((page << 8) + reg) << 2;

	return ioread32(sf2->base + offset);
}

static void bcm63138_sf2_write8(struct bcm63138_sf2 *sf2,
				unsigned int page, unsigned int reg, u8 value)
{
	unsigned int offset = ((page << 8) + reg) << 2;

	iowrite32(value, sf2->base + offset);
}

static void bcm63138_sf2_configure_wan_crossbar(struct bcm63138_sf2 *sf2)
{
	u32 shift = SF2_CROSSBAR_WAN_INTERNAL_PORT *
		    SF2_CROSSBAR_PORT_SHIFT;
	u32 value;

	value = ioread32(sf2->base + SF2_CROSSBAR_SWITCH_CONTROL);
	value &= ~(SF2_CROSSBAR_PORT_MASK << shift);
	value |= SF2_CROSSBAR_GPHY4_EXTERNAL_PORT << shift;
	iowrite32(value, sf2->base + SF2_CROSSBAR_SWITCH_CONTROL);
}

static void bcm63138_sf2_reset_quad_phy(struct bcm63138_sf2 *sf2)
{
	u32 qphy = SF2_QUAD_PHY_ADDRESS << SF2_QUAD_PHY_ADDRESS_SHIFT;
	u32 sphy = SF2_SINGLE_PHY_ADDRESS << SF2_SINGLE_PHY_ADDRESS_SHIFT;

	iowrite32(qphy | SF2_QUAD_PHY_RESET,
		  sf2->base + SF2_QUAD_PHY_CONTROL);
	iowrite32(sphy | SF2_SINGLE_PHY_RESET,
		  sf2->base + SF2_SINGLE_PHY_CONTROL);
	udelay(1000);
	iowrite32(qphy, sf2->base + SF2_QUAD_PHY_CONTROL);
	iowrite32(sphy, sf2->base + SF2_SINGLE_PHY_CONTROL);
	udelay(1000);
}

static void bcm63138_sf2_phy_write_misc(struct bcm63138_sf2 *sf2,
					unsigned int phy, unsigned int reg,
					unsigned int channel, u16 value)
{
	u16 selector;

	bcm63138_sf2_mdio_write(sf2, phy, 0x18, 0x0007);
	selector = bcm63138_sf2_mdio_read(sf2, phy, 0x18);
	bcm63138_sf2_mdio_write(sf2, phy, 0x18, selector | 0x0800);
	bcm63138_sf2_mdio_write(sf2, phy, 0x17,
				0x0800 | (channel << 13) | reg);
	bcm63138_sf2_mdio_write(sf2, phy, 0x15, value);
}

static void bcm63138_sf2_phy_write_exp(struct bcm63138_sf2 *sf2,
				       unsigned int phy, unsigned int reg,
				       u16 value)
{
	bcm63138_sf2_mdio_write(sf2, phy, 0x17, reg | 0x0f00);
	bcm63138_sf2_mdio_write(sf2, phy, 0x15, value);
}

static void bcm63138_sf2_adjust_afe(struct bcm63138_sf2 *sf2,
				    unsigned int base, unsigned int count)
{
	unsigned int phy;

	for (phy = base; phy < base + count; phy++) {
		bcm63138_sf2_mdio_write(sf2, phy, 0, 0x9140);
		udelay(100);
		bcm63138_sf2_phy_write_misc(sf2, phy, 0x38, 1, 0x9b2f);
		bcm63138_sf2_phy_write_misc(sf2, phy, 0x39, 0, 0x0431);
		bcm63138_sf2_phy_write_misc(sf2, phy, 0x39, 1, 0xa7da);
		bcm63138_sf2_phy_write_misc(sf2, phy, 0x3a, 0, 0x00e3);
	}

	bcm63138_sf2_mdio_write(sf2, base, 0x1e, 0x0010);
	for (phy = base; phy < base + count; phy++)
		bcm63138_sf2_phy_write_misc(sf2, phy, 0x0a, 0, 0x011b);
	bcm63138_sf2_phy_write_exp(sf2, base, 0xb0, 0x0010);
	bcm63138_sf2_phy_write_exp(sf2, base, 0xb0, 0x0000);
}

static void bcm63138_sf2_fixup_phys(struct bcm63138_sf2 *sf2)
{
	unsigned int phy;
	int value;

	bcm63138_sf2_mdio_read(sf2, SF2_QUAD_PHY_ADDRESS, 2);
	bcm63138_sf2_adjust_afe(sf2, SF2_QUAD_PHY_ADDRESS, 4);
	bcm63138_sf2_adjust_afe(sf2, SF2_SINGLE_PHY_ADDRESS, 1);

	for (phy = SF2_QUAD_PHY_ADDRESS; phy <= SF2_SINGLE_PHY_ADDRESS;
	     phy++) {
		value = bcm63138_sf2_mdio_read(sf2, phy, 9);
		if (value >= 0)
			bcm63138_sf2_mdio_write(sf2, phy, 9,
					       value | BIT(10));
	}
}

static ssize_t switch_state_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct bcm63138_sf2 *sf2 = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned int port;

	len += sysfs_emit_at(buf, len,
			     "switch_mode=0x%02x mii_control=0x%02x mii_override=0x%02x forward=0x%02x\n",
			     bcm63138_sf2_read8(sf2, 0, SF2_SWITCH_MODE),
			     bcm63138_sf2_read8(sf2, 0, 0x08),
			     bcm63138_sf2_read8(
				     sf2, 0, SF2_MII_PORT_STATE_OVERRIDE),
			     bcm63138_sf2_read8(sf2, 0, SF2_PORT_FORWARD));

	for (port = 0; port < SF2_PORT_COUNT; port++)
		len += sysfs_emit_at(buf, len,
				     "port%u_ctrl=0x%02x state=0x%02x pbvlan=0x%03x\n",
				     port,
				     bcm63138_sf2_read8(
					     sf2, 0,
					     SF2_PORT_CONTROL + port),
				     bcm63138_sf2_read8(
					     sf2, 0,
					     SF2_PORT_STATE + port),
				     ioread32(
					     sf2->base +
					     SF2_PORT_VLAN_CONTROL +
					     port * SF2_PORT_VLAN_STRIDE));

	len += sysfs_emit_at(buf, len, "management_global=0x%02x\n",
			     bcm63138_sf2_read8(sf2, 2, 0));

	len += sysfs_emit_at(buf, len, "mdio_command=0x%08x\n",
			     ioread32(sf2->base + SF2_MDIO_COMMAND));
	len += sysfs_emit_at(buf, len, "switch_control=0x%08x\n",
			     ioread32(sf2->base + SF2_SWITCH_CONTROL));
	len += sysfs_emit_at(buf, len, "crossbar_control=0x%08x\n",
			     ioread32(sf2->base +
				      SF2_CROSSBAR_SWITCH_CONTROL));
	len += sysfs_emit_at(buf, len, "quad_phy_control=0x%08x\n",
			     ioread32(sf2->base + SF2_QUAD_PHY_CONTROL));
	len += sysfs_emit_at(buf, len, "cpu_port_configured=%u\n",
			     sf2->cpu_port_configured);

	return len;
}
static DEVICE_ATTR_RO(switch_state);

static int bcm63138_sf2_mdio_read(struct bcm63138_sf2 *sf2,
				  unsigned int phy, unsigned int reg)
{
	u32 command;
	u32 value;
	int ret;

	command = SF2_MDIO_C22_READ |
		  (phy << SF2_MDIO_PHY_SHIFT) |
		  (reg << SF2_MDIO_REG_SHIFT);

	iowrite32(command | SF2_MDIO_BUSY,
		  sf2->base + SF2_MDIO_COMMAND);
	ret = readl_poll_timeout(sf2->base + SF2_MDIO_COMMAND, value,
				 !(value & SF2_MDIO_BUSY), 20, 1000);
	if (ret)
		return ret;
	if (value & SF2_MDIO_FAIL)
		return -EIO;

	return value & 0xffff;
}

static int bcm63138_sf2_mdio_write(struct bcm63138_sf2 *sf2,
				   unsigned int phy, unsigned int reg,
				   u16 data)
{
	u32 command;
	u32 value;
	int ret;

	command = SF2_MDIO_C22_WRITE |
		  (phy << SF2_MDIO_PHY_SHIFT) |
		  (reg << SF2_MDIO_REG_SHIFT) | data;

	iowrite32(command | SF2_MDIO_BUSY,
		  sf2->base + SF2_MDIO_COMMAND);
	ret = readl_poll_timeout(sf2->base + SF2_MDIO_COMMAND, value,
				 !(value & SF2_MDIO_BUSY), 20, 1000);
	if (ret)
		return ret;
	if (value & SF2_MDIO_FAIL)
		return -EIO;

	return 0;
}

static int bcm63138_sf2_mii_read(struct mii_bus *bus, int phy, int reg)
{
	struct bcm63138_sf2 *sf2 = bus->priv;
	u32 switch_control;
	int ret;

	mutex_lock(&sf2->mdio_lock);
	switch_control = ioread32(sf2->base + SF2_SWITCH_CONTROL);
	iowrite32(switch_control & ~SF2_SWITCH_CONTROL_MDIO_MASTER,
		  sf2->base + SF2_SWITCH_CONTROL);
	ret = bcm63138_sf2_mdio_read(sf2, phy, reg);
	iowrite32(switch_control, sf2->base + SF2_SWITCH_CONTROL);
	mutex_unlock(&sf2->mdio_lock);

	return ret;
}

static int bcm63138_sf2_mii_write(struct mii_bus *bus, int phy, int reg,
				  u16 data)
{
	struct bcm63138_sf2 *sf2 = bus->priv;
	u32 switch_control;
	int ret;

	mutex_lock(&sf2->mdio_lock);
	switch_control = ioread32(sf2->base + SF2_SWITCH_CONTROL);
	iowrite32(switch_control & ~SF2_SWITCH_CONTROL_MDIO_MASTER,
		  sf2->base + SF2_SWITCH_CONTROL);
	ret = bcm63138_sf2_mdio_write(sf2, phy, reg, data);
	iowrite32(switch_control, sf2->base + SF2_SWITCH_CONTROL);
	mutex_unlock(&sf2->mdio_lock);

	return ret;
}

static ssize_t phy_state_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct bcm63138_sf2 *sf2 = dev_get_drvdata(dev);
	u32 switch_control;
	ssize_t len = 0;
	unsigned int phy;
	int regs[10];
	int reg;

	mutex_lock(&sf2->mdio_lock);

	switch_control = ioread32(sf2->base + SF2_SWITCH_CONTROL);
	iowrite32(switch_control & ~SF2_SWITCH_CONTROL_MDIO_MASTER,
		  sf2->base + SF2_SWITCH_CONTROL);

	for (phy = 8; phy <= 12; phy++) {
		for (reg = 0; reg < ARRAY_SIZE(regs); reg++)
			regs[reg] = bcm63138_sf2_mdio_read(sf2, phy, reg);

		len += sysfs_emit_at(buf, len,
				     "phy%u bmcr=%#06x bmsr=%#06x id1=%#06x id2=%#06x adv=%#06x ctrl1000=%#06x\n",
				     phy, regs[0], regs[1], regs[2], regs[3],
				     regs[4], regs[9]);
	}

	iowrite32(switch_control, sf2->base + SF2_SWITCH_CONTROL);
	mutex_unlock(&sf2->mdio_lock);

	return len;
}
static DEVICE_ATTR_RO(phy_state);

static ssize_t cpu_port_setup_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct bcm63138_sf2 *sf2 = dev_get_drvdata(dev);
	bool enable;
	unsigned int port;
	int err;

	err = kstrtobool(buf, &enable);
	if (err)
		return err;

	mutex_lock(&sf2->mdio_lock);
	if (enable) {
		if (sf2->cpu_port_configured) {
			err = -EALREADY;
			goto out_unlock;
		}

		sf2->saved_switch_mode = bcm63138_sf2_read8(
			sf2, 0, SF2_SWITCH_MODE);
		sf2->saved_mii_override = bcm63138_sf2_read8(
			sf2, 0, SF2_MII_PORT_STATE_OVERRIDE);
		sf2->saved_port_forward = bcm63138_sf2_read8(
			sf2, 0, SF2_PORT_FORWARD);
		sf2->saved_switch_control = bcm63138_sf2_read8(
			sf2, 0, SF2_SWITCH_CTRL);
		bcm63138_sf2_write8(sf2, 0, SF2_SWITCH_MODE,
				   SF2_CFE_SWITCH_MODE);
		bcm63138_sf2_write8(sf2, 0, SF2_MII_PORT_STATE_OVERRIDE,
				   SF2_CFE_MII_OVERRIDE);
		bcm63138_sf2_write8(sf2, 0, SF2_PORT_FORWARD,
				   SF2_CFE_PORT_FORWARD);
		bcm63138_sf2_write8(
			sf2, 0, SF2_SWITCH_CTRL,
			sf2->saved_switch_control |
				SF2_SWITCH_CTRL_MII_DUMP_FORWARDING_EN);

		for (port = 0; port < ARRAY_SIZE(sf2->saved_port_vlan);
		     port++) {
			sf2->saved_port_control[port] =
				bcm63138_sf2_read8(
					sf2, 0, SF2_PORT_CONTROL + port);
			sf2->saved_port_vlan[port] = ioread32(
				sf2->base + SF2_PORT_VLAN_CONTROL +
				port * SF2_PORT_VLAN_STRIDE);

			bcm63138_sf2_write8(
				sf2, 0, SF2_PORT_CONTROL + port,
				sf2->saved_port_control[port] & ~0x23);
			iowrite32(SF2_CFE_PORT_VLAN_MAP,
				  sf2->base + SF2_PORT_VLAN_CONTROL +
				  port * SF2_PORT_VLAN_STRIDE);
		}
		sf2->saved_cpu_port_vlan = ioread32(
			sf2->base + SF2_PORT_VLAN_CONTROL +
			SF2_CPU_PORT * SF2_PORT_VLAN_STRIDE);
		iowrite32(SF2_CFE_PORT_VLAN_MAP,
			  sf2->base + SF2_PORT_VLAN_CONTROL +
			  SF2_CPU_PORT * SF2_PORT_VLAN_STRIDE);
		sf2->cpu_port_configured = true;
		dev_info(dev,
			 "SF2 LAN ports configured with the CFE PBVLAN fan-out\n");
	} else {
		if (!sf2->cpu_port_configured) {
			err = -EALREADY;
			goto out_unlock;
		}

		for (port = 0; port < ARRAY_SIZE(sf2->saved_port_vlan);
		     port++) {
			iowrite32(sf2->saved_port_vlan[port],
				  sf2->base + SF2_PORT_VLAN_CONTROL +
				  port * SF2_PORT_VLAN_STRIDE);
			bcm63138_sf2_write8(
				sf2, 0, SF2_PORT_CONTROL + port,
				sf2->saved_port_control[port]);
		}
		iowrite32(sf2->saved_cpu_port_vlan,
			  sf2->base + SF2_PORT_VLAN_CONTROL +
			  SF2_CPU_PORT * SF2_PORT_VLAN_STRIDE);
		bcm63138_sf2_write8(sf2, 0, SF2_SWITCH_MODE,
				   sf2->saved_switch_mode);
		bcm63138_sf2_write8(sf2, 0, SF2_MII_PORT_STATE_OVERRIDE,
				   sf2->saved_mii_override);
		bcm63138_sf2_write8(sf2, 0, SF2_PORT_FORWARD,
				   sf2->saved_port_forward);
		bcm63138_sf2_write8(sf2, 0, SF2_SWITCH_CTRL,
				   sf2->saved_switch_control);
		sf2->cpu_port_configured = false;
		dev_info(dev, "SF2 CPU port configuration restored\n");
	}

	err = count;

out_unlock:
	mutex_unlock(&sf2->mdio_lock);

	return err;
}
static DEVICE_ATTR_WO(cpu_port_setup);

static struct attribute *bcm63138_sf2_attrs[] = {
	&dev_attr_cpu_port_setup.attr,
	&dev_attr_switch_state.attr,
	&dev_attr_phy_state.attr,
	NULL,
};

static const struct attribute_group bcm63138_sf2_group = {
	.attrs = bcm63138_sf2_attrs,
};

static int bcm63138_sf2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bcm63138_sf2 *sf2;
	int err;

	sf2 = devm_kzalloc(dev, sizeof(*sf2), GFP_KERNEL);
	if (!sf2)
		return -ENOMEM;

	sf2->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sf2->base))
		return PTR_ERR(sf2->base);

	mutex_init(&sf2->mdio_lock);
	platform_set_drvdata(pdev, sf2);

	bcm63138_sf2_configure_wan_crossbar(sf2);
	dev_info(dev, "crossbar connected Runner WAN to GPHY4/PHY 12\n");

	if (of_property_read_bool(dev->of_node, "brcm,preserve-cfe-state")) {
		dev_info(dev, "preserving CFE switch and GPHY state\n");
	} else {
		bcm63138_sf2_reset_quad_phy(sf2);
		bcm63138_sf2_fixup_phys(sf2);
		dev_info(dev,
			 "GPHY blocks reset; BCM63138 AFE calibration applied\n");

	}

	sf2->mii_bus = devm_mdiobus_alloc(dev);
	if (!sf2->mii_bus)
		return -ENOMEM;
	sf2->mii_bus->name = "BCM63138 SF2 MDIO";
	sf2->mii_bus->read = bcm63138_sf2_mii_read;
	sf2->mii_bus->write = bcm63138_sf2_mii_write;
	sf2->mii_bus->priv = sf2;
	sf2->mii_bus->parent = dev;
	snprintf(sf2->mii_bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));

	err = devm_of_mdiobus_register(dev, sf2->mii_bus, dev->of_node);
	if (err)
		return dev_err_probe(dev, err,
				     "failed to register MDIO bus\n");

	err = devm_device_add_group(dev, &bcm63138_sf2_group);
	if (err)
		return dev_err_probe(dev, err,
				     "failed to create switch state attribute\n");

	if (!of_property_read_bool(dev->of_node, "brcm,preserve-cfe-state") &&
	    cpu_port_setup_store(dev, NULL, "1", 1) < 0)
		return dev_err_probe(dev, -EIO,
				     "failed to configure SF2 LAN ports\n");

	return 0;
}

static const struct of_device_id bcm63138_sf2_of_match[] = {
	{ .compatible = "brcm,bcm63138-sf2-discovery" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcm63138_sf2_of_match);

static struct platform_driver bcm63138_sf2_driver = {
	.probe = bcm63138_sf2_probe,
	.driver = {
		.name = "bcm63138-sf2",
		.of_match_table = bcm63138_sf2_of_match,
	},
};
module_platform_driver(bcm63138_sf2_driver);

MODULE_DESCRIPTION("Broadcom BCM63138 SF2 discovery driver");
MODULE_LICENSE("GPL");
