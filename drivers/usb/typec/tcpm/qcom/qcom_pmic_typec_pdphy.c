// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023, Linaro Ltd. All rights reserved.
 */

#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/string_choices.h>
#include <linux/usb/pd.h>
#include <linux/usb/tcpm.h>
#include <linux/workqueue.h>
#include "qcom_pmic_typec_pdphy.h"

#define PM8150B_SAVED_REGS			7	/* pm8150b_saved_regs */

/* smb5's health words for the charger's comparators, and the states of stock's CTM and STD-PD. */
enum { PM8150B_COOL, PM8150B_WARM, PM8150B_HOT, PM8150B_OVERHEAT };
enum { PM8150B_CTM_DONE, PM8150B_CTM_HOT, PM8150B_CTM_HOT_MIN, PM8150B_CTM_WARM, PM8150B_CTM_COOL,
       PM8150B_CTM_CRITICAL };
enum { PM8150B_PD_NONE, PM8150B_PD_HOT, PM8150B_PD_WARM, PM8150B_PD_COOL_LIMITED, PM8150B_PD_COOL_FAST,
       PM8150B_PD_COOL };

/* Cleared at a detach up to caps, which the work sets back to the first contract's. */
struct pm8150b_thermal {
	int				die_health;
	int				ctm_health;	/* the connector's */
	int				ctm_state;
	u32				ctm_ma;		/* the connector's input limit, 0 none */
	u32				ctm_before_critical;
	int				ctm_start;	/* the settled input at the first Hot */
	int				ctm_reduction;
	unsigned long			ctm_next;
	unsigned long			since;		/* the contract's start */
	int				pd_mode;
	unsigned long			pd_next;
	unsigned int			caps;		/* the selected sink-capability set */
};

struct pmic_typec_pdphy_irq_data {
	int				virq;
	int				irq;
	struct pmic_typec_pdphy		*pmic_typec_pdphy;
};

struct pmic_typec_pdphy {
	struct device			*dev;
	struct tcpm_port		*tcpm_port;
	struct regmap			*regmap;
	u32				base;

	unsigned int			nr_irqs;
	struct pmic_typec_pdphy_irq_data	*irq_data;

	struct work_struct		reset_work;
	struct work_struct		receive_work;
	struct regulator		*vdd_pdphy;
	bool				pm8150b;	/* PM8150B's charger, not PM8550B's */
	bool				pm8150b_programmed;	/* a limit was set */
	u8				pm8150b_saved[PM8150B_SAVED_REGS];	/* the charger before it */
	u32				pm8150b_ma;	/* the last limit programmed */
	u32				pm8150b_mv;
	struct mutex			pm8150b_lock;	/* the charger's programming */
	struct delayed_work		pm8150b_work;
	struct pm8150b_thermal		pm8150b_th;
	bool				pm8150b_stopped;
	spinlock_t			lock;		/* Register atomicity */
};

static void qcom_pmic_typec_pdphy_reset_on(struct pmic_typec_pdphy *pmic_typec_pdphy)
{
	struct device *dev = pmic_typec_pdphy->dev;
	int ret;

	/* Terminate TX */
	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_TX_CONTROL_REG, 0);
	if (ret)
		goto err;

	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_FRAME_FILTER_REG, 0);
	if (ret)
		goto err;

	return;
err:
	dev_err(dev, "pd_reset_on error\n");
}

static void qcom_pmic_typec_pdphy_reset_off(struct pmic_typec_pdphy *pmic_typec_pdphy)
{
	struct device *dev = pmic_typec_pdphy->dev;
	int ret;

	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_FRAME_FILTER_REG,
			   FRAME_FILTER_EN_SOP | FRAME_FILTER_EN_HARD_RESET);
	if (ret)
		dev_err(dev, "pd_reset_off error\n");
}

static void qcom_pmic_typec_pdphy_sig_reset_work(struct work_struct *work)
{
	struct pmic_typec_pdphy *pmic_typec_pdphy = container_of(work, struct pmic_typec_pdphy,
						     reset_work);
	unsigned long flags;

	spin_lock_irqsave(&pmic_typec_pdphy->lock, flags);

	qcom_pmic_typec_pdphy_reset_on(pmic_typec_pdphy);
	qcom_pmic_typec_pdphy_reset_off(pmic_typec_pdphy);

	spin_unlock_irqrestore(&pmic_typec_pdphy->lock, flags);

	tcpm_pd_hard_reset(pmic_typec_pdphy->tcpm_port);
}

static int
qcom_pmic_typec_pdphy_clear_tx_control_reg(struct pmic_typec_pdphy *pmic_typec_pdphy)
{
	struct device *dev = pmic_typec_pdphy->dev;
	unsigned int val;
	int ret;

	/* Clear TX control register */
	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_TX_CONTROL_REG, 0);
	if (ret)
		goto done;

	/* Perform readback to ensure sufficient delay for command to latch */
	ret = regmap_read(pmic_typec_pdphy->regmap,
			  pmic_typec_pdphy->base + USB_PDPHY_TX_CONTROL_REG, &val);

done:
	if (ret)
		dev_err(dev, "pd_clear_tx_control_reg: clear tx flag\n");

	return ret;
}

static int
qcom_pmic_typec_pdphy_pd_transmit_signal(struct pmic_typec_pdphy *pmic_typec_pdphy,
					 enum tcpm_transmit_type type,
					 unsigned int negotiated_rev)
{
	struct device *dev = pmic_typec_pdphy->dev;
	unsigned int val;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pmic_typec_pdphy->lock, flags);

	/* Clear TX control register */
	ret = qcom_pmic_typec_pdphy_clear_tx_control_reg(pmic_typec_pdphy);
	if (ret)
		goto done;

	val = TX_CONTROL_SEND_SIGNAL;
	if (negotiated_rev == PD_REV30)
		val |= TX_CONTROL_RETRY_COUNT(2);
	else
		val |= TX_CONTROL_RETRY_COUNT(3);

	if (type == TCPC_TX_CABLE_RESET || type == TCPC_TX_HARD_RESET)
		val |= TX_CONTROL_FRAME_TYPE(1);

	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_TX_CONTROL_REG, val);

done:
	spin_unlock_irqrestore(&pmic_typec_pdphy->lock, flags);

	dev_vdbg(dev, "pd_transmit_signal: type %d negotiate_rev %d send %d\n",
		 type, negotiated_rev, ret);

	return ret;
}

static int
qcom_pmic_typec_pdphy_pd_transmit_payload(struct pmic_typec_pdphy *pmic_typec_pdphy,
					  enum tcpm_transmit_type type,
					  const struct pd_message *msg,
					  unsigned int negotiated_rev)
{
	struct device *dev = pmic_typec_pdphy->dev;
	unsigned int val, hdr_len, txbuf_len, txsize_len;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pmic_typec_pdphy->lock, flags);

	hdr_len = sizeof(msg->header);
	txbuf_len = pd_header_cnt_le(msg->header) * 4;
	txsize_len = hdr_len + txbuf_len - 1;

	ret = regmap_read(pmic_typec_pdphy->regmap,
			  pmic_typec_pdphy->base + USB_PDPHY_RX_ACKNOWLEDGE_REG,
			  &val);
	if (ret)
		goto done;

	if (val) {
		dev_err(dev, "pd_transmit_payload: RX message pending\n");
		ret = -EBUSY;
		goto done;
	}

	/* Clear TX control register */
	ret = qcom_pmic_typec_pdphy_clear_tx_control_reg(pmic_typec_pdphy);
	if (ret)
		goto done;

	hdr_len = sizeof(msg->header);
	txbuf_len = pd_header_cnt_le(msg->header) * 4;
	txsize_len = hdr_len + txbuf_len - 1;

	/* Write message header sizeof(u16) to USB_PDPHY_TX_BUFFER_HDR_REG */
	ret = regmap_bulk_write(pmic_typec_pdphy->regmap,
				pmic_typec_pdphy->base + USB_PDPHY_TX_BUFFER_HDR_REG,
				&msg->header, hdr_len);
	if (ret)
		goto done;

	/* Write payload to USB_PDPHY_TX_BUFFER_DATA_REG for txbuf_len */
	if (txbuf_len) {
		ret = regmap_bulk_write(pmic_typec_pdphy->regmap,
					pmic_typec_pdphy->base + USB_PDPHY_TX_BUFFER_DATA_REG,
					&msg->payload, txbuf_len);
		if (ret)
			goto done;
	}

	/* Write total length ((header + data) - 1) to USB_PDPHY_TX_SIZE_REG */
	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_TX_SIZE_REG,
			   txsize_len);
	if (ret)
		goto done;

	/* Clear TX control register */
	ret = qcom_pmic_typec_pdphy_clear_tx_control_reg(pmic_typec_pdphy);
	if (ret)
		goto done;

	/* Initiate transmit with retry count as indicated by PD revision */
	val = TX_CONTROL_FRAME_TYPE(type) | TX_CONTROL_SEND_MSG;
	if (pd_header_rev(msg->header) == PD_REV30)
		val |= TX_CONTROL_RETRY_COUNT(2);
	else
		val |= TX_CONTROL_RETRY_COUNT(3);

	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_TX_CONTROL_REG, val);

done:
	spin_unlock_irqrestore(&pmic_typec_pdphy->lock, flags);

	if (ret) {
		dev_err(dev, "pd_transmit_payload: hdr %*ph data %*ph ret %d\n",
			hdr_len, &msg->header, txbuf_len, &msg->payload, ret);
	}

	return ret;
}

int qcom_pmic_typec_pdphy_pd_transmit(struct pmic_typec_pdphy *pmic_typec_pdphy,
				      enum tcpm_transmit_type type,
				      const struct pd_message *msg,
				      unsigned int negotiated_rev)
{
	struct device *dev = pmic_typec_pdphy->dev;
	int ret;

	if (msg) {
		ret = qcom_pmic_typec_pdphy_pd_transmit_payload(pmic_typec_pdphy,
								type, msg,
								negotiated_rev);
	} else {
		ret = qcom_pmic_typec_pdphy_pd_transmit_signal(pmic_typec_pdphy,
							       type,
							       negotiated_rev);
	}

	if (ret)
		dev_dbg(dev, "pd_transmit: type %x result %d\n", type, ret);

	return ret;
}

static void qcom_pmic_typec_pdphy_pd_receive(struct pmic_typec_pdphy *pmic_typec_pdphy)
{
	struct device *dev = pmic_typec_pdphy->dev;
	struct pd_message msg;
	unsigned int size, rx_status;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pmic_typec_pdphy->lock, flags);

	ret = regmap_read(pmic_typec_pdphy->regmap,
			  pmic_typec_pdphy->base + USB_PDPHY_RX_SIZE_REG, &size);
	if (ret)
		goto done;

	/* Hardware requires +1 of the real read value to be passed */
	if (size < 1 || size > sizeof(msg.payload) + 1) {
		dev_dbg(dev, "pd_receive: invalid size %d\n", size);
		goto done;
	}

	size += 1;
	ret = regmap_read(pmic_typec_pdphy->regmap,
			  pmic_typec_pdphy->base + USB_PDPHY_RX_STATUS_REG,
			  &rx_status);

	if (ret)
		goto done;

	ret = regmap_bulk_read(pmic_typec_pdphy->regmap,
			       pmic_typec_pdphy->base + USB_PDPHY_RX_BUFFER_REG,
			       (u8 *)&msg, size);
	if (ret)
		goto done;

	/* Return ownership of RX buffer to hardware */
	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_RX_ACKNOWLEDGE_REG, 0);

done:
	spin_unlock_irqrestore(&pmic_typec_pdphy->lock, flags);

	if (!ret) {
		dev_vdbg(dev, "pd_receive: handing %d bytes to tcpm\n", size);
		tcpm_pd_receive(pmic_typec_pdphy->tcpm_port, &msg, TCPC_TX_SOP);
	}
}

static irqreturn_t qcom_pmic_typec_pdphy_isr(int irq, void *dev_id)
{
	struct pmic_typec_pdphy_irq_data *irq_data = dev_id;
	struct pmic_typec_pdphy *pmic_typec_pdphy = irq_data->pmic_typec_pdphy;
	struct device *dev = pmic_typec_pdphy->dev;

	switch (irq_data->virq) {
	case PMIC_PDPHY_SIG_TX_IRQ:
		dev_err(dev, "isr: tx_sig\n");
		break;
	case PMIC_PDPHY_SIG_RX_IRQ:
		schedule_work(&pmic_typec_pdphy->reset_work);
		break;
	case PMIC_PDPHY_MSG_TX_IRQ:
		tcpm_pd_transmit_complete(pmic_typec_pdphy->tcpm_port,
					  TCPC_TX_SUCCESS);
		break;
	case PMIC_PDPHY_MSG_RX_IRQ:
		qcom_pmic_typec_pdphy_pd_receive(pmic_typec_pdphy);
		break;
	case PMIC_PDPHY_MSG_TX_FAIL_IRQ:
		tcpm_pd_transmit_complete(pmic_typec_pdphy->tcpm_port,
					  TCPC_TX_FAILED);
		break;
	case PMIC_PDPHY_MSG_TX_DISCARD_IRQ:
		tcpm_pd_transmit_complete(pmic_typec_pdphy->tcpm_port,
					  TCPC_TX_DISCARDED);
		break;
	}

	return IRQ_HANDLED;
}

int qcom_pmic_typec_pdphy_set_pd_rx(struct pmic_typec_pdphy *pmic_typec_pdphy, bool on)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pmic_typec_pdphy->lock, flags);

	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_RX_ACKNOWLEDGE_REG, !on);

	spin_unlock_irqrestore(&pmic_typec_pdphy->lock, flags);

	dev_dbg(pmic_typec_pdphy->dev, "set_pd_rx: %s\n", str_on_off(on));

	return ret;
}

int qcom_pmic_typec_pdphy_set_roles(struct pmic_typec_pdphy *pmic_typec_pdphy,
				    bool data_role_host, bool power_role_src)
{
	struct device *dev = pmic_typec_pdphy->dev;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&pmic_typec_pdphy->lock, flags);

	ret = regmap_update_bits(pmic_typec_pdphy->regmap,
				 pmic_typec_pdphy->base + USB_PDPHY_MSG_CONFIG_REG,
				 MSG_CONFIG_PORT_DATA_ROLE |
				 MSG_CONFIG_PORT_POWER_ROLE,
				 data_role_host << 3 | power_role_src << 2);

	spin_unlock_irqrestore(&pmic_typec_pdphy->lock, flags);

	dev_dbg(dev, "pdphy_set_roles: data_role_host=%d power_role_src=%d\n",
		data_role_host, power_role_src);

	return ret;
}

/*
 * PM8150B's charger (SM8250's; the stock kernel's smb5) for a PD contract, as smb5 programs it:
 * its USB input block is at 0x1300, its buck's switching frequency in the DC-DC block at 0x1100.
 * Valve's code above is PM8550B's (its USB power block at 0x2900) and must never run on PM8150B,
 * where 0x2900 is not the charger. The PMIC the Type-C block sits on decides which runs.
 *
 * Writing to the charger is on by default; with pm8150b_charger_control off, the PD PHY
 * negotiates and logs what it would set, and leaves the charger to the hardware and the boot
 * loader, as without a Type-C driver.
 */
static bool pm8150b_charger_control = true;
module_param(pm8150b_charger_control, bool, 0644);
MODULE_PARM_DESC(pm8150b_charger_control,
		 "Program PM8150B's charger input for a PD contract (else only log it)");

#define PM8150B_CHGR_BASE			0x1000
#define PM8150B_CHARGING_ENABLE_CMD_REG		(PM8150B_CHGR_BASE + 0x42)
#define PM8150B_BATTERY_CHARGER_STATUS_1_REG	(PM8150B_CHGR_BASE + 0x06)

#define PM8150B_DCDC_BASE			0x1100
#define PM8150B_DCDC_FSW_SEL_REG		(PM8150B_DCDC_BASE + 0x50)

#define PM8150B_USBIN_BASE			0x1300
#define PM8150B_APSD_RESULT_STATUS_REG		(PM8150B_USBIN_BASE + 0x08)
#define PM8150B_APSD_QC_3P0			BIT(6)
#define PM8150B_APSD_QC_2P0			BIT(5)
#define PM8150B_APSD_FLOAT			BIT(4)
#define PM8150B_APSD_DCP			BIT(3)
#define PM8150B_APSD_CDP			BIT(2)
#define PM8150B_APSD_OCP			BIT(1)
#define PM8150B_APSD_SDP			BIT(0)
#define PM8150B_CMD_ICL_OVERRIDE_REG		(PM8150B_USBIN_BASE + 0x42)
#define PM8150B_ICL_OVERRIDE			BIT(0)
#define PM8150B_ADAPTER_ALLOW_OVERRIDE_REG	(PM8150B_USBIN_BASE + 0x44)
#define PM8150B_ALLOW_CONTINUOUS		BIT(3)
#define PM8150B_ALLOW_12V			BIT(2)
#define PM8150B_ALLOW_9V			BIT(1)
#define PM8150B_ALLOW_5V			BIT(0)
#define PM8150B_ADAPTER_ALLOW_CFG_REG		(PM8150B_USBIN_BASE + 0x60)
#define PM8150B_USBIN_OPTIONS_1_CFG_REG		(PM8150B_USBIN_BASE + 0x62)
#define PM8150B_BC1P2_SRC_DETECT		BIT(3)
#define PM8150B_USBIN_LOAD_CFG_REG		(PM8150B_USBIN_BASE + 0x65)
#define PM8150B_ICL_OVERRIDE_AFTER_APSD		BIT(4)
#define PM8150B_USBIN_ICL_OPTIONS_REG		(PM8150B_USBIN_BASE + 0x66)
#define PM8150B_CFG_USB3P0_SEL			BIT(2)
#define PM8150B_USB51_MODE			BIT(1)
#define PM8150B_USBIN_MODE_CHG			BIT(0)
#define PM8150B_USBIN_CURRENT_LIMIT_CFG_REG	(PM8150B_USBIN_BASE + 0x70)
#define PM8150B_ICL_STEP_MA			50
#define PM8150B_ICL_MAX_MA			3000	/* the sink capabilities' highest current */

/* smb5's default input currents by charger type (smb5-lib.h). */
#define PM8150B_SDP_MA				500
#define PM8150B_DCP_MA				1500
#define PM8150B_CDP_MA				1500

/*
 * smb5's buck frequencies by input voltage (qpnp-smb5.c's chg_freq) as DCDC_FSW_SEL_REG values
 * (smblib_set_chg_freq: twice the frequency, 2400/2100/1600/1200 kHz are 7/8/11/15): 600 kHz at
 * 5 V, 800 at 6-8 V, 1050 at 9-11 V, 1200 at 12 V. smb5 sets a contract's frequency before it
 * requests the voltage; TCPM reports a voltage only once it is there, so a lower frequency of the
 * old voltage would run at the new one meanwhile (more ripple current). The boot loader's 1050 kHz
 * is the floor instead, which it runs at 5 V itself: a change up to 9 V never runs below its
 * frequency, one to 12 V runs at 1050 kHz until the source is ready (stock's 1200 after it).
 */
#define PM8150B_FSW_1050KHZ			8	/* up to 11 V: the boot loader's */
#define PM8150B_FSW_1200KHZ			7	/* 12 V */

static bool pdphy_is_pm8150b(struct device *dev)
{
	return dev->parent && of_device_is_compatible(dev->parent->of_node, "qcom,pm8150b");
}

/* The charger input's state, read only: what the hardware, the boot loader or a contract set. */
static void pm8150b_log_input(struct pmic_typec_pdphy *pdphy, const char *when)
{
	static const struct {
		const char *name;
		unsigned int reg;
	} regs[] = {
		{ "apsd", PM8150B_APSD_RESULT_STATUS_REG },
		{ "chg_en", PM8150B_CHARGING_ENABLE_CMD_REG },
		{ "chg_state", PM8150B_BATTERY_CHARGER_STATUS_1_REG },
		{ "fsw", PM8150B_DCDC_FSW_SEL_REG },
		{ "allow_cfg", PM8150B_ADAPTER_ALLOW_CFG_REG },
		{ "allow_ovr", PM8150B_ADAPTER_ALLOW_OVERRIDE_REG },
		{ "opt1", PM8150B_USBIN_OPTIONS_1_CFG_REG },
		{ "icl_ovr", PM8150B_CMD_ICL_OVERRIDE_REG },
		{ "load_cfg", PM8150B_USBIN_LOAD_CFG_REG },
		{ "icl_opt", PM8150B_USBIN_ICL_OPTIONS_REG },
		{ "icl", PM8150B_USBIN_CURRENT_LIMIT_CFG_REG },
	};
	char line[192];
	int len = 0, i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		unsigned int val;

		if (regmap_read(pdphy->regmap, regs[i].reg, &val))
			len += scnprintf(line + len, sizeof(line) - len, " %s=?", regs[i].name);
		else
			len += scnprintf(line + len, sizeof(line) - len, " %s=%02x", regs[i].name, val);
	}
	dev_info(pdphy->dev, "PM8150B input %s:%s\n", when, line);
}

/* smb5's default input current for the charger type the hardware detected (BC1.2). */
static int pm8150b_get_current_limit(struct pmic_typec_pdphy *pdphy)
{
	unsigned int apsd = 0;

	if (regmap_read(pdphy->regmap, PM8150B_APSD_RESULT_STATUS_REG, &apsd))
		return PM8150B_SDP_MA;
	if (apsd & PM8150B_APSD_CDP)
		return PM8150B_CDP_MA;
	/* QC without HVDCP negotiation stays a 5 V DCP; a float charger is taken as one too. */
	if (apsd & (PM8150B_APSD_DCP | PM8150B_APSD_FLOAT | PM8150B_APSD_QC_2P0 | PM8150B_APSD_QC_3P0))
		return PM8150B_DCP_MA;
	return PM8150B_SDP_MA;
}

/*
 * smb5's smblib_set_usb_pd_allowed_voltage: a fixed contract allows exactly its voltage. In the
 * hardware's own allowance (FORCE_NULL) only what its QC detection negotiated counts, so a PD
 * contract's 9 V reads as over-voltage (USBIN_OV) and charging pauses.
 */
static u8 pm8150b_allowance_for(u32 mv)
{
	switch (mv) {
	case 5000:
		return PM8150B_ALLOW_5V;
	case 9000:
		return PM8150B_ALLOW_9V;
	case 12000:
		return PM8150B_ALLOW_12V;
	default:
		return PM8150B_ALLOW_CONTINUOUS;
	}
}

/*
 * TCPM's sink standby (PD_P_SNK_STDBY_MW): after the source accepted a new voltage and before it
 * switches, TCPM lowers the limit to 2.5 W at the old voltage. Stock's policy engine widens the
 * allowance to the range from the old to the new voltage at this point (on Accept), which smb5
 * makes CONTINUOUS; TCPM does not say the new voltage, and CONTINUOUS covers any change.
 */
#define PM8150B_STANDBY_UW			2500000

static bool pm8150b_is_standby(struct pmic_typec_pdphy *pdphy, u32 max_ma, u32 mv)
{
	return pdphy->pm8150b_programmed && (u64)max_ma * mv <= PM8150B_STANDBY_UW &&
	       (u64)pdphy->pm8150b_ma * pdphy->pm8150b_mv > PM8150B_STANDBY_UW;
}

static u8 pm8150b_fsw_for(u32 mv)
{
	return mv >= 12000 ? PM8150B_FSW_1200KHZ : PM8150B_FSW_1050KHZ;
}

/*
 * smb5's smblib_icl_override and smblib_set_icl_current: at most 500 mA through the USB 2.0 option
 * (SW_OVERRIDE_USB51_MODE), above it the ICL register in high-current mode (SW_OVERRIDE_HC_MODE).
 */
static int pm8150b_set_icl(struct pmic_typec_pdphy *pdphy, u32 max_ma)
{
	struct regmap *map = pdphy->regmap;
	int ret;

	if (max_ma <= PM8150B_SDP_MA) {
		ret = regmap_update_bits(map, PM8150B_USBIN_ICL_OPTIONS_REG,
					 PM8150B_CFG_USB3P0_SEL | PM8150B_USB51_MODE | PM8150B_USBIN_MODE_CHG,
					 PM8150B_USB51_MODE);
		if (ret)
			return ret;
		ret = regmap_update_bits(map, PM8150B_CMD_ICL_OVERRIDE_REG, PM8150B_ICL_OVERRIDE,
					 PM8150B_ICL_OVERRIDE);
		if (ret)
			return ret;
		return regmap_update_bits(map, PM8150B_USBIN_LOAD_CFG_REG,
					  PM8150B_ICL_OVERRIDE_AFTER_APSD, 0);
	}

	ret = regmap_write(map, PM8150B_USBIN_CURRENT_LIMIT_CFG_REG,
			   min_t(u32, max_ma, PM8150B_ICL_MAX_MA) / PM8150B_ICL_STEP_MA);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, PM8150B_USBIN_ICL_OPTIONS_REG, PM8150B_USBIN_MODE_CHG,
				 PM8150B_USBIN_MODE_CHG);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, PM8150B_CMD_ICL_OVERRIDE_REG, PM8150B_ICL_OVERRIDE, 0);
	if (ret)
		return ret;
	return regmap_update_bits(map, PM8150B_USBIN_LOAD_CFG_REG, PM8150B_ICL_OVERRIDE_AFTER_APSD,
				  PM8150B_ICL_OVERRIDE_AFTER_APSD);
}

/*
 * The registers a contract changes, in the order they go back: the input limit first, the
 * frequency last. smb5 needs no copy, as its software sets the limit again at every insertion
 * (APSD's votes); here nothing does, and HW_AUTO_MODE alone would leave a contract's 3 A limit
 * and high-current mode for the next charger or PC port. So the boot loader's values, which the
 * hardware's detection works with, are kept from before the first contract and written back.
 */
static const unsigned int pm8150b_saved_regs[PM8150B_SAVED_REGS] = {
	PM8150B_CMD_ICL_OVERRIDE_REG,
	PM8150B_USBIN_CURRENT_LIMIT_CFG_REG,
	PM8150B_USBIN_ICL_OPTIONS_REG,
	PM8150B_USBIN_LOAD_CFG_REG,
	PM8150B_USBIN_OPTIONS_1_CFG_REG,
	PM8150B_ADAPTER_ALLOW_OVERRIDE_REG,
	PM8150B_DCDC_FSW_SEL_REG,
};

static int pm8150b_save(struct pmic_typec_pdphy *pdphy)
{
	unsigned int val;
	int i, ret;

	for (i = 0; i < PM8150B_SAVED_REGS; i++) {
		ret = regmap_read(pdphy->regmap, pm8150b_saved_regs[i], &val);
		if (ret)
			return ret;
		pdphy->pm8150b_saved[i] = val;
	}
	return 0;
}

/* smb5's removal (typec_sink_removal), to the state before the contract: see pm8150b_saved_regs. */
static int pm8150b_restore(struct pmic_typec_pdphy *pdphy)
{
	int i, ret, err = 0;

	/* Every register, also after a failed one: a partial restore is worse than a full one. */
	for (i = 0; i < PM8150B_SAVED_REGS; i++) {
		ret = regmap_write(pdphy->regmap, pm8150b_saved_regs[i], pdphy->pm8150b_saved[i]);
		if (ret && !err)
			err = ret;
	}
	return err;
}

/*
 * TCPM's calls as stock's policy engine sets the PD voltages (smblib_set_prop_pd_voltage_min/max):
 * the start and every settled contract (PS_RDY) force their voltage (FORCE_5V at the start, as
 * PE_SNK_STARTUP's 5 V maximum); TCPM's standby before a change widens it (CONTINUOUS, as the
 * range on Accept) and leaves the rest as it is until the source is ready. The frequency follows
 * the voltage (pm8150b_fsw_for), and BC1.2 detection is off while a contract above 5 V runs
 * (smblib_apsd_enable with PD active). 0 mA is TCPM's start or a detach: the boot loader's state
 * stays at the start, a detach writes back the values from before the contract, also when
 * pm8150b_charger_control was switched off meanwhile.
 */
static int pm8150b_program(struct pmic_typec_pdphy *pdphy, u32 max_ma, u32 mv)
{
	struct regmap *map = pdphy->regmap;
	bool standby;
	int ret;

	dev_info(pdphy->dev, "PM8150B limit %u mA at %u mV%s\n", max_ma, mv,
		 pm8150b_charger_control || pdphy->pm8150b_programmed ?
		 "" : " (not programmed: pm8150b_charger_control is off)");

	if (!max_ma) {
		if (!pdphy->pm8150b_programmed) {
			pm8150b_log_input(pdphy, "now");
			return 0;
		}
		ret = pm8150b_restore(pdphy);
		pdphy->pm8150b_programmed = false;
		pdphy->pm8150b_ma = 0;
		pdphy->pm8150b_mv = 0;
		pm8150b_log_input(pdphy, "restored");
		/* Stock's daemon on a lost input: no limit, its state from the start. */
		memset(&pdphy->pm8150b_th, 0, offsetof(struct pm8150b_thermal, caps));
		return ret;
	}

	if (!pm8150b_charger_control) {
		pm8150b_log_input(pdphy, "now");
		return 0;
	}

	if (!pdphy->pm8150b_programmed) {
		/* Nothing written without a copy to go back to. */
		ret = pm8150b_save(pdphy);
		if (ret) {
			dev_warn(pdphy->dev, "PM8150B input not readable (%d): not programmed\n", ret);
			return ret;
		}
		pdphy->pm8150b_programmed = true;
	}
	standby = pm8150b_is_standby(pdphy, max_ma, mv);
	pdphy->pm8150b_ma = max_ma;
	pdphy->pm8150b_mv = mv;
	/* The connector's limit stays across contracts, as stock's vote on the input limit does. */
	if (pdphy->pm8150b_th.ctm_ma)
		max_ma = min_t(u32, max_ma, pdphy->pm8150b_th.ctm_ma);

	/* The allowance first: the source may switch 25 ms after its Accept (tSrcTransition). */
	ret = regmap_write(map, PM8150B_ADAPTER_ALLOW_OVERRIDE_REG,
			   standby ? PM8150B_ALLOW_CONTINUOUS : pm8150b_allowance_for(mv));
	if (ret)
		return ret;
	if (!standby) {
		ret = regmap_write(map, PM8150B_DCDC_FSW_SEL_REG, pm8150b_fsw_for(mv));
		if (ret)
			return ret;
		ret = regmap_update_bits(map, PM8150B_USBIN_OPTIONS_1_CFG_REG, PM8150B_BC1P2_SRC_DETECT,
					 mv > 5000 ? 0 : PM8150B_BC1P2_SRC_DETECT);
		if (ret)
			return ret;
	}
	ret = pm8150b_set_icl(pdphy, max_ma);
	pm8150b_log_input(pdphy, standby ? "standby" : "programmed");
	return ret;
}

/*
 * Stock's temperature control while charging. PICO OS runs it in user space (Qualcomm's
 * hvdcp_opti, its STD-PD and CTM parts); this is the same behaviour in the driver, so that it also
 * runs in charger mode and without a daemon. It uses no temperatures of its own: it acts on the
 * charger's hardware comparators as smb5 reports them on PM8150B, the die's (die_health) and the
 * connector thermistor's (connector_health), with stock's steps and intervals:
 *
 * - The die lowers the PD voltage: Hot or Overheat one sink-capability set lower at once and every
 *   60 s, Warm holds, Cool raises it again every second while the input is current-limited and
 *   lowers it every 60 s while it is not and the battery is past its full-current phase. The first
 *   step waits 5 s after the contract. Stock requests 5 V first and raises from there, at most to
 *   persist.vendor.chg.max_volt_mv (9 V): the sets here are 5 V (the first contract) and up to 9 V.
 * - The connector lowers the input current: from the settled input current when it first turns Hot,
 *   200 mA at once and every 30 s down to 500 mA; Warm holds; Cool gives back 100 mA at once and every
 *   60 s until the limit is gone; Overheat goes to 500 mA at once and back to the earlier limit after.
 *
 * Stock reacts to the charger's interrupts; this reads the comparators every second. They read
 * garbage without input, so only a present input counts.
 */
#define PM8150B_DCDC_ICL_STATUS_REG		(PM8150B_DCDC_BASE + 0x07)
#define PM8150B_DCDC_AICL_STATUS_REG		(PM8150B_DCDC_BASE + 0x0a)
#define PM8150B_SOFT_ILIMIT			BIT(6)
#define PM8150B_USBIN_INT_RT_STS_REG		(PM8150B_USBIN_BASE + 0x10)
#define PM8150B_USBIN_PLUGIN			BIT(4)
#define PM8150B_MISC_BASE			0x1600
#define PM8150B_DIE_TEMP_STATUS_REG		(PM8150B_MISC_BASE + 0x07)
#define PM8150B_CONNECTOR_TEMP_STATUS_REG	(PM8150B_MISC_BASE + 0x09)
#define PM8150B_TEMP_RST			BIT(2)
#define PM8150B_TEMP_UB				BIT(1)
#define PM8150B_TEMP_LB				BIT(0)
#define PM8150B_CHARGER_STATE_MASK		GENMASK(2, 0)
#define PM8150B_FULLON_CHARGE			3

#define PM8150B_POLL_MS				1000
#define PM8150B_PD_INIT_WAIT_MS			5000
#define PM8150B_PD_INC_MS			1000
#define PM8150B_PD_DEC_MS			60000
#define PM8150B_CTM_DEC_MA			200
#define PM8150B_CTM_DEC_MS			30000
#define PM8150B_CTM_INC_MA			100
#define PM8150B_CTM_INC_MS			60000
#define PM8150B_CTM_MIN_MA			500
#define PM8150B_CAPS_MAX			1	/* the connector's capabilities: 5 V, up to 9 V */

static const char *const pm8150b_health_names[] = { "Cool", "Warm", "Hot", "Overheat" };

static int pm8150b_health(struct pmic_typec_pdphy *pdphy, unsigned int reg)
{
	unsigned int stat;

	if (regmap_read(pdphy->regmap, reg, &stat))
		return PM8150B_COOL;
	if (stat & PM8150B_TEMP_RST)
		return PM8150B_OVERHEAT;
	if (stat & PM8150B_TEMP_UB)
		return PM8150B_HOT;
	if (stat & PM8150B_TEMP_LB)
		return PM8150B_WARM;
	return PM8150B_COOL;
}

static bool pm8150b_reg_bit(struct pmic_typec_pdphy *pdphy, unsigned int reg, unsigned int bit)
{
	unsigned int val;

	return !regmap_read(pdphy->regmap, reg, &val) && (val & bit);
}

static void pm8150b_ctm_apply(struct pmic_typec_pdphy *pdphy, u32 ma)
{
	struct pm8150b_thermal *th = &pdphy->pm8150b_th;

	th->ctm_ma = ma;
	if (pdphy->pm8150b_programmed)
		pm8150b_set_icl(pdphy, ma ? min_t(u32, pdphy->pm8150b_ma, ma) : pdphy->pm8150b_ma);
	dev_info(pdphy->dev, "PM8150B connector %s: input limit %s%u mA\n",
		 pm8150b_health_names[th->ctm_health], ma ? "" : "none, ", ma ? ma : pdphy->pm8150b_ma);
}

/* CTM's Hot and Cool steps: the reduction counts on below the floor, the limit stops at it. */
static void pm8150b_ctm_step(struct pmic_typec_pdphy *pdphy, bool hot, unsigned long now)
{
	struct pm8150b_thermal *th = &pdphy->pm8150b_th;

	if (hot) {
		th->ctm_reduction += PM8150B_CTM_DEC_MA;
	} else {
		th->ctm_reduction = max(0, th->ctm_reduction - PM8150B_CTM_INC_MA);
		if (!th->ctm_reduction) {
			th->ctm_state = PM8150B_CTM_DONE;
			pm8150b_ctm_apply(pdphy, 0);
			return;
		}
	}
	pm8150b_ctm_apply(pdphy, max(th->ctm_start - th->ctm_reduction, PM8150B_CTM_MIN_MA));
	if (hot && th->ctm_ma == PM8150B_CTM_MIN_MA)
		th->ctm_state = PM8150B_CTM_HOT_MIN;
	th->ctm_next = now + msecs_to_jiffies(hot ? PM8150B_CTM_DEC_MS : PM8150B_CTM_INC_MS);
}

static void pm8150b_ctm(struct pmic_typec_pdphy *pdphy, int health, unsigned long now)
{
	struct pm8150b_thermal *th = &pdphy->pm8150b_th;
	unsigned int settled = 0;

	if (health != th->ctm_health) {
		int was = th->ctm_health;

		th->ctm_health = health;
		if (th->ctm_state == PM8150B_CTM_DONE) {
			/* Without a limit Warm and Cool change nothing; the first Hot starts from the settled input. */
			if (health < PM8150B_HOT)
				return;
			th->ctm_start = regmap_read(pdphy->regmap, PM8150B_DCDC_ICL_STATUS_REG, &settled) ?
					pdphy->pm8150b_ma : settled * PM8150B_ICL_STEP_MA;
		} else if (was == PM8150B_OVERHEAT) {
			pm8150b_ctm_apply(pdphy, th->ctm_before_critical);
		}
		if (health == PM8150B_OVERHEAT) {
			th->ctm_before_critical = th->ctm_ma;
			th->ctm_state = PM8150B_CTM_CRITICAL;
			pm8150b_ctm_apply(pdphy, PM8150B_CTM_MIN_MA);
		} else if (health == PM8150B_HOT) {
			if (th->ctm_state == PM8150B_CTM_HOT || th->ctm_state == PM8150B_CTM_HOT_MIN)
				return;
			th->ctm_state = PM8150B_CTM_HOT;
			pm8150b_ctm_step(pdphy, true, now);
		} else if (health == PM8150B_WARM) {
			th->ctm_state = PM8150B_CTM_WARM;
		} else {
			th->ctm_state = PM8150B_CTM_COOL;
			pm8150b_ctm_step(pdphy, false, now);
		}
		return;
	}
	if ((th->ctm_state == PM8150B_CTM_HOT || th->ctm_state == PM8150B_CTM_COOL) &&
	    time_after_eq(now, th->ctm_next))
		pm8150b_ctm_step(pdphy, th->ctm_state == PM8150B_CTM_HOT, now);
}

/* STD-PD: the sink-capability set to ask for, or the current one. */
static unsigned int pm8150b_std_pd(struct pmic_typec_pdphy *pdphy, int health, unsigned long now)
{
	struct pm8150b_thermal *th = &pdphy->pm8150b_th;
	unsigned int state, val = 0;
	int mode;

	if (time_before(now, th->since + msecs_to_jiffies(PM8150B_PD_INIT_WAIT_MS)))
		return th->caps;
	if (health >= PM8150B_HOT) {
		mode = PM8150B_PD_HOT;
	} else if (health == PM8150B_WARM) {
		mode = PM8150B_PD_WARM;
	} else if (pm8150b_reg_bit(pdphy, PM8150B_DCDC_AICL_STATUS_REG, PM8150B_SOFT_ILIMIT)) {
		mode = PM8150B_PD_COOL_LIMITED;
	} else {
		state = regmap_read(pdphy->regmap, PM8150B_BATTERY_CHARGER_STATUS_1_REG, &val) ? 0 :
			val & PM8150B_CHARGER_STATE_MASK;
		mode = state == PM8150B_FULLON_CHARGE ? PM8150B_PD_COOL_FAST : PM8150B_PD_COOL;
	}
	if (mode != th->pd_mode) {
		th->pd_mode = mode;
		/* Hot steps at once, the others after their interval. */
		th->pd_next = now + (mode == PM8150B_PD_HOT ? 0 :
				     msecs_to_jiffies(mode == PM8150B_PD_COOL_LIMITED ?
						      PM8150B_PD_INC_MS : PM8150B_PD_DEC_MS));
	}
	if (mode == PM8150B_PD_WARM || mode == PM8150B_PD_COOL_FAST || time_before(now, th->pd_next))
		return th->caps;
	if (mode == PM8150B_PD_COOL_LIMITED) {
		th->pd_next = now + msecs_to_jiffies(PM8150B_PD_INC_MS);
		return min(th->caps + 1, (unsigned int)PM8150B_CAPS_MAX);
	}
	th->pd_next = now + msecs_to_jiffies(PM8150B_PD_DEC_MS);
	return th->caps ? th->caps - 1 : 0;
}

static void pm8150b_thermal_work(struct work_struct *work)
{
	struct pmic_typec_pdphy *pdphy = container_of(to_delayed_work(work), struct pmic_typec_pdphy,
						      pm8150b_work);
	struct pm8150b_thermal *th = &pdphy->pm8150b_th;
	unsigned long now = jiffies;
	unsigned int caps;
	bool again;
	int die, ret;

	mutex_lock(&pdphy->pm8150b_lock);
	if (!pdphy->pm8150b_programmed) {
		/* Detached: the next contract starts at 5 V again, as stock's first request. */
		caps = 0;
		again = false;
	} else if (!pm8150b_reg_bit(pdphy, PM8150B_USBIN_INT_RT_STS_REG, PM8150B_USBIN_PLUGIN)) {
		caps = th->caps;
		again = true;
	} else {
		die = pm8150b_health(pdphy, PM8150B_DIE_TEMP_STATUS_REG);
		if (die != th->die_health) {
			dev_info(pdphy->dev, "PM8150B die %s\n", pm8150b_health_names[die]);
			th->die_health = die;
		}
		pm8150b_ctm(pdphy, pm8150b_health(pdphy, PM8150B_CONNECTOR_TEMP_STATUS_REG), now);
		caps = pm8150b_std_pd(pdphy, die, now);
		again = true;
	}
	mutex_unlock(&pdphy->pm8150b_lock);

	/* Outside the lock: TCPM calls back into set_current_limit with its own lock held. */
	if (caps != th->caps && pdphy->tcpm_port) {
		ret = tcpm_select_pd(pdphy->tcpm_port, caps);
		dev_info(pdphy->dev, "PM8150B die %s: sink capabilities up to %s (%d)\n",
			 pm8150b_health_names[th->die_health], caps ? "9 V" : "5 V", ret);
		if (!ret)
			th->caps = caps;
	}
	if (again && !READ_ONCE(pdphy->pm8150b_stopped))
		schedule_delayed_work(&pdphy->pm8150b_work, msecs_to_jiffies(PM8150B_POLL_MS));
}

static int pm8150b_set_current_limit(struct pmic_typec_pdphy *pdphy, u32 max_ma, u32 mv)
{
	bool was, changed;
	int ret;

	mutex_lock(&pdphy->pm8150b_lock);
	was = pdphy->pm8150b_programmed;
	ret = pm8150b_program(pdphy, max_ma, mv);
	if (!was && pdphy->pm8150b_programmed)
		pdphy->pm8150b_th.since = jiffies;
	changed = was != pdphy->pm8150b_programmed && !pdphy->pm8150b_stopped;
	mutex_unlock(&pdphy->pm8150b_lock);
	if (changed)
		mod_delayed_work(system_wq, &pdphy->pm8150b_work, 0);
	return ret;
}

static void pm8150b_thermal_init(struct pmic_typec_pdphy *pdphy)
{
	mutex_init(&pdphy->pm8150b_lock);
	INIT_DELAYED_WORK(&pdphy->pm8150b_work, pm8150b_thermal_work);
}

/* Before TCPM goes: nothing may run or be scheduled after the PD PHY stopped. */
static void pm8150b_thermal_stop(struct pmic_typec_pdphy *pdphy)
{
	mutex_lock(&pdphy->pm8150b_lock);
	WRITE_ONCE(pdphy->pm8150b_stopped, true);
	mutex_unlock(&pdphy->pm8150b_lock);
	cancel_delayed_work_sync(&pdphy->pm8150b_work);
}
#define USBIN_BASE								0x2900

#define USBIN_CURRENT_LIMIT_CFG_REG				(USBIN_BASE + 0x52)
#define MAX_USBIN_CURRENT_MA					3000 
#define MIN_USBIN_CURRENT_MA					0
#define USBIN_CURRENT_STEP_MA					50

#define USBIN_CURRENT_LIMIT_OVERRIDE_REG		(USBIN_BASE + 0x51)
#define ICL_OVERRIDE_ENABLED_AFTER_APSD			0x10

#define USBIN_AICL_CFG_REG						(USBIN_BASE + 0x60)
#define USBIN_AICL_ENABLED						BIT(7)
#define USBIN_AICL_QWLS_BD_TRIG_EN				BIT(6)
#define USBIN_AICL_HDC_EN						BIT(5)
#define USBIN_AICL_START_AT_MAX					BIT(4)

int pdphy_set_usbin_current_limit(struct pmic_typec_pdphy *pmic_typec_pdphy, int icl_ma)
{
	int val_u = icl_ma;
	int val_raw;

	if (icl_ma > MAX_USBIN_CURRENT_MA || icl_ma < MIN_USBIN_CURRENT_MA)
		dev_err(pmic_typec_pdphy->dev, "USBIN current %d is out of range [%d, %d]\n", icl_ma, MAX_USBIN_CURRENT_MA, MIN_USBIN_CURRENT_MA);

	if (val_u > MAX_USBIN_CURRENT_MA)
		val_u = MAX_USBIN_CURRENT_MA;
	if (val_u < MIN_USBIN_CURRENT_MA)
		val_u = MIN_USBIN_CURRENT_MA;

	val_raw = (val_u - MIN_USBIN_CURRENT_MA) / USBIN_CURRENT_STEP_MA;


	regmap_write(pmic_typec_pdphy->regmap, USBIN_AICL_CFG_REG, USBIN_AICL_START_AT_MAX | USBIN_AICL_ENABLED | USBIN_AICL_HDC_EN);
	regmap_write(pmic_typec_pdphy->regmap, USBIN_CURRENT_LIMIT_OVERRIDE_REG, ICL_OVERRIDE_ENABLED_AFTER_APSD);
	return regmap_write(pmic_typec_pdphy->regmap, USBIN_CURRENT_LIMIT_CFG_REG, val_raw);
}

#define USBIN_ADAPTER_USB_OVP_CTRL_REG		(USBIN_BASE + 0xA8)
#define USBIN_ADAPTER_USB_OVP_CTRL_MASK		(BIT(2) | BIT(1) | BIT(0))
#define USBIN_ADAPTER_USB_OVP_CTRL_13P5V	0x01
#define USBIN_ADAPTER_USB_OVP_CTRL_16P4V	0x02

static int qcom_pmic_typec_pdphy_set_over_voltage_protection(struct pmic_typec_pdphy *pmic_typec_pdphy)
{
	int ret;

	// Just hardcode to 16.4 Volts for now, default is 6.9 Volts
	ret = regmap_update_bits(pmic_typec_pdphy->regmap, USBIN_ADAPTER_USB_OVP_CTRL_REG,
		USBIN_ADAPTER_USB_OVP_CTRL_MASK, USBIN_ADAPTER_USB_OVP_CTRL_16P4V);

	return ret;
}

int qcom_pmic_typec_pdphy_get_current_limit(struct pmic_typec_pdphy *pmic_typec_pdphy)
{
	if (pmic_typec_pdphy->pm8150b)
		return pm8150b_get_current_limit(pmic_typec_pdphy);

	// 900 mA
	int limit = 900;

	// TODO Add QC/BC1.2/DC1.5A Support Here
	// Default to 900 mA for USB 3.1

	return limit;
}

#define USBIN_ADAPTER_ALLOW_OVERRIDE_REG		(USBIN_BASE + 0x41)
#define CONTINUOUS								BIT(3)
#define FORCE_12V								BIT(2)
#define FORCE_9V								BIT(1)
#define FORCE_5V								BIT(0)
#define FORCE_NULL								0

#define USBIN_HVOV_SEL							(USBIN_BASE + 0x42)
#define VOLT_14P2								0x00
#define VOLT_17P2								0x01
#define VOLT_20P2								0x10
#define VOLT_22P0								0x11

#define USBIN_CONT_AICL_THRESHOLD_REG			(USBIN_BASE + 0x62)
// #define FORCE_AICL_4P7V							0x06
// #define FORCE_AICL_4P8V							0x07
// #define FORCE_AICL_8P6V							0x1A
// #define FORCE_AICL_8P8V							0x1B
// #define FORCE_AICL_11P6V						0x29
// #define FORCE_AICL_11P8V						0x2A
// #define FORCE_AICL_14P4V						0x33
// #define FORCE_AICL_14P8V						0x34

int qcom_pmic_typec_pdphy_set_current_limit(struct pmic_typec_pdphy *pmic_typec_pdphy,
					u32 max_ma, u32 mv)
{
	if (pmic_typec_pdphy->pm8150b)
		return pm8150b_set_current_limit(pmic_typec_pdphy, max_ma, mv);

	// NOTE If we want to allow more than 15 Volts, 0x2940 must be updated from 0x08 -> 0x0C

	// AICL Threshold is from PD Spec.
	// 95/100 is -5% tolernace allowed on output voltage.
	// 1000 is mV drop, PD spec is 750 mV at connecter. Since we have another 80 mOhms in headstrap, we can drop another 250 mV at 3 Amps.
	u32 aicl_threshold_mv = mv * 95 / 100 - 1000 * max_ma / 3000;
	u8 cont_aicl_thres_reg = 0;

	if (aicl_threshold_mv > 19200) {
		// Upper Bound
		cont_aicl_thres_reg = 63;

	} else if (aicl_threshold_mv > 12800) {
		// 400 mV steps
		cont_aicl_thres_reg = (aicl_threshold_mv - 12800) / 400 + 47;

	} else if (aicl_threshold_mv > 4800) {
		cont_aicl_thres_reg = (aicl_threshold_mv - 4800) / 200 + 7;

	} else if (aicl_threshold_mv > 4100) {
		cont_aicl_thres_reg = (aicl_threshold_mv - 4100) / 100;

	} // else Min Threshold

	// Set max allowed voltage and AICL
	if (mv >= 15000) {
		regmap_write(pmic_typec_pdphy->regmap, USBIN_ADAPTER_ALLOW_OVERRIDE_REG, CONTINUOUS);
		regmap_write(pmic_typec_pdphy->regmap, USBIN_HVOV_SEL, VOLT_17P2);
		regmap_write(pmic_typec_pdphy->regmap, USBIN_CONT_AICL_THRESHOLD_REG, cont_aicl_thres_reg);
	} else if (mv >= 12000) {
		regmap_write(pmic_typec_pdphy->regmap, USBIN_ADAPTER_ALLOW_OVERRIDE_REG, FORCE_12V);
		regmap_write(pmic_typec_pdphy->regmap, USBIN_HVOV_SEL, VOLT_14P2);
		regmap_write(pmic_typec_pdphy->regmap, USBIN_CONT_AICL_THRESHOLD_REG, cont_aicl_thres_reg);
	} else if (mv >= 9000) {
		regmap_write(pmic_typec_pdphy->regmap, USBIN_ADAPTER_ALLOW_OVERRIDE_REG, FORCE_9V);
		regmap_write(pmic_typec_pdphy->regmap, USBIN_HVOV_SEL, VOLT_14P2);
		regmap_write(pmic_typec_pdphy->regmap, USBIN_CONT_AICL_THRESHOLD_REG, cont_aicl_thres_reg);
	} else {
		regmap_write(pmic_typec_pdphy->regmap, USBIN_ADAPTER_ALLOW_OVERRIDE_REG, FORCE_5V);
		regmap_write(pmic_typec_pdphy->regmap, USBIN_HVOV_SEL, VOLT_14P2);
		regmap_write(pmic_typec_pdphy->regmap, USBIN_CONT_AICL_THRESHOLD_REG, cont_aicl_thres_reg);
	}

	// Set the max allowed input current
	pdphy_set_usbin_current_limit(pmic_typec_pdphy, max_ma);

	return 0;
};

static int qcom_pmic_typec_pdphy_enable(struct pmic_typec_pdphy *pmic_typec_pdphy)
{
	struct device *dev = pmic_typec_pdphy->dev;
	int ret;

	/* PD 2.0, DR=TYPEC_DEVICE, PR=TYPEC_SINK */
	ret = regmap_update_bits(pmic_typec_pdphy->regmap,
				 pmic_typec_pdphy->base + USB_PDPHY_MSG_CONFIG_REG,
				 MSG_CONFIG_SPEC_REV_MASK, PD_REV20);
	if (ret)
		goto done;

	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_EN_CONTROL_REG, 0);
	if (ret)
		goto done;

	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_EN_CONTROL_REG,
			   CONTROL_ENABLE);
	if (ret)
		goto done;

	qcom_pmic_typec_pdphy_reset_off(pmic_typec_pdphy);
done:
	if (ret) {
		regulator_disable(pmic_typec_pdphy->vdd_pdphy);
		dev_err(dev, "pdphy_enable fail %d\n", ret);
	}

	return ret;
}

static int qcom_pmic_typec_pdphy_disable(struct pmic_typec_pdphy *pmic_typec_pdphy)
{
	int ret;

	qcom_pmic_typec_pdphy_reset_on(pmic_typec_pdphy);

	ret = regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_EN_CONTROL_REG, 0);

	return ret;
}

static int pmic_typec_pdphy_reset(struct pmic_typec_pdphy *pmic_typec_pdphy)
{
	int ret;

	ret = qcom_pmic_typec_pdphy_disable(pmic_typec_pdphy);
	if (ret)
		goto done;

	usleep_range(400, 500);
	ret = qcom_pmic_typec_pdphy_enable(pmic_typec_pdphy);
done:
	return ret;
}

int qcom_pmic_typec_pdphy_start(struct pmic_typec_pdphy *pmic_typec_pdphy,
				struct tcpm_port *tcpm_port)
{
	int i;
	int ret;

	ret = regulator_enable(pmic_typec_pdphy->vdd_pdphy);
	if (ret)
		return ret;

	pmic_typec_pdphy->tcpm_port = tcpm_port;

	ret = pmic_typec_pdphy_reset(pmic_typec_pdphy);
	if (ret)
		return ret;

	for (i = 0; i < pmic_typec_pdphy->nr_irqs; i++)
		enable_irq(pmic_typec_pdphy->irq_data[i].irq);

	return 0;
}

void qcom_pmic_typec_pdphy_stop(struct pmic_typec_pdphy *pmic_typec_pdphy)
{
	int i;

	if (pmic_typec_pdphy->pm8150b)
		pm8150b_thermal_stop(pmic_typec_pdphy);

	for (i = 0; i < pmic_typec_pdphy->nr_irqs; i++)
		disable_irq(pmic_typec_pdphy->irq_data[i].irq);

	qcom_pmic_typec_pdphy_set_pd_rx(pmic_typec_pdphy, false);
	qcom_pmic_typec_pdphy_reset_on(pmic_typec_pdphy);
	regmap_write(pmic_typec_pdphy->regmap,
			   pmic_typec_pdphy->base + USB_PDPHY_EN_CONTROL_REG, 0);
	qcom_pmic_typec_pdphy_set_roles(pmic_typec_pdphy, 0, 0);

	regulator_disable(pmic_typec_pdphy->vdd_pdphy);
}

struct pmic_typec_pdphy *qcom_pmic_typec_pdphy_alloc(struct device *dev)
{
	return devm_kzalloc(dev, sizeof(struct pmic_typec_pdphy), GFP_KERNEL);
}

int qcom_pmic_typec_pdphy_probe(struct platform_device *pdev,
				struct pmic_typec_pdphy *pmic_typec_pdphy,
				struct pmic_typec_pdphy_resources *res,
				struct regmap *regmap,
				u32 base)
{
	struct device *dev = &pdev->dev;
	struct pmic_typec_pdphy_irq_data *irq_data;
	int i, ret, irq;

	if (!res->nr_irqs || res->nr_irqs > PMIC_PDPHY_MAX_IRQS)
		return -EINVAL;

	irq_data = devm_kcalloc(dev, res->nr_irqs, sizeof(*irq_data),
				GFP_KERNEL);
	if (!irq_data)
		return -ENOMEM;

	pmic_typec_pdphy->vdd_pdphy = devm_regulator_get(dev, "vdd-pdphy");
	if (IS_ERR(pmic_typec_pdphy->vdd_pdphy))
		return PTR_ERR(pmic_typec_pdphy->vdd_pdphy);

	pmic_typec_pdphy->dev = dev;
	pmic_typec_pdphy->base = base;
	pmic_typec_pdphy->regmap = regmap;
	pmic_typec_pdphy->nr_irqs = res->nr_irqs;
	pmic_typec_pdphy->irq_data = irq_data;
	spin_lock_init(&pmic_typec_pdphy->lock);
	INIT_WORK(&pmic_typec_pdphy->reset_work, qcom_pmic_typec_pdphy_sig_reset_work);

	pmic_typec_pdphy->pm8150b = pdphy_is_pm8150b(dev);
	if (pmic_typec_pdphy->pm8150b) {
		/* PM8150B's input over-voltage follows its adapter allowance, as smb5 leaves it. */
		pm8150b_thermal_init(pmic_typec_pdphy);
		pm8150b_log_input(pmic_typec_pdphy, "at probe");
	} else {
		// Set Over Voltage Protection to Allow 9 VDC USB In
		ret = qcom_pmic_typec_pdphy_set_over_voltage_protection(pmic_typec_pdphy);
		if (ret) {
			dev_warn(dev, "pdphy_probe failed to set OVP: %d\n", ret);
		}
	}

	for (i = 0; i < res->nr_irqs; i++, irq_data++) {
		irq = platform_get_irq_byname(pdev, res->irq_params[i].irq_name);
		if (irq < 0)
			return irq;

		irq_data->pmic_typec_pdphy = pmic_typec_pdphy;
		irq_data->irq = irq;
		irq_data->virq = res->irq_params[i].virq;

		ret = devm_request_threaded_irq(dev, irq, NULL,
						qcom_pmic_typec_pdphy_isr,
						IRQF_ONESHOT | IRQF_NO_AUTOEN,
						res->irq_params[i].irq_name,
						irq_data);
		if (ret)
			return ret;
	}

	return 0;
}
