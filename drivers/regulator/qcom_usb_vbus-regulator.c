// SPDX-License-Identifier: GPL-2.0-only
//
// Qualcomm PMIC VBUS output regulator driver
//
// Copyright (c) 2020, The Linux Foundation. All rights reserved.

#include <linux/module.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regmap.h>

#define OTG_EN				BIT(0)
#define OTG_CURRENT_LIMIT_MASK		GENMASK(2, 0)
#define OTG_EN_SRC_CFG			BIT(1)

/*
 * The OTG booster's registers in each PMIC's DC-DC block: PM8550B's at 0x50, 0x53 and 0x56
 * (the 0 bit in its 0x56 is undocumented), PM8150B's at 0x40, 0x52 and 0x53 with 0.5 A steps
 * up to 3 A (the stock smb5's DCDC_CMD_OTG, DCDC_OTG_CURRENT_LIMIT_CFG and DCDC_OTG_CFG).
 */
struct qcom_usb_vbus_data {
	unsigned int cmd_otg;
	unsigned int otg_current_limit_cfg;
	unsigned int otg_cfg;
	const unsigned int *curr_table;
	unsigned int n_current_limits;
};

static const unsigned int pm8550b_curr_table[] = {
	1000000, 1250000, 1500000, 1750000, 2000000, 2500000, 3000000, 3500000
};

static const unsigned int pm8150b_curr_table[] = {
	500000, 1000000, 1500000, 2000000, 2500000, 3000000
};

static const struct qcom_usb_vbus_data pm8550b_data = {
	.cmd_otg = 0x50,
	.otg_current_limit_cfg = 0x53,
	.otg_cfg = 0x56,
	.curr_table = pm8550b_curr_table,
	.n_current_limits = ARRAY_SIZE(pm8550b_curr_table),
};

static const struct qcom_usb_vbus_data pm8150b_data = {
	.cmd_otg = 0x40,
	.otg_current_limit_cfg = 0x52,
	.otg_cfg = 0x53,
	.curr_table = pm8150b_curr_table,
	.n_current_limits = ARRAY_SIZE(pm8150b_curr_table),
};

static const struct regulator_ops qcom_usb_vbus_reg_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_current_limit = regulator_get_current_limit_regmap,
	.set_current_limit = regulator_set_current_limit_regmap,
};

static int qcom_usb_vbus_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct qcom_usb_vbus_data *data;
	struct regulator_desc *rdesc;
	struct regulator_dev *rdev;
	struct regmap *regmap;
	struct regulator_config config = { };
	struct regulator_init_data *init_data;
	int ret;
	u32 base;

	data = of_device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	ret = of_property_read_u32(dev->of_node, "reg", &base);
	if (ret < 0) {
		dev_err(dev, "no base address found\n");
		return ret;
	}

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap) {
		dev_err(dev, "Failed to get regmap\n");
		return -ENOENT;
	}

	rdesc = devm_kzalloc(dev, sizeof(*rdesc), GFP_KERNEL);
	if (!rdesc)
		return -ENOMEM;
	rdesc->name = "usb_vbus";
	rdesc->ops = &qcom_usb_vbus_reg_ops;
	rdesc->owner = THIS_MODULE;
	rdesc->type = REGULATOR_VOLTAGE;
	rdesc->curr_table = data->curr_table;
	rdesc->n_current_limits = data->n_current_limits;
	rdesc->enable_reg = base + data->cmd_otg;
	rdesc->enable_mask = OTG_EN;
	rdesc->csel_reg = base + data->otg_current_limit_cfg;
	rdesc->csel_mask = OTG_CURRENT_LIMIT_MASK;

	init_data = of_get_regulator_init_data(dev, dev->of_node, rdesc);
	if (!init_data)
		return -ENOMEM;

	config.dev = dev;
	config.init_data = init_data;
	config.of_node = dev->of_node;
	config.regmap = regmap;

	rdev = devm_regulator_register(dev, rdesc, &config);
	if (IS_ERR(rdev)) {
		ret = PTR_ERR(rdev);
		dev_err(dev, "not able to register vbus reg %d\n", ret);
		return ret;
	}

	/* Disable HW logic for VBUS enable */
	regmap_update_bits(regmap, base + data->otg_cfg, OTG_EN_SRC_CFG, 0);

	return 0;
}

static const struct of_device_id qcom_usb_vbus_regulator_match[] = {
	{ .compatible = "qcom,pm8150b-vbus-reg", .data = &pm8150b_data },
	{ .compatible = "qcom,pm8550b-vbus-reg", .data = &pm8550b_data },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_usb_vbus_regulator_match);

static struct platform_driver qcom_usb_vbus_regulator_driver = {
	.driver		= {
		.name	= "qcom-usb-vbus-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = qcom_usb_vbus_regulator_match,
	},
	.probe		= qcom_usb_vbus_regulator_probe,
};
module_platform_driver(qcom_usb_vbus_regulator_driver);

MODULE_DESCRIPTION("Qualcomm USB vbus regulator driver");
MODULE_LICENSE("GPL v2");
