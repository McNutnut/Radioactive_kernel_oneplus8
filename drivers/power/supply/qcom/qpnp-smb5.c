// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2020 The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt) "SMB2: %s: " fmt, __func__

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/log2.h>
#include <linux/qpnp/qpnp-revid.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/iio/consumer.h>
#include <linux/pmic-voter.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/msm-bus.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/usb/typec.h>
#include "smb5-reg.h"
#include "smb5-lib.h"
#include "schgm-flash.h"

static struct smb_params smb5_pmi632_params = {
	.fcc			= {
		.name   = "fast charge current",
		.reg    = CHGR_FAST_CHARGE_CURRENT_CFG_REG,
		.min_u  = 0,
		.max_u  = 3000000,
		.step_u = 50000,
	},
	.fv			= {
		.name   = "float voltage",
		.reg    = CHGR_FLOAT_VOLTAGE_CFG_REG,
		.min_u  = 3600000,
		.max_u  = 4800000,
		.step_u = 10000,
	},
	.usb_icl		= {
		.name   = "usb input current limit",
		.reg    = USBIN_CURRENT_LIMIT_CFG_REG,
		.min_u  = 0,
		.max_u  = 3000000,
		.step_u = 50000,
	},
	.icl_max_stat		= {
		.name   = "dcdc icl max status",
		.reg    = ICL_MAX_STATUS_REG,
		.min_u  = 0,
		.max_u  = 3000000,
		.step_u = 50000,
	},
	.icl_stat		= {
		.name   = "input current limit status",
		.reg    = ICL_STATUS_REG,
		.min_u  = 0,
		.max_u  = 3000000,
		.step_u = 50000,
	},
	.otg_cl			= {
		.name	= "usb otg current limit",
		.reg	= DCDC_OTG_CURRENT_LIMIT_CFG_REG,
		.min_u	= 500000,
		.max_u	= 1000000,
		.step_u	= 250000,
	},
	.jeita_cc_comp_hot	= {
		.name	= "jeita fcc reduction",
		.reg	= JEITA_CCCOMP_CFG_HOT_REG,
		.min_u	= 0,
		.max_u	= 1575000,
		.step_u	= 25000,
	},
	.jeita_cc_comp_cold	= {
		.name	= "jeita fcc reduction",
		.reg	= JEITA_CCCOMP_CFG_COLD_REG,
		.min_u	= 0,
		.max_u	= 1575000,
		.step_u	= 25000,
	},
	.freq_switcher		= {
		.name	= "switching frequency",
		.reg	= DCDC_FSW_SEL_REG,
		.min_u	= 600,
		.max_u	= 1200,
		.step_u	= 400,
		.set_proc = smblib_set_chg_freq,
	},
	.aicl_5v_threshold		= {
		.name   = "AICL 5V threshold",
		.reg    = USBIN_5V_AICL_THRESHOLD_REG,
		.min_u  = 4000,
		.max_u  = 4700,
		.step_u = 100,
	},
	.aicl_cont_threshold		= {
		.name   = "AICL CONT threshold",
		.reg    = USBIN_CONT_AICL_THRESHOLD_REG,
		.min_u  = 4000,
		.max_u  = 8800,
		.step_u = 100,
		.get_proc = smblib_get_aicl_cont_threshold,
		.set_proc = smblib_set_aicl_cont_threshold,
	},
};

static struct smb_params smb5_pm8150b_params = {
	.fcc			= {
		.name   = "fast charge current",
		.reg    = CHGR_FAST_CHARGE_CURRENT_CFG_REG,
		.min_u  = 0,
		.max_u  = 8000000,
		.step_u = 50000,
	},
	.fv			= {
		.name   = "float voltage",
		.reg    = CHGR_FLOAT_VOLTAGE_CFG_REG,
		.min_u  = 3600000,
		.max_u  = 4790000,
		.step_u = 10000,
	},
	.usb_icl		= {
		.name   = "usb input current limit",
		.reg    = USBIN_CURRENT_LIMIT_CFG_REG,
		.min_u  = 0,
		.max_u  = 5000000,
		.step_u = 50000,
	},
	.icl_max_stat		= {
		.name   = "dcdc icl max status",
		.reg    = ICL_MAX_STATUS_REG,
		.min_u  = 0,
		.max_u  = 5000000,
		.step_u = 50000,
	},
	.icl_stat		= {
		.name   = "aicl icl status",
		.reg    = AICL_ICL_STATUS_REG,
		.min_u  = 0,
		.max_u  = 5000000,
		.step_u = 50000,
	},
	.otg_cl			= {
		.name	= "usb otg current limit",
		.reg	= DCDC_OTG_CURRENT_LIMIT_CFG_REG,
		.min_u	= 500000,
		.max_u	= 3000000,
		.step_u	= 500000,
	},
	.otg_vol		= {
		.name	= "usb otg output voltage",
		.reg	= DCDC_VBOOST_CFG,
		.min_u	= 4800000,
		.max_u	= 5500000,
		.step_u	= 100000,
	},
	.dc_icl		= {
		.name   = "DC input current limit",
		.reg    = DCDC_CFG_REF_MAX_PSNS_REG,
		.min_u  = 0,
		.max_u  = DCIN_ICL_MAX_UA,
		.step_u = 50000,
	},
	.jeita_cc_comp_hot	= {
		.name	= "jeita fcc reduction",
		.reg	= JEITA_CCCOMP_CFG_HOT_REG,
		.min_u	= 0,
		.max_u	= 8000000,
		.step_u	= 25000,
		.set_proc = NULL,
	},
	.jeita_cc_comp_cold	= {
		.name	= "jeita fcc reduction",
		.reg	= JEITA_CCCOMP_CFG_COLD_REG,
		.min_u	= 0,
		.max_u	= 8000000,
		.step_u	= 25000,
		.set_proc = NULL,
	},
	.freq_switcher		= {
		.name	= "switching frequency",
		.reg	= DCDC_FSW_SEL_REG,
		.min_u	= 600,
		.max_u	= 1200,
		.step_u	= 400,
		.set_proc = smblib_set_chg_freq,
	},
	.aicl_5v_threshold		= {
		.name   = "AICL 5V threshold",
		.reg    = USBIN_5V_AICL_THRESHOLD_REG,
		.min_u  = 4000,
		.max_u  = 4700,
		.step_u = 100,
	},
	.aicl_cont_threshold		= {
		.name   = "AICL CONT threshold",
		.reg    = USBIN_CONT_AICL_THRESHOLD_REG,
		.min_u  = 4000,
		.max_u  = 11800,
		.step_u = 100,
		.get_proc = smblib_get_aicl_cont_threshold,
		.set_proc = smblib_set_aicl_cont_threshold,
	},
};

struct smb_dt_props {
	int			usb_icl_ua;
	struct device_node	*revid_dev_node;
	enum float_options	float_option;
	int			chg_inhibit_thr_mv;
	bool			no_battery;
	bool			hvdcp_disable;
	bool			hvdcp_autonomous;
	bool			adc_based_aicl;
	int			sec_charger_config;
	int			auto_recharge_soc;
	int			auto_recharge_vbat_mv;
	int			wd_bark_time;
	int			wd_snarl_time_cfg;
	int			batt_profile_fcc_ua;
	int			batt_profile_fv_uv;
	int			term_current_src;
	int			term_current_thresh_hi_ma;
	int			term_current_thresh_lo_ma;
	int			disable_suspend_on_collapse;
};

struct smb5 {
	struct smb_charger	chg;
	struct dentry		*dfs_root;
	struct smb_dt_props	dt;
};

static int smbchg_cutoff_volt_with_charger = 3240;
struct smb_charger *g_chip;
static const struct file_operations proc_ship_mode_operations;
module_param_named(
	cutoff_volt_with_charger,
	smbchg_cutoff_volt_with_charger,
	int, 0600);

#define OF_PROP_READ(node, dt_property, prop, retval, optional)		\
do {									\
	if (retval)							\
		break;							\
	if (optional)							\
		prop = -EINVAL;						\
									\
	retval = of_property_read_u32(node,		\
					dt_property,	\
					&prop);				\
									\
	if ((retval == -EINVAL) && optional)				\
		retval = 0;						\
	else if (retval)						\
		pr_err("Error reading " #dt_property	\
				" property rc = %d\n", rc);		\
} while (0)

#ifdef	CONFIG_OP_DEBUG_CHG
static int __debug_mask = PR_OP_DEBUG;
#else
static int __debug_mask;
#endif
module_param_named(
	debug_mask, __debug_mask, int, 0600
);


static ssize_t pd_disabled_show(struct device *dev, struct device_attribute
				*attr, char *buf)
{
	struct smb5 *chip = dev_get_drvdata(dev);
	struct smb_charger *chg = &chip->chg;

	return snprintf(buf, PAGE_SIZE, "%d\n", chg->pd_disabled);
}

static ssize_t pd_disabled_store(struct device *dev, struct device_attribute
				 *attr, const char *buf, size_t count)
{
	int val;
	struct smb5 *chip = dev_get_drvdata(dev);
	struct smb_charger *chg = &chip->chg;

	if (kstrtos32(buf, 0, &val))
		return -EINVAL;

	chg->pd_disabled = val;

	return count;
}
static DEVICE_ATTR_RW(pd_disabled);

static ssize_t weak_chg_icl_ua_show(struct device *dev, struct device_attribute
				    *attr, char *buf)
{
	struct smb5 *chip = dev_get_drvdata(dev);
	struct smb_charger *chg = &chip->chg;

	return snprintf(buf, PAGE_SIZE, "%d\n", chg->weak_chg_icl_ua);
}

static ssize_t weak_chg_icl_ua_store(struct device *dev, struct device_attribute
				 *attr, const char *buf, size_t count)
{
	int val;
	struct smb5 *chip = dev_get_drvdata(dev);
	struct smb_charger *chg = &chip->chg;

	if (kstrtos32(buf, 0, &val))
		return -EINVAL;

	chg->weak_chg_icl_ua = val;

	return count;
}
static DEVICE_ATTR_RW(weak_chg_icl_ua);

static struct attribute *smb5_attrs[] = {
	&dev_attr_pd_disabled.attr,
	&dev_attr_weak_chg_icl_ua.attr,
	NULL,
};
ATTRIBUTE_GROUPS(smb5);

static int __usb_connector_temp;
module_param_named(
	usb_connector_temp, __usb_connector_temp, int, 0644
);

static int __usb_interval_temp;
module_param_named(
	usb_interval_temp, __usb_interval_temp, int, 0644
);

static int __disable_connector_protect;
module_param_named(
	disable_connector_protect, __disable_connector_protect, int, 0644
);

static int __call_on;
module_param_named(
	call_on, __call_on, int, 0644
);

static int __video_call_on;
module_param_named(
	video_call_on, __video_call_on, int, 0644
);

enum {
	BAT_THERM = 0,
	MISC_THERM,
	CONN_THERM,
	SMB_THERM,
};

static const struct clamp_config clamp_levels[] = {
	{ {0x11C6, 0x11F9, 0x13F1}, {0x60, 0x2E, 0x90} },
	{ {0x11C6, 0x11F9, 0x13F1}, {0x60, 0x2B, 0x9C} },
};

#define PMI632_MAX_ICL_UA	3000000
#define PM6150_MAX_FCC_UA	3000000
static int smb5_chg_config_init(struct smb5 *chip)
{
	struct smb_charger *chg = &chip->chg;
	struct pmic_revid_data *pmic_rev_id;
	struct device_node *revid_dev_node, *node = chg->dev->of_node;
	int rc = 0;

	revid_dev_node = of_parse_phandle(node, "qcom,pmic-revid", 0);
	if (!revid_dev_node) {
		pr_err("Missing qcom,pmic-revid property\n");
		return -EINVAL;
	}

	pmic_rev_id = get_revid_data(revid_dev_node);
	if (IS_ERR_OR_NULL(pmic_rev_id)) {
		/*
		 * the revid peripheral must be registered, any failure
		 * here only indicates that the rev-id module has not
		 * probed yet.
		 */
		rc =  -EPROBE_DEFER;
		goto out;
	}

	switch (pmic_rev_id->pmic_subtype) {
	case PM8150B_SUBTYPE:
		chip->chg.chg_param.smb_version = PM8150B_SUBTYPE;
		chg->param = smb5_pm8150b_params;
		chg->name = "pm8150b_charger";
		chg->wa_flags |= CHG_TERMINATION_WA;
		break;
	case PM7250B_SUBTYPE:
		chip->chg.chg_param.smb_version = PM7250B_SUBTYPE;
		chg->param = smb5_pm8150b_params;
		chg->name = "pm7250b_charger";
		chg->wa_flags |= CHG_TERMINATION_WA;
		chg->uusb_moisture_protection_capable = true;
		break;
	case PM6150_SUBTYPE:
		chip->chg.chg_param.smb_version = PM6150_SUBTYPE;
		chg->param = smb5_pm8150b_params;
		chg->name = "pm6150_charger";
		chg->wa_flags |= SW_THERM_REGULATION_WA | CHG_TERMINATION_WA;
		if (pmic_rev_id->rev4 >= 2)
			chg->uusb_moisture_protection_capable = true;
		chg->main_fcc_max = PM6150_MAX_FCC_UA;
		break;
	case PMI632_SUBTYPE:
		chip->chg.chg_param.smb_version = PMI632_SUBTYPE;
		chg->wa_flags |= WEAK_ADAPTER_WA | USBIN_OV_WA
				| CHG_TERMINATION_WA | USBIN_ADC_WA
				| SKIP_MISC_PBS_IRQ_WA;
		chg->param = smb5_pmi632_params;
		chg->use_extcon = true;
		chg->name = "pmi632_charger";
		/* PMI632 does not support PD */
		chg->pd_not_supported = true;
		chg->lpd_disabled = true;
		if (pmic_rev_id->rev4 >= 2)
			chg->uusb_moisture_protection_enabled = true;
		chg->hw_max_icl_ua =
			(chip->dt.usb_icl_ua > 0) ? chip->dt.usb_icl_ua
						: PMI632_MAX_ICL_UA;
		break;
	default:
		pr_err("PMIC subtype %d not supported\n",
				pmic_rev_id->pmic_subtype);
		rc = -EINVAL;
		goto out;
	}

	chg->chg_freq.freq_5V			= 600;
	chg->chg_freq.freq_6V_8V		= 800;
	chg->chg_freq.freq_9V			= 1050;
	chg->chg_freq.freq_12V                  = 1200;
	chg->chg_freq.freq_removal		= 1050;
	chg->chg_freq.freq_below_otg_threshold	= 800;
	chg->chg_freq.freq_above_otg_threshold	= 800;

	if (of_property_read_bool(node, "qcom,disable-sw-thermal-regulation"))
		chg->wa_flags &= ~SW_THERM_REGULATION_WA;

	if (of_property_read_bool(node, "qcom,disable-fcc-restriction"))
		chg->main_fcc_max = -EINVAL;

out:
	of_node_put(revid_dev_node);
	return rc;
}

#define PULL_NO_PULL	0
#define PULL_30K	30
#define PULL_100K	100
#define PULL_400K	400
static int get_valid_pullup(int pull_up)
{
	/* pull up can only be 0/30K/100K/400K) */
	switch (pull_up) {
	case PULL_NO_PULL:
		return INTERNAL_PULL_NO_PULL;
	case PULL_30K:
		return INTERNAL_PULL_30K_PULL;
	case PULL_100K:
		return INTERNAL_PULL_100K_PULL;
	case PULL_400K:
		return INTERNAL_PULL_400K_PULL;
	default:
		return INTERNAL_PULL_100K_PULL;
	}
}

#define INTERNAL_PULL_UP_MASK	0x3
static int smb5_configure_internal_pull(struct smb_charger *chg, int type,
					int pull)
{
	int rc;
	int shift = type * 2;
	u8 mask = INTERNAL_PULL_UP_MASK << shift;
	u8 val = pull << shift;

	rc = smblib_masked_write(chg, BATIF_ADC_INTERNAL_PULL_UP_REG,
				mask, val);
	if (rc < 0)
		dev_err(chg->dev,
			"Couldn't configure ADC pull-up reg rc=%d\n", rc);

	return rc;
}

#define MICRO_1P5A			1500000
#define MICRO_P1A			100000
#define MICRO_1PA			1000000
#define MICRO_3PA			3000000
#define MICRO_4PA			4000000
#define OTG_DEFAULT_DEGLITCH_TIME_MS	50
#define DEFAULT_WD_BARK_TIME		64
#define DEFAULT_WD_SNARL_TIME_8S	0x07
#define DEFAULT_FCC_STEP_SIZE_UA	100000
#define DEFAULT_FCC_STEP_UPDATE_DELAY_MS	1000
#ifdef OP_SWARP_SUPPORTED
static int read_array_data_from_node(struct device_node *node,
		const char *prop_str, int *addr, int len)
{
	int rc = 0, length;

	if (!node || !prop_str || !addr) {
		pr_err("Invalid parameters passed\n");
		return -EINVAL;
	}

	rc = of_property_count_elems_of_size(node, prop_str, sizeof(u32));
	if (rc < 0) {
		pr_err("Count %s failed, rc=%d\n", prop_str, rc);
		return rc;
	}

	length = rc;

	if (length > len) {
		pr_err("too many entries(%d), only %d allowed\n", length, len);
		length = len;
	}

	rc = of_property_read_u32_array(node, prop_str,
			(u32 *)addr, length);
	if (rc) {
		pr_err("Read %s failed, rc=%d\n", prop_str, rc);
		return rc;
	}

	return rc;
}
#endif

static int smb5_parse_dt_misc(struct smb5 *chip, struct device_node *node)
{
	int rc = 0, byte_len;
	struct smb_charger *chg = &chip->chg;
	enum of_gpio_flags flags;
	int retval = 0;

	if (!node) {
		pr_err("device tree node missing\n");
		return -EINVAL;
	}

	of_property_read_u32(node, "qcom,sec-charger-config",
					&chip->dt.sec_charger_config);
	chg->sec_cp_present =
		chip->dt.sec_charger_config == POWER_SUPPLY_CHARGER_SEC_CP ||
		chip->dt.sec_charger_config == POWER_SUPPLY_CHARGER_SEC_CP_PL;

	chg->sec_pl_present =
		chip->dt.sec_charger_config == POWER_SUPPLY_CHARGER_SEC_PL ||
		chip->dt.sec_charger_config == POWER_SUPPLY_CHARGER_SEC_CP_PL;

/*read ffc param*/
	OF_PROP_READ(node, "ffc-pre-normal-decidegc",
			chg->FFC_TEMP_T1, retval, 1);
	OF_PROP_READ(node, "ffc-normal-decidegc",
		chg->FFC_TEMP_T2, retval, 1);
	OF_PROP_READ(node, "ffc-warm-decidegc",
		chg->FFC_TEMP_T3, retval, 1);
	OF_PROP_READ(node, "ffc-normal-fcc-ma",
		chg->FFC_NOR_FCC, retval, 1);
	OF_PROP_READ(node, "ffc-warm-fcc-ma",
		chg->FFC_WARM_FCC, retval, 1);
	OF_PROP_READ(node, "ffc-normal-cutoff-ma",
		chg->FFC_NORMAL_CUTOFF, retval, 1);
	OF_PROP_READ(node, "ffc-warm-cutoff-ma",
		chg->FFC_WARM_CUTOFF, retval, 1);
	OF_PROP_READ(node, "ffc-full-vbat-mv",
		chg->FFC_VBAT_FULL, retval, 1);
#ifdef OP_SWARP_SUPPORTED
	OF_PROP_READ(node, "ffc-fast-fcc-ma",
			chg->FFC_FAST_FCC, retval, 1);
	OF_PROP_READ(node, "ffc-fast-vbat-mv",
			chg->FFC_FAST_FV, retval, 1);
	OF_PROP_READ(node, "ffc-fast-warm-vbat-mv",
			chg->FFC_FAST_WARM_FV, retval, 1);
	OF_PROP_READ(node, "ffc-full-warm-vbat-mv",
			chg->FFC_WARM_VBAT_FULL, retval, 1);
	OF_PROP_READ(node, "ffc-fast-cutoff-ma",
		chg->FFC_FAST_CUTOFF, retval, 1);
#endif
	pr_info("T1:%d, T2:%d, T3:%d, fcc1:%d, fcc1:%d, cut1:%d, cut2:%d,full:%d\n",
		chg->FFC_TEMP_T1, chg->FFC_TEMP_T2, chg->FFC_TEMP_T3,
		chg->FFC_NOR_FCC, chg->FFC_WARM_FCC, chg->FFC_NORMAL_CUTOFF,
		chg->FFC_WARM_CUTOFF, chg->FFC_VBAT_FULL);

	OF_PROP_READ(node, "ibatmax-little-cold-ma",
			chg->ibatmax[BATT_TEMP_LITTLE_COLD], retval, 1);
	OF_PROP_READ(node, "ibatmax-cool-ma",
			chg->ibatmax[BATT_TEMP_COOL], retval, 1);
	OF_PROP_READ(node, "ibatmax-little-cool-ma",
			chg->ibatmax[BATT_TEMP_LITTLE_COOL], retval, 1);
	OF_PROP_READ(node, "ibatmax-pre-normal-ma",
			chg->ibatmax[BATT_TEMP_PRE_NORMAL], retval, 1);
	OF_PROP_READ(node, "ibatmax-normal-ma",
			chg->ibatmax[BATT_TEMP_NORMAL], retval, 1);
	OF_PROP_READ(node, "ibatmax-warm-ma",
			chg->ibatmax[BATT_TEMP_WARM], retval, 1);

#ifdef OP_SWARP_SUPPORTED
	/* read dash/warp ibatmax setting for different temp regions*/
	rc = read_array_data_from_node(node, "dash-temp-ibatmax",
			chg->dash_ibatmax, TEMP_REGION_MAX);
	if (rc) {
		chg->dash_ibatmax[BATT_TEMP_COLD] = 0;
		chg->dash_ibatmax[BATT_TEMP_LITTLE_COLD] = 300;
		chg->dash_ibatmax[BATT_TEMP_COOL] = 1200;
		chg->dash_ibatmax[BATT_TEMP_LITTLE_COOL] = 1800;
		chg->dash_ibatmax[BATT_TEMP_PRE_NORMAL] = 1800;
		chg->dash_ibatmax[BATT_TEMP_NORMAL] = 1800;
		chg->dash_ibatmax[BATT_TEMP_WARM] = 550;
		chg->dash_ibatmax[BATT_TEMP_HOT] = 0;
	}
	rc = read_array_data_from_node(node, "dash-temp-ibatmax-thr-ma",
			chg->dash_ibatmax_thr_ma, TEMP_REGION_MAX);
	if (rc) {
		chg->dash_ibatmax_thr_ma[BATT_TEMP_COLD] = 0;
		chg->dash_ibatmax_thr_ma[BATT_TEMP_LITTLE_COLD] = 300;
		chg->dash_ibatmax_thr_ma[BATT_TEMP_COOL] = 950;
		chg->dash_ibatmax_thr_ma[BATT_TEMP_LITTLE_COOL] = 1200;
		chg->dash_ibatmax_thr_ma[BATT_TEMP_PRE_NORMAL] = 1600;
		chg->dash_ibatmax_thr_ma[BATT_TEMP_NORMAL] = 1100;
		chg->dash_ibatmax_thr_ma[BATT_TEMP_WARM] = 550;
		chg->dash_ibatmax_thr_ma[BATT_TEMP_HOT] = 0;
	}
	rc = of_property_read_u32(node, "dw-vbat-thr-mv",
					&chg->dash_vbat_thr_mv);
	if (retval < 0)
		chg->dash_vbat_thr_mv = 4180;

	rc = read_array_data_from_node(node, "v9a2-temp-ibatmax",
			chg->qc_9v_ibatmax, TEMP_REGION_MAX);
	if (rc) {
		chg->qc_9v_ibatmax[BATT_TEMP_COLD] = 0;
		chg->qc_9v_ibatmax[BATT_TEMP_LITTLE_COLD] = 500;
		chg->qc_9v_ibatmax[BATT_TEMP_COOL] = 1050;
		chg->qc_9v_ibatmax[BATT_TEMP_LITTLE_COOL] = 2100;
		chg->qc_9v_ibatmax[BATT_TEMP_PRE_NORMAL] = 2000;
		chg->qc_9v_ibatmax[BATT_TEMP_NORMAL] = 2000;
		chg->qc_9v_ibatmax[BATT_TEMP_WARM] = 600;
		chg->qc_9v_ibatmax[BATT_TEMP_HOT] = 0;
	}
	rc = read_array_data_from_node(node, "v9a2-temp-ibatmax_thr_ma",
			chg->qc_9v_ibatmax_thr_ma, TEMP_REGION_MAX);
	if (rc) {
		chg->qc_9v_ibatmax_thr_ma[BATT_TEMP_COLD] = 0;
		chg->qc_9v_ibatmax_thr_ma[BATT_TEMP_LITTLE_COLD] = 500;
		chg->qc_9v_ibatmax_thr_ma[BATT_TEMP_COOL] = 1050;
		chg->qc_9v_ibatmax_thr_ma[BATT_TEMP_LITTLE_COOL] = 1450;
		chg->qc_9v_ibatmax_thr_ma[BATT_TEMP_PRE_NORMAL] = 1000;
		chg->qc_9v_ibatmax_thr_ma[BATT_TEMP_NORMAL] = 1000;
		chg->qc_9v_ibatmax_thr_ma[BATT_TEMP_WARM] = 600;
		chg->qc_9v_ibatmax_thr_ma[BATT_TEMP_HOT] = 0;
	}

	rc = read_array_data_from_node(node, "pd-temp-ibatmax",
			chg->pd_9v_ibatmax, TEMP_REGION_MAX);
	if (rc) {
		chg->pd_9v_ibatmax[BATT_TEMP_COLD] = 0;
		chg->pd_9v_ibatmax[BATT_TEMP_LITTLE_COLD] = 500;
		chg->pd_9v_ibatmax[BATT_TEMP_COOL] = 1050;
		chg->pd_9v_ibatmax[BATT_TEMP_LITTLE_COOL] = 2100;
		chg->pd_9v_ibatmax[BATT_TEMP_PRE_NORMAL] = 3000;
		chg->pd_9v_ibatmax[BATT_TEMP_NORMAL] = 3000;
		chg->pd_9v_ibatmax[BATT_TEMP_WARM] = 600;
		chg->pd_9v_ibatmax[BATT_TEMP_HOT] = 0;
	}
	rc = read_array_data_from_node(node, "pd-temp-ibatmax_thr_ma",
			chg->pd_9v_ibatmax_thr_ma, TEMP_REGION_MAX);
	if (rc) {
		chg->pd_9v_ibatmax_thr_ma[BATT_TEMP_COLD] = 0;
		chg->pd_9v_ibatmax_thr_ma[BATT_TEMP_LITTLE_COLD] = 500;
		chg->pd_9v_ibatmax_thr_ma[BATT_TEMP_COOL] = 1050;
		chg->pd_9v_ibatmax_thr_ma[BATT_TEMP_LITTLE_COOL] = 1450;
		chg->pd_9v_ibatmax_thr_ma[BATT_TEMP_PRE_NORMAL] = 1500;
		chg->pd_9v_ibatmax_thr_ma[BATT_TEMP_NORMAL] = 1500;
		chg->pd_9v_ibatmax_thr_ma[BATT_TEMP_WARM] = 600;
		chg->pd_9v_ibatmax_thr_ma[BATT_TEMP_HOT] = 0;
	}
#endif
	/* read vbatmax setting for different temp regions */
	OF_PROP_READ(node, "vbatmax-little-cold-mv",
			chg->vbatmax[BATT_TEMP_LITTLE_COLD], retval, 1);
	OF_PROP_READ(node, "vbatmax-cool-mv",
			chg->vbatmax[BATT_TEMP_COOL], retval, 1);
	OF_PROP_READ(node, "vbatmax-little-cool-mv",
			chg->vbatmax[BATT_TEMP_LITTLE_COOL], retval, 1);
	OF_PROP_READ(node, "vbatmax-pre-normal-mv",
			chg->vbatmax[BATT_TEMP_PRE_NORMAL], retval, 1);
	OF_PROP_READ(node, "vbatmax-normal-mv",
			chg->vbatmax[BATT_TEMP_NORMAL], retval, 1);
	OF_PROP_READ(node, "vbatmax-warm-mv",
			chg->vbatmax[BATT_TEMP_WARM], retval, 1);

	/* read vbatdet setting for different temp regions */
	OF_PROP_READ(node, "vbatdet-little-cold-mv",
			chg->vbatdet[BATT_TEMP_LITTLE_COLD], retval, 1);
	OF_PROP_READ(node, "vbatdet-cool-mv",
			chg->vbatdet[BATT_TEMP_COOL], retval, 1);
	OF_PROP_READ(node, "vbatdet-little-cool-mv",
			chg->vbatdet[BATT_TEMP_LITTLE_COOL], retval, 1);
	OF_PROP_READ(node, "vbatdet-pre-normal-mv",
			chg->vbatdet[BATT_TEMP_PRE_NORMAL], retval, 1);
	OF_PROP_READ(node, "vbatdet-normal-mv",
			chg->vbatdet[BATT_TEMP_NORMAL], retval, 1);
	OF_PROP_READ(node, "vbatdet-warm-mv",
			chg->vbatdet[BATT_TEMP_WARM], retval, 1);
	OF_PROP_READ(node, "little-cool-vbat-thr-mv",
			chg->temp_littel_cool_voltage, retval, 1);
	if (chg->temp_littel_cool_voltage < 0)
		chg->temp_littel_cool_voltage = 4180;
	OF_PROP_READ(node, "cool-vbat-thr-mv",
			chg->temp_cool_voltage, retval, 1);
	if (chg->temp_cool_voltage < 0)
		chg->temp_cool_voltage = 4180;
	OF_PROP_READ(node, "ibatmax-little-cool-thr-ma",
			chg->temp_littel_cool_current, retval, 1);
	if (chg->temp_littel_cool_current < 0)
		chg->temp_littel_cool_current = 2000;
	OF_PROP_READ(node, "ibatmax-cool-thr-ma",
			chg->temp_cool_current, retval, 1);
	if (chg->temp_cool_current < 0)
		chg->temp_cool_current = 1140;
	/* read temp region settings */
	OF_PROP_READ(node, "cold-bat-decidegc",
			chg->BATT_TEMP_T0, retval, 1);
	chg->BATT_TEMP_T0 = 0 - chg->BATT_TEMP_T0;
	OF_PROP_READ(node, "little-cold-bat-decidegc",
			chg->BATT_TEMP_T1, retval, 1);
	OF_PROP_READ(node, "cool-bat-decidegc",
			chg->BATT_TEMP_T2, retval, 1);
	OF_PROP_READ(node, "little-cool-bat-decidegc",
			chg->BATT_TEMP_T3, retval, 1);
	OF_PROP_READ(node, "pre-normal-bat-decidegc",
			chg->BATT_TEMP_T4, retval, 1);
	OF_PROP_READ(node, "warm-bat-decidegc",
			chg->BATT_TEMP_T5, retval, 1);
	OF_PROP_READ(node, "hot-bat-decidegc",
			chg->BATT_TEMP_T6, retval, 1);
	chg->pd_not_supported = of_property_read_bool(node,
						"disable-pd");
#ifdef OP_SWARP_SUPPORTED
	OF_PROP_READ(node, "normal-bat-decidegc",
				chg->BATT_TEMP_NOR, retval, 1);
	chg->swarp_supported = of_property_read_bool(node,
						"op,swarp_supported");
#endif
	chg->check_batt_full_by_sw = of_property_read_bool(node,
				"op,sw-check-full-enable");
	rc = of_property_read_u32(node,
					"op,sw-iterm-ma",
					&chg->sw_iterm_ma);
	if (rc < 0)
		chg->sw_iterm_ma = 130;
	pr_info("sw_iterm_ma=%d,check_batt_full_by_sw=%d",
				chg->sw_iterm_ma, chg->check_batt_full_by_sw);
	rc = of_property_read_u32(node,
					"op,little_cold_term_current",
					&chg->little_cold_iterm_ma);
	pr_info("little_cold_iterm_ma=%d", chg->little_cold_iterm_ma);
/* otg-icl set 1A if battery lower than 15%*/
	chg->OTG_ICL_CTRL = of_property_read_bool(node,
						"op,otg-icl-ctrl-enable");
	OF_PROP_READ(node, "otg-low-battery-thr",
			chg->OTG_LOW_BAT, retval, 1);
	if (retval < 0)
		chg->OTG_LOW_BAT = -EINVAL;
	OF_PROP_READ(node, "otg-low-bat-icl-thr",
			chg->OTG_LOW_BAT_ICL, retval, 1);
	if (retval < 0)
		chg->OTG_LOW_BAT_ICL = -EINVAL;
	OF_PROP_READ(node, "otg-normal-bat-icl-thr",
			chg->OTG_NORMAL_BAT_ICL, retval, 1);
	if (retval < 0)
		chg->OTG_NORMAL_BAT_ICL = -EINVAL;
	pr_info("OTG_ICL:enable:%d,CapThr:%d,LowThr:%d,NorThr:%d\n",
		chg->OTG_ICL_CTRL,
		chg->OTG_LOW_BAT,
		chg->OTG_LOW_BAT_ICL,
		chg->OTG_NORMAL_BAT_ICL);
	chg->shipmode_en = of_get_named_gpio_flags(node,
						"op,stm-ctrl-gpio", 0, &flags);
	chg->vbus_ctrl = of_get_named_gpio_flags(node,
					"op,vbus-ctrl-gpio", 0, &flags);
	chg->plug_irq = of_get_named_gpio_flags(node,
					"op,usb-check", 0, &flags);
	chg->vph_sel_disable = of_property_read_bool(node,
					"vph-sel-disable");
	/* read other settings */
	OF_PROP_READ(node, "qcom,cutoff-voltage-with-charger",
				smbchg_cutoff_volt_with_charger, retval, 1);
	chg->chg_enabled = !(of_property_read_bool(node,
						"qcom,charging-disabled"));
	chg->support_sdram_store_soc = of_property_read_bool(node,
						"op,support-sdram-store-soc");

	pr_info("T0=%d, T1=%d, T2=%d, T3=%d, T4=%d, T5=%d, T6=%d\n",
		chg->BATT_TEMP_T0, chg->BATT_TEMP_T1, chg->BATT_TEMP_T2,
		chg->BATT_TEMP_T3, chg->BATT_TEMP_T4, chg->BATT_TEMP_T5,
		chg->BATT_TEMP_T6);
	pr_info("BATT_TEMP_LITTLE_COLD=%d, %d, %d\n",
		chg->ibatmax[BATT_TEMP_LITTLE_COLD],
		chg->vbatmax[BATT_TEMP_LITTLE_COLD],
		chg->vbatdet[BATT_TEMP_LITTLE_COLD]);
	pr_info("BATT_TEMP_COOL=%d, %d, %d\n",
		chg->ibatmax[BATT_TEMP_COOL],
		chg->vbatmax[BATT_TEMP_COOL],
		chg->vbatdet[BATT_TEMP_COOL]);
	pr_info("BATT_TEMP_LITTLE_COOL=%d, %d, %d\n",
		chg->ibatmax[BATT_TEMP_LITTLE_COOL],
		chg->vbatmax[BATT_TEMP_LITTLE_COOL],
		chg->vbatdet[BATT_TEMP_LITTLE_COOL]);
	pr_info("BATT_TEMP_PRE_NORMAL=%d, %d, %d\n",
		chg->ibatmax[BATT_TEMP_PRE_NORMAL],
		chg->vbatmax[BATT_TEMP_PRE_NORMAL],
		chg->vbatdet[BATT_TEMP_PRE_NORMAL]);
	pr_info("BATT_TEMP_NORMAL=%d, %d, %d\n",
		chg->ibatmax[BATT_TEMP_NORMAL],
		chg->vbatmax[BATT_TEMP_NORMAL],
		chg->vbatdet[BATT_TEMP_NORMAL]);
	pr_info("BATT_TEMP_WARM=%d, %d, %d\n",
		chg->ibatmax[BATT_TEMP_WARM],
		chg->vbatmax[BATT_TEMP_WARM],
		chg->vbatdet[BATT_TEMP_WARM]);
	pr_info("cutoff_volt_with_charger=%d, disable-pd=%d\n",
		smbchg_cutoff_volt_with_charger, chg->pd_disabled);

	OF_PROP_READ(node, "op,fv-offset-voltage-mv",
			chg->fv_offset_voltage_mv, retval, 1);
	if (chg->fv_offset_voltage_mv <= 0)
		chg->fv_offset_voltage_mv =
			FV_OFFSET_VOLTAGE;
	pr_info("fv_offset_voltage_mv=%d\n",
		chg->fv_offset_voltage_mv);

	OF_PROP_READ(node, "op,normal-check-interval-period",
			chg->normal_check_interval_period, retval, 1);
	if (chg->normal_check_interval_period <= 0)
		chg->normal_check_interval_period =
			NORMAL_CHECK_INTERVAL_PERIOD;
	pr_info("normal_check_interval_period=%d\n",
		chg->normal_check_interval_period);

	OF_PROP_READ(node, "op,fast-check-interval-period",
			chg->fast_check_interval_period, retval, 1);
	if (chg->fast_check_interval_period <= 0)
		chg->fast_check_interval_period =
			FAST_CHECK_INTERVAL_PERIOD;
	pr_info("fast_check_interval_period=%d\n",
		chg->fast_check_interval_period);

	OF_PROP_READ(node, "op,fast-check-threshold-temp",
			chg->fast_check_threshold_temp, retval, 1);
	if (chg->fast_check_threshold_temp <= 0)
		chg->fast_check_threshold_temp =
			FAST_CHECK_THRESHOLD_TEMP;
	pr_info("fast_check_threshold_temp=%d\n",
		chg->fast_check_threshold_temp);

	OF_PROP_READ(node, "op,high-temp-short-check-timeout",
			chg->high_temp_short_check_timeout, retval, 1);
	if (chg->high_temp_short_check_timeout <= 0)
		chg->high_temp_short_check_timeout =
			HIGH_TEMP_SHORT_CHECK_TIMEOUT;
	pr_info("high_temp_short_check_timeout=%d\n",
		chg->high_temp_short_check_timeout);

	OF_PROP_READ(node, "op,first-protect-connecter-temp",
			chg->first_protect_connecter_temp, retval, 1);
	if (chg->first_protect_connecter_temp <= 0)
		chg->first_protect_connecter_temp =
			FIRST_PROTECT_CONNECTER_TEMP;
	pr_info("first_protect_connecter_temp=%d\n",
		chg->first_protect_connecter_temp);

	OF_PROP_READ(node, "op,second-protect-connecter-temp",
			chg->second_protect_connecter_temp, retval, 1);
	if (chg->second_protect_connecter_temp <= 0)
		chg->second_protect_connecter_temp =
			SECOND_PROTECT_CONNECTER_TEMP;
	pr_info("second_protect_connecter_temp=%d\n",
		chg->second_protect_connecter_temp);

	OF_PROP_READ(node, "op,second-protect-interval-temp",
			chg->second_protect_interval_temp, retval, 1);
	if (chg->second_protect_interval_temp <= 0)
		chg->second_protect_interval_temp =
			SECOND_PROTECT_INTERVAL_TEMP;
	pr_info("second_protect_interval_temp=%d\n",
		chg->second_protect_interval_temp);

	OF_PROP_READ(node, "op,third-protect-rise-rate",
			chg->third_protect_rise_rate, retval, 1);
	if (chg->third_protect_rise_rate <= 0)
		chg->third_protect_rise_rate =
			THIRD_PROTECT_RISE_RATE;
	pr_info("third_protect_rise_rate=%d\n",
		chg->third_protect_rise_rate);

	OF_PROP_READ(node, "op,third-protect-loop-temp",
			chg->third_protect_loop_temp, retval, 1);
	if (chg->third_protect_loop_temp <= 0)
		chg->third_protect_loop_temp =
			THIRD_PROTECT_LOOP_TEMP;
	pr_info("third_protect_loop_temp=%d\n",
		chg->third_protect_loop_temp);

	OF_PROP_READ(node, "op,third-protect-interval-temp",
			chg->third_protect_interval_temp, retval, 1);
	if (chg->third_protect_interval_temp <= 0)
		chg->third_protect_interval_temp =
			THIRD_PROTECT_INTERVAL_TEMP;
	pr_info("third_protect_interval_temp=%d\n",
		chg->third_protect_interval_temp);

	OF_PROP_READ(node, "op,third-protect-base-temp",
			chg->third_protect_base_temp, retval, 1);
	if (chg->third_protect_base_temp <= 0)
		chg->third_protect_base_temp =
			THIRD_PROTECT_BASE_TEMP;
	pr_info("third_protect_base_temp=%d\n",
		chg->third_protect_base_temp);

	OF_PROP_READ(node, "op,skin-thermal-high-threshold",
			chg->skin_thermal_high_threshold, retval, 1);
	if (chg->skin_thermal_high_threshold <= 0)
		chg->skin_thermal_high_threshold = 39;
	pr_info("skin_thermal_high_threshold=%d\n",
		chg->skin_thermal_high_threshold);

	OF_PROP_READ(node, "op,skin-thermal-pre-high-threshold",
			chg->skin_thermal_pre_high_threshold, retval, 1);
	if (chg->skin_thermal_pre_high_threshold <= 0)
		chg->skin_thermal_pre_high_threshold = 38;
	pr_info("skin_thermal_pre_high_threshold=%d\n",
		chg->skin_thermal_pre_high_threshold);

	OF_PROP_READ(node, "op,skin-thermal-warm-threshold",
			chg->skin_thermal_warm_threshold, retval, 1);
	if (chg->skin_thermal_warm_threshold <= 0)
		chg->skin_thermal_warm_threshold = 37;
	pr_info("skin_thermal_warm_threshold=%d\n",
		chg->skin_thermal_warm_threshold);

	OF_PROP_READ(node, "op,skin-thermal-pre-warm-threshold",
		chg->skin_thermal_pre_warm_threshold, retval, 1);
	if (chg->skin_thermal_pre_warm_threshold <= 0)
		chg->skin_thermal_pre_warm_threshold = 36;
	pr_info("skin_thermal_pre_warm_threshold=%d\n",
		chg->skin_thermal_pre_warm_threshold);

	OF_PROP_READ(node, "op,skin-thermal-medium-threshold",
			chg->skin_thermal_medium_threshold, retval, 1);
	if (chg->skin_thermal_medium_threshold <= 0)
		chg->skin_thermal_medium_threshold = 34;
	pr_info("skin_thermal_medium_threshold=%d\n",
		chg->skin_thermal_medium_threshold);

	OF_PROP_READ(node, "op,skin-thermal-normal-threshold",
			chg->skin_thermal_normal_threshold, retval, 1);
	if (chg->skin_thermal_normal_threshold <= 0)
		chg->skin_thermal_normal_threshold = 33;
	pr_info("skin_thermal_normal_threshold=%d\n",
		chg->skin_thermal_normal_threshold);

	OF_PROP_READ(node, "op,skin-thermal-lcdoff-high-threshold",
			chg->skin_thermal_lcdoff_high_threshold, retval, 1);
	if (chg->skin_thermal_lcdoff_high_threshold <= 0)
		chg->skin_thermal_lcdoff_high_threshold =
			SKIN_THERMAL_HIGH;
	pr_info("skin_thermal_lcdoff_high_threshold=%d\n",
		chg->skin_thermal_lcdoff_high_threshold);

	OF_PROP_READ(node, "op,skin-thermal-lcdoff-pre-high-threshold",
			chg->skin_thermal_lcdoff_pre_high_threshold, retval, 1);
	if (chg->skin_thermal_lcdoff_pre_high_threshold <= 0)
		chg->skin_thermal_lcdoff_pre_high_threshold =
			SKIN_THERMAL_PRE_HIGH;
	pr_info("skin_thermal_lcdoff_pre_high_threshold=%d\n",
		chg->skin_thermal_lcdoff_pre_high_threshold);

	OF_PROP_READ(node, "op,skin-thermal-lcdoff-medium-threshold",
			chg->skin_thermal_lcdoff_medium_threshold, retval, 1);
	if (chg->skin_thermal_lcdoff_medium_threshold <= 0)
		chg->skin_thermal_lcdoff_medium_threshold =
			SKIM_THERMAL_MEDIUM;
	pr_info("skin_thermal_lcdoff_medium_threshold=%d\n",
		chg->skin_thermal_lcdoff_medium_threshold);

	OF_PROP_READ(node, "op,skin-thermal-lcdoff-normal-threshold",
			chg->skin_thermal_lcdoff_normal_threshold, retval, 1);
	if (chg->skin_thermal_lcdoff_normal_threshold <= 0)
		chg->skin_thermal_lcdoff_normal_threshold =
			SKIN_THERMAL_NORMAL;
	pr_info("skin_thermal_lcdoff_normal_threshold=%d\n",
		chg->skin_thermal_lcdoff_normal_threshold);

	chg->enable_dash_current_adjust = of_property_read_bool(node,
					"op,enable-dash-current-dynamic-adjust");
	pr_info("enable_dash_current_adjust=%d\n",
		chg->enable_dash_current_adjust);

	OF_PROP_READ(node, "op,nor-skin-thermal-high-threshold",
			chg->nor_skin_thermal_high_threshold, retval, 1);
	if (chg->nor_skin_thermal_high_threshold <= 0)
		chg->nor_skin_thermal_high_threshold = 39;
	pr_info("nor_skin_thermal_high_threshold=%d\n",
		chg->nor_skin_thermal_high_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-pre-high-threshold",
			chg->nor_skin_thermal_pre_high_threshold, retval, 1);
	if (chg->nor_skin_thermal_pre_high_threshold <= 0)
		chg->nor_skin_thermal_pre_high_threshold = 38;
	pr_info("nor_skin_thermal_pre_high_threshold=%d\n",
		chg->nor_skin_thermal_pre_high_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-warm-threshold",
			chg->nor_skin_thermal_warm_threshold, retval, 1);
	if (chg->nor_skin_thermal_warm_threshold <= 0)
		chg->nor_skin_thermal_warm_threshold = 37;
	pr_info("nor_skin_thermal_warm_threshold=%d\n",
		chg->nor_skin_thermal_warm_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-pre-warm-threshold",
		chg->nor_skin_thermal_pre_warm_threshold, retval, 1);
	if (chg->nor_skin_thermal_pre_warm_threshold <= 0)
		chg->nor_skin_thermal_pre_warm_threshold = 36;
	pr_info("nor_skin_thermal_pre_warm_threshold=%d\n",
		chg->nor_skin_thermal_pre_warm_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-medium-threshold",
			chg->nor_skin_thermal_medium_threshold, retval, 1);
	if (chg->nor_skin_thermal_medium_threshold <= 0)
		chg->nor_skin_thermal_medium_threshold = 34;
	pr_info("nor_skin_thermal_medium_threshold=%d\n",
		chg->nor_skin_thermal_medium_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-normal-threshold",
			chg->nor_skin_thermal_normal_threshold, retval, 1);
	if (chg->nor_skin_thermal_normal_threshold <= 0)
		chg->nor_skin_thermal_normal_threshold = 33;
	pr_info("nor_skin_thermal_normal_threshold=%d\n",
		chg->nor_skin_thermal_normal_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-lcdoff-high-threshold",
			chg->nor_skin_thermal_lcdoff_high_threshold, retval, 1);
	if (chg->nor_skin_thermal_lcdoff_high_threshold <= 0)
		chg->nor_skin_thermal_lcdoff_high_threshold = 45;
	pr_info("nor_skin_thermal_lcdoff_high_threshold=%d\n",
		chg->nor_skin_thermal_lcdoff_high_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-lcdoff-pre-high-threshold",
			chg->nor_skin_thermal_lcdoff_pre_high_threshold, retval, 1);
	if (chg->nor_skin_thermal_lcdoff_pre_high_threshold <= 0)
		chg->nor_skin_thermal_lcdoff_pre_high_threshold = 44;
	pr_info("nor_skin_thermal_lcdoff_pre_high_threshold=%d\n",
		chg->nor_skin_thermal_lcdoff_pre_high_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-lcdoff-warm-threshold",
		chg->nor_skin_thermal_lcdoff_warm_threshold, retval, 1);
	if (chg->nor_skin_thermal_lcdoff_warm_threshold <= 0)
		chg->nor_skin_thermal_lcdoff_warm_threshold = 44;
	pr_info("nor_skin_thermal_lcdoff_warm_threshold=%d\n",
		chg->nor_skin_thermal_lcdoff_warm_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-lcdoff-pre-warm-threshold",
		chg->nor_skin_thermal_lcdoff_pre_warm_threshold, retval, 1);
	if (chg->nor_skin_thermal_lcdoff_pre_warm_threshold <= 0)
		chg->nor_skin_thermal_lcdoff_pre_warm_threshold = 42;
	pr_info("nor_skin_thermal_lcdoff_pre_warm_threshold=%d\n",
		chg->nor_skin_thermal_lcdoff_pre_warm_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-lcdoff-medium-threshold",
			chg->nor_skin_thermal_lcdoff_medium_threshold, retval, 1);
	if (chg->nor_skin_thermal_lcdoff_medium_threshold <= 0)
		chg->nor_skin_thermal_lcdoff_medium_threshold = 42;
	pr_info("nor_skin_thermal_lcdoff_medium_threshold=%d\n",
		chg->nor_skin_thermal_lcdoff_medium_threshold);

	OF_PROP_READ(node, "op,nor-skin-thermal-lcdoff-normal-threshold",
			chg->nor_skin_thermal_lcdoff_normal_threshold, retval, 1);
	if (chg->nor_skin_thermal_lcdoff_normal_threshold <= 0)
		chg->nor_skin_thermal_lcdoff_normal_threshold = 41;
	pr_info("nor_skin_thermal_lcdoff_normal_threshold=%d\n",
		chg->nor_skin_thermal_lcdoff_normal_threshold);

	OF_PROP_READ(node, "op,nor-call-on-current-ma",
			chg->nor_call_on_current_ma, retval, 1);
	if (chg->nor_call_on_current_ma <= 0)
		chg->nor_call_on_current_ma = 500;
	pr_info("nor_call_on_current_ma=%d\n",
		chg->nor_call_on_current_ma);

	OF_PROP_READ(node, "op,nor-skin-thermal-high-current-ma",
			chg->nor_skin_thermal_hi_current_ma, retval, 1);
	if (chg->nor_skin_thermal_hi_current_ma <= 0)
		chg->nor_skin_thermal_hi_current_ma = 500;
	pr_info("nor_skin_thermal_hi_current_ma=%d\n",
		chg->nor_skin_thermal_hi_current_ma);

	OF_PROP_READ(node, "op,nor-skin-thermal-warm-current-ma",
			chg->nor_skin_thermal_wrm_current_ma, retval, 1);
	if (chg->nor_skin_thermal_wrm_current_ma <= 0)
		chg->nor_skin_thermal_wrm_current_ma = 800;
	pr_info("nor_skin_thermal_wrm_current_ma=%d\n",
		chg->nor_skin_thermal_wrm_current_ma);

	OF_PROP_READ(node, "op,nor-skin-thermal-medium-current-ma",
			chg->nor_skin_thermal_med_current_ma, retval, 1);
	if (chg->nor_skin_thermal_med_current_ma <= 0)
		chg->nor_skin_thermal_med_current_ma = 1000;
	pr_info("nor_skin_thermal_med_current_ma=%d\n",
		chg->nor_skin_thermal_med_current_ma);

	chg->enable_nor_current_adjust = of_property_read_bool(node,
					"op,enable-nor-current-dynamic-adjust");
	pr_info("enable_nor_current_adjust=%d\n",
		chg->enable_nor_current_adjust);
	chg->check_slow_charge = of_property_read_bool(node,
							"op,slowy-charge-check");
	pr_info("op,slowy-charge-check=%d\n",
				chg->check_slow_charge);

	OF_PROP_READ(node, "op,full-count-sw-numb",
			chg->full_count_sw_num, retval, 1);
	if (chg->full_count_sw_num <= 0)
		chg->full_count_sw_num =
			FULL_COUNT_SW_NUM;
	pr_info("full_count_sw_num=%d\n",
		chg->full_count_sw_num);
	/* disable step_chg */
	chg->step_chg_enabled = false;

	chg->typec_legacy_use_rp_icl = of_property_read_bool(node,
				"qcom,typec-legacy-rp-icl");

	chg->sw_jeita_enabled = of_property_read_bool(node,
				"qcom,sw-jeita-enable");

	chg->pd_not_supported = chg->pd_not_supported ||
			of_property_read_bool(node, "qcom,usb-pd-disable");

	chg->pd_9vsupported = of_property_read_bool(node, "op,pd-support9v-enable");

	chg->support_hvdcp = of_property_read_bool(node, "op,support-hvdcp");

	rc = of_property_read_u32(node, "op,pdqc-to-5v-vbat-thr",
				  &chg->qcpd_to_5v_vbat_thr);
	if (rc < 0)
		chg->qcpd_to_5v_vbat_thr = 4100;

	rc = of_property_read_u32(node, "op,pdqc-9v-vbat-thr",
				  &chg->qcpd_9v_vbat_thr);
	if (rc < 0)
		chg->qcpd_9v_vbat_thr = 4000;

	chg->lpd_disabled = of_property_read_bool(node, "qcom,lpd-disable");

	rc = of_property_read_u32(node, "qcom,wd-bark-time-secs",
					&chip->dt.wd_bark_time);
	if (rc < 0 || chip->dt.wd_bark_time < MIN_WD_BARK_TIME)
		chip->dt.wd_bark_time = DEFAULT_WD_BARK_TIME;

	rc = of_property_read_u32(node, "qcom,wd-snarl-time-config",
					&chip->dt.wd_snarl_time_cfg);
	if (rc < 0)
		chip->dt.wd_snarl_time_cfg = DEFAULT_WD_SNARL_TIME_8S;

	chip->dt.no_battery = of_property_read_bool(node,
						"qcom,batteryless-platform");

	rc = of_property_read_u32(node,
			"qcom,fcc-max-ua", &chip->dt.batt_profile_fcc_ua);
	if (rc < 0)
		chip->dt.batt_profile_fcc_ua = -EINVAL;

	rc = of_property_read_u32(node,
				"qcom,fv-max-uv", &chip->dt.batt_profile_fv_uv);
	if (rc < 0)
		chip->dt.batt_profile_fv_uv = -EINVAL;

	rc = of_property_read_u32(node,
				"qcom,usb-icl-ua", &chip->dt.usb_icl_ua);
	if (rc < 0)
		chip->dt.usb_icl_ua = -EINVAL;

	rc = of_property_read_u32(node,
				"qcom,otg-cl-ua", &chg->otg_cl_ua);
	if (rc < 0)
		chg->otg_cl_ua = (chip->chg.chg_param.smb_version == PMI632_SUBTYPE) ?
							MICRO_1PA : MICRO_3PA;

	rc = of_property_read_u32(node, "qcom,chg-term-src",
			&chip->dt.term_current_src);
	if (rc < 0)
		chip->dt.term_current_src = ITERM_SRC_UNSPECIFIED;

	rc = of_property_read_u32(node, "qcom,chg-term-current-ma",
			&chip->dt.term_current_thresh_hi_ma);

	if (chip->dt.term_current_src == ITERM_SRC_ADC)
		rc = of_property_read_u32(node, "qcom,chg-term-base-current-ma",
				&chip->dt.term_current_thresh_lo_ma);

	if (of_find_property(node, "qcom,thermal-mitigation", &byte_len)) {
		chg->thermal_mitigation = devm_kzalloc(chg->dev, byte_len,
			GFP_KERNEL);

		if (chg->thermal_mitigation == NULL)
			return -ENOMEM;

		chg->thermal_levels = byte_len / sizeof(u32);
		rc = of_property_read_u32_array(node,
				"qcom,thermal-mitigation",
				chg->thermal_mitigation,
				chg->thermal_levels);
		if (rc < 0) {
			dev_err(chg->dev,
				"Couldn't read threm limits rc = %d\n", rc);
			return rc;
		}
	}

	rc = of_property_read_u32(node, "qcom,charger-temp-max",
			&chg->charger_temp_max);
	if (rc < 0)
		chg->charger_temp_max = -EINVAL;

	rc = of_property_read_u32(node, "qcom,smb-temp-max",
			&chg->smb_temp_max);
	if (rc < 0)
		chg->smb_temp_max = -EINVAL;

	rc = of_property_read_u32(node, "qcom,float-option",
						&chip->dt.float_option);
	if (!rc && (chip->dt.float_option < 0 || chip->dt.float_option > 4)) {
		pr_err("qcom,float-option is out of range [0, 4]\n");
		return -EINVAL;
	}

	chip->dt.hvdcp_disable = of_property_read_bool(node,
						"qcom,hvdcp-disable");
	chg->hvdcp_disable = chip->dt.hvdcp_disable;

	chip->dt.hvdcp_autonomous = of_property_read_bool(node,
						"qcom,hvdcp-autonomous-enable");

	chip->dt.auto_recharge_soc = -EINVAL;
	rc = of_property_read_u32(node, "qcom,auto-recharge-soc",
				&chip->dt.auto_recharge_soc);
	if (!rc && (chip->dt.auto_recharge_soc < 0 ||
			chip->dt.auto_recharge_soc > 100)) {
		pr_err("qcom,auto-recharge-soc is incorrect\n");
		return -EINVAL;
	}
	chg->auto_recharge_soc = chip->dt.auto_recharge_soc;

	chg->suspend_input_on_debug_batt = of_property_read_bool(node,
					"qcom,suspend-input-on-debug-batt");

	chg->fake_chg_status_on_debug_batt = of_property_read_bool(node,
					"qcom,fake-chg-status-on-debug-batt");

	rc = of_property_read_u32(node, "qcom,otg-deglitch-time-ms",
					&chg->otg_delay_ms);
	if (rc < 0)
		chg->otg_delay_ms = OTG_DEFAULT_DEGLITCH_TIME_MS;

	rc = of_property_match_string(node, "io-channel-names",
			"gpio1_voltage");
	if (rc >= 0) {
		chg->iio.op_connector_temp_chan = iio_channel_get(chg->dev,
				"gpio1_voltage");
		if (IS_ERR(chg->iio.op_connector_temp_chan)) {
			rc = PTR_ERR(chg->iio.op_connector_temp_chan);
			if (rc != -EPROBE_DEFER)
				dev_err(chg->dev,
				"op_connector_temp_chan channel unavailable,%d\n",
				rc);
			chg->iio.op_connector_temp_chan = NULL;
			return rc;
		}
	}

	rc = of_property_match_string(node, "io-channel-names",
			"gpio8_voltage");
	if (rc >= 0) {
		chg->iio.op_connector_temp_chan_sec = iio_channel_get(chg->dev,
				"gpio8_voltage");
		if (IS_ERR(chg->iio.op_connector_temp_chan_sec)) {
			rc = PTR_ERR(chg->iio.op_connector_temp_chan_sec);
			if (rc != -EPROBE_DEFER)
				dev_err(chg->dev,
				"op_connector_temp_chan_sec channel unavailable,%d\n",
				rc);
			chg->iio.op_connector_temp_chan_sec = NULL;
			return rc;
		}
	}

	rc = of_property_match_string(node, "io-channel-names",
			"skin_therm");
	if (rc >= 0) {
		chg->iio.op_skin_therm_chan = iio_channel_get(chg->dev,
				"skin_therm");
		if (IS_ERR(chg->iio.op_skin_therm_chan)) {
			rc = PTR_ERR(chg->iio.op_skin_therm_chan);
			if (rc != -EPROBE_DEFER)
				dev_err(chg->dev,
				"op_skin_therm_chan channel unavailable,%d\n",
				rc);
			chg->iio.op_skin_therm_chan = NULL;
			return rc;
		}
	}

	chg->fcc_stepper_enable = of_property_read_bool(node,
					"qcom,fcc-stepping-enable");
	chg->low_voltage_charger = of_property_read_bool(node,
					"op,low-voltage-charger");
	if (chg->uusb_moisture_protection_capable)
		chg->uusb_moisture_protection_enabled =
			of_property_read_bool(node,
					"qcom,uusb-moisture-protection-enable");

	chg->hw_die_temp_mitigation = of_property_read_bool(node,
					"qcom,hw-die-temp-mitigation");

	chg->hw_connector_mitigation = of_property_read_bool(node,
					"qcom,hw-connector-mitigation");

	chg->hw_skin_temp_mitigation = of_property_read_bool(node,
					"qcom,hw-skin-temp-mitigation");

	chg->en_skin_therm_mitigation = of_property_read_bool(node,
					"qcom,en-skin-therm-mitigation");

	chg->connector_pull_up = -EINVAL;
	of_property_read_u32(node, "qcom,connector-internal-pull-kohm",
					&chg->connector_pull_up);

	chip->dt.disable_suspend_on_collapse = of_property_read_bool(node,
					"qcom,disable-suspend-on-collapse");
	chg->smb_pull_up = -EINVAL;
	of_property_read_u32(node, "qcom,smb-internal-pull-kohm",
					&chg->smb_pull_up);

	chip->dt.adc_based_aicl = of_property_read_bool(node,
					"qcom,adc-based-aicl");

	of_property_read_u32(node, "qcom,fcc-step-delay-ms",
					&chg->chg_param.fcc_step_delay_ms);
	if (chg->chg_param.fcc_step_delay_ms <= 0)
		chg->chg_param.fcc_step_delay_ms =
					DEFAULT_FCC_STEP_UPDATE_DELAY_MS;

	of_property_read_u32(node, "qcom,fcc-step-size-ua",
					&chg->chg_param.fcc_step_size_ua);
	if (chg->chg_param.fcc_step_size_ua <= 0)
		chg->chg_param.fcc_step_size_ua = DEFAULT_FCC_STEP_SIZE_UA;

	/*
	 * If property is present parallel charging with CP is disabled
	 * with HVDCP3 adapter.
	 */
	chg->hvdcp3_standalone_config = of_property_read_bool(node,
					"qcom,hvdcp3-standalone-config");

	of_property_read_u32(node, "qcom,hvdcp3-max-icl-ua",
					&chg->chg_param.hvdcp3_max_icl_ua);
	if (chg->chg_param.hvdcp3_max_icl_ua <= 0)
		chg->chg_param.hvdcp3_max_icl_ua = MICRO_3PA;

	of_property_read_u32(node, "qcom,hvdcp2-max-icl-ua",
					&chg->chg_param.hvdcp2_max_icl_ua);
	if (chg->chg_param.hvdcp2_max_icl_ua <= 0)
		chg->chg_param.hvdcp2_max_icl_ua = MICRO_3PA;

	/* Used only in Adapter CV mode of operation */
	of_property_read_u32(node, "qcom,qc4-max-icl-ua",
					&chg->chg_param.qc4_max_icl_ua);
	if (chg->chg_param.qc4_max_icl_ua <= 0)
		chg->chg_param.qc4_max_icl_ua = MICRO_4PA;

	return 0;
}

static int smb5_parse_dt_adc_channels(struct smb_charger *chg)
{
	int rc = 0;

	rc = smblib_get_iio_channel(chg, "mid_voltage", &chg->iio.mid_chan);
	if (rc < 0)
		return rc;

	if (!chg->iio.mid_chan || chg->low_voltage_charger) {
		rc = smblib_get_iio_channel(chg, "usb_in_voltage",
				&chg->iio.usbin_v_chan);
		if (rc < 0)
			return rc;

		if (!chg->iio.usbin_v_chan) {
			dev_err(chg->dev, "No voltage channel defined");
			return -EINVAL;
		}
	}

	rc = smblib_get_iio_channel(chg, "chg_temp", &chg->iio.temp_chan);
	if (rc < 0)
		return rc;

	rc = smblib_get_iio_channel(chg, "usb_in_current",
					&chg->iio.usbin_i_chan);
	if (rc < 0)
		return rc;

	rc = smblib_get_iio_channel(chg, "sbux_res", &chg->iio.sbux_chan);
	if (rc < 0)
		return rc;

	rc = smblib_get_iio_channel(chg, "vph_voltage", &chg->iio.vph_v_chan);
	if (rc < 0)
		return rc;

	rc = smblib_get_iio_channel(chg, "die_temp", &chg->iio.die_temp_chan);
	if (rc < 0)
		return rc;

	rc = smblib_get_iio_channel(chg, "conn_temp",
					&chg->iio.connector_temp_chan);
	if (rc < 0)
		return rc;

	rc = smblib_get_iio_channel(chg, "skin_temp", &chg->iio.skin_temp_chan);
	if (rc < 0)
		return rc;

	rc = smblib_get_iio_channel(chg, "smb_temp", &chg->iio.smb_temp_chan);
	if (rc < 0)
		return rc;

	return 0;
}

static int smb5_parse_dt_currents(struct smb5 *chip, struct device_node *node)
{
	int rc = 0, tmp;
	struct smb_charger *chg = &chip->chg;

	rc = of_property_read_u32(node,
			"qcom,fcc-max-ua", &chip->dt.batt_profile_fcc_ua);
	if (rc < 0)
		chip->dt.batt_profile_fcc_ua = -EINVAL;

	rc = of_property_read_u32(node,
				"qcom,usb-icl-ua", &chip->dt.usb_icl_ua);
	if (rc < 0)
		chip->dt.usb_icl_ua = -EINVAL;
	chg->dcp_icl_ua = chip->dt.usb_icl_ua;

	rc = of_property_read_u32(node,
				"qcom,otg-cl-ua", &chg->otg_cl_ua);
	if (rc < 0)
		chg->otg_cl_ua =
			(chip->chg.chg_param.smb_version == PMI632_SUBTYPE) ?
							MICRO_1PA : MICRO_3PA;

	rc = of_property_read_u32(node, "qcom,chg-term-src",
			&chip->dt.term_current_src);
	if (rc < 0)
		chip->dt.term_current_src = ITERM_SRC_UNSPECIFIED;

	if (chip->dt.term_current_src == ITERM_SRC_ADC)
		rc = of_property_read_u32(node, "qcom,chg-term-base-current-ma",
				&chip->dt.term_current_thresh_lo_ma);

	rc = of_property_read_u32(node, "qcom,chg-term-current-ma",
			&chip->dt.term_current_thresh_hi_ma);

	chg->wls_icl_ua = DCIN_ICL_MAX_UA;
	rc = of_property_read_u32(node, "qcom,wls-current-max-ua",
			&tmp);
	if (!rc && tmp < DCIN_ICL_MAX_UA)
		chg->wls_icl_ua = tmp;

	return 0;
}

static int smb5_parse_dt_voltages(struct smb5 *chip, struct device_node *node)
{
	int rc = 0;

	rc = of_property_read_u32(node,
				"qcom,fv-max-uv", &chip->dt.batt_profile_fv_uv);
	if (rc < 0)
		chip->dt.batt_profile_fv_uv = -EINVAL;

	rc = of_property_read_u32(node, "qcom,chg-inhibit-threshold-mv",
				&chip->dt.chg_inhibit_thr_mv);
	if (!rc && (chip->dt.chg_inhibit_thr_mv < 0 ||
				chip->dt.chg_inhibit_thr_mv > 300)) {
		pr_err("qcom,chg-inhibit-threshold-mv is incorrect\n");
		return -EINVAL;
	}

	chip->dt.auto_recharge_vbat_mv = -EINVAL;
	rc = of_property_read_u32(node, "qcom,auto-recharge-vbat-mv",
				&chip->dt.auto_recharge_vbat_mv);
	if (!rc && (chip->dt.auto_recharge_vbat_mv < 0)) {
		pr_err("qcom,auto-recharge-vbat-mv is incorrect\n");
		return -EINVAL;
	}

	return 0;
}

static int smb5_parse_sdam(struct smb5 *chip, struct device_node *node)
{
	struct device_node *child;
	struct smb_charger *chg = &chip->chg;
	struct property *prop;
	const char *name;
	int rc;
	u32 base;
	u8 type;

	for_each_available_child_of_node(node, child) {
		of_property_for_each_string(child, "reg", prop, name) {
			rc = of_property_read_u32(child, "reg", &base);
			if (rc < 0) {
				pr_err("Failed to read base rc=%d\n", rc);
				return rc;
			}

			rc = smblib_read(chg, base + PERPH_TYPE_OFFSET, &type);
			if (rc < 0) {
				pr_err("Failed to read type rc=%d\n", rc);
				return rc;
			}

			switch (type) {
			case SDAM_TYPE:
				chg->sdam_base = base;
				break;
			default:
				break;
			}
		}
	}

	if (!chg->sdam_base)
		pr_debug("SDAM node not defined\n");

	return 0;
}

static int smb5_parse_dt(struct smb5 *chip)
{
	struct smb_charger *chg = &chip->chg;
	struct device_node *node = chg->dev->of_node;
	int rc = 0;

	if (!node) {
		pr_err("device tree node missing\n");
		return -EINVAL;
	}

	rc = smb5_parse_dt_voltages(chip, node);
	if (rc < 0)
		return rc;

	rc = smb5_parse_dt_currents(chip, node);
	if (rc < 0)
		return rc;

	rc = smb5_parse_dt_misc(chip, node);
	if (rc < 0)
		return rc;

	rc = smb5_parse_dt_adc_channels(chg);
	if (rc < 0)
		return rc;

	rc = smb5_parse_sdam(chip, node);
	if (rc < 0)
		return rc;

	return 0;
}

static int smb5_set_prop_comp_clamp_level(struct smb_charger *chg,
			     const union power_supply_propval *val)
{
	int rc = 0, i;
	struct clamp_config clamp_config;
	enum comp_clamp_levels level;

	level = val->intval;
	if (level >= MAX_CLAMP_LEVEL) {
		pr_err("Invalid comp clamp level=%d\n", val->intval);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(clamp_config.reg); i++) {
		rc = smblib_write(chg, clamp_levels[level].reg[i],
			     clamp_levels[level].val[i]);
		if (rc < 0)
			dev_err(chg->dev,
				"Failed to configure comp clamp settings for reg=0x%04x rc=%d\n",
				   clamp_levels[level].reg[i], rc);
	}

	chg->comp_clamp_level = val->intval;

	return rc;
}

/************************
 * USB PSY REGISTRATION *
 ************************/
static enum power_supply_property smb5_usb_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_SWARP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_PD_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_TYPEC_MODE,
	POWER_SUPPLY_PROP_TYPEC_POWER_ROLE,
	POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION,
	POWER_SUPPLY_PROP_OTG_SWITCH,
	POWER_SUPPLY_PROP_HW_DETECT,
	POWER_SUPPLY_PROP_OEM_TYPEC_CC_ORIENTATION,
	POWER_SUPPLY_PROP_LOW_POWER,
	POWER_SUPPLY_PROP_PD_ACTIVE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_SETTLED,
	POWER_SUPPLY_PROP_INPUT_CURRENT_NOW,
	POWER_SUPPLY_PROP_BOOST_CURRENT,
	POWER_SUPPLY_PROP_PE_START,
	POWER_SUPPLY_PROP_CTM_CURRENT_MAX,
	POWER_SUPPLY_PROP_HW_CURRENT_MAX,
	POWER_SUPPLY_PROP_REAL_TYPE,
	POWER_SUPPLY_PROP_PD_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_PD_VOLTAGE_MIN,
	POWER_SUPPLY_PROP_CONNECTOR_TYPE,
	POWER_SUPPLY_PROP_CONNECTOR_HEALTH,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_LIMIT,
	POWER_SUPPLY_PROP_SMB_EN_MODE,
	POWER_SUPPLY_PROP_SMB_EN_REASON,
	POWER_SUPPLY_PROP_ADAPTER_CC_MODE,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MOISTURE_DETECTED,
	POWER_SUPPLY_PROP_HVDCP_OPTI_ALLOWED,
	POWER_SUPPLY_PROP_QC_OPTI_DISABLE,
	POWER_SUPPLY_PROP_VOLTAGE_VPH,
	POWER_SUPPLY_PROP_THERM_ICL_LIMIT,
	POWER_SUPPLY_PROP_SKIN_HEALTH,
	POWER_SUPPLY_PROP_APSD_RERUN,
	POWER_SUPPLY_PROP_APSD_TIMEOUT,
	POWER_SUPPLY_PROP_CHARGER_STATUS,
	POWER_SUPPLY_PROP_INPUT_VOLTAGE_SETTLED,
	POWER_SUPPLY_PROP_ADAPTER_SID,
};

static int smb5_usb_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;
	u8 reg = 0, buff[2] = {0};
	val->intval = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		rc = smblib_get_prop_usb_present(chg, val);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		rc = smblib_get_usb_online(chg, val);
		break;
	case POWER_SUPPLY_PROP_SWARP_ONLINE:
		val->intval = chg->swarp_online;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		rc = smblib_get_prop_usb_voltage_max_design(chg, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		rc = smblib_get_prop_usb_voltage_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_LIMIT:
		if (chg->usbin_forced_max_uv)
			val->intval = chg->usbin_forced_max_uv;
		else
			smblib_get_prop_usb_voltage_max_design(chg, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		rc = smblib_get_prop_usb_voltage_now(chg, val);
		break;
	case POWER_SUPPLY_PROP_PD_CURRENT_MAX:
		val->intval = get_client_vote(chg->usb_icl_votable, PD_VOTER);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smblib_get_prop_input_current_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = POWER_SUPPLY_TYPE_USB_PD;
		break;
	case POWER_SUPPLY_PROP_REAL_TYPE:
		val->intval = chg->real_charger_type;
		break;
	case POWER_SUPPLY_PROP_TYPEC_MODE:
		rc = smblib_get_usb_prop_typec_mode(chg, val);
		break;
	case POWER_SUPPLY_PROP_OTG_SWITCH:
		val->intval = chg->otg_switch;
		break;
	case POWER_SUPPLY_PROP_HW_DETECT:
		val->intval = chg->hw_detect;
		break;

	case POWER_SUPPLY_PROP_TYPEC_POWER_ROLE:
		rc = smblib_get_prop_typec_power_role(chg, val);
		break;
	case POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION:
	case POWER_SUPPLY_PROP_OEM_TYPEC_CC_ORIENTATION:
		if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
			val->intval = 0;
		else
			rc = smblib_get_prop_typec_cc_orientation(chg, val);
		break;
	case POWER_SUPPLY_PROP_TYPEC_SRC_RP:
		rc = smblib_get_prop_typec_select_rp(chg, val);
		break;
	case POWER_SUPPLY_PROP_LOW_POWER:
		rc = smblib_get_prop_low_power(chg, val);
		break;
	case POWER_SUPPLY_PROP_PD_ACTIVE:
		val->intval = chg->pd_active;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_SETTLED:
		rc = smblib_get_prop_input_current_settled(chg, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_NOW:
		rc = smblib_get_prop_usb_current_now(chg, val);
		break;
	case POWER_SUPPLY_PROP_BOOST_CURRENT:
		val->intval = chg->boost_current_ua;
		break;
	case POWER_SUPPLY_PROP_PD_IN_HARD_RESET:
		rc = smblib_get_prop_pd_in_hard_reset(chg, val);
		break;
	case POWER_SUPPLY_PROP_PD_USB_SUSPEND_SUPPORTED:
		val->intval = chg->system_suspend_supported;
		break;
	case POWER_SUPPLY_PROP_PE_START:
		rc = smblib_get_pe_start(chg, val);
		break;
	case POWER_SUPPLY_PROP_CTM_CURRENT_MAX:
		val->intval = get_client_vote(chg->usb_icl_votable, CTM_VOTER);
		break;
	case POWER_SUPPLY_PROP_HW_CURRENT_MAX:
		rc = smblib_get_charge_current(chg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_PR_SWAP:
		rc = smblib_get_prop_pr_swap_in_progress(chg, val);
		break;
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MAX:
		val->intval = chg->voltage_max_uv;
		break;
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MIN:
		val->intval = chg->voltage_min_uv;
		break;
	case POWER_SUPPLY_PROP_SDP_CURRENT_MAX:
		val->intval = get_client_vote(chg->usb_icl_votable,
					      USB_PSY_VOTER);
		break;
	case POWER_SUPPLY_PROP_CONNECTOR_TYPE:
		val->intval = chg->connector_type;
		break;
	case POWER_SUPPLY_PROP_CONNECTOR_HEALTH:
		val->intval = smblib_get_prop_connector_health(chg);
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		rc = smblib_get_prop_scope(chg, val);
		break;
	case POWER_SUPPLY_PROP_SMB_EN_MODE:
		mutex_lock(&chg->smb_lock);
		val->intval = chg->sec_chg_selected;
		mutex_unlock(&chg->smb_lock);
		break;
	case POWER_SUPPLY_PROP_SMB_EN_REASON:
		val->intval = chg->cp_reason;
		break;
	case POWER_SUPPLY_PROP_MOISTURE_DETECTED:
		val->intval = chg->moisture_present;
		break;
	case POWER_SUPPLY_PROP_HVDCP_OPTI_ALLOWED:
		val->intval = !chg->flash_active;
		break;
	case POWER_SUPPLY_PROP_QC_OPTI_DISABLE:
		if (chg->hw_die_temp_mitigation)
			val->intval = POWER_SUPPLY_QC_THERMAL_BALANCE_DISABLE
					| POWER_SUPPLY_QC_INOV_THERMAL_DISABLE;
		if (chg->hw_connector_mitigation)
			val->intval |= POWER_SUPPLY_QC_CTM_DISABLE;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_VPH:
		rc = smblib_get_prop_vph_voltage_now(chg, val);
		break;
	case POWER_SUPPLY_PROP_THERM_ICL_LIMIT:
		val->intval = get_client_vote(chg->usb_icl_votable,
					THERMAL_THROTTLE_VOTER);
		break;
	case POWER_SUPPLY_PROP_ADAPTER_CC_MODE:
		val->intval = chg->adapter_cc_mode;
		break;
	case POWER_SUPPLY_PROP_SKIN_HEALTH:
		val->intval = smblib_get_skin_temp_status(chg);
		break;
	case POWER_SUPPLY_PROP_APSD_RERUN:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_APSD_TIMEOUT:
		val->intval = chg->apsd_ext_timeout;
		break;
	case POWER_SUPPLY_PROP_CHARGER_STATUS:
		val->intval = 0;
		if (chg->sdam_base) {
			rc = smblib_read(chg,
				chg->sdam_base + SDAM_QC_DET_STATUS_REG, &reg);
			if (!rc)
				val->intval = reg;
		}
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_SETTLED:
		val->intval = 0;
		if (chg->sdam_base) {
			rc = smblib_batch_read(chg,
				chg->sdam_base + SDAM_QC_ADC_LSB_REG, buff, 2);
			if (!rc)
				val->intval = (buff[1] << 8 | buff[0]) * 1038;
		}
		break;
	case POWER_SUPPLY_PROP_DISCONNECT_PD:
		val->intval = chg->disconnect_pd;
		break;
	case POWER_SUPPLY_PROP_ADAPTER_SID:
		val->intval = chg->adapter_sid;
		break;
	default:
		pr_err("get prop %d is not supported in usb\n", psp);
		rc = -EINVAL;
		break;
	}

	if (rc < 0) {
		pr_debug("Couldn't get prop %d rc = %d\n", psp, rc);
		return -ENODATA;
	}

	return 0;
}

#define MIN_THERMAL_VOTE_UA	500000
static int smb5_usb_set_prop(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int icl, rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_PD_CURRENT_MAX:
		rc = smblib_set_prop_pd_current_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_OTG_SWITCH:
		rc = vote(chg->otg_toggle_votable, USER_VOTER,
						val->intval, 0);
		break;
	case POWER_SUPPLY_PROP_RESET_RD:
		rc = smblib_set_prop_reset_rd(chg, val);
		break;
	case POWER_SUPPLY_PROP_TYPEC_POWER_ROLE:
		rc = smblib_set_prop_typec_power_role(chg, val);
		break;
	case POWER_SUPPLY_PROP_TYPEC_SRC_RP:
		rc = smblib_set_prop_typec_select_rp(chg, val);
		break;
	case POWER_SUPPLY_PROP_PD_ACTIVE:
		rc = smblib_set_prop_pd_active(chg, val);
		break;
	case POWER_SUPPLY_PROP_PD_IN_HARD_RESET:
		rc = smblib_set_prop_pd_in_hard_reset(chg, val);
		break;
	case POWER_SUPPLY_PROP_PD_USB_SUSPEND_SUPPORTED:
		chg->system_suspend_supported = val->intval;
		break;
	case POWER_SUPPLY_PROP_BOOST_CURRENT:
		rc = smblib_set_prop_boost_current(chg, val);
		break;
	case POWER_SUPPLY_PROP_CTM_CURRENT_MAX:
		rc = vote(chg->usb_icl_votable, CTM_VOTER,
						val->intval >= 0, val->intval);
		break;
	case POWER_SUPPLY_PROP_PR_SWAP:
		rc = smblib_set_prop_pr_swap_in_progress(chg, val);
		break;
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MAX:
		rc = smblib_set_prop_pd_voltage_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MIN:
		rc = smblib_set_prop_pd_voltage_min(chg, val);
		break;
	case POWER_SUPPLY_PROP_SDP_CURRENT_MAX:
		rc = smblib_set_prop_sdp_current_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_CONNECTOR_HEALTH:
		chg->connector_health = val->intval;
		power_supply_changed(chg->usb_psy);
		break;
	case POWER_SUPPLY_PROP_THERM_ICL_LIMIT:
		if (!is_client_vote_enabled(chg->usb_icl_votable,
						THERMAL_THROTTLE_VOTER)) {
			chg->init_thermal_ua = get_effective_result(
							chg->usb_icl_votable);
			icl = chg->init_thermal_ua + val->intval;
		} else {
			icl = get_client_vote(chg->usb_icl_votable,
					THERMAL_THROTTLE_VOTER) + val->intval;
		}

		if (icl >= MIN_THERMAL_VOTE_UA)
			rc = vote(chg->usb_icl_votable, THERMAL_THROTTLE_VOTER,
				(icl != chg->init_thermal_ua) ? true : false,
				icl);
		else
			rc = -EINVAL;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_LIMIT:
		smblib_set_prop_usb_voltage_max_limit(chg, val);
		break;
	case POWER_SUPPLY_PROP_ADAPTER_CC_MODE:
		chg->adapter_cc_mode = val->intval;
		break;
	case POWER_SUPPLY_PROP_APSD_RERUN:
		del_timer_sync(&chg->apsd_timer);
		chg->apsd_ext_timeout = false;
		smblib_rerun_apsd(chg);
		break;
	default:
		pr_err("set prop %d is not supported\n", psp);
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smb5_usb_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_OTG_SWITCH:
	case POWER_SUPPLY_PROP_RESET_RD:
	case POWER_SUPPLY_PROP_CTM_CURRENT_MAX:
	case POWER_SUPPLY_PROP_CONNECTOR_HEALTH:
	case POWER_SUPPLY_PROP_THERM_ICL_LIMIT:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_LIMIT:
	case POWER_SUPPLY_PROP_ADAPTER_CC_MODE:
	case POWER_SUPPLY_PROP_APSD_RERUN:
		return 1;
	default:
		break;
	}

	return 0;
}

static const struct power_supply_desc usb_psy_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB_PD,
	.properties = smb5_usb_props,
	.num_properties = ARRAY_SIZE(smb5_usb_props),
	.get_property = smb5_usb_get_prop,
	.set_property = smb5_usb_set_prop,
	.property_is_writeable = smb5_usb_prop_is_writeable,
};

static int smb5_init_usb_psy(struct smb5 *chip)
{
	struct power_supply_config usb_cfg = {};
	struct smb_charger *chg = &chip->chg;

	usb_cfg.drv_data = chip;
	usb_cfg.of_node = chg->dev->of_node;
	chg->usb_psy = devm_power_supply_register(chg->dev,
						  &usb_psy_desc,
						  &usb_cfg);
	if (IS_ERR(chg->usb_psy)) {
		pr_err("Couldn't register USB power supply\n");
		return PTR_ERR(chg->usb_psy);
	}

	return 0;
}

/********************************
 * USB PC_PORT PSY REGISTRATION *
 ********************************/
static enum power_supply_property smb5_usb_port_props[] = {
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static int smb5_usb_port_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = POWER_SUPPLY_TYPE_USB;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		rc = smblib_get_prop_usb_online(chg, val);
		if (!val->intval)
			break;

		if (((chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) ||
			(chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_MEDIUM) ||
			(chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH) ||
		   (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB))
			&& ((chg->real_charger_type == POWER_SUPPLY_TYPE_USB) ||
			(chg->real_charger_type == POWER_SUPPLY_TYPE_USB_CDP)))
			val->intval = 1;
		else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = 5000000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smblib_get_prop_input_current_settled(chg, val);
		break;
	default:
		pr_err_ratelimited("Get prop %d is not supported in pc_port\n",
				psp);
		return -EINVAL;
	}

	if (rc < 0) {
		pr_debug("Couldn't get prop %d rc = %d\n", psp, rc);
		return -ENODATA;
	}

	return 0;
}

static int smb5_usb_port_set_prop(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	int rc = 0;

	switch (psp) {
	default:
		pr_err_ratelimited("Set prop %d is not supported in pc_port\n",
				psp);
		rc = -EINVAL;
		break;
	}

	return rc;
}

static const struct power_supply_desc usb_port_psy_desc = {
	.name		= "pc_port",
	.type		= POWER_SUPPLY_TYPE_USB,
	.properties	= smb5_usb_port_props,
	.num_properties	= ARRAY_SIZE(smb5_usb_port_props),
	.get_property	= smb5_usb_port_get_prop,
	.set_property	= smb5_usb_port_set_prop,
};

static int smb5_init_usb_port_psy(struct smb5 *chip)
{
	struct power_supply_config usb_port_cfg = {};
	struct smb_charger *chg = &chip->chg;

	usb_port_cfg.drv_data = chip;
	usb_port_cfg.of_node = chg->dev->of_node;
	chg->usb_port_psy = devm_power_supply_register(chg->dev,
						  &usb_port_psy_desc,
						  &usb_port_cfg);
	if (IS_ERR(chg->usb_port_psy)) {
		pr_err("Couldn't register USB pc_port power supply\n");
		return PTR_ERR(chg->usb_port_psy);
	}

	return 0;
}

/*****************************
 * USB MAIN PSY REGISTRATION *
 *****************************/

static enum power_supply_property smb5_usb_main_props[] = {
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_SETTLED,
	POWER_SUPPLY_PROP_INPUT_VOLTAGE_SETTLED,
	POWER_SUPPLY_PROP_FCC_DELTA,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_FLASH_ACTIVE,
	POWER_SUPPLY_PROP_FLASH_TRIGGER,
	POWER_SUPPLY_PROP_TOGGLE_STAT,
	POWER_SUPPLY_PROP_MAIN_FCC_MAX,
	POWER_SUPPLY_PROP_IRQ_STATUS,
	POWER_SUPPLY_PROP_FORCE_MAIN_FCC,
	POWER_SUPPLY_PROP_FORCE_MAIN_ICL,
	POWER_SUPPLY_PROP_COMP_CLAMP_LEVEL,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_HOT_TEMP,
};

static int smb5_usb_main_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
#ifdef OP_SWARP_SUPPORTED
		if (chg->swarp_supported) {
			if (is_op_chg_available(chg)) {
				rc = power_supply_get_property(chg->op_chg_psy,	psp, val);
				if (rc) {
					pr_err("can't get op_chg_psy prop FV rc = %d\n", rc);
					val->intval = 0;
				}
			}
		} else {
#endif
			rc = smblib_get_charge_param(chg, &chg->param.fv, &val->intval);
#ifdef OP_SWARP_SUPPORTED
		}
#endif
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
#ifdef OP_SWARP_SUPPORTED
		if (chg->swarp_supported) {
			if (is_op_chg_available(chg)) {
				rc = power_supply_get_property(chg->op_chg_psy,	psp, val);
				if (rc) {
					pr_err("can't get op_chg_psy prop FCC rc = %d\n", rc);
					val->intval = 0;
				}
			}
		} else {
#endif
			rc = smblib_get_charge_param(chg, &chg->param.fcc,
							&val->intval);
#ifdef OP_SWARP_SUPPORTED
				}
#endif
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = POWER_SUPPLY_TYPE_MAIN;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_SETTLED:
		rc = smblib_get_prop_input_current_settled(chg, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_SETTLED:
		rc = smblib_get_prop_input_voltage_settled(chg, val);
		break;
	case POWER_SUPPLY_PROP_FCC_DELTA:
		rc = smblib_get_prop_fcc_delta(chg, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smblib_get_icl_current(chg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_FLASH_ACTIVE:
		val->intval = chg->flash_active;
		break;
	case POWER_SUPPLY_PROP_FLASH_TRIGGER:
		val->intval = 0;
		if (chg->chg_param.smb_version == PMI632_SUBTYPE)
			rc = schgm_flash_get_vreg_ok(chg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_TOGGLE_STAT:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_MAIN_FCC_MAX:
		val->intval = chg->main_fcc_max;
		break;
	case POWER_SUPPLY_PROP_IRQ_STATUS:
		rc = smblib_get_irq_status(chg, val);
		break;
	case POWER_SUPPLY_PROP_FORCE_MAIN_FCC:
		rc = smblib_get_charge_param(chg, &chg->param.fcc,
							&val->intval);
		break;
	case POWER_SUPPLY_PROP_FORCE_MAIN_ICL:
		rc = smblib_get_charge_param(chg, &chg->param.usb_icl,
							&val->intval);
		break;
	case POWER_SUPPLY_PROP_COMP_CLAMP_LEVEL:
		val->intval = chg->comp_clamp_level;
		break;
	/* Use this property to report SMB health */
	case POWER_SUPPLY_PROP_HEALTH:
		rc = val->intval = smblib_get_prop_smb_health(chg);
		break;
	/* Use this property to report overheat status */
	case POWER_SUPPLY_PROP_HOT_TEMP:
		val->intval = chg->thermal_overheat;
		break;
	default:
		pr_debug("get prop %d is not supported in usb-main\n", psp);
		rc = -EINVAL;
		break;
	}
	if (rc < 0)
		pr_debug("Couldn't get prop %d rc = %d\n", psp, rc);

	return rc;
}

static int smb5_usb_main_set_prop(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	union power_supply_propval pval = {0, };
	enum power_supply_type real_chg_type = chg->real_charger_type;
	int rc = 0, offset_ua = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
#ifdef OP_SWARP_SUPPORTED
		if (chg->swarp_supported) {
			if (is_op_chg_available(chg)) {
				rc = power_supply_set_property(chg->op_chg_psy,	psp, val);
				if (rc)
					pr_err("can't set op_chg_psy prop FV rc = %d\n", rc);
			}
		} else {
#endif
			rc = smblib_set_charge_param(chg, &chg->param.fv, val->intval);
#ifdef OP_SWARP_SUPPORTED
		}
#endif
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
#ifdef OP_SWARP_SUPPORTED
		if (chg->swarp_supported) {
			if (is_op_chg_available(chg)) {
				rc = power_supply_set_property(chg->op_chg_psy,	psp, val);
				if (rc)
					pr_err("can't set op_chg_psy prop FCC rc = %d\n", rc);
			}
		} else {
#endif
			/* Adjust Main FCC for QC3.0 + SMB1390 */
			rc = smblib_get_qc3_main_icl_offset(chg, &offset_ua);
			if (rc < 0)
				offset_ua = 0;

			rc = smblib_set_charge_param(chg, &chg->param.fcc,
							val->intval + offset_ua);
#ifdef OP_SWARP_SUPPORTED
		}
#endif
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smblib_set_icl_current(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_FLASH_ACTIVE:
		if ((chg->chg_param.smb_version == PMI632_SUBTYPE)
				&& (chg->flash_active != val->intval)) {
			chg->flash_active = val->intval;

			rc = smblib_get_prop_usb_present(chg, &pval);
			if (rc < 0)
				pr_err("Failed to get USB preset status rc=%d\n",
						rc);
			if (pval.intval) {
				rc = smblib_force_vbus_voltage(chg,
					chg->flash_active ? FORCE_5V_BIT
								: IDLE_BIT);
				if (rc < 0)
					pr_err("Failed to force 5V\n");
				else
					chg->pulse_cnt = 0;
			} else {
				/* USB absent & flash not-active - vote 100mA */
				vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER,
							true, SDP_100_MA);
			}

			pr_debug("flash active VBUS 5V restriction %s\n",
				chg->flash_active ? "applied" : "removed");

			/* Update userspace */
			if (chg->batt_psy)
				power_supply_changed(chg->batt_psy);
		}
		break;
	case POWER_SUPPLY_PROP_TOGGLE_STAT:
		rc = smblib_toggle_smb_en(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_MAIN_FCC_MAX:
		chg->main_fcc_max = val->intval;
		rerun_election(chg->fcc_votable);
		break;
	case POWER_SUPPLY_PROP_FORCE_MAIN_FCC:
		vote_override(chg->fcc_main_votable, CC_MODE_VOTER,
				(val->intval < 0) ? false : true, val->intval);
		if (val->intval >= 0)
			chg->chg_param.forced_main_fcc = val->intval;
		/*
		 * Remove low vote on FCC_MAIN, for WLS, to allow FCC_MAIN to
		 * rise to its full value.
		 */
		if (val->intval < 0)
			vote(chg->fcc_main_votable, WLS_PL_CHARGING_VOTER,
								false, 0);
		/* Main FCC updated re-calculate FCC */
		rerun_election(chg->fcc_votable);
		break;
	case POWER_SUPPLY_PROP_FORCE_MAIN_ICL:
		vote_override(chg->usb_icl_votable, CC_MODE_VOTER,
				(val->intval < 0) ? false : true, val->intval);
		/* Main ICL updated re-calculate ILIM */
		if (real_chg_type == POWER_SUPPLY_TYPE_USB_HVDCP_3 ||
			real_chg_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5)
			rerun_election(chg->fcc_votable);
		break;
	case POWER_SUPPLY_PROP_COMP_CLAMP_LEVEL:
		rc = smb5_set_prop_comp_clamp_level(chg, val);
		break;
	case POWER_SUPPLY_PROP_HOT_TEMP:
		rc = smblib_set_prop_thermal_overheat(chg, val->intval);
		break;
	default:
		pr_err("set prop %d is not supported\n", psp);
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smb5_usb_main_prop_is_writeable(struct power_supply *psy,
				enum power_supply_property psp)
{
	int rc;

	switch (psp) {
	case POWER_SUPPLY_PROP_TOGGLE_STAT:
	case POWER_SUPPLY_PROP_MAIN_FCC_MAX:
	case POWER_SUPPLY_PROP_FORCE_MAIN_FCC:
	case POWER_SUPPLY_PROP_FORCE_MAIN_ICL:
	case POWER_SUPPLY_PROP_COMP_CLAMP_LEVEL:
	case POWER_SUPPLY_PROP_HOT_TEMP:
		rc = 1;
		break;
	default:
		rc = 0;
		break;
	}

	return rc;
}

static const struct power_supply_desc usb_main_psy_desc = {
	.name		= "main",
	.type		= POWER_SUPPLY_TYPE_MAIN,
	.properties	= smb5_usb_main_props,
	.num_properties	= ARRAY_SIZE(smb5_usb_main_props),
	.get_property	= smb5_usb_main_get_prop,
	.set_property	= smb5_usb_main_set_prop,
	.property_is_writeable = smb5_usb_main_prop_is_writeable,
};

static int smb5_init_usb_main_psy(struct smb5 *chip)
{
	struct power_supply_config usb_main_cfg = {};
	struct smb_charger *chg = &chip->chg;

	usb_main_cfg.drv_data = chip;
	usb_main_cfg.of_node = chg->dev->of_node;
	chg->usb_main_psy = devm_power_supply_register(chg->dev,
						  &usb_main_psy_desc,
						  &usb_main_cfg);
	if (IS_ERR(chg->usb_main_psy)) {
		pr_err("Couldn't register USB main power supply\n");
		return PTR_ERR(chg->usb_main_psy);
	}

	return 0;
}

/*************************
 * DC PSY REGISTRATION   *
 *************************/

static enum power_supply_property smb5_dc_props[] = {
	POWER_SUPPLY_PROP_INPUT_SUSPEND,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION,
	POWER_SUPPLY_PROP_REAL_TYPE,
	POWER_SUPPLY_PROP_DC_RESET,
	POWER_SUPPLY_PROP_AICL_DONE,
};

static int smb5_dc_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		val->intval = get_effective_result(chg->dc_suspend_votable);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		rc = smblib_get_prop_dc_present(chg, val);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		rc = smblib_get_prop_dc_online(chg, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		rc = smblib_get_prop_dc_voltage_now(chg, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smblib_get_prop_dc_current_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		rc = smblib_get_prop_dc_voltage_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_REAL_TYPE:
		val->intval = POWER_SUPPLY_TYPE_WIRELESS;
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION:
		rc = smblib_get_prop_voltage_wls_output(chg, val);
		break;
	case POWER_SUPPLY_PROP_DC_RESET:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_AICL_DONE:
		val->intval = chg->dcin_aicl_done;
		break;
	default:
		return -EINVAL;
	}
	if (rc < 0) {
		pr_debug("Couldn't get prop %d rc = %d\n", psp, rc);
		return -ENODATA;
	}
	return 0;
}

static int smb5_dc_set_prop(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		rc = vote(chg->dc_suspend_votable, WBC_VOTER,
				(bool)val->intval, 0);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smblib_set_prop_dc_current_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION:
		rc = smblib_set_prop_voltage_wls_output(chg, val);
		break;
	case POWER_SUPPLY_PROP_DC_RESET:
		rc = smblib_set_prop_dc_reset(chg);
		break;
	default:
		return -EINVAL;
	}

	return rc;
}

static int smb5_dc_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		return 1;
	default:
		break;
	}

	return 0;
}

static const struct power_supply_desc dc_psy_desc = {
	.name = "dc",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = smb5_dc_props,
	.num_properties = ARRAY_SIZE(smb5_dc_props),
	.get_property = smb5_dc_get_prop,
	.set_property = smb5_dc_set_prop,
	.property_is_writeable = smb5_dc_prop_is_writeable,
};

static int smb5_init_dc_psy(struct smb5 *chip)
{
	struct power_supply_config dc_cfg = {};
	struct smb_charger *chg = &chip->chg;

	dc_cfg.drv_data = chip;
	dc_cfg.of_node = chg->dev->of_node;
	chg->dc_psy = devm_power_supply_register(chg->dev,
						  &dc_psy_desc,
						  &dc_cfg);
	if (IS_ERR(chg->dc_psy)) {
		pr_err("Couldn't register USB power supply\n");
		return PTR_ERR(chg->dc_psy);
	}

	return 0;
}

/*************************
 * BATT PSY REGISTRATION *
 *************************/
static enum power_supply_property smb5_batt_props[] = {
	POWER_SUPPLY_PROP_INPUT_SUSPEND,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_APSD_NOT_DONE,
	POWER_SUPPLY_PROP_FASTCHG_IS_OK,
	POWER_SUPPLY_PROP_FASTCHG_TEMP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHG_PROTECT_STATUS,
	POWER_SUPPLY_PROP_FASTCHG_STATUS,
#ifdef OP_SWARP_SUPPORTED
	POWER_SUPPLY_PROP_FASTCHG_TYPE,
	POWER_SUPPLY_PROP_ENGINEER_MODE,
#endif
	POWER_SUPPLY_PROP_FASTCHG_STARTING,
	POWER_SUPPLY_CUTOFF_VOLT_WITH_CHARGER,
	POWER_SUPPLY_PROP_CHARGING_ENABLED,
	POWER_SUPPLY_PROP_INPUT_CURRENT_MAX,
	POWER_SUPPLY_PROP_IS_AGING_TEST,
	POWER_SUPPLY_PROP_CONNECTER_TEMP_ONE,
	POWER_SUPPLY_PROP_CONNECTER_TEMP_TWO,
	POWER_SUPPLY_PROP_CONNECT_DISABLE,
	POWER_SUPPLY_PROP_CHARGER_TEMP,
	POWER_SUPPLY_PROP_CHARGER_TEMP_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMITED,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
//	POWER_SUPPLY_PROP_VOLTAGE_QNOVO,
	POWER_SUPPLY_PROP_CURRENT_NOW,
//	POWER_SUPPLY_PROP_CURRENT_QNOVO,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
//	POWER_SUPPLY_PROP_STEP_CHARGING_ENABLED,
//	POWER_SUPPLY_PROP_SW_JEITA_ENABLED,
	POWER_SUPPLY_PROP_CHARGE_DONE,
//	POWER_SUPPLY_PROP_PARALLEL_DISABLE,
	POWER_SUPPLY_PROP_SET_SHIP_MODE,
	POWER_SUPPLY_PROP_DIE_HEALTH,
	POWER_SUPPLY_PROP_RERUN_AICL,
//	POWER_SUPPLY_PROP_DP_DM,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_RECHARGE_SOC,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_FORCE_RECHARGE,
//	POWER_SUPPLY_PROP_FCC_STEPPER_ENABLE,
	POWER_SUPPLY_PROP_OP_DISABLE_CHARGE,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_DUMP_REG,
};

#define DEBUG_ACCESSORY_TEMP_DECIDEGC	250
static int smb5_batt_get_prop(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smb_charger *chg = power_supply_get_drvdata(psy);
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = get_prop_batt_status(chg);
		break;
	case POWER_SUPPLY_PROP_FASTCHG_IS_OK:
		val->intval = get_prop_fastchg_is_ok(chg)?1:0;
		break;
	case POWER_SUPPLY_PROP_FASTCHG_TEMP_STATUS:
		val->intval = chg->disallow_fast_high_temp;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		rc = smblib_get_prop_batt_health(chg, val);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		rc = smblib_get_prop_batt_present(chg, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		rc = smblib_get_prop_input_suspend(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		rc = smblib_get_prop_batt_charge_type(chg, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		rc = smblib_get_prop_batt_capacity(chg, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		rc = smblib_get_prop_batt_capacity_level(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		rc = smblib_get_prop_usb_voltage_now(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHG_PROTECT_STATUS:
		val->intval = get_prop_chg_protect_status(chg);
		break;
	case POWER_SUPPLY_PROP_FASTCHG_STATUS:
		val->intval = get_prop_fastchg_status(chg);
		break;
#ifdef OP_SWARP_SUPPORTED
	case POWER_SUPPLY_PROP_FASTCHG_TYPE:
		val->intval = chg->fastchg_type;
		break;
	case POWER_SUPPLY_PROP_ENGINEER_MODE:
		val->intval = chg->engineer_mode;
		break;
#endif
	case POWER_SUPPLY_CUTOFF_VOLT_WITH_CHARGER:
		val->intval = smbchg_cutoff_volt_with_charger;
		break;
	case POWER_SUPPLY_PROP_FASTCHG_STARTING:
		val->intval = op_get_fastchg_ing(chg);
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		val->intval = chg->chg_enabled;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_MAX:
		rc = smblib_get_prop_input_current_limited(chg, val);
		break;
	case POWER_SUPPLY_PROP_OP_DISABLE_CHARGE:
		val->intval = chg->chg_disabled;
		break;
	case POWER_SUPPLY_PROP_APSD_NOT_DONE:
		val->intval = chg->slow_charger;
		break;
	case POWER_SUPPLY_PROP_IS_AGING_TEST:
		val->intval = chg->is_aging_test;
		break;
	case POWER_SUPPLY_PROP_DUMP_REG:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CONNECTER_TEMP_ONE:
		val->intval = chg->connecter_temp_1;
		break;
	case POWER_SUPPLY_PROP_CONNECTER_TEMP_TWO:
		val->intval = chg->connecter_temp_2;
		break;
	case POWER_SUPPLY_PROP_CONNECT_DISABLE:
		val->intval = chg->disconnect_vbus;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		rc = smblib_get_prop_system_temp_level(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		rc = smblib_get_prop_system_temp_level_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGER_TEMP:
		rc = smblib_get_prop_charger_temp(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGER_TEMP_MAX:
		val->intval = chg->charger_temp_max;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMITED:
		rc = smblib_get_prop_input_current_limited(chg, val);
		break;
	case POWER_SUPPLY_PROP_STEP_CHARGING_ENABLED:
		val->intval = chg->step_chg_enabled;
		break;
	case POWER_SUPPLY_PROP_SW_JEITA_ENABLED:
		val->intval = chg->sw_jeita_enabled;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = get_client_vote(chg->fv_votable,
					      QNOVO_VOTER);
		if (val->intval < 0)
			val->intval = get_client_vote(chg->fv_votable,
						      BATT_PROFILE_VOTER);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_QNOVO:
		val->intval = get_client_vote_locked(chg->fv_votable,
				QNOVO_VOTER);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		rc = smblib_get_batt_current_now(chg, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_QNOVO:
		val->intval = get_client_vote_locked(chg->fcc_votable,
				QNOVO_VOTER);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = get_client_vote(chg->fcc_votable,
					      BATT_PROFILE_VOTER);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = get_effective_result(chg->fcc_votable);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		rc = smblib_get_prop_batt_iterm(chg, val);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY)
			val->intval = DEBUG_ACCESSORY_TEMP_DECIDEGC;
		else
			rc = smblib_get_prop_batt_temp(chg, val);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CHARGE_DONE:
		rc = smblib_get_prop_batt_charge_done(chg, val);
		break;
	case POWER_SUPPLY_PROP_PARALLEL_DISABLE:
		val->intval = get_client_vote(chg->pl_disable_votable,
					      USER_VOTER);
		break;
	case POWER_SUPPLY_PROP_SET_SHIP_MODE:
		/* Not in ship mode as long as device is active */
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_DIE_HEALTH:
		rc = smblib_get_die_health(chg, val);
		break;
	case POWER_SUPPLY_PROP_DP_DM:
		val->intval = chg->pulse_cnt;
		break;
	case POWER_SUPPLY_PROP_RERUN_AICL:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_CHARGE_COUNTER, val);
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_CYCLE_COUNT, val);
		break;
	case POWER_SUPPLY_PROP_RECHARGE_SOC:
		val->intval = chg->auto_recharge_soc;
		break;
	case POWER_SUPPLY_PROP_CHARGE_QNOVO_ENABLE:
		val->intval = 0;
		if (!chg->qnovo_disable_votable)
			chg->qnovo_disable_votable =
				find_votable("QNOVO_DISABLE");

		if (chg->qnovo_disable_votable)
			val->intval =
				!get_effective_result(
					chg->qnovo_disable_votable);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_CHARGE_FULL, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, val);
		break;
	case POWER_SUPPLY_PROP_FORCE_RECHARGE:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_FCC_STEPPER_ENABLE:
		val->intval = chg->fcc_stepper_enable;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_AVG:
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_TIME_TO_FULL_AVG, val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_TIME_TO_FULL_NOW, val);
		break;
	default:
		pr_err("batt power supply prop %d not supported\n", psp);
		return -EINVAL;
	}

	if (rc < 0) {
		pr_debug("Couldn't get prop %d rc = %d\n", psp, rc);
		return -ENODATA;
	}

	return 0;
}

static int smb5_batt_set_prop(struct power_supply *psy,
		enum power_supply_property prop,
		const union power_supply_propval *val)
{
	int rc = 0;
	struct smb_charger *chg = power_supply_get_drvdata(psy);

	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
		rc = smblib_set_prop_batt_status(chg, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		rc = smblib_set_prop_input_suspend(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		rc = smblib_set_prop_system_temp_level(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHECK_USB_UNPLUG:
		if (chg->vbus_present && !chg->dash_present)
			update_dash_unplug_status();
		break;
	case POWER_SUPPLY_PROP_SWITCH_DASH:
		rc = check_allow_switch_dash(chg, val);
		break;
	case POWER_SUPPLY_PROP_FASTCHG_IS_OK:
		rc = 0;
		break;
	case POWER_SUPPLY_PROP_DUMP_REG:
		if (val->intval == 1)
			schedule_work(&chg->dump_reg_work);
		break;
#ifdef OP_SWARP_SUPPORTED
	case POWER_SUPPLY_PROP_FASTCHG_TYPE:
		if (val->intval >= 0 && val->intval < CHARGER_MAX)
			chg->fastchg_type = val->intval;
		break;
	case POWER_SUPPLY_PROP_ENGINEER_MODE:
		chg->engineer_mode = val->intval;
		break;
#endif
	case POWER_SUPPLY_PROP_INPUT_CURRENT_MAX:
		pr_info("set iusb %d mA\n", val->intval);
		if (__debug_mask == PR_OP_DEBUG
			|| val->intval == 2000000 || val->intval == 1700000
			|| val->intval == 1500000 || val->intval == 1000000)
			op_usb_icl_set(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_OP_DISABLE_CHARGE:
		chg->chg_disabled = (bool)val->intval;
		vote(chg->chg_disable_votable, FORCE_RECHARGE_VOTER,
					(bool)val->intval, 0);
		if (val->intval) {
			chg->ignore_irq_flag = true;
			switch_mode_to_normal();
			op_set_fast_chg_allow(chg, false);
		}
		if (!chg->wls_psy)
			chg->wls_psy = power_supply_get_by_name("wireless");
		if (chg->wls_psy) {
			rc = power_supply_set_property(chg->wls_psy,
				POWER_SUPPLY_PROP_OP_DISABLE_CHARGE, val);
			if (rc < 0)
				pr_err("Couldn't disable wireless charge, rc=%d\n", rc);
		}
		if (!chg->chg_disabled) {
			schedule_delayed_work(&chg->check_switch_dash_work,
					      msecs_to_jiffies(3000));
		} else
			schedule_delayed_work(&chg->disable_chg_flag_work,
				msecs_to_jiffies(1500));
		pr_info("user set disable chg %d\n", val->intval);
		break;
	case POWER_SUPPLY_PROP_APSD_NOT_DONE:
		chg->apsd_not_done  = (bool)val->intval;
		break;
	case POWER_SUPPLY_PROP_CONNECT_DISABLE:
		op_disconnect_vbus(chg, (bool)val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		rc = smblib_set_prop_chg_voltage(chg, val);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		rc = smblib_set_prop_batt_temp(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHG_PROTECT_STATUS:
		rc = smblib_set_prop_chg_protect_status(chg, val);
		break;
	case POWER_SUPPLY_PROP_NOTIFY_CHARGER_SET_PARAMETER:
		rc = smblib_set_prop_charge_parameter_set(chg);
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		__pm_stay_awake(chg->chg_aging_wake_lock);
		pr_err("acquire chg_aging_wake_lock\n");
		if (!val->intval) {
			chg->dash_on = get_prop_fast_chg_started(chg);
			if (chg->dash_on
				|| is_charger_not_match(chg)) {
				switch_mode_to_normal();
				op_set_fast_chg_allow(chg, false);
			}
		}
		rc = vote(chg->usb_icl_votable, USER_VOTER,
				!val->intval, 0);
		rc = vote(chg->dc_suspend_votable, USER_VOTER,
				!val->intval, 0);
		chg->chg_enabled = (bool)val->intval;
		chg->chg_disabled = !(bool)val->intval;
		if (chg->swarp_supported)
			smblib_set_usb_suspend(chg, chg->chg_disabled);
		if (chg->chg_enabled) {
			if (!chg->chg_wake_lock_on && chg->vbus_present) {
				pr_info("acquire chg_wake_lock\n");
				__pm_stay_awake(chg->chg_wake_lock);
				chg->chg_wake_lock_on = true;
			} else {
				pr_err("chg_wake_lock is already stay awake,vbus_present:%d\n",
							chg->vbus_present);
			}
			schedule_delayed_work(&chg->check_switch_dash_work,
					      msecs_to_jiffies(3000));
		} else {
			if (chg->chg_wake_lock_on) {
				pr_info("release chg_wake_lock\n");
				__pm_relax(chg->chg_wake_lock);
				chg->chg_wake_lock_on = false;
				if (chg->is_aging_test) {
					chg->ignore_irq_flag = true;
					schedule_delayed_work(&chg->disable_chg_flag_work,
											msecs_to_jiffies(2000));
				}
			} else {
				pr_err("chg_wake_lock is already relax.");
			}
			if (chg->usb_enum_status == false)
				op_release_usb_lock();
		}
		__pm_relax(chg->chg_aging_wake_lock);
		pr_err("release chg_aging_wake_lock\n");
		break;
	case POWER_SUPPLY_PROP_IS_AGING_TEST:
		chg->is_aging_test = (bool)val->intval;
		__debug_mask = PR_OP_DEBUG;
		pr_info("user set is_aging_test:%d\n", chg->is_aging_test);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		rc = smblib_set_prop_batt_capacity(chg, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		rc = smblib_set_prop_batt_capacity_level(chg, val);
		break;
	case POWER_SUPPLY_PROP_PARALLEL_DISABLE:
		vote(chg->pl_disable_votable, USER_VOTER, (bool)val->intval, 0);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		chg->batt_profile_fv_uv = val->intval;
		vote(chg->fv_votable, BATT_PROFILE_VOTER, true, val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_QNOVO:
		if (val->intval == -EINVAL) {
			vote(chg->fv_votable, BATT_PROFILE_VOTER, true,
					chg->batt_profile_fv_uv);
			vote(chg->fv_votable, QNOVO_VOTER, false, 0);
		} else {
			vote(chg->fv_votable, QNOVO_VOTER, true, val->intval);
			vote(chg->fv_votable, BATT_PROFILE_VOTER, false, 0);
		}
		break;
	case POWER_SUPPLY_PROP_STEP_CHARGING_ENABLED:
		chg->step_chg_enabled = !!val->intval;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		chg->batt_profile_fcc_ua = val->intval;
		vote(chg->fcc_votable, BATT_PROFILE_VOTER, true, val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_QNOVO:
		vote(chg->pl_disable_votable, PL_QNOVO_VOTER,
			val->intval != -EINVAL && val->intval < 2000000, 0);
		if (val->intval == -EINVAL) {
			vote(chg->fcc_votable, BATT_PROFILE_VOTER,
					true, chg->batt_profile_fcc_ua);
			vote(chg->fcc_votable, QNOVO_VOTER, false, 0);
		} else {
			vote(chg->fcc_votable, QNOVO_VOTER, true, val->intval);
			vote(chg->fcc_votable, BATT_PROFILE_VOTER, false, 0);
		}
		break;
	case POWER_SUPPLY_PROP_SET_SHIP_MODE:
		/* Not in ship mode as long as the device is active */
		if (!val->intval)
			break;
		if (chg->pl.psy)
			power_supply_set_property(chg->pl.psy,
				POWER_SUPPLY_PROP_SET_SHIP_MODE, val);
		rc = smblib_set_prop_ship_mode(chg, val);
		break;
	case POWER_SUPPLY_PROP_RERUN_AICL:
		rc = smblib_run_aicl(chg, RERUN_AICL);
		break;
	case POWER_SUPPLY_PROP_DP_DM:
		if (!chg->flash_active)
			rc = smblib_dp_dm(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMITED:
		rc = smblib_set_prop_input_current_limited(chg, val);
		break;
	case POWER_SUPPLY_PROP_DIE_HEALTH:
		chg->die_health = val->intval;
		power_supply_changed(chg->batt_psy);
		break;
	case POWER_SUPPLY_PROP_RECHARGE_SOC:
		rc = smblib_set_prop_rechg_soc_thresh(chg, val);
		break;
	case POWER_SUPPLY_PROP_FORCE_RECHARGE:
			/* toggle charging to force recharge */
			vote(chg->chg_disable_votable, FORCE_RECHARGE_VOTER,
					true, 0);
			/* charge disable delay */
			msleep(50);
			vote(chg->chg_disable_votable, FORCE_RECHARGE_VOTER,
					false, 0);
			if (chg->chg_done)
				chg->chg_done = false;
		break;
	case POWER_SUPPLY_PROP_FCC_STEPPER_ENABLE:
		chg->fcc_stepper_enable = val->intval;
		break;
	default:
		rc = -EINVAL;
	}

	return rc;
}

static int smb5_batt_prop_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
	case POWER_SUPPLY_PROP_SYSTEM_TEMP_LEVEL:
	case POWER_SUPPLY_PROP_CAPACITY:
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
	case POWER_SUPPLY_PROP_CHARGE_NOW:
	case POWER_SUPPLY_PROP_TEMP:
	case POWER_SUPPLY_PROP_CHG_PROTECT_STATUS:
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_MAX:
	case POWER_SUPPLY_PROP_IS_AGING_TEST:
	case POWER_SUPPLY_PROP_CONNECT_DISABLE:
	case POWER_SUPPLY_PROP_FASTCHG_IS_OK:
#ifdef OP_SWARP_SUPPORTED
	// Fall-through.
	case POWER_SUPPLY_PROP_FASTCHG_TYPE:
	case POWER_SUPPLY_PROP_ENGINEER_MODE:
#endif
	case POWER_SUPPLY_PROP_PARALLEL_DISABLE:
	case POWER_SUPPLY_PROP_DP_DM:
	case POWER_SUPPLY_PROP_RERUN_AICL:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMITED:
	case POWER_SUPPLY_PROP_STEP_CHARGING_ENABLED:
	case POWER_SUPPLY_PROP_DIE_HEALTH:
	case POWER_SUPPLY_PROP_OP_DISABLE_CHARGE:
	case POWER_SUPPLY_PROP_APSD_NOT_DONE:
	case POWER_SUPPLY_PROP_DUMP_REG:
		return 1;
	default:
		break;
	}

	return 0;
}

static const struct power_supply_desc batt_psy_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = smb5_batt_props,
	.num_properties = ARRAY_SIZE(smb5_batt_props),
	.get_property = smb5_batt_get_prop,
	.set_property = smb5_batt_set_prop,
	.property_is_writeable = smb5_batt_prop_is_writeable,
};

static int smb5_init_batt_psy(struct smb5 *chip)
{
	struct power_supply_config batt_cfg = {};
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	batt_cfg.drv_data = chg;
	batt_cfg.of_node = chg->dev->of_node;
	chg->batt_psy = devm_power_supply_register(chg->dev,
					   &batt_psy_desc,
					   &batt_cfg);
	if (IS_ERR(chg->batt_psy)) {
		pr_err("Couldn't register battery power supply\n");
		return PTR_ERR(chg->batt_psy);
	}

	return rc;
}

/******************************
 * VBUS REGULATOR REGISTRATION *
 ******************************/

static struct regulator_ops smb5_vbus_reg_ops = {
	.enable = smblib_vbus_regulator_enable,
	.disable = smblib_vbus_regulator_disable,
	.is_enabled = smblib_vbus_regulator_is_enabled,
};

static int smb5_init_vbus_regulator(struct smb5 *chip)
{
	struct smb_charger *chg = &chip->chg;
	struct regulator_config cfg = {};
	int rc = 0;

	chg->vbus_vreg = devm_kzalloc(chg->dev, sizeof(*chg->vbus_vreg),
				      GFP_KERNEL);
	if (!chg->vbus_vreg)
		return -ENOMEM;

	cfg.dev = chg->dev;
	cfg.driver_data = chip;

	chg->vbus_vreg->rdesc.owner = THIS_MODULE;
	chg->vbus_vreg->rdesc.type = REGULATOR_VOLTAGE;
	chg->vbus_vreg->rdesc.ops = &smb5_vbus_reg_ops;
	chg->vbus_vreg->rdesc.of_match = "qcom,smb5-vbus";
	chg->vbus_vreg->rdesc.name = "qcom,smb5-vbus";

	chg->vbus_vreg->rdev = devm_regulator_register(chg->dev,
						&chg->vbus_vreg->rdesc, &cfg);
	if (IS_ERR(chg->vbus_vreg->rdev)) {
		rc = PTR_ERR(chg->vbus_vreg->rdev);
		chg->vbus_vreg->rdev = NULL;
		if (rc != -EPROBE_DEFER)
			pr_err("Couldn't register VBUS regulator rc=%d\n", rc);
	}

	return rc;
}

/******************************
 * VCONN REGULATOR REGISTRATION *
 ******************************/

static struct regulator_ops smb5_vconn_reg_ops = {
	.enable = smblib_vconn_regulator_enable,
	.disable = smblib_vconn_regulator_disable,
	.is_enabled = smblib_vconn_regulator_is_enabled,
};

static int smb5_init_vconn_regulator(struct smb5 *chip)
{
	struct smb_charger *chg = &chip->chg;
	struct regulator_config cfg = {};
	int rc = 0;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		return 0;

	chg->vconn_vreg = devm_kzalloc(chg->dev, sizeof(*chg->vconn_vreg),
				      GFP_KERNEL);
	if (!chg->vconn_vreg)
		return -ENOMEM;

	cfg.dev = chg->dev;
	cfg.driver_data = chip;

	chg->vconn_vreg->rdesc.owner = THIS_MODULE;
	chg->vconn_vreg->rdesc.type = REGULATOR_VOLTAGE;
	chg->vconn_vreg->rdesc.ops = &smb5_vconn_reg_ops;
	chg->vconn_vreg->rdesc.of_match = "qcom,smb5-vconn";
	chg->vconn_vreg->rdesc.name = "qcom,smb5-vconn";

	chg->vconn_vreg->rdev = devm_regulator_register(chg->dev,
						&chg->vconn_vreg->rdesc, &cfg);
	if (IS_ERR(chg->vconn_vreg->rdev)) {
		rc = PTR_ERR(chg->vconn_vreg->rdev);
		chg->vconn_vreg->rdev = NULL;
		if (rc != -EPROBE_DEFER)
			pr_err("Couldn't register VCONN regulator rc=%d\n", rc);
	}

	return rc;
}

/***************************
 * HARDWARE INITIALIZATION *
 ***************************/
static int smb5_configure_typec(struct smb_charger *chg)
{
	union power_supply_propval pval = {0, };
	int rc;
	u8  stat;
	u8 val = 0;

	rc = smblib_read(chg, LEGACY_CABLE_STATUS_REG, &val);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read Legacy status rc=%d\n", rc);
		return rc;
	}

	/*
	 * Across reboot, standard typeC cables get detected as legacy cables
	 * due to VBUS attachment prior to CC attach/dettach. To handle this,
	 * "early_usb_attach" flag is used, which assumes that across reboot,
	 * the cable connected can be standard typeC. However, its jurisdiction
	 * is limited to PD capable designs only. Hence, for non-PD type designs
	 * reset legacy cable detection by disabling/enabling typeC mode.
	 */
	if (chg->pd_not_supported && (val & TYPEC_LEGACY_CABLE_STATUS_BIT)) {
		pval.intval = POWER_SUPPLY_TYPEC_PR_NONE;
		smblib_set_prop_typec_power_role(chg, &pval);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't disable TYPEC rc=%d\n", rc);
			return rc;
		}

		/* delay before enabling typeC */
		msleep(50);

		pval.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		smblib_set_prop_typec_power_role(chg, &pval);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't enable TYPEC rc=%d\n", rc);
			return rc;
		}
	}

	smblib_apsd_enable(chg, true);

	rc = smblib_masked_write(chg, TYPE_C_CFG_REG,
					BC1P2_START_ON_CC_BIT, 0);
	if (rc < 0) {
		dev_err(chg->dev, "failed to write TYPE_C_CFG_REG rc=%d\n", rc);
		return rc;
	}

	rc = smblib_write(chg, DEBUG_ACCESS_SNK_CFG_REG, 0x7);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't write DEBUG_ACCESS_SNK_CFG_REG rc=%d\n", rc);
		return rc;
	}

	/* Use simple write to clear interrupts */
	rc = smblib_write(chg, TYPE_C_INTERRUPT_EN_CFG_1_REG,
				TYPEC_CCOUT_DETACH_INT_EN_BIT);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure Type-C interrupts rc=%d\n", rc);
		return rc;
	}

	rc = smblib_read(chg, TYPE_C_INTERRUPT_EN_CFG_1_REG, &stat);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure Type-C interrupts rc=%d\n", rc);
	}
	pr_info("TYPE_C_INTERRUPT_EN_CFG_1_REG:0x%02x=0x%02x\n",
			TYPE_C_INTERRUPT_EN_CFG_1_REG, stat);

	val = chg->lpd_disabled ? 0 : TYPEC_WATER_DETECTION_INT_EN_BIT;
	/* Use simple write to enable only required interrupts */
	rc = smblib_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
				TYPEC_SRC_BATT_HPWR_INT_EN_BIT | val);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure Type-C interrupts rc=%d\n", rc);
		return rc;
	}

	val = EN_TRY_SNK_BIT;
	/* PMI632 doesn't support try snk */
	if (chg->chg_param.smb_version == PMI632_SUBTYPE)
		val = 0;

	if (chg->otg_switch) {
		rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
					EN_TRY_SNK_BIT, val);
		if (rc < 0) {
			dev_err(chg->dev,
				"Couldn't enable try.snk rc=%d\n", rc);
			return rc;
		}
		chg->typec_try_mode |= EN_TRY_SNK_BIT;
	} else {
		rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
					EN_SNK_ONLY_BIT, EN_SNK_ONLY_BIT);
		if (rc < 0) {
			dev_err(chg->dev,
				"Couldn't enable snk.only rc=%d\n", rc);
			return rc;
		}
	}
	/* enable try.snk and clear force sink for DRP mode */
/*
	rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
				EN_TRY_SNK_BIT | EN_SNK_ONLY_BIT,
				EN_TRY_SNK_BIT);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure TYPE_C_MODE_CFG_REG rc=%d\n",
				rc);
		return rc;
	}
	chg->typec_try_mode |= EN_TRY_SNK_BIT;
*/

	/* For PD capable targets configure VCONN for software control */
	if (!chg->pd_not_supported) {
		rc = smblib_masked_write(chg, TYPE_C_VCONN_CONTROL_REG,
				 VCONN_EN_SRC_BIT | VCONN_EN_VALUE_BIT,
				 VCONN_EN_SRC_BIT);
		if (rc < 0) {
			dev_err(chg->dev,
				"Couldn't configure VCONN for SW control rc=%d\n",
				rc);
			return rc;
		}
	}

	if (chg->chg_param.smb_version != PMI632_SUBTYPE) {
		/*
		 * Enable detection of unoriented debug
		 * accessory in source mode.
		 */
		rc = smblib_masked_write(chg, DEBUG_ACCESS_SRC_CFG_REG,
					 EN_UNORIENTED_DEBUG_ACCESS_SRC_BIT,
					 EN_UNORIENTED_DEBUG_ACCESS_SRC_BIT);
		if (rc < 0) {
			dev_err(chg->dev,
				"Couldn't configure TYPE_C_DEBUG_ACCESS_SRC_CFG_REG rc=%d\n",
					rc);
			return rc;
		}

		rc = smblib_masked_write(chg, USBIN_LOAD_CFG_REG,
				USBIN_IN_COLLAPSE_GF_SEL_MASK |
				USBIN_AICL_STEP_TIMING_SEL_MASK,
				0);
		if (rc < 0) {
			dev_err(chg->dev,
				"Couldn't set USBIN_LOAD_CFG_REG rc=%d\n", rc);
			return rc;
		}
	}

	/* Set CC threshold to 1.6 V in source mode */
	rc = smblib_masked_write(chg, TYPE_C_EXIT_STATE_CFG_REG,
				SEL_SRC_UPPER_REF_BIT, SEL_SRC_UPPER_REF_BIT);
	if (rc < 0)
		dev_err(chg->dev,
			"Couldn't configure CC threshold voltage rc=%d\n", rc);

	return rc;
}

static int smb5_configure_micro_usb(struct smb_charger *chg)
{
	int rc;

	/* For micro USB connector, use extcon by default */
	chg->use_extcon = true;
	chg->pd_not_supported = true;

	rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					MICRO_USB_STATE_CHANGE_INT_EN_BIT,
					MICRO_USB_STATE_CHANGE_INT_EN_BIT);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure Type-C interrupts rc=%d\n", rc);
		return rc;
	}

	if (chg->uusb_moisture_protection_enabled) {
		/* Enable moisture detection interrupt */
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
				TYPEC_WATER_DETECTION_INT_EN_BIT,
				TYPEC_WATER_DETECTION_INT_EN_BIT);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't enable moisture detection interrupt rc=%d\n",
				rc);
			return rc;
		}

		/* Enable uUSB factory mode */
		rc = smblib_masked_write(chg, TYPEC_U_USB_CFG_REG,
					EN_MICRO_USB_FACTORY_MODE_BIT,
					EN_MICRO_USB_FACTORY_MODE_BIT);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't enable uUSB factory mode c=%d\n",
				rc);
			return rc;
		}

		/* Disable periodic monitoring of CC_ID pin */
		rc = smblib_write(chg,
			((chg->chg_param.smb_version == PMI632_SUBTYPE) ?
				PMI632_TYPEC_U_USB_WATER_PROTECTION_CFG_REG :
				TYPEC_U_USB_WATER_PROTECTION_CFG_REG), 0);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't disable periodic monitoring of CC_ID rc=%d\n",
				rc);
			return rc;
		}
	}

	/* Enable HVDCP detection and authentication */
	if (!chg->hvdcp_disable)
		smblib_hvdcp_detect_enable(chg, true);

	return rc;
}

#define RAW_ITERM(iterm_ma, max_range)				\
		div_s64((int64_t)iterm_ma * ADC_CHG_ITERM_MASK, max_range)
static int smb5_configure_iterm_thresholds_adc(struct smb5 *chip)
{
	u8 *buf;
	int rc = 0;
	s16 raw_hi_thresh, raw_lo_thresh, max_limit_ma;
	struct smb_charger *chg = &chip->chg;

	if (chip->chg.chg_param.smb_version == PMI632_SUBTYPE)
		max_limit_ma = ITERM_LIMITS_PMI632_MA;
	else
		max_limit_ma = ITERM_LIMITS_PM8150B_MA;

	if (chip->dt.term_current_thresh_hi_ma < (-1 * max_limit_ma)
		|| chip->dt.term_current_thresh_hi_ma > max_limit_ma
		|| chip->dt.term_current_thresh_lo_ma < (-1 * max_limit_ma)
		|| chip->dt.term_current_thresh_lo_ma > max_limit_ma) {
		dev_err(chg->dev, "ITERM threshold out of range rc=%d\n", rc);
		return -EINVAL;
	}

	/*
	 * Conversion:
	 *	raw (A) = (term_current * ADC_CHG_ITERM_MASK) / max_limit_ma
	 * Note: raw needs to be converted to big-endian format.
	 */

	if (chip->dt.term_current_thresh_hi_ma) {
		raw_hi_thresh = RAW_ITERM(chip->dt.term_current_thresh_hi_ma,
					max_limit_ma);
		raw_hi_thresh = sign_extend32(raw_hi_thresh, 15);
		buf = (u8 *)&raw_hi_thresh;
		raw_hi_thresh = buf[1] | (buf[0] << 8);

		rc = smblib_batch_write(chg, CHGR_ADC_ITERM_UP_THD_MSB_REG,
				(u8 *)&raw_hi_thresh, 2);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't configure ITERM threshold HIGH rc=%d\n",
					rc);
			return rc;
		}
	}

	if (chip->dt.term_current_thresh_lo_ma) {
		raw_lo_thresh = RAW_ITERM(chip->dt.term_current_thresh_lo_ma,
					max_limit_ma);
		raw_lo_thresh = sign_extend32(raw_lo_thresh, 15);
		buf = (u8 *)&raw_lo_thresh;
		raw_lo_thresh = buf[1] | (buf[0] << 8);

		rc = smblib_batch_write(chg, CHGR_ADC_ITERM_LO_THD_MSB_REG,
				(u8 *)&raw_lo_thresh, 2);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't configure ITERM threshold LOW rc=%d\n",
					rc);
			return rc;
		}
	}

	return rc;
}

static int smb5_configure_iterm_thresholds(struct smb5 *chip)
{
	int rc = 0;
	struct smb_charger *chg = &chip->chg;

	switch (chip->dt.term_current_src) {
	case ITERM_SRC_ADC:
		if (chip->chg.chg_param.smb_version == PM8150B_SUBTYPE) {
			rc = smblib_masked_write(chg, CHGR_ADC_TERM_CFG_REG,
					TERM_BASED_ON_SYNC_CONV_OR_SAMPLE_CNT,
					TERM_BASED_ON_SAMPLE_CNT);
			if (rc < 0) {
				dev_err(chg->dev, "Couldn't configure ADC_ITERM_CFG rc=%d\n",
						rc);
				return rc;
			}
		}
		rc = smb5_configure_iterm_thresholds_adc(chip);
		break;
	default:
		break;
	}

	return rc;
}

static int smb5_configure_mitigation(struct smb_charger *chg)
{
	int rc;
	u8 chan = 0, src_cfg = 0;

	if (!chg->hw_die_temp_mitigation && !chg->hw_connector_mitigation &&
			!chg->hw_skin_temp_mitigation) {
		src_cfg = THERMREG_SW_ICL_ADJUST_BIT;
	} else {
		if (chg->hw_die_temp_mitigation) {
			chan = DIE_TEMP_CHANNEL_EN_BIT;
			src_cfg = THERMREG_DIE_ADC_SRC_EN_BIT
				| THERMREG_DIE_CMP_SRC_EN_BIT;
		}

		if (chg->hw_connector_mitigation) {
			chan |= CONN_THM_CHANNEL_EN_BIT;
			src_cfg |= THERMREG_CONNECTOR_ADC_SRC_EN_BIT;
		}

		if (chg->hw_skin_temp_mitigation) {
			chan |= MISC_THM_CHANNEL_EN_BIT;
			src_cfg |= THERMREG_SKIN_ADC_SRC_EN_BIT;
		}

		rc = smblib_masked_write(chg, BATIF_ADC_CHANNEL_EN_REG,
			CONN_THM_CHANNEL_EN_BIT | DIE_TEMP_CHANNEL_EN_BIT |
			MISC_THM_CHANNEL_EN_BIT, chan);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't enable ADC channel rc=%d\n",
				rc);
			return rc;
		}
	}

	rc = smblib_masked_write(chg, MISC_THERMREG_SRC_CFG_REG,
		THERMREG_SW_ICL_ADJUST_BIT | THERMREG_DIE_ADC_SRC_EN_BIT |
		THERMREG_DIE_CMP_SRC_EN_BIT | THERMREG_SKIN_ADC_SRC_EN_BIT |
		SKIN_ADC_CFG_BIT | THERMREG_CONNECTOR_ADC_SRC_EN_BIT, src_cfg);
	if (rc < 0) {
		dev_err(chg->dev,
				"Couldn't configure THERM_SRC reg rc=%d\n", rc);
		return rc;
	}

	return 0;
}

static int smb5_init_dc_peripheral(struct smb_charger *chg)
{
	int rc = 0;

	/* PMI632 does not have DC peripheral */
	if (chg->chg_param.smb_version == PMI632_SUBTYPE)
		return 0;

	/* Set DCIN ICL to 100 mA */
	rc = smblib_set_charge_param(chg, &chg->param.dc_icl, DCIN_ICL_MIN_UA);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't set dc_icl rc=%d\n", rc);
		return rc;
	}

	/* Disable DC Input missing poller function */
	rc = smblib_masked_write(chg, DCIN_LOAD_CFG_REG,
					INPUT_MISS_POLL_EN_BIT, 0);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't disable DC Input missing poller rc=%d\n", rc);
		return rc;
	}

	return rc;
}

static int smb5_configure_recharging(struct smb5 *chip)
{
	int rc = 0;
	struct smb_charger *chg = &chip->chg;
	union power_supply_propval pval;
	/* Configure VBATT-based or automatic recharging */

	rc = smblib_masked_write(chg, CHGR_CFG2_REG, RECHG_MASK,
				(chip->dt.auto_recharge_vbat_mv != -EINVAL) ?
				VBAT_BASED_RECHG_BIT : 0);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure VBAT-rechg CHG_CFG2_REG rc=%d\n",
			rc);
		return rc;
	}

	/* program the auto-recharge VBAT threshold */
	if (chip->dt.auto_recharge_vbat_mv != -EINVAL) {
		u32 temp = VBAT_TO_VRAW_ADC(chip->dt.auto_recharge_vbat_mv);

		temp = ((temp & 0xFF00) >> 8) | ((temp & 0xFF) << 8);
		rc = smblib_batch_write(chg,
			CHGR_ADC_RECHARGE_THRESHOLD_MSB_REG, (u8 *)&temp, 2);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't configure ADC_RECHARGE_THRESHOLD REG rc=%d\n",
				rc);
			return rc;
		}
		/* Program the sample count for VBAT based recharge to 3 */
		rc = smblib_masked_write(chg, CHGR_NO_SAMPLE_TERM_RCHG_CFG_REG,
					NO_OF_SAMPLE_FOR_RCHG,
					2 << NO_OF_SAMPLE_FOR_RCHG_SHIFT);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't configure CHGR_NO_SAMPLE_FOR_TERM_RCHG_CFG rc=%d\n",
				rc);
			return rc;
		}
	}

	rc = smblib_masked_write(chg, CHGR_CFG2_REG, RECHG_MASK,
				(chip->dt.auto_recharge_soc != -EINVAL) ?
				SOC_BASED_RECHG_BIT : VBAT_BASED_RECHG_BIT);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure SOC-rechg CHG_CFG2_REG rc=%d\n",
			rc);
		return rc;
	}

	/* program the auto-recharge threshold */
	if (chip->dt.auto_recharge_soc != -EINVAL) {
		pval.intval = chip->dt.auto_recharge_soc;
		rc = smblib_set_prop_rechg_soc_thresh(chg, &pval);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't configure CHG_RCHG_SOC_REG rc=%d\n",
					rc);
			return rc;
		}

		/* Program the sample count for SOC based recharge to 1 */
		rc = smblib_masked_write(chg, CHGR_NO_SAMPLE_TERM_RCHG_CFG_REG,
						NO_OF_SAMPLE_FOR_RCHG, 0);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't configure CHGR_NO_SAMPLE_FOR_TERM_RCHG_CFG rc=%d\n",
				rc);
			return rc;
		}
	}

	return 0;
}

static int smb5_configure_float_charger(struct smb5 *chip)
{
	int rc = 0;
	u8 val = 0;
	struct smb_charger *chg = &chip->chg;

	/* configure float charger options */
	switch (chip->dt.float_option) {
	case FLOAT_SDP:
		val = FORCE_FLOAT_SDP_CFG_BIT;
		break;
	case DISABLE_CHARGING:
		val = FLOAT_DIS_CHGING_CFG_BIT;
		break;
	case SUSPEND_INPUT:
		val = SUSPEND_FLOAT_CFG_BIT;
		break;
	case FLOAT_DCP:
	default:
		val = 0;
		break;
	}

	chg->float_cfg = val;
	/* Update float charger setting and set DCD timeout 300ms */
	rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
				FLOAT_OPTIONS_MASK | DCD_TIMEOUT_SEL_BIT, val);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't change float charger setting rc=%d\n",
			rc);
		return rc;
	}

	return 0;
}

static int smb5_init_connector_type(struct smb_charger *chg)
{
	int rc, type = 0;
	u8 val = 0;

	/*
	 * PMI632 can have the connector type defined by a dedicated register
	 * PMI632_TYPEC_MICRO_USB_MODE_REG or by a common TYPEC_U_USB_CFG_REG.
	 */
	if (chg->chg_param.smb_version == PMI632_SUBTYPE) {
		rc = smblib_read(chg, PMI632_TYPEC_MICRO_USB_MODE_REG, &val);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't read USB mode rc=%d\n", rc);
			return rc;
		}
		type = !!(val & MICRO_USB_MODE_ONLY_BIT);
	}

	/*
	 * If PMI632_TYPEC_MICRO_USB_MODE_REG is not set and for all non-PMI632
	 * check the connector type using TYPEC_U_USB_CFG_REG.
	 */
	if (!type) {
		rc = smblib_read(chg, TYPEC_U_USB_CFG_REG, &val);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't read U_USB config rc=%d\n",
					rc);
			return rc;
		}

		type = !!(val & EN_MICRO_USB_MODE_BIT);
	}

	pr_debug("Connector type=%s\n", type ? "Micro USB" : "TypeC");

	if (type) {
		chg->connector_type = POWER_SUPPLY_CONNECTOR_MICRO_USB;
		rc = smb5_configure_micro_usb(chg);
	} else {
		chg->connector_type = POWER_SUPPLY_CONNECTOR_TYPEC;
		rc = smb5_configure_typec(chg);
	}
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure TypeC/micro-USB mode rc=%d\n", rc);
		return rc;
	}

	/*
	 * PMI632 based hw init:
	 * - Rerun APSD to ensure proper charger detection if device
	 *   boots with charger connected.
	 * - Initialize flash module for PMI632
	 */
	if (chg->chg_param.smb_version == PMI632_SUBTYPE) {
		schgm_flash_init(chg);
		smblib_rerun_apsd_if_required(chg);
	}

	return 0;

}

static int smb5_init_hw(struct smb5 *chip)
{
	struct smb_charger *chg = &chip->chg;
	int rc;
	u8 val = 0, mask = 0, buf[2] = {0};

	if (chip->dt.no_battery) {
		chg->fake_capacity = 50;
		chg->fake_capacity_level = 3;
	}

	if (chg->sdam_base) {
		rc = smblib_write(chg,
			chg->sdam_base + SDAM_QC_DET_STATUS_REG, 0);
		if (rc < 0)
			pr_err("Couldn't clear SDAM QC status rc=%d\n", rc);

		rc = smblib_batch_write(chg,
			chg->sdam_base + SDAM_QC_ADC_LSB_REG, buf, 2);
		if (rc < 0)
			pr_err("Couldn't clear SDAM ADC status rc=%d\n", rc);
	}

	if (chip->dt.batt_profile_fcc_ua < 0)
		smblib_get_charge_param(chg, &chg->param.fcc,
				&chg->batt_profile_fcc_ua);

	if (chip->dt.batt_profile_fv_uv < 0)
		smblib_get_charge_param(chg, &chg->param.fv,
				&chg->batt_profile_fv_uv);

	smblib_get_charge_param(chg, &chg->param.usb_icl,
				&chg->default_icl_ua);

	pr_info("vbat_max=%d, ibat_max=%d, iusb_max=%d\n",
		chg->batt_profile_fv_uv,
		chg->batt_profile_fcc_ua, chip->dt.usb_icl_ua);

	smblib_get_charge_param(chg, &chg->param.aicl_5v_threshold,
				&chg->default_aicl_5v_threshold_mv);
	chg->aicl_5v_threshold_mv = chg->default_aicl_5v_threshold_mv;
	smblib_get_charge_param(chg, &chg->param.aicl_cont_threshold,
				&chg->default_aicl_cont_threshold_mv);
	chg->aicl_cont_threshold_mv = chg->default_aicl_cont_threshold_mv;

	if (chg->charger_temp_max == -EINVAL) {
		rc = smblib_get_thermal_threshold(chg,
					DIE_REG_H_THRESHOLD_MSB_REG,
					&chg->charger_temp_max);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't get charger_temp_max rc=%d\n",
					rc);
			return rc;
		}
	}

	/*
	 * If SW thermal regulation WA is active then all the HW temperature
	 * comparators need to be disabled to prevent HW thermal regulation,
	 * apart from DIE_TEMP analog comparator for SHDN regulation.
	 */
	if (chg->wa_flags & SW_THERM_REGULATION_WA) {
		rc = smblib_write(chg, MISC_THERMREG_SRC_CFG_REG,
					THERMREG_SW_ICL_ADJUST_BIT
					| THERMREG_DIE_CMP_SRC_EN_BIT);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't disable HW thermal regulation rc=%d\n",
				rc);
			return rc;
		}
	} else {
		/* configure temperature mitigation */
		rc = smb5_configure_mitigation(chg);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't configure mitigation rc=%d\n",
					rc);
			return rc;
		}
	}

	/* Set HVDCP autonomous mode per DT option */
	smblib_hvdcp_hw_inov_enable(chg, chip->dt.hvdcp_autonomous);

	/* Enable HVDCP authentication algorithm for non-PD designs */
	if (chg->pd_not_supported)
		smblib_hvdcp_detect_enable(chg, true);

	/* Disable HVDCP and authentication algorithm if specified in DT */
	if (chg->hvdcp_disable)
		smblib_hvdcp_detect_enable(chg, false);

	rc = smb5_init_connector_type(chg);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure connector type rc=%d\n",
				rc);
		return rc;
	}

	/* Use ICL results from HW */
	rc = smblib_icl_override(chg, HW_AUTO_MODE);
	if (rc < 0) {
		pr_err("Couldn't disable ICL override rc=%d\n", rc);
		return rc;
	}

	vote(chg->usb_icl_votable,
		DEFAULT_VOTER, !chg->chg_enabled, 0);
	vote(chg->dc_suspend_votable,
		DEFAULT_VOTER, !chg->chg_enabled, 0);
	smblib_set_charge_param(chg, &chg->param.fcc,
			chg->ibatmax[BATT_TEMP_NORMAL] * 1000);
	smblib_set_charge_param(chg, &chg->param.fv,
			chg->vbatmax[BATT_TEMP_NORMAL] * 1000);
	/* set OTG current limit */
	rc = smblib_set_charge_param(chg, &chg->param.otg_cl, chg->otg_cl_ua);
	if (rc < 0) {
		pr_err("Couldn't set otg current limit rc=%d\n", rc);
		return rc;
	}

	/* vote 0mA on usb_icl for non battery platforms */
	vote(chg->usb_icl_votable,
		DEFAULT_VOTER, chip->dt.no_battery, 0);
	vote(chg->dc_suspend_votable,
		DEFAULT_VOTER, chip->dt.no_battery, 0);
	vote(chg->fcc_votable,
		BATT_PROFILE_VOTER, chg->batt_profile_fcc_ua > 0,
		chg->batt_profile_fcc_ua);
	vote(chg->fv_votable,
		BATT_PROFILE_VOTER, chg->batt_profile_fv_uv > 0,
		chg->batt_profile_fv_uv);

	/* Some h/w limit maximum supported ICL */
	vote(chg->usb_icl_votable, HW_LIMIT_VOTER,
			chg->hw_max_icl_ua > 0, chg->hw_max_icl_ua);

	/* disable HVDCP */
	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
		HVDCP_EN_BIT, 0);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't disable HVDCP rc=%d\n", rc);

	/* aicl rerun time */
	rc = smblib_masked_write(chg, AICL_RERUN_TIME_CFG_REG,
		BIT(0)|BIT(1), 0);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't set aicl rerunTimerc=%d\n", rc);

	/* Initialize DC peripheral configurations */
	rc = smb5_init_dc_peripheral(chg);
	if (rc < 0)
		return rc;

	/*
	 * AICL configuration: enable aicl and aicl rerun and based on DT
	 * configuration enable/disable ADB based AICL and Suspend on collapse.
	 */
	mask = USBIN_AICL_PERIODIC_RERUN_EN_BIT | USBIN_AICL_ADC_EN_BIT
			| USBIN_AICL_EN_BIT | SUSPEND_ON_COLLAPSE_USBIN_BIT;
#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported)
		val = USBIN_AICL_PERIODIC_RERUN_EN_BIT;
	else
#endif
		val = USBIN_AICL_PERIODIC_RERUN_EN_BIT | USBIN_AICL_EN_BIT;
	if (!chip->dt.disable_suspend_on_collapse)
		val |= SUSPEND_ON_COLLAPSE_USBIN_BIT;
	if (chip->dt.adc_based_aicl)
		val |= USBIN_AICL_ADC_EN_BIT;

	rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
			mask, val);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't config AICL rc=%d\n", rc);
		return rc;
	}

	rc = smblib_write(chg, AICL_RERUN_TIME_CFG_REG,
				AICL_RERUN_TIME_12S_VAL);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure AICL rerun interval rc=%d\n", rc);
		return rc;
	}

	/* enable the charging path */
	rc = vote(chg->chg_disable_votable, DEFAULT_VOTER, false, 0);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't enable charging rc=%d\n", rc);
		return rc;
	}

	/* configure VBUS for software control */
	rc = smblib_masked_write(chg, DCDC_OTG_CFG_REG, OTG_EN_SRC_CFG_BIT, 0);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure VBUS for SW control rc=%d\n", rc);
		return rc;
	}

	val = (ilog2(chip->dt.wd_bark_time / 16) << BARK_WDOG_TIMEOUT_SHIFT)
			& BARK_WDOG_TIMEOUT_MASK;
	val |= (BITE_WDOG_TIMEOUT_8S | BITE_WDOG_DISABLE_CHARGING_CFG_BIT);
	val |= (chip->dt.wd_snarl_time_cfg << SNARL_WDOG_TIMEOUT_SHIFT)
			& SNARL_WDOG_TIMEOUT_MASK;

	rc = smblib_masked_write(chg, SNARL_BARK_BITE_WD_CFG_REG,
			BITE_WDOG_DISABLE_CHARGING_CFG_BIT |
			SNARL_WDOG_TIMEOUT_MASK | BARK_WDOG_TIMEOUT_MASK |
			BITE_WDOG_TIMEOUT_MASK,
			val);
	if (rc < 0) {
		pr_err("Couldn't configue WD config rc=%d\n", rc);
		return rc;
	}

	/* enable WD BARK and enable it on plugin */
	val = WDOG_TIMER_EN_ON_PLUGIN_BIT | BARK_WDOG_INT_EN_BIT;
	rc = smblib_masked_write(chg, WD_CFG_REG,
			WATCHDOG_TRIGGER_AFP_EN_BIT |
			WDOG_TIMER_EN_ON_PLUGIN_BIT |
			BARK_WDOG_INT_EN_BIT, val);
	if (rc < 0) {
		pr_err("Couldn't configue WD config rc=%d\n", rc);
		return rc;
	}

	/* set termination current threshold values */
	rc = smb5_configure_iterm_thresholds(chip);
	if (rc < 0) {
		pr_err("Couldn't configure ITERM thresholds rc=%d\n",
				rc);
		return rc;
	}

	rc = smb5_configure_float_charger(chip);
	if (rc < 0)
		return rc;

	switch (chip->dt.chg_inhibit_thr_mv) {
	case 50:
		rc = smblib_masked_write(chg, CHARGE_INHIBIT_THRESHOLD_CFG_REG,
				CHARGE_INHIBIT_THRESHOLD_MASK,
				INHIBIT_ANALOG_VFLT_MINUS_50MV);
		break;
	case 100:
		rc = smblib_masked_write(chg, CHARGE_INHIBIT_THRESHOLD_CFG_REG,
				CHARGE_INHIBIT_THRESHOLD_MASK,
				INHIBIT_ANALOG_VFLT_MINUS_100MV);
		break;
	case 200:
		rc = smblib_masked_write(chg, CHARGE_INHIBIT_THRESHOLD_CFG_REG,
				CHARGE_INHIBIT_THRESHOLD_MASK,
				INHIBIT_ANALOG_VFLT_MINUS_200MV);
		break;
	case 300:
		rc = smblib_masked_write(chg, CHARGE_INHIBIT_THRESHOLD_CFG_REG,
				CHARGE_INHIBIT_THRESHOLD_MASK,
				INHIBIT_ANALOG_VFLT_MINUS_300MV);
		break;
	case 0:
		rc = smblib_masked_write(chg, CHGR_CFG2_REG,
				CHARGER_INHIBIT_BIT, 0);
	default:
		break;
	}

	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure charge inhibit threshold rc=%d\n",
			rc);
		return rc;
	}

	rc = smblib_write(chg, CHGR_FAST_CHARGE_SAFETY_TIMER_CFG_REG,
					FAST_CHARGE_SAFETY_TIMER_768_MIN);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't set CHGR_FAST_CHARGE_SAFETY_TIMER_CFG_REG rc=%d\n",
			rc);
		return rc;
	}

	rc = smb5_configure_recharging(chip);
	if (rc < 0)
		return rc;

	rc = smblib_disable_hw_jeita(chg, true);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't set hw jeita rc=%d\n", rc);
		return rc;
	}

	rc = smblib_masked_write(chg, DCDC_ENG_SDCDC_CFG5_REG,
			ENG_SDCDC_BAT_HPWR_MASK, BOOST_MODE_THRESH_3P6_V);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure DCDC_ENG_SDCDC_CFG5 rc=%d\n",
				rc);
		return rc;
	}

	if (chg->connector_pull_up != -EINVAL) {
		rc = smb5_configure_internal_pull(chg, CONN_THERM,
				get_valid_pullup(chg->connector_pull_up));
		if (rc < 0) {
			dev_err(chg->dev,
				"Couldn't configure CONN_THERM pull-up rc=%d\n",
				rc);
			return rc;
		}
	}

	if (chg->smb_pull_up != -EINVAL) {
		rc = smb5_configure_internal_pull(chg, SMB_THERM,
				get_valid_pullup(chg->smb_pull_up));
		if (rc < 0) {
			dev_err(chg->dev,
				"Couldn't configure SMB pull-up rc=%d\n",
				rc);
			return rc;
		}
	}

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT, 0);
		if (rc < 0) {
			pr_err("Couldn't disable charging rc=%d\n", rc);
			return rc;
		}
		rc = smblib_masked_write(chg, USBIN_CMD_IL_REG,
				USBIN_SUSPEND_BIT, USBIN_SUSPEND_BIT);
		if (rc < 0) {
			pr_err("Couldn't do pmic usb suspend rc=%d\n", rc);
			return rc;
		}
	}
#endif

	return rc;
}

static int smb5_post_init(struct smb5 *chip)
{
	struct smb_charger *chg = &chip->chg;
	union power_supply_propval pval;
	int rc;

	/*
	 * In case the usb path is suspended, we would have missed disabling
	 * the icl change interrupt because the interrupt could have been
	 * not requested
	 */
	rerun_election(chg->usb_icl_votable);

	/* configure power role for dual-role */
	pval.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
	rc = smblib_set_prop_typec_power_role(chg, &pval);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure DRP role rc=%d\n",
				rc);
		return rc;
	}

	rerun_election(chg->temp_change_irq_disable_votable);

	return 0;
}

/****************************
 * DETERMINE INITIAL STATUS *
 ****************************/

static int smb5_determine_initial_status(struct smb5 *chip)
{
	struct smb_irq_data irq_data = {chip, "determine-initial-status"};
	struct smb_charger *chg = &chip->chg;
	union power_supply_propval val;
	int rc;

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		pr_err("Couldn't get usb present rc=%d\n", rc);
		return rc;
	}
	chg->early_usb_attach = val.intval;

	if (chg->bms_psy)
		smblib_suspend_on_debug_battery(chg);

	usb_plugin_irq_handler(0, &irq_data);
	dc_plugin_irq_handler(0, &irq_data);
	typec_attach_detach_irq_handler(0, &irq_data);
	typec_state_change_irq_handler(0, &irq_data);
	usb_source_change_irq_handler(0, &irq_data);
	chg_state_change_irq_handler(0, &irq_data);
	icl_change_irq_handler(0, &irq_data);
	batt_temp_changed_irq_handler(0, &irq_data);
	wdog_bark_irq_handler(0, &irq_data);
	typec_or_rid_detection_change_irq_handler(0, &irq_data);
	wdog_snarl_irq_handler(0, &irq_data);

	return 0;
}

/**************************
 * INTERRUPT REGISTRATION *
 **************************/

static struct smb_irq_info smb5_irqs[] = {
	/* CHARGER IRQs */
	[CHGR_ERROR_IRQ] = {
		.name		= "chgr-error",
		.handler	= default_irq_handler,
	},
	[CHG_STATE_CHANGE_IRQ] = {
		.name		= "chg-state-change",
		.handler	= chg_state_change_irq_handler,
		.wake		= true,
	},
	[STEP_CHG_STATE_CHANGE_IRQ] = {
		.name		= "step-chg-state-change",
	},
	[STEP_CHG_SOC_UPDATE_FAIL_IRQ] = {
		.name		= "step-chg-soc-update-fail",
	},
	[STEP_CHG_SOC_UPDATE_REQ_IRQ] = {
		.name		= "step-chg-soc-update-req",
	},
	[FG_FVCAL_QUALIFIED_IRQ] = {
		.name		= "fg-fvcal-qualified",
	},
	[VPH_ALARM_IRQ] = {
		.name		= "vph-alarm",
	},
	[VPH_DROP_PRECHG_IRQ] = {
		.name		= "vph-drop-prechg",
	},
	/* DCDC IRQs */
	[OTG_FAIL_IRQ] = {
		.name		= "otg-fail",
		.handler	= default_irq_handler,
	},
	[OTG_OC_DISABLE_SW_IRQ] = {
		.name		= "otg-oc-disable-sw",
	},
	[OTG_OC_HICCUP_IRQ] = {
		.name		= "otg-oc-hiccup",
	},
	[BSM_ACTIVE_IRQ] = {
		.name		= "bsm-active",
	},
	[HIGH_DUTY_CYCLE_IRQ] = {
		.name		= "high-duty-cycle",
		.handler	= high_duty_cycle_irq_handler,
		.wake		= true,
	},
	[INPUT_CURRENT_LIMITING_IRQ] = {
		.name		= "input-current-limiting",
		.handler	= default_irq_handler,
	},
	[CONCURRENT_MODE_DISABLE_IRQ] = {
		.name		= "concurrent-mode-disable",
	},
	[SWITCHER_POWER_OK_IRQ] = {
		.name		= "switcher-power-ok",
		.handler	= switcher_power_ok_irq_handler,
	},
	/* BATTERY IRQs */
	[BAT_TEMP_IRQ] = {
		.name		= "bat-temp",
		.handler	= batt_temp_changed_irq_handler,
		.wake		= true,
	},
	[ALL_CHNL_CONV_DONE_IRQ] = {
		.name		= "all-chnl-conv-done",
	},
	[BAT_OV_IRQ] = {
		.name		= "bat-ov",
		.handler	= batt_psy_changed_irq_handler,
	},
	[BAT_LOW_IRQ] = {
		.name		= "bat-low",
		.handler	= batt_psy_changed_irq_handler,
	},
	[BAT_THERM_OR_ID_MISSING_IRQ] = {
		.name		= "bat-therm-or-id-missing",
		.handler	= batt_psy_changed_irq_handler,
	},
	[BAT_TERMINAL_MISSING_IRQ] = {
		.name		= "bat-terminal-missing",
		.handler	= batt_psy_changed_irq_handler,
	},
	[BUCK_OC_IRQ] = {
		.name		= "buck-oc",
	},
	[VPH_OV_IRQ] = {
		.name		= "vph-ov",
	},
	/* USB INPUT IRQs */
	[USBIN_COLLAPSE_IRQ] = {
		.name		= "usbin-collapse",
		.handler	= default_irq_handler,
	},
	[USBIN_VASHDN_IRQ] = {
		.name		= "usbin-vashdn",
		.handler	= default_irq_handler,
	},
	[USBIN_UV_IRQ] = {
		.name		= "usbin-uv",
		.handler	= usbin_uv_irq_handler,
		.wake		= true,
		.storm_data	= {true, 3000, 5},
	},
	[USBIN_OV_IRQ] = {
		.name		= "usbin-ov",
		.handler	= usbin_ov_irq_handler,
	},
	[USBIN_PLUGIN_IRQ] = {
		.name		= "usbin-plugin",
		.handler	= usb_plugin_irq_handler,
		.wake           = true,
	},
	[USBIN_REVI_CHANGE_IRQ] = {
		.name		= "usbin-revi-change",
	},
	[USBIN_SRC_CHANGE_IRQ] = {
		.name		= "usbin-src-change",
		.handler	= usb_source_change_irq_handler,
		.wake           = true,
	},
	[USBIN_ICL_CHANGE_IRQ] = {
		.name		= "usbin-icl-change",
		.handler	= icl_change_irq_handler,
		.wake           = true,
	},
	/* DC INPUT IRQs */
	[DCIN_VASHDN_IRQ] = {
		.name		= "dcin-vashdn",
	},
	[DCIN_UV_IRQ] = {
		.name		= "dcin-uv",
		.handler	= dcin_uv_irq_handler,
		.wake		= true,
	},
	[DCIN_OV_IRQ] = {
		.name		= "dcin-ov",
		.handler	= default_irq_handler,
	},
	[DCIN_PLUGIN_IRQ] = {
		.name		= "dcin-plugin",
		.handler	= dc_plugin_irq_handler,
		.wake           = true,
	},
	[DCIN_REVI_IRQ] = {
		.name		= "dcin-revi",
	},
	[DCIN_PON_IRQ] = {
		.name		= "dcin-pon",
		.handler	= default_irq_handler,
	},
	[DCIN_EN_IRQ] = {
		.name		= "dcin-en",
		.handler	= default_irq_handler,
	},
	/* TYPEC IRQs */
	[TYPEC_OR_RID_DETECTION_CHANGE_IRQ] = {
		.name		= "typec-or-rid-detect-change",
		.handler	= typec_or_rid_detection_change_irq_handler,
		.wake           = true,
	},
	[TYPEC_VPD_DETECT_IRQ] = {
		.name		= "typec-vpd-detect",
	},
	[TYPEC_CC_STATE_CHANGE_IRQ] = {
		.name		= "typec-cc-state-change",
		.handler	= typec_state_change_irq_handler,
		.wake           = true,
	},
	[TYPEC_VCONN_OC_IRQ] = {
		.name		= "typec-vconn-oc",
		.handler	= default_irq_handler,
	},
	[TYPEC_VBUS_CHANGE_IRQ] = {
		.name		= "typec-vbus-change",
	},
	[TYPEC_ATTACH_DETACH_IRQ] = {
		.name		= "typec-attach-detach",
		.handler	= typec_attach_detach_irq_handler,
		.wake		= true,
	},
	[TYPEC_LEGACY_CABLE_DETECT_IRQ] = {
		.name		= "typec-legacy-cable-detect",
		.handler	= default_irq_handler,
	},
	[TYPEC_TRY_SNK_SRC_DETECT_IRQ] = {
		.name		= "typec-try-snk-src-detect",
	},
	/* MISCELLANEOUS IRQs */
	[WDOG_SNARL_IRQ] = {
		.name		= "wdog-snarl",
		.handler	= wdog_snarl_irq_handler,
		.wake		= true,
	},
	[WDOG_BARK_IRQ] = {
		.name		= "wdog-bark",
		.handler	= wdog_bark_irq_handler,
		.wake		= true,
	},
	[AICL_FAIL_IRQ] = {
		.name		= "aicl-fail",
	},
	[AICL_DONE_IRQ] = {
		.name		= "aicl-done",
		.handler	= default_irq_handler,
	},
	[SMB_EN_IRQ] = {
		.name		= "smb-en",
		.handler	= smb_en_irq_handler,
	},
	[IMP_TRIGGER_IRQ] = {
		.name		= "imp-trigger",
	},
	/*
	 * triggered when DIE or SKIN or CONNECTOR temperature across
	 * either of the _REG_L, _REG_H, _RST, or _SHDN thresholds
	 */
	[TEMP_CHANGE_IRQ] = {
		.name		= "temp-change",
		.handler	= temp_change_irq_handler,
		.wake		= true,
	},
	[TEMP_CHANGE_SMB_IRQ] = {
		.name		= "temp-change-smb",
	},
	/* FLASH */
	[VREG_OK_IRQ] = {
		.name		= "vreg-ok",
	},
	[ILIM_S2_IRQ] = {
		.name		= "ilim2-s2",
		.handler	= schgm_flash_ilim2_irq_handler,
	},
	[ILIM_S1_IRQ] = {
		.name		= "ilim1-s1",
	},
	[VOUT_DOWN_IRQ] = {
		.name		= "vout-down",
	},
	[VOUT_UP_IRQ] = {
		.name		= "vout-up",
	},
	[FLASH_STATE_CHANGE_IRQ] = {
		.name		= "flash-state-change",
		.handler	= schgm_flash_state_change_irq_handler,
	},
	[TORCH_REQ_IRQ] = {
		.name		= "torch-req",
	},
	[FLASH_EN_IRQ] = {
		.name		= "flash-en",
	},
	/* SDAM */
	[SDAM_STS_IRQ] = {
		.name		= "sdam-sts",
		.handler	= sdam_sts_change_irq_handler,
	},
};

static int smb5_get_irq_index_byname(const char *irq_name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(smb5_irqs); i++) {
		if (strcmp(smb5_irqs[i].name, irq_name) == 0)
			return i;
	}

	return -ENOENT;
}

static int smb5_request_interrupt(struct smb5 *chip,
				struct device_node *node, const char *irq_name)
{
	struct smb_charger *chg = &chip->chg;
	int rc, irq, irq_index;
	struct smb_irq_data *irq_data;

	irq = of_irq_get_byname(node, irq_name);
	if (irq < 0) {
		pr_err("Couldn't get irq %s byname\n", irq_name);
		return irq;
	}

	irq_index = smb5_get_irq_index_byname(irq_name);
	if (irq_index < 0) {
		pr_err("%s is not a defined irq\n", irq_name);
		return irq_index;
	}

	if (!smb5_irqs[irq_index].handler)
		return 0;

	irq_data = devm_kzalloc(chg->dev, sizeof(*irq_data), GFP_KERNEL);
	if (!irq_data)
		return -ENOMEM;

	irq_data->parent_data = chip;
	irq_data->name = irq_name;
	irq_data->storm_data = smb5_irqs[irq_index].storm_data;
	mutex_init(&irq_data->storm_data.storm_lock);

	smb5_irqs[irq_index].enabled = true;
	rc = devm_request_threaded_irq(chg->dev, irq, NULL,
					smb5_irqs[irq_index].handler,
					IRQF_ONESHOT, irq_name, irq_data);
	if (rc < 0) {
		pr_err("Couldn't request irq %d\n", irq);
		return rc;
	}

	smb5_irqs[irq_index].irq = irq;
	smb5_irqs[irq_index].irq_data = irq_data;
	if (smb5_irqs[irq_index].wake)
		enable_irq_wake(irq);

	return rc;
}

static int smb5_request_interrupts(struct smb5 *chip)
{
	struct smb_charger *chg = &chip->chg;
	struct device_node *node = chg->dev->of_node;
	struct device_node *child;
	int rc = 0;
	const char *name;
	struct property *prop;

	for_each_available_child_of_node(node, child) {
		of_property_for_each_string(child, "interrupt-names",
					    prop, name) {
			rc = smb5_request_interrupt(chip, child, name);
			if (rc < 0)
				return rc;
		}
	}

	vote(chg->limited_irq_disable_votable, CHARGER_TYPE_VOTER, true, 0);
	vote(chg->hdc_irq_disable_votable, CHARGER_TYPE_VOTER, true, 0);

	return rc;
}

static void smb5_free_interrupts(struct smb_charger *chg)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(smb5_irqs); i++) {
		if (smb5_irqs[i].irq > 0) {
			if (smb5_irqs[i].wake)
				disable_irq_wake(smb5_irqs[i].irq);

			devm_free_irq(chg->dev, smb5_irqs[i].irq,
						smb5_irqs[i].irq_data);
		}
	}
}

static void smb5_disable_interrupts(struct smb_charger *chg)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(smb5_irqs); i++) {
		if (smb5_irqs[i].irq > 0)
			disable_irq(smb5_irqs[i].irq);
	}
}

#if defined(CONFIG_DEBUG_FS)

static int force_batt_psy_update_write(void *data, u64 val)
{
	struct smb_charger *chg = data;

	power_supply_changed(chg->batt_psy);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(force_batt_psy_update_ops, NULL,
			force_batt_psy_update_write, "0x%02llx\n");

static int force_usb_psy_update_write(void *data, u64 val)
{
	struct smb_charger *chg = data;

	power_supply_changed(chg->usb_psy);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(force_usb_psy_update_ops, NULL,
			force_usb_psy_update_write, "0x%02llx\n");

static int force_dc_psy_update_write(void *data, u64 val)
{
	struct smb_charger *chg = data;

	power_supply_changed(chg->dc_psy);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(force_dc_psy_update_ops, NULL,
			force_dc_psy_update_write, "0x%02llx\n");

static void smb5_create_debugfs(struct smb5 *chip)
{
	struct dentry *file;

	chip->dfs_root = debugfs_create_dir("charger", NULL);
	if (IS_ERR_OR_NULL(chip->dfs_root)) {
		pr_err("Couldn't create charger debugfs rc=%ld\n",
			(long)chip->dfs_root);
		return;
	}

	file = debugfs_create_file("force_batt_psy_update", 0600,
			    chip->dfs_root, chip, &force_batt_psy_update_ops);
	if (IS_ERR_OR_NULL(file))
		pr_err("Couldn't create force_batt_psy_update file rc=%ld\n",
			(long)file);

	file = debugfs_create_file("force_usb_psy_update", 0600,
			    chip->dfs_root, chip, &force_usb_psy_update_ops);
	if (IS_ERR_OR_NULL(file))
		pr_err("Couldn't create force_usb_psy_update file rc=%ld\n",
			(long)file);

	file = debugfs_create_file("force_dc_psy_update", 0600,
			    chip->dfs_root, chip, &force_dc_psy_update_ops);
	if (IS_ERR_OR_NULL(file))
		pr_err("Couldn't create force_dc_psy_update file rc=%ld\n",
			(long)file);

	file = debugfs_create_u32("debug_mask", 0600, chip->dfs_root,
			&__debug_mask);
	if (IS_ERR_OR_NULL(file))
		pr_err("Couldn't create debug_mask file rc=%ld\n", (long)file);
}

#else

static void smb5_create_debugfs(struct smb5 *chip)
{}

#endif

static ssize_t proc_skin_threld_read(struct file *file, char __user *buf,
					    size_t count, loff_t *ppos)
{
	int ret = 0;
	char page[64];
	int len = 64;
	struct smb_charger *chg = g_chip;

	if (chg == NULL) {
		pr_err("smb driver is not ready");
		return -ENODEV;
	}

	memset(page, 0, len);
	len = snprintf(page, len, "[lcd_on] Hi:%d,pre-Hi:%d,Wrm:%d,pre-Wrm:%d,Med:%d,Nor:%d\n",
		chg->skin_thermal_high_threshold,
		chg->skin_thermal_pre_high_threshold,
		chg->skin_thermal_warm_threshold,
		chg->skin_thermal_pre_warm_threshold,
		chg->skin_thermal_medium_threshold,
		chg->skin_thermal_normal_threshold);
	ret = simple_read_from_buffer(buf, count, ppos, page, len);

	return ret;
}

static ssize_t proc_skin_threld_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *lo)
{
	char buffer[32] = { 0 };
	struct smb_charger *chg = g_chip;
	int hi_val, pre_hi_val;
	int wrm_val, pre_wrm_val;
	int med_val, nor_val;
	int ret = 0;

	if (chg == NULL) {
		pr_err("smb driver is not ready");
		return -ENODEV;
	}

	if (count > 32) {
		pr_err("input too many words.");
		return -EFAULT;
	}

	if (copy_from_user(buffer, buf, count)) {
		pr_err("copy parameter from user error.\n");
		return -EFAULT;
	}

	pr_info("buffer=%s", buffer);
	ret = sscanf(buffer, "%d %d %d %d %d %d", &hi_val, &pre_hi_val,
		&wrm_val, &pre_wrm_val, &med_val, &nor_val);
	pr_err("lcd_on config: hi_val=%d, pre_hi_val=%d, med_val=%d, nor_val=%d",
		hi_val, pre_hi_val, med_val, nor_val);

	if (ret == 6) {
		if ((hi_val >= pre_hi_val)
			&& (wrm_val >= pre_wrm_val)
			&& (med_val >= nor_val)) {
			chg->skin_thermal_high_threshold = hi_val;
			chg->skin_thermal_pre_high_threshold = pre_hi_val;
			chg->skin_thermal_warm_threshold = wrm_val;
			chg->skin_thermal_pre_warm_threshold = pre_wrm_val;
			chg->skin_thermal_medium_threshold = med_val;
			chg->skin_thermal_normal_threshold = nor_val;
		} else {
			pr_err("val not bigger one by one.");
			return -EINVAL;
		}
	} else {
		pr_err("need four decimal number.");
		return -EINVAL;
	}

	return count;
}

static const struct file_operations proc_skin_threld_ops = {
	.read = proc_skin_threld_read,
	.write = proc_skin_threld_write,
	.open = simple_open,
	.owner = THIS_MODULE,
};

static ssize_t proc_skin_lcdoff_threld_read(struct file *file, char __user *buf,
					    size_t count, loff_t *ppos)
{
	int ret = 0;
	char page[64];
	int len = 64;
	struct smb_charger *chg = g_chip;

	if (chg == NULL) {
		pr_err("smb driver is not ready");
		return -ENODEV;
	}

	memset(page, 0, len);
	len = snprintf(page, len, "[lcd_off] Hi:%d,pre-Hi:%d,Med:%d,Nor:%d\n",
		chg->skin_thermal_lcdoff_high_threshold,
		chg->skin_thermal_lcdoff_pre_high_threshold,
		chg->skin_thermal_lcdoff_medium_threshold,
		chg->skin_thermal_lcdoff_normal_threshold);
	ret = simple_read_from_buffer(buf, count, ppos, page, len);

	return ret;
}

static ssize_t proc_skin_lcdoff_threld_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *lo)
{
	char buffer[32] = { 0 };
	struct smb_charger *chg = g_chip;
	int hi_val, pre_hi_val, med_val, nor_val;
	int ret = 0;

	if (chg == NULL) {
		pr_err("smb driver is not ready");
		return -ENODEV;
	}

	if (count > 32) {
		pr_err("input too many words.");
		return -EFAULT;
	}

	if (copy_from_user(buffer, buf, count)) {
		pr_err("copy parameter from user error.\n");
		return -EFAULT;
	}

	pr_info("buffer=%s", buffer);
	ret = sscanf(buffer, "%d %d %d %d", &hi_val, &pre_hi_val, &med_val, &nor_val);
	pr_err("lcd_off: hi_val=%d, pre_hi_val=%d, med_val=%d, nor_val=%d",
		hi_val, pre_hi_val, med_val, nor_val);

	if (ret == 4) {
		if ((hi_val >= pre_hi_val)
			&& (med_val >= nor_val)) {
			chg->skin_thermal_lcdoff_high_threshold = hi_val;
			chg->skin_thermal_lcdoff_pre_high_threshold = pre_hi_val;
			chg->skin_thermal_lcdoff_medium_threshold = med_val;
			chg->skin_thermal_lcdoff_normal_threshold = nor_val;
		} else {
			pr_err("val not bigger one by one.");
			return -EINVAL;
		}
	} else {
		pr_err("need four decimal number.");
		return -EINVAL;
	}

	return count;
}

static const struct file_operations proc_skin_lcdoff_threld_ops = {
	.read = proc_skin_lcdoff_threld_read,
	.write = proc_skin_lcdoff_threld_write,
	.open = simple_open,
	.owner = THIS_MODULE,
};

static ssize_t proc_nor_skin_threld_read(struct file *file, char __user *buf,
					    size_t count, loff_t *ppos)
{
	int ret = 0;
	char page[64];
	int len = 64;
	struct smb_charger *chg = g_chip;

	if (chg == NULL) {
		pr_err("smb driver is not ready");
		return -ENODEV;
	}

	memset(page, 0, len);
	len = snprintf(page, len, "[lcd_on] Hi:%d,pre-Hi:%d,Wrm:%d,pre-Wrm:%d,Med:%d,Nor:%d\n",
		chg->nor_skin_thermal_high_threshold,
		chg->nor_skin_thermal_pre_high_threshold,
		chg->nor_skin_thermal_warm_threshold,
		chg->nor_skin_thermal_pre_warm_threshold,
		chg->nor_skin_thermal_medium_threshold,
		chg->nor_skin_thermal_normal_threshold);
	ret = simple_read_from_buffer(buf, count, ppos, page, len);

	return ret;
}

static ssize_t proc_nor_skin_threld_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *lo)
{
	char buffer[32] = { 0 };
	struct smb_charger *chg = g_chip;
	int hi_val, pre_hi_val;
	int wrm_val, pre_wrm_val;
	int med_val, nor_val;
	int ret = 0;

	if (chg == NULL) {
		pr_err("smb driver is not ready");
		return -ENODEV;
	}

	if (count > 32) {
		pr_err("input too many words.");
		return -EFAULT;
	}

	if (copy_from_user(buffer, buf, count)) {
		pr_err("copy parameter from user error.\n");
		return -EFAULT;
	}

	pr_info("buffer=%s", buffer);
	ret = sscanf(buffer, "%d %d %d %d %d %d", &hi_val, &pre_hi_val,
		&wrm_val, &pre_wrm_val, &med_val, &nor_val);
	pr_err("lcd_on config: hi_val=%d, pre_hi_val=%d, med_val=%d, nor_val=%d",
		hi_val, pre_hi_val, med_val, nor_val);

	if (ret == 6) {
		if ((hi_val >= pre_hi_val)
			&& (wrm_val >= pre_wrm_val)
			&& (med_val >= nor_val)) {
			chg->nor_skin_thermal_high_threshold = hi_val;
			chg->nor_skin_thermal_pre_high_threshold = pre_hi_val;
			chg->nor_skin_thermal_warm_threshold = wrm_val;
			chg->nor_skin_thermal_pre_warm_threshold = pre_wrm_val;
			chg->nor_skin_thermal_medium_threshold = med_val;
			chg->nor_skin_thermal_normal_threshold = nor_val;
		} else {
			pr_err("val not bigger one by one.");
			return -EINVAL;
		}
	} else {
		pr_err("need four decimal number.");
		return -EINVAL;
	}

	return count;
}

static const struct file_operations proc_nor_skin_threld_ops = {
	.read = proc_nor_skin_threld_read,
	.write = proc_nor_skin_threld_write,
	.open = simple_open,
	.owner = THIS_MODULE,
};

static ssize_t proc_nor_skin_lcdoff_threld_read(struct file *file, char __user *buf,
					    size_t count, loff_t *ppos)
{
	int ret = 0;
	char page[64];
	int len = 64;
	struct smb_charger *chg = g_chip;

	if (chg == NULL) {
		pr_err("smb driver is not ready");
		return -ENODEV;
	}

	memset(page, 0, len);
	len = snprintf(page, len, "[lcd_off] Hi:%d,pre-Hi:%d,Wrm:%d,pre-Wrm:%d,Med:%d,Nor:%d\n",
		chg->nor_skin_thermal_lcdoff_high_threshold,
		chg->nor_skin_thermal_lcdoff_pre_high_threshold,
		chg->nor_skin_thermal_lcdoff_warm_threshold,
		chg->nor_skin_thermal_lcdoff_pre_warm_threshold,
		chg->nor_skin_thermal_lcdoff_medium_threshold,
		chg->nor_skin_thermal_lcdoff_normal_threshold);
	ret = simple_read_from_buffer(buf, count, ppos, page, len);

	return ret;
}

static ssize_t proc_nor_skin_lcdoff_threld_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *lo)
{
	char buffer[32] = { 0 };
	struct smb_charger *chg = g_chip;
	int hi_val, pre_hi_val;
	int wrm_val, pre_wrm_val;
	int med_val, nor_val;
	int ret = 0;

	if (chg == NULL) {
		pr_err("smb driver is not ready");
		return -ENODEV;
	}

	if (count > 32) {
		pr_err("input too many words.");
		return -EFAULT;
	}

	if (copy_from_user(buffer, buf, count)) {
		pr_err("copy parameter from user error.\n");
		return -EFAULT;
	}

	pr_info("buffer=%s", buffer);
	ret = sscanf(buffer, "%d %d %d %d %d %d", &hi_val, &pre_hi_val,
		&wrm_val, &pre_wrm_val, &med_val, &nor_val);
	pr_err("lcd_on config: hi_val=%d, pre_hi_val=%d, med_val=%d, nor_val=%d",
		hi_val, pre_hi_val, med_val, nor_val);

	if (ret == 6) {
		if ((hi_val >= pre_hi_val)
			&& (wrm_val >= pre_wrm_val)
			&& (med_val >= nor_val)) {
			chg->nor_skin_thermal_lcdoff_high_threshold = hi_val;
			chg->nor_skin_thermal_lcdoff_pre_high_threshold = pre_hi_val;
			chg->nor_skin_thermal_lcdoff_warm_threshold = wrm_val;
			chg->nor_skin_thermal_lcdoff_pre_warm_threshold = pre_wrm_val;
			chg->nor_skin_thermal_lcdoff_medium_threshold = med_val;
			chg->nor_skin_thermal_lcdoff_normal_threshold = nor_val;
		} else {
			pr_err("val not bigger one by one.");
			return -EINVAL;
		}
	} else {
		pr_err("need four decimal number.");
		return -EINVAL;
	}

	return count;
}

static const struct file_operations proc_nor_skin_lcdoff_threld_ops = {
	.read = proc_nor_skin_lcdoff_threld_read,
	.write = proc_nor_skin_lcdoff_threld_write,
	.open = simple_open,
	.owner = THIS_MODULE,
};

static int create_skin_thermal_proc(void)
{
	struct proc_dir_entry *skin_threshold_dir;

	skin_threshold_dir = proc_mkdir("skin_temp_thrd", NULL);
	if (!skin_threshold_dir) {
		pr_err("proc create skin_temp_thrd failed");
		return -EINVAL;
	}

	if (!proc_create("fastchg_thd", 0644, skin_threshold_dir, &proc_skin_threld_ops))
		pr_err("Failed to register fastchg_thd proc interface\n");
	if (!proc_create("fastchg_lcdoff_thd", 0644, skin_threshold_dir, &proc_skin_lcdoff_threld_ops))
		pr_err("Failed to register fastchg_lcdoff_thd proc interface\n");
	if (!proc_create("norchg_thd", 0644, skin_threshold_dir, &proc_nor_skin_threld_ops))
		pr_err("Failed to register norchg_thd proc interface\n");
	if (!proc_create("norchg_lcdoff_thd", 0644, skin_threshold_dir, &proc_nor_skin_lcdoff_threld_ops))
		pr_err("Failed to register norchg_lcdoff_thd proc interface\n");
	return 0;
}
#ifdef CONFIG_PROC_FS
static ssize_t write_ship_mode(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{

	if (count) {
		g_chip->ship_mode = true;
		pr_err(" * * * XCB * * * write ship mode\n");
	}
	return count;
}

static const struct file_operations proc_ship_mode_operations = {
	.write		= write_ship_mode,
	.llseek		= noop_llseek,
};
#endif

static int op_ship_mode_gpio_request(struct smb_charger *chip)
{
	int rc;

	chip->pinctrl = devm_pinctrl_get(chip->dev);
	if (IS_ERR_OR_NULL(chip->pinctrl)) {
		dev_err(chip->dev,
				"Unable to acquire pinctrl\n");
		chip->pinctrl = NULL;
		return -EINVAL;
	}

	chip->ship_mode_default =
		pinctrl_lookup_state(chip->pinctrl, "op_ship_mode_default");
	if (IS_ERR_OR_NULL(chip->ship_mode_default)) {
		dev_err(chip->dev,
				"Can not lookup ship_mode_default\n");
		devm_pinctrl_put(chip->pinctrl);
		chip->pinctrl = NULL;
		return PTR_ERR(chip->ship_mode_default);
	}

	if (pinctrl_select_state(chip->pinctrl,
				chip->ship_mode_default) < 0)
		dev_err(chip->dev, "pinctrl set ship_mode_default fail\n");

	if (gpio_is_valid(chip->shipmode_en)) {
		rc = gpio_request(chip->shipmode_en, "stm6620_ctrl");
		if (rc) {
			pr_err("gpio_request failed for %d rc=%d\n",
				chip->shipmode_en, rc);
			return -EINVAL;
		}
		gpio_direction_output(chip->shipmode_en, 0);

		pr_info("ship_mode_gpio_request default mode success!\n");
	}

	return 0;

}

void requset_vbus_ctrl_gpio(struct smb_charger *chg)
{
	int ret;

	if (!gpio_is_valid(chg->vbus_ctrl))
		return;
	ret = gpio_request(chg->vbus_ctrl, "VbusCtrl");
	if (ret)
		pr_err("request failed,gpio:%d ret=%d\n", chg->vbus_ctrl, ret);
}

static int op_config_usb_temperature_adc(struct smb_charger *chip)
{
	if (IS_ERR_OR_NULL(chip->pinctrl)) {
		chip->pinctrl = devm_pinctrl_get(chip->dev);
		if (IS_ERR_OR_NULL(chip->pinctrl)) {
			dev_err(chip->dev,
					"Unable to acquire pinctrl\n");
			chip->pinctrl = NULL;
			return -EINVAL;
		}
	}

	chip->usb_temperature_default =
		pinctrl_lookup_state(chip->pinctrl, "op_usb_temp_adc_default");
	if (IS_ERR_OR_NULL(chip->usb_temperature_default)) {
		dev_err(chip->dev,
				"Can not lookup op_usb_temp_adc_default\n");
		devm_pinctrl_put(chip->pinctrl);
		chip->pinctrl = NULL;
		return PTR_ERR(chip->usb_temperature_default);
	}

	if (pinctrl_select_state(chip->pinctrl,
				chip->usb_temperature_default) < 0)
		dev_err(chip->dev, "pinctrl set op_usb_temp_adc_default fail\n");

	chip->usb_temperature_sec =
		pinctrl_lookup_state(chip->pinctrl, "op_usb_temp_adc_sec");
	if (IS_ERR_OR_NULL(chip->usb_temperature_sec)) {
		dev_err(chip->dev,
				"Can not lookup op_usb_temp_adc_sec\n");
		devm_pinctrl_put(chip->pinctrl);
		chip->pinctrl = NULL;
		return PTR_ERR(chip->usb_temperature_sec);
	}

	if (pinctrl_select_state(chip->pinctrl,
				chip->usb_temperature_sec) < 0)
		dev_err(chip->dev, "pinctrl set op_usb_temp_adc_sec fail\n");
	return 0;
}

/*usb connector hw auto detection*/
static irqreturn_t op_usb_plugin_irq_handler(int irq, void *dev_id)
{
	schedule_work(&g_chip->otg_switch_work);
	return IRQ_HANDLED;
}

static void request_plug_irq(struct smb_charger *chip)
{
	int ret;

	if (!gpio_is_valid(chip->plug_irq))
		return;
	ret = gpio_request(chip->plug_irq, "op_usb_plug");
	if (ret) {
		pr_err("request failed,gpio:%d ret=%d\n", chip->plug_irq, ret);
		return;
	}
	gpio_direction_input(chip->plug_irq);
	ret = request_irq(gpio_to_irq(chip->plug_irq),
			op_usb_plugin_irq_handler,
			IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			"op_usb_plug", chip);
	if (ret < 0) {
		pr_err("request usb_plug irq failed.\n");
		return;
	}
	enable_irq_wake(gpio_to_irq(chip->plug_irq));
	pr_info("request usb_plug irq success\n");
	/*connect with usb cable when reboot, give a vote 1*/
	if (!gpio_get_value(chip->plug_irq)) {
		pr_info("%s:reboot time hw detect gpio low, vote 1\n",
			__func__);
		vote(chip->otg_toggle_votable, HW_DETECT_VOTER, 1, 0);
		vote(chip->fcc_votable, FCC_STEPPER_VOTER, false, 0);
		chip->hw_detect = 1;
		chip->pre_cable_pluged = 1;
		ret = plugin_update(chip);
		pr_info("%s:hw_detect=%d and report rc: %d\n",
						__func__, chip->hw_detect, ret);
	} else
		chip->pre_cable_pluged = 0;
}

static int smb5_show_charger_status(struct smb5 *chip)
{
	struct smb_charger *chg = &chip->chg;
	union power_supply_propval val;
	int usb_present, batt_present, batt_health, batt_charge_type;
	int rc;

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		pr_err("Couldn't get usb present rc=%d\n", rc);
		return rc;
	}
	usb_present = val.intval;

	rc = smblib_get_prop_batt_present(chg, &val);
	if (rc < 0) {
		pr_err("Couldn't get batt present rc=%d\n", rc);
		return rc;
	}
	batt_present = val.intval;

	rc = smblib_get_prop_batt_health(chg, &val);
	if (rc < 0) {
		pr_err("Couldn't get batt health rc=%d\n", rc);
		val.intval = POWER_SUPPLY_HEALTH_UNKNOWN;
	}
	batt_health = val.intval;

	rc = smblib_get_prop_batt_charge_type(chg, &val);
	if (rc < 0) {
		pr_err("Couldn't get batt charge type rc=%d\n", rc);
		return rc;
	}
	batt_charge_type = val.intval;

	pr_info("SMB5 status - usb:present=%d type=%d batt:present = %d health = %d charge = %d\n",
		usb_present, chg->real_charger_type,
		batt_present, batt_health, batt_charge_type);
	return rc;
}

/*********************************
 * TYPEC CLASS REGISTRATION *
 **********************************/

static int smb5_init_typec_class(struct smb5 *chip)
{
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	/* Register typec class for only non-PD TypeC and uUSB designs */
	if (!chg->pd_not_supported)
		return rc;

	mutex_init(&chg->typec_lock);
	chg->typec_caps.type = TYPEC_PORT_DRP;
	chg->typec_caps.data = TYPEC_PORT_DRD;
	chg->typec_partner_desc.usb_pd = false;
	chg->typec_partner_desc.accessory = TYPEC_ACCESSORY_NONE;
	chg->typec_caps.port_type_set = smblib_typec_port_type_set;
	chg->typec_caps.revision = 0x0130;

	chg->typec_port = typec_register_port(chg->dev, &chg->typec_caps);
	if (IS_ERR(chg->typec_port)) {
		rc = PTR_ERR(chg->typec_port);
		pr_err("failed to register typec_port rc=%d\n", rc);
		return rc;
	}

	return rc;
}

static int smb5_probe(struct platform_device *pdev)
{
	struct smb5 *chip;
	struct smb_charger *chg;
	int rc = 0;
	struct msm_bus_scale_pdata *pdata;
	union power_supply_propval val;
	int usb_present;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chg = &chip->chg;
	g_chip = chg;
	chg->dev = &pdev->dev;
	chg->debug_mask = &__debug_mask;
	chg->pd_disabled = 0;
	chg->weak_chg_icl_ua = 500000;
	chg->usb_connector_temp = &__usb_connector_temp;
	chg->usb_interval_temp = &__usb_interval_temp;
	chg->disable_connector_protect = &__disable_connector_protect;
	chg->call_on = &__call_on;
	chg->video_call_on = &__video_call_on;
	chg->mode = PARALLEL_MASTER;
	chg->irq_info = smb5_irqs;
	chg->die_health = -EINVAL;
	chg->connector_health = -EINVAL;
	chg->otg_present = false;
	chg->main_fcc_max = -EINVAL;
	mutex_init(&chg->adc_lock);

	chg->regmap = dev_get_regmap(chg->dev->parent, NULL);
	if (!chg->regmap) {
		pr_err("parent regmap is missing\n");
		return -EINVAL;
	}

	rc = smb5_chg_config_init(chip);
	if (rc < 0) {
		if (rc != -EPROBE_DEFER)
			pr_err("Couldn't setup chg_config rc=%d\n", rc);
		return rc;
	}

	rc = smb5_parse_dt(chip);
	if (rc < 0) {
		pr_err("Couldn't parse device tree rc=%d\n", rc);
		return rc;
	}

	if (alarmtimer_get_rtcdev())
		alarm_init(&chg->lpd_recheck_timer, ALARM_REALTIME,
				smblib_lpd_recheck_timer);
	else
		return -EPROBE_DEFER;

	rc = smblib_init(chg);
	if (rc < 0) {
		pr_err("Smblib_init failed rc=%d\n", rc);
		return rc;
	}

	/* set driver data before resources request it */
	platform_set_drvdata(pdev, chip);

	op_charge_info_init(chg);
	pdata = msm_bus_cl_get_pdata(pdev);
	if (!pdata)
		pr_err("GPIO** failed get_pdata client_id\n");
	else
		chg->bus_client = msm_bus_scale_register_client(pdata);


	/* extcon registration */
	chg->extcon = devm_extcon_dev_allocate(chg->dev, smblib_extcon_cable);
	if (IS_ERR(chg->extcon)) {
		rc = PTR_ERR(chg->extcon);
		dev_err(chg->dev, "failed to allocate extcon device rc=%d\n",
				rc);
		goto cleanup;
	}

	rc = devm_extcon_dev_register(chg->dev, chg->extcon);
	if (rc < 0) {
		dev_err(chg->dev, "failed to register extcon device rc=%d\n",
				rc);
		goto cleanup;
	}

	/* Support reporting polarity and speed via properties */
	rc = extcon_set_property_capability(chg->extcon,
			EXTCON_USB, EXTCON_PROP_USB_TYPEC_POLARITY);
	rc |= extcon_set_property_capability(chg->extcon,
			EXTCON_USB, EXTCON_PROP_USB_SS);
	rc |= extcon_set_property_capability(chg->extcon,
			EXTCON_USB_HOST, EXTCON_PROP_USB_TYPEC_POLARITY);
	rc |= extcon_set_property_capability(chg->extcon,
			EXTCON_USB_HOST, EXTCON_PROP_USB_SS);
	if (rc < 0) {
		dev_err(chg->dev,
			"failed to configure extcon capabilities\n");
		goto cleanup;
	}

	rc = smb5_init_hw(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize hardware rc=%d\n", rc);
		goto cleanup;
	}

	/*
	 * VBUS regulator enablement/disablement for host mode is handled
	 * by USB-PD driver only. For micro-USB and non-PD typeC designs,
	 * the VBUS regulator is enabled/disabled by the smb driver itself
	 * before sending extcon notifications.
	 * Hence, register vbus and vconn regulators for PD supported designs
	 * only.
	 */
	if (!chg->pd_not_supported) {
		rc = smb5_init_vbus_regulator(chip);
		if (rc < 0) {
			pr_err("Couldn't initialize vbus regulator rc=%d\n",
				rc);
			goto cleanup;
		}

		rc = smb5_init_vconn_regulator(chip);
		if (rc < 0) {
			pr_err("Couldn't initialize vconn regulator rc=%d\n",
				rc);
			goto cleanup;
		}
	}

	switch (chg->chg_param.smb_version) {
	case PM8150B_SUBTYPE:
	case PM6150_SUBTYPE:
	case PM7250B_SUBTYPE:
		rc = smb5_init_dc_psy(chip);
		if (rc < 0) {
			pr_err("Couldn't initialize dc psy rc=%d\n", rc);
			goto cleanup;
		}
		break;
	default:
		break;
	}

	rc = smb5_init_usb_psy(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize usb psy rc=%d\n", rc);
		goto cleanup;
	}

	rc = smb5_init_usb_main_psy(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize usb main psy rc=%d\n", rc);
		goto cleanup;
	}

	rc = smb5_init_usb_port_psy(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize usb pc_port psy rc=%d\n", rc);
		goto cleanup;
	}

	rc = smb5_init_batt_psy(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize batt psy rc=%d\n", rc);
		goto cleanup;
	}

	rc = smb5_init_typec_class(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize typec class rc=%d\n", rc);
		goto cleanup;
	}

	rc = smb5_determine_initial_status(chip);
	if (rc < 0) {
		pr_err("Couldn't determine initial status rc=%d\n",
			rc);
		goto cleanup;
	}

	rc = smb5_request_interrupts(chip);
	if (rc < 0) {
		pr_err("Couldn't request interrupts rc=%d\n", rc);
		goto cleanup;
	}

	rc = smb5_post_init(chip);
	if (rc < 0) {
		pr_err("Failed in post init rc=%d\n", rc);
		goto free_irq;
	}

	smb5_create_debugfs(chip);

	rc = sysfs_create_groups(&chg->dev->kobj, smb5_groups);
	if (rc < 0) {
		pr_err("Couldn't create sysfs files rc=%d\n", rc);
		goto free_irq;
	}

	rc = smb5_show_charger_status(chip);
	if (rc < 0) {
		pr_err("Failed in getting charger status rc=%d\n", rc);
		goto free_irq;
	}

#ifdef CONFIG_PROC_FS
	if (!proc_create("ship_mode", 0644, NULL, &proc_ship_mode_operations))
		pr_err("Failed to register proc interface\n");
#endif

	rc = create_skin_thermal_proc();
	if (rc < 0)
		pr_err("create skin thermal threshold proc failed.");

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		pr_err("Couldn't get usb present rc=%d\n", rc);
		goto cleanup;
	}
	usb_present = val.intval;

	if (usb_present) {
		schedule_delayed_work(&chg->non_standard_charger_check_work,
				msecs_to_jiffies(TIME_1000MS));
		chg->boot_usb_present = true;
	}
	if (!usb_present && chg->vbus_present)
		op_handle_usb_plugin(chg);

	device_init_wakeup(chg->dev, true);

	op_ship_mode_gpio_request(chg);
	requset_vbus_ctrl_gpio(chg);
	op_config_usb_temperature_adc(chg);
	request_plug_irq(chg);
	exchg_information_register(chg);
	chg->probe_done = true;
	pr_info("QPNP SMB5 probed successfully\n");

	return rc;

free_irq:
	smb5_free_interrupts(chg);
cleanup:
	smblib_deinit(chg);
	platform_set_drvdata(pdev, NULL);

	return rc;
}

static int smb5_remove(struct platform_device *pdev)
{
	struct smb5 *chip = platform_get_drvdata(pdev);
	struct smb_charger *chg = &chip->chg;

	/* force enable APSD */
	smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
				BC1P2_SRC_DETECT_BIT, BC1P2_SRC_DETECT_BIT);

	smb5_free_interrupts(chg);
	smblib_deinit(chg);
	sysfs_remove_groups(&chg->dev->kobj, smb5_groups);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static void stm6620_enter_ship_mode(struct smb_charger *chg)
{
	int i;

	for (i = 0; i < 5; i++) {
		gpio_set_value(chg->shipmode_en, 1);
		usleep_range(4000, 4001);
		gpio_set_value(chg->shipmode_en, 0);
		usleep_range(4000, 4001);
	}
}

static void smb5_shutdown(struct platform_device *pdev)
{
	struct smb5 *chip = platform_get_drvdata(pdev);
	struct smb_charger *chg = &chip->chg;

#ifdef CONFIG_PROC_FS
	pr_info("smbchg_shutdown\n");

	if (chg->ship_mode) {
		pr_info("smbchg_shutdown enter ship_mode\n");
		/* Enable external stm6620 ship mode */
		if (gpio_is_valid(chg->shipmode_en)) {
			vote(chg->usb_icl_votable,
					DEFAULT_VOTER, true, 0);
			stm6620_enter_ship_mode(chg);
		} else {
			smblib_masked_write(chg, SHIP_MODE_REG,
			SHIP_MODE_EN_BIT, SHIP_MODE_EN_BIT);
		}
		clean_backup_soc_ex();
		msleep(1000);
		pr_err("after 1s\n");
		while (1)
			;
	}
#endif

	/* disable all interrupts */
	smb5_disable_interrupts(chg);

	/* disable the SMB_EN configuration */
	smblib_masked_write(chg, MISC_SMB_EN_CMD_REG, EN_CP_CMD_BIT, 0);

	/* configure power role for UFP */
	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_TYPEC)
		smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
				TYPEC_POWER_ROLE_CMD_MASK, EN_SNK_ONLY_BIT);

	/* force enable and rerun APSD */
	smblib_apsd_enable(chg, true);
	smblib_hvdcp_exit_config(chg);
}

static const struct of_device_id match_table[] = {
	{ .compatible = "qcom,qpnp-smb5", },
	{ },
};

static struct platform_driver smb5_driver = {
	.driver		= {
		.name		= "qcom,qpnp-smb5",
		.of_match_table	= match_table,
	},
	.probe		= smb5_probe,
	.remove		= smb5_remove,
	.shutdown	= smb5_shutdown,
};
module_platform_driver(smb5_driver);

MODULE_DESCRIPTION("QPNP SMB5 Charger Driver");
MODULE_LICENSE("GPL v2");
