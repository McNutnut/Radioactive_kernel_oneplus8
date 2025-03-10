// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2020 The Linux Foundation. All rights reserved.
 */
#define pr_fmt(fmt) "SMBLIB: %s: " fmt, __func__

#define CONFIG_MSM_RDM_NOTIFY
#undef CONFIG_FB

#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/regulator/driver.h>
#include <linux/qpnp/qpnp-revid.h>
#include <linux/irq.h>
#include <linux/iio/consumer.h>
#include <linux/pmic-voter.h>
#include <linux/of_batterydata.h>
#include <linux/ktime.h>
#include "smb5-lib.h"
#include "smb5-reg.h"
#include "schgm-flash.h"
#include "step-chg-jeita.h"
#include "storm-watch.h"
#include "schgm-flash.h"
#include <linux/oem/power/oem_external_fg.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/input/qpnp-power-on.h>
#include <linux/spmi.h>
#if defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#elif defined(CONFIG_MSM_RDM_NOTIFY)
#include <linux/msm_drm_notify.h>
#include <drm/drm_panel.h>
#include <linux/notifier.h>
#endif /*CONFIG_FB*/
#include <linux/moduleparam.h>
#include <linux/msm-bus.h>
#include <linux/oem/power/op_charge.h>
#include <linux/oem/boot_mode.h>
#include <linux/oem/power/op_wlc_helper.h>
#include <linux/usb/usbpd.h>

#ifdef CONFIG_FORCE_FAST_CHARGE
#include <linux/fastchg.h>
#endif

#define SOC_INVALID                   0x7E
#define SOC_DATA_REG_SDRAM            0xB1A0
#define SOC_DATA_FLAG_SDRAM           0x88E
#define SOC_FLAG_BIT                  BIT(7)

#define SOC_DATA_REG_0                0x88D
#define SOC_FLAG_REG                  0x88D
#define HEARTBEAT_INTERVAL_MS         6000
#define CHG_TIMEOUT_COUNT             6000 /* 10hr */
#define CHG_SOFT_OVP_MV               5800
#define SWARP_CHG_SOFT_OVP_MV         9900
#define BATT_SOFT_OVP_MV              4500
#define CHG_SOFT_UVP_MV               4300
#define CHG_VOLTAGE_NORMAL            5000
#define BATT_REMOVE_TEMP              -400
#define BATT_TEMP_HYST                20
#define DASH_VALID_TEMP_LOW_THRESHOLD	125
#define DASH_VALID_TEMP_HIG_THRESHOLD	430
#define DASH_VALID_CAPACITY_HIG_THRESHOLD 90
#define DASH_VALID_VBAT_LOW_THRE_MV 3450

struct smb_charger *g_chg;
struct regmap *pm_regmap;
struct regmap *pm8150_regmap;
struct regmap *sdram_regmap;

static struct external_battery_gauge *fast_charger;
static bool set_prop_fast_switch_to_normal_false(struct smb_charger *chg);

static void op_battery_temp_region_set(struct smb_charger *chg,
		enum temp_region_type batt_temp_region);
static void set_usb_switch(struct smb_charger *chg, bool enable);
static void op_handle_usb_removal(struct smb_charger *chg);
static bool get_prop_fast_switch_to_normal(struct smb_charger *chg);
static int get_prop_batt_temp(struct smb_charger *chg);
static int get_prop_batt_capacity(struct smb_charger *chg);
static int get_prop_batt_current_now(struct smb_charger *chg);
static int get_prop_batt_voltage_now(struct smb_charger *chg);
static int set_property_on_fg(struct smb_charger *chg,
		enum power_supply_property prop, int val);
static int set_dash_charger_present(int status);
static enum temp_region_type
		op_battery_temp_region_get(struct smb_charger *chg);
static int get_prop_fg_capacity(struct smb_charger *chg);
static int get_prop_fg_current_now(struct smb_charger *chg);
static int get_prop_fg_voltage_now(struct smb_charger *chg);
static void op_check_charger_collapse(struct smb_charger *chg);
static int op_set_collapse_fet(struct smb_charger *chg, bool on);
static int op_check_battery_temp(struct smb_charger *chg);
static int get_usb_temp(struct smb_charger *chg);
static void op_clean_dash_status(void);
static void op_typec_state_change_irq_handler(void);
static int sys_boot_complete;
static int usb_enum_check(const char *val, const struct kernel_param *kp);
static enum batt_status_type op_battery_status_get(struct smb_charger *chg);
static bool is_usb_present(struct smb_charger *chg);
static void op_register_pps_work(struct work_struct *work);
static void pps_usbpd_connect_cb(struct usbpd_svid_handler *hdlr,
		bool peer_usb_comm);
static void pps_usbpd_disconnect_cb(struct usbpd_svid_handler *hdlr);
static struct op_pps op_pps_chg;

module_param_call(sys_boot_complete, usb_enum_check, param_get_int, &sys_boot_complete, 0644);
MODULE_PARM_DESC(sys_boot_complete, "sys_boot_complete");

#define smblib_err(chg, fmt, ...)		\
	pr_err("%s: %s: " fmt, chg->name,	\
		__func__, ##__VA_ARGS__)	\

#define smblib_dbg(chg, reason, fmt, ...)			\
	do {							\
		if (*chg->debug_mask & (reason))		\
			pr_info("%s: %s: " fmt, chg->name,	\
				__func__, ##__VA_ARGS__);	\
		else						\
			pr_debug("%s: %s: " fmt, chg->name,	\
				__func__, ##__VA_ARGS__);	\
	} while (0)

#define typec_rp_med_high(chg, typec_mode)			\
	((typec_mode == POWER_SUPPLY_TYPEC_SOURCE_MEDIUM	\
	|| typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH)	\
	&& (!chg->typec_legacy || chg->typec_legacy_use_rp_icl))

static void update_sw_icl_max(struct smb_charger *chg, int pst);
static int smblib_get_prop_typec_mode(struct smb_charger *chg);

int smblib_read(struct smb_charger *chg, u16 addr, u8 *val)
{
	unsigned int value;
	int rc = 0;

	rc = regmap_read(chg->regmap, addr, &value);
	if (rc >= 0)
		*val = (u8)value;

	return rc;
}

int smblib_batch_read(struct smb_charger *chg, u16 addr, u8 *val,
			int count)
{
	return regmap_bulk_read(chg->regmap, addr, val, count);
}

int smblib_write(struct smb_charger *chg, u16 addr, u8 val)
{
	return regmap_write(chg->regmap, addr, val);
}

int smblib_batch_write(struct smb_charger *chg, u16 addr, u8 *val,
			int count)
{
	return regmap_bulk_write(chg->regmap, addr, val, count);
}

int smblib_masked_write(struct smb_charger *chg, u16 addr, u8 mask, u8 val)
{
	return regmap_update_bits(chg->regmap, addr, mask, val);
}

int smblib_get_iio_channel(struct smb_charger *chg, const char *propname,
					struct iio_channel **chan)
{
	int rc = 0;

	rc = of_property_match_string(chg->dev->of_node,
					"io-channel-names", propname);
	if (rc < 0)
		return 0;

	*chan = iio_channel_get(chg->dev, propname);
	if (IS_ERR(*chan)) {
		rc = PTR_ERR(*chan);
		if (rc != -EPROBE_DEFER)
			smblib_err(chg, "%s channel unavailable, %d\n",
							propname, rc);
		*chan = NULL;
	}

	return rc;
}

#define DIV_FACTOR_MICRO_V_I	1
#define DIV_FACTOR_MILI_V_I	1000
#define DIV_FACTOR_DECIDEGC	100
int smblib_read_iio_channel(struct smb_charger *chg, struct iio_channel *chan,
							int div, int *data)
{
	int rc = 0;
	*data = -ENODATA;

	if (chan) {
		rc = iio_read_channel_processed(chan, data);
		if (rc < 0) {
			smblib_err(chg, "Error in reading IIO channel data, rc=%d\n",
					rc);
			return rc;
		}

		if (div != 0)
			*data /= div;
	}

	return rc;
}

int smblib_get_jeita_cc_delta(struct smb_charger *chg, int *cc_delta_ua)
{
	int rc, cc_minus_ua;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_7_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}

	if (stat & BAT_TEMP_STATUS_HOT_SOFT_BIT) {
		rc = smblib_get_charge_param(chg, &chg->param.jeita_cc_comp_hot,
					&cc_minus_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get jeita cc minus rc=%d\n",
					rc);
			return rc;
		}
	} else if (stat & BAT_TEMP_STATUS_COLD_SOFT_BIT) {
		rc = smblib_get_charge_param(chg,
					&chg->param.jeita_cc_comp_cold,
					&cc_minus_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get jeita cc minus rc=%d\n",
					rc);
			return rc;
		}
	} else {
		cc_minus_ua = 0;
	}

	*cc_delta_ua = -cc_minus_ua;

	return 0;
}

int smblib_icl_override(struct smb_charger *chg, enum icl_override_mode  mode)
{
	int rc;
	u8 usb51_mode, icl_override, apsd_override;

	switch (mode) {
	case SW_OVERRIDE_USB51_MODE:
		usb51_mode = 0;
		icl_override = ICL_OVERRIDE_BIT;
		apsd_override = 0;
		break;
	case SW_OVERRIDE_HC_MODE:
		usb51_mode = USBIN_MODE_CHG_BIT;
		icl_override = 0;
		apsd_override = ICL_OVERRIDE_AFTER_APSD_BIT;
		break;
	case SW_OVERRIDE_WIRELESS_MODE:
		usb51_mode = USBIN_MODE_CHG_BIT;
		icl_override = ICL_OVERRIDE_BIT;
		apsd_override = ICL_OVERRIDE_AFTER_APSD_BIT;
		break;
	case HW_AUTO_MODE:
	default:
		usb51_mode = USBIN_MODE_CHG_BIT;
		icl_override = 0;
		apsd_override = 0;
		break;
	}

	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
				USBIN_MODE_CHG_BIT, usb51_mode);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set USBIN_ICL_OPTIONS rc=%d\n", rc);
		return rc;
	}

	rc = smblib_masked_write(chg, CMD_ICL_OVERRIDE_REG,
				ICL_OVERRIDE_BIT, icl_override);
	if (rc < 0) {
		smblib_err(chg, "Couldn't override ICL rc=%d\n", rc);
		return rc;
	}

	rc = smblib_masked_write(chg, USBIN_LOAD_CFG_REG,
				ICL_OVERRIDE_AFTER_APSD_BIT, apsd_override);
	if (rc < 0) {
		smblib_err(chg, "Couldn't override ICL_AFTER_APSD rc=%d\n", rc);
		return rc;
	}

	return rc;
}

/*
 * This function does smb_en pin access, which is lock protected.
 * It should be called with smb_lock held.
 */
static int smblib_select_sec_charger_locked(struct smb_charger *chg,
					int sec_chg)
{
	int rc = 0;

	switch (sec_chg) {
	case POWER_SUPPLY_CHARGER_SEC_CP:
		vote(chg->pl_disable_votable, PL_SMB_EN_VOTER, true, 0);

		/* select Charge Pump instead of slave charger */
		rc = smblib_masked_write(chg, MISC_SMB_CFG_REG,
					SMB_EN_SEL_BIT, SMB_EN_SEL_BIT);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't select SMB charger rc=%d\n",
				rc);
			return rc;
		}
		/* Enable Charge Pump, under HW control */
		rc = smblib_masked_write(chg, MISC_SMB_EN_CMD_REG,
					EN_CP_CMD_BIT, EN_CP_CMD_BIT);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't enable SMB charger rc=%d\n",
						rc);
			return rc;
		}
		vote(chg->smb_override_votable, PL_SMB_EN_VOTER, false, 0);
		break;
	case POWER_SUPPLY_CHARGER_SEC_PL:
		/* select slave charger instead of Charge Pump */
		rc = smblib_masked_write(chg, MISC_SMB_CFG_REG,
					SMB_EN_SEL_BIT, 0);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't select SMB charger rc=%d\n",
				rc);
			return rc;
		}
		/* Enable slave charger, under HW control */
		rc = smblib_masked_write(chg, MISC_SMB_EN_CMD_REG,
					EN_STAT_CMD_BIT, EN_STAT_CMD_BIT);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't enable SMB charger rc=%d\n",
						rc);
			return rc;
		}
		vote(chg->smb_override_votable, PL_SMB_EN_VOTER, false, 0);

		vote(chg->pl_disable_votable, PL_SMB_EN_VOTER, false, 0);

		break;
	case POWER_SUPPLY_CHARGER_SEC_NONE:
	default:
		vote(chg->pl_disable_votable, PL_SMB_EN_VOTER, true, 0);

		/* SW override, disabling secondary charger(s) */
		vote(chg->smb_override_votable, PL_SMB_EN_VOTER, true, 0);
		break;
	}

	return rc;
}

static int smblib_select_sec_charger(struct smb_charger *chg, int sec_chg,
					int reason, bool toggle)
{
	int rc;

	mutex_lock(&chg->smb_lock);

	if (toggle && sec_chg == POWER_SUPPLY_CHARGER_SEC_CP) {
		rc = smblib_select_sec_charger_locked(chg,
					POWER_SUPPLY_CHARGER_SEC_NONE);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't disable secondary charger rc=%d\n",
				rc);
			goto unlock_out;
		}

		/*
		 * A minimum of 20us delay is expected before switching on STAT
		 * pin.
		 */
		usleep_range(20, 30);
	}

	rc = smblib_select_sec_charger_locked(chg, sec_chg);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't switch secondary charger rc=%d\n",
			rc);
		goto unlock_out;
	}

	chg->sec_chg_selected = sec_chg;
	chg->cp_reason = reason;

unlock_out:
	mutex_unlock(&chg->smb_lock);

	return rc;
}

static void smblib_notify_extcon_props(struct smb_charger *chg, int id)
{
	union extcon_property_value val;
	union power_supply_propval prop_val;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_TYPEC) {
		smblib_get_prop_typec_cc_orientation(chg, &prop_val);
		val.intval = ((prop_val.intval == 2) ? 1 : 0);
		extcon_set_property(chg->extcon, id,
				EXTCON_PROP_USB_TYPEC_POLARITY, val);
		val.intval = true;
		extcon_set_property(chg->extcon, id,
				EXTCON_PROP_USB_SS, val);
	} else if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		val.intval = false;
		extcon_set_property(chg->extcon, id,
				EXTCON_PROP_USB_SS, val);
	}
}

static void smblib_notify_device_mode(struct smb_charger *chg, bool enable)
{
	if (enable)
		smblib_notify_extcon_props(chg, EXTCON_USB);

	extcon_set_state_sync(chg->extcon, EXTCON_USB, enable);
}

static void smblib_notify_usb_host(struct smb_charger *chg, bool enable)
{
	int rc = 0;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval en_val = {0};

	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			en_val.intval = enable ? 1 : 0;
			rc =  power_supply_set_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_OTG_SWITCH, &en_val);
			if (rc) {
				smblib_err(chg,	"Couldn't %sable op_chg_psy OTG mode rc=%d\n",
					enable ? "en" : "dis", rc);
				return;
			}
		}
	} else {
#endif
		if (enable) {
			smblib_dbg(chg, PR_OTG, "enabling VBUS in OTG mode\n");
			rc = smblib_masked_write(chg, DCDC_CMD_OTG_REG,
						OTG_EN_BIT, OTG_EN_BIT);
			if (rc < 0) {
				smblib_err(chg,
					"Couldn't enable VBUS in OTG mode rc=%d\n",
					rc);
				return;
			}

			smblib_notify_extcon_props(chg, EXTCON_USB_HOST);
		} else {
			smblib_dbg(chg, PR_OTG, "disabling VBUS in OTG mode\n");
			rc = smblib_masked_write(chg, DCDC_CMD_OTG_REG,
						OTG_EN_BIT, 0);
			if (rc < 0) {
				smblib_err(chg,
					"Couldn't disable VBUS in OTG mode rc=%d\n",
					rc);
				return;
			}
		}
#ifdef OP_SWARP_SUPPORTED
	}
#endif

	extcon_set_state_sync(chg->extcon, EXTCON_USB_HOST, enable);
}

#define DEFAULT_SDP_MA		500
#define DEFAULT_CDP_MA		1500
#define DEFAULT_DCP_MA		2000
#define DEFAULT_AGAING_CHG_MA		1500
#ifdef OP_SWARP_SUPPORTED
#define DEFAULT_WARP_FOR_SWARP_MA   3600
#endif
int op_rerun_apsd(struct smb_charger *chg)
{
	union power_supply_propval val;
	int rc;

	if (chg->wireless_present) {
		pr_info("wireless charge needn't rerun apsd\n");
		return 0;
	}

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb present rc = %d\n", rc);
		return rc;
	}

	if (!val.intval)
		return 0;
	/* rerun APSD */
	pr_info("OP Reruning APSD type\n");
	chg->switch_on_fastchg = false;
	rc = smblib_masked_write(chg, CMD_APSD_REG,
				APSD_RERUN_BIT,
				APSD_RERUN_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't rerun APSD rc = %d\n", rc);
		return rc;
	}
	return 0;
}

/********************
 * REGISTER GETTERS *
 ********************/

int smblib_get_charge_param(struct smb_charger *chg,
			    struct smb_chg_param *param, int *val_u)
{
	int rc = 0;
	u8 val_raw;

	rc = smblib_read(chg, param->reg, &val_raw);
	if (rc < 0) {
		smblib_err(chg, "%s: Couldn't read from 0x%04x rc=%d\n",
			param->name, param->reg, rc);
		return rc;
	}

	if (param->get_proc)
		*val_u = param->get_proc(param, val_raw);
	else
		*val_u = val_raw * param->step_u + param->min_u;
	smblib_dbg(chg, PR_REGISTER, "%s = %d (0x%02x)\n",
		   param->name, *val_u, val_raw);

	return rc;
}

int smblib_get_usb_suspend(struct smb_charger *chg, int *suspend)
{
	int rc = 0;
	u8 temp;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval int_suspend = {0, };

	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			rc = power_supply_get_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_INPUT_SUSPEND, &int_suspend);
			if (rc)
				pr_err("op_chg psy does not allow updating prop INPUT_SUSPEND  rc = %d\n", rc);
			else
				*suspend = int_suspend.intval;
		} else {
			pr_err("no op_chg psy found\n");
			return -EINVAL;
		}
	} else {
#endif
		rc = smblib_read(chg, USBIN_CMD_IL_REG, &temp);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read USBIN_CMD_IL rc=%d\n",
				rc);
			return rc;
		}
		*suspend = temp & USBIN_SUSPEND_BIT;
#ifdef OP_SWARP_SUPPORTED
	}
#endif

	return rc;
}


static const s16 therm_lookup_table[] = {
	/* Index -30C~85C, ADC raw code */
	0x6C92, 0x6C43, 0x6BF0, 0x6B98, 0x6B3A, 0x6AD8, 0x6A70, 0x6A03,
	0x6990, 0x6916, 0x6897, 0x6811, 0x6785, 0x66F2, 0x6658, 0x65B7,
	0x650F, 0x6460, 0x63AA, 0x62EC, 0x6226, 0x6159, 0x6084, 0x5FA8,
	0x5EC3, 0x5DD8, 0x5CE4, 0x5BE9, 0x5AE7, 0x59DD, 0x58CD, 0x57B5,
	0x5696, 0x5571, 0x5446, 0x5314, 0x51DD, 0x50A0, 0x4F5E, 0x4E17,
	0x4CCC, 0x4B7D, 0x4A2A, 0x48D4, 0x477C, 0x4621, 0x44C4, 0x4365,
	0x4206, 0x40A6, 0x3F45, 0x3DE6, 0x3C86, 0x3B28, 0x39CC, 0x3872,
	0x3719, 0x35C4, 0x3471, 0x3322, 0x31D7, 0x308F, 0x2F4C, 0x2E0D,
	0x2CD3, 0x2B9E, 0x2A6E, 0x2943, 0x281D, 0x26FE, 0x25E3, 0x24CF,
	0x23C0, 0x22B8, 0x21B5, 0x20B8, 0x1FC2, 0x1ED1, 0x1DE6, 0x1D01,
	0x1C22, 0x1B49, 0x1A75, 0x19A8, 0x18E0, 0x181D, 0x1761, 0x16A9,
	0x15F7, 0x154A, 0x14A2, 0x13FF, 0x1361, 0x12C8, 0x1234, 0x11A4,
	0x1119, 0x1091, 0x100F, 0x0F90, 0x0F15, 0x0E9E, 0x0E2B, 0x0DBC,
	0x0D50, 0x0CE8, 0x0C83, 0x0C21, 0x0BC3, 0x0B67, 0x0B0F, 0x0AB9,
	0x0A66, 0x0A16, 0x09C9, 0x097E,
};

int smblib_get_thermal_threshold(struct smb_charger *chg, u16 addr, int *val)
{
	u8 buff[2];
	s16 temp;
	int rc = 0;
	int i, lower, upper;

	rc = smblib_batch_read(chg, addr, buff, 2);
	if (rc < 0) {
		pr_err("failed to write to 0x%04X, rc=%d\n", addr, rc);
		return rc;
	}

	temp = buff[1] | buff[0] << 8;

	lower = 0;
	upper = ARRAY_SIZE(therm_lookup_table) - 1;
	while (lower <= upper) {
		i = (upper + lower) / 2;
		if (therm_lookup_table[i] < temp)
			upper = i - 1;
		else if (therm_lookup_table[i] > temp)
			lower = i + 1;
		else
			break;
	}

	/* index 0 corresonds to -30C */
	*val = (i - 30) * 10;

	return rc;
}

struct apsd_result {
	const char * const name;
	const u8 bit;
	const enum power_supply_type pst;
};

enum {
	UNKNOWN,
	SDP,
	CDP,
	DCP,
	OCP,
	FLOAT,
	HVDCP2,
	HVDCP3,
	MAX_TYPES
};

static const struct apsd_result smblib_apsd_results[] = {
	[UNKNOWN] = {
		.name	= "UNKNOWN",
		.bit	= 0,
		.pst	= POWER_SUPPLY_TYPE_UNKNOWN
	},
	[SDP] = {
		.name	= "SDP",
		.bit	= SDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB
	},
	[CDP] = {
		.name	= "CDP",
		.bit	= CDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_CDP
	},
	[DCP] = {
		.name	= "DCP",
		.bit	= DCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[OCP] = {
		.name	= "OCP",
		.bit	= OCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[FLOAT] = {
		.name	= "FLOAT",
		.bit	= FLOAT_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[HVDCP2] = {
		.name	= "HVDCP2",
		.bit	= DCP_CHARGER_BIT | QC_2P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP
	},
	[HVDCP3] = {
		.name	= "HVDCP3",
		.bit	= DCP_CHARGER_BIT | QC_3P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP_3,
	},
};

static const struct apsd_result *smblib_get_apsd_result(struct smb_charger *chg)
{
	int rc, i;
	u8 apsd_stat, stat;
	const struct apsd_result *result = &smblib_apsd_results[UNKNOWN];

	rc = smblib_read(chg, APSD_STATUS_REG, &apsd_stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return result;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", apsd_stat);
	if (!(apsd_stat & APSD_DTC_STATUS_DONE_BIT)) {
		pr_info("APSD_DTC_STATUS_DONE_BIT is 0\n");
		return result;
	}
	rc = smblib_read(chg, APSD_RESULT_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_RESULT_STATUS rc=%d\n",
			rc);
		return result;
	}
	stat &= APSD_RESULT_STATUS_MASK;

	for (i = 0; i < ARRAY_SIZE(smblib_apsd_results); i++) {
		if (smblib_apsd_results[i].bit == stat)
			result = &smblib_apsd_results[i];
	}
	if (apsd_stat & QC_CHARGER_BIT) {
		/* since its a qc_charger, either return HVDCP3 or HVDCP2 */
		if (result != &smblib_apsd_results[HVDCP3])
			result = &smblib_apsd_results[HVDCP2];
	}

	return result;
}

#define INPUT_NOT_PRESENT	0
#define INPUT_PRESENT_USB	BIT(1)
#define INPUT_PRESENT_DC	BIT(2)
static int smblib_is_input_present(struct smb_charger *chg,
				   int *present)
{
	int rc;
	union power_supply_propval pval = {0, };

	*present = INPUT_NOT_PRESENT;

	rc = smblib_get_prop_usb_present(chg, &pval);
	if (rc < 0) {
		pr_err("Couldn't get usb presence status rc=%d\n", rc);
		return rc;
	}
	*present |= pval.intval ? INPUT_PRESENT_USB : INPUT_NOT_PRESENT;

	rc = smblib_get_prop_dc_present(chg, &pval);
	if (rc < 0) {
		pr_err("Couldn't get dc presence status rc=%d\n", rc);
		return rc;
	}
	*present |= pval.intval ? INPUT_PRESENT_DC : INPUT_NOT_PRESENT;

	return 0;
}

#define AICL_RANGE2_MIN_MV		5600
#define AICL_RANGE2_STEP_DELTA_MV	200
#define AICL_RANGE2_OFFSET		16
int smblib_get_aicl_cont_threshold(struct smb_chg_param *param, u8 val_raw)
{
	int base = param->min_u;
	u8 reg = val_raw;
	int step = param->step_u;


	if (val_raw >= AICL_RANGE2_OFFSET) {
		reg = val_raw - AICL_RANGE2_OFFSET;
		base = AICL_RANGE2_MIN_MV;
		step = AICL_RANGE2_STEP_DELTA_MV;
	}

	return base + (reg * step);
}

/********************
 * REGISTER SETTERS *
 ********************/
static const struct buck_boost_freq chg_freq_list[] = {
	[0] = {
		.freq_khz	= 2400,
		.val		= 7,
	},
	[1] = {
		.freq_khz	= 2100,
		.val		= 8,
	},
	[2] = {
		.freq_khz	= 1600,
		.val		= 11,
	},
	[3] = {
		.freq_khz	= 1200,
		.val		= 15,
	},
};

int smblib_set_chg_freq(struct smb_chg_param *param,
				int val_u, u8 *val_raw)
{
	u8 i;

	if (val_u > param->max_u || val_u < param->min_u)
		return -EINVAL;

	/* Charger FSW is the configured freqency / 2 */
	val_u *= 2;
	for (i = 0; i < ARRAY_SIZE(chg_freq_list); i++) {
		if (chg_freq_list[i].freq_khz == val_u)
			break;
	}
	if (i == ARRAY_SIZE(chg_freq_list)) {
		pr_err("Invalid frequency %d Hz\n", val_u / 2);
		return -EINVAL;
	}

	*val_raw = chg_freq_list[i].val;

	return 0;
}

int smblib_set_opt_switcher_freq(struct smb_charger *chg, int fsw_khz)
{
	union power_supply_propval pval = {0, };
	int rc = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher, fsw_khz);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_buck rc=%d\n", rc);

	if (chg->mode == PARALLEL_MASTER && chg->pl.psy) {
		pval.intval = fsw_khz;
		/*
		 * Some parallel charging implementations may not have
		 * PROP_BUCK_FREQ property - they could be running
		 * with a fixed frequency
		 */
		power_supply_set_property(chg->pl.psy,
				POWER_SUPPLY_PROP_BUCK_FREQ, &pval);
	}

	return rc;
}

int smblib_set_charge_param(struct smb_charger *chg,
			    struct smb_chg_param *param, int val_u)
{
	int rc = 0;
	u8 val_raw;

	if (param->set_proc) {
		rc = param->set_proc(param, val_u, &val_raw);
		if (rc < 0)
			return -EINVAL;
	} else {
		if (val_u > param->max_u || val_u < param->min_u)
			smblib_dbg(chg, PR_MISC,
				"%s: %d is out of range [%d, %d]\n",
				param->name, val_u, param->min_u, param->max_u);

		if (val_u > param->max_u)
			val_u = param->max_u;
		if (val_u < param->min_u)
			val_u = param->min_u;

		val_raw = (val_u - param->min_u) / param->step_u;
	}

	rc = smblib_write(chg, param->reg, val_raw);
	if (rc < 0) {
		smblib_err(chg, "%s: Couldn't write 0x%02x to 0x%04x rc=%d\n",
			param->name, val_raw, param->reg, rc);
		return rc;
	}

	smblib_dbg(chg, PR_REGISTER, "%s = %d (0x%02x)\n",
		   param->name, val_u, val_raw);

	return rc;
}

int smblib_set_usb_suspend(struct smb_charger *chg, bool suspend)
{
	int rc = 0;
	int boot_mode = 0;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval int_suspend = {0, };
#endif
	boot_mode = get_boot_mode();
	pr_info("suspend=%d\n", suspend);

	if (!suspend) {
		if (boot_mode == MSM_BOOT_MODE_RF
			|| boot_mode == MSM_BOOT_MODE_FACTORY) {
			pr_info("RF/WLAN,ingnore suspend=%d,keep charge disable!\n",
				suspend);
			return 0;
		}

		if (chg->swarp_supported) {
			if (chg->disable_normal_chg_for_dash) {
				pr_info("suspend by fastchg, keep suspend.");
				return 0;
			}
			if (chg->suspend_by_voltage_switch) {
				pr_info("suspend by voltage switch, keep suspend.");
				return 0;
			}
		}
	}

	if (suspend)
		vote(chg->icl_irq_disable_votable, USB_SUSPEND_VOTER,
				true, 0);

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			int_suspend.intval = suspend ? 1 : 0;
			rc = power_supply_set_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_INPUT_SUSPEND, &int_suspend);
			if (rc)
				pr_err("op_chg psy does not allow updating prop INPUT_SUSPEND  rc = %d\n", rc);
		} else {
			pr_err("no op_chg psy found\n");
			return -EINVAL;
		}
		rc = smblib_masked_write(chg, USBIN_CMD_IL_REG, USBIN_SUSPEND_BIT,
					 USBIN_SUSPEND_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't write %s to USBIN_SUSPEND_BIT rc=%d\n",
				"suspend", rc);
	} else {
#endif
		rc = smblib_masked_write(chg, USBIN_CMD_IL_REG, USBIN_SUSPEND_BIT,
					 suspend ? USBIN_SUSPEND_BIT : 0);
		if (rc < 0)
			smblib_err(chg, "Couldn't write %s to USBIN_SUSPEND_BIT rc=%d\n",
				suspend ? "suspend" : "resume", rc);
#ifdef OP_SWARP_SUPPORTED
	}
#endif

	if (!suspend)
		vote(chg->icl_irq_disable_votable, USB_SUSPEND_VOTER,
				false, 0);

	return rc;
}

int smblib_set_dc_suspend(struct smb_charger *chg, bool suspend)
{
	int rc = 0;
	pr_info("%d\n", suspend);

	rc = smblib_masked_write(chg, DCIN_CMD_IL_REG, DCIN_SUSPEND_BIT,
				 suspend ? DCIN_SUSPEND_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't write %s to DCIN_SUSPEND_BIT rc=%d\n",
			suspend ? "suspend" : "resume", rc);

	return rc;
}

#define MICRO_5V	5000000
#define MICRO_9V	9000000
#define MICRO_12V	12000000
static int op_set_wireless_fsw(struct smb_charger *chg, int voltage)
{
	int rc = 0;

	if (voltage == MICRO_5V)
		rc = smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_6V_8V);
	else if (voltage > MICRO_5V && voltage < MICRO_9V)
		rc = smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_6V_8V);
	else if (voltage >= MICRO_9V && voltage < MICRO_12V)
		rc = smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_12V);
	else if (voltage == MICRO_12V)
		rc = smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_12V);
	else {
		smblib_err(chg, "Couldn't set Fsw: invalid voltage %d\n",
				voltage);
		return -EINVAL;
	}

	return rc;
}

int op_wireless_high_vol_en(bool enable)
{
	int rc;
	static bool reconfiged_aicl;

	g_chg->wireless_high_vol_mode = enable;
	smblib_hvdcp_detect_enable(g_chg, enable);
	rc = smblib_write(g_chg, USBIN_ADAPTER_ALLOW_CFG_REG,
			  enable ? USBIN_ADAPTER_ALLOW_5V_TO_12V :
				   USBIN_ADAPTER_ALLOW_5V);
	if (rc < 0) {
		smblib_err(g_chg,
			   "Couldn't write USBIN_ADAPTER_ALLOW_CFG_REG, rc=%d\n",
			   rc);
		return rc;
	}

	if (g_chg->wireless_present) {
		rc = op_set_wireless_fsw(g_chg, enable ? MICRO_9V : MICRO_5V);
		if (rc < 0) {
			smblib_err(g_chg, "Couldn't set buck freq, rc=%d\n", rc);
			return rc;
		}

		if (!reconfiged_aicl) {
			// usbin collapse keep FET on.
			rc = smblib_masked_write(g_chg, USBIN_AICL_OPTIONS_CFG_REG,
						USBIN_AICL_EN_BIT | USBIN_HV_COLLAPSE_RESPONSE_BIT
						| USBIN_LV_COLLAPSE_RESPONSE_BIT, USBIN_AICL_EN_BIT);
			if (rc < 0) {
				smblib_err(g_chg, "Couldn't turn on USBIN_AICL_OPTIONS_CFG_REG AICL rc=%d\n",
							rc);
				return rc;
			}
			rc = smblib_masked_write(g_chg, USBIN_LOAD_CFG_REG,
						USBIN_AICL_STEP_TIMING_SEL_MASK | USBIN_IN_COLLAPSE_GF_SEL_MASK,
						USBIN_AICL_STEP_TIMING_SEL_MASK | BIT(1));
			if (rc < 0) {
				smblib_err(g_chg, "Couldn't turn on USBIN_LOAD_CFG_REG rc=%d\n", rc);
				return rc;
			}
			reconfiged_aicl = true;
		}
	} else {
		reconfiged_aicl = false;
		// restore usbin aicl step timing.
		rc = smblib_masked_write(g_chg, USBIN_LOAD_CFG_REG,
					USBIN_AICL_STEP_TIMING_SEL_MASK | USBIN_IN_COLLAPSE_GF_SEL_MASK,
					0);
		if (rc < 0) {
			smblib_err(g_chg, "Couldn't turn on USBIN_LOAD_CFG_REG rc=%d\n", rc);
			return rc;
		}
	}

	return rc;
}

static int smblib_usb_pd_adapter_allowance_override(struct smb_charger *chg,
					u8 allowed_voltage)
{
	int rc = 0;

	if (chg->chg_param.smb_version == PMI632_SUBTYPE)
		return 0;

	if (chg->wireless_high_vol_mode) {
		smblib_dbg(chg, PR_MISC, "Wireless high voltage mode, exit\n");
		return 0;
	}

	rc = smblib_write(chg, USBIN_ADAPTER_ALLOW_OVERRIDE_REG,
						allowed_voltage);
	if (rc < 0)
		smblib_err(chg, "Couldn't write 0x%02x to USBIN_ADAPTER_ALLOW_OVERRIDE_REG rc=%d\n",
			allowed_voltage, rc);

	smblib_dbg(chg, PR_MISC, "set USBIN_ALLOW_OVERRIDE: %d\n",
			allowed_voltage);
	return rc;
}

static int smblib_set_usb_pd_fsw(struct smb_charger *chg, int voltage)
{
	int rc = 0;

	if (voltage == MICRO_5V)
		rc = smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_5V);
	else if (voltage > MICRO_5V && voltage < MICRO_9V)
		rc = smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_6V_8V);
	else if (voltage >= MICRO_9V && voltage < MICRO_12V)
		rc = smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_9V);
	else if (voltage == MICRO_12V)
		rc = smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_12V);
	else {
		smblib_err(chg, "Couldn't set Fsw: invalid voltage %d\n",
				voltage);
		return -EINVAL;
	}

	return rc;
}

#define CONT_AICL_HEADROOM_MV		1000
#define AICL_THRESHOLD_MV_IN_CC		5000
static int smblib_set_usb_pd_allowed_voltage(struct smb_charger *chg,
					int min_allowed_uv, int max_allowed_uv)
{
	int rc, aicl_threshold;
	u8 vbus_allowance;

	if (chg->chg_param.smb_version == PMI632_SUBTYPE)
		return 0;

	if (chg->pd_active == POWER_SUPPLY_PD_PPS_ACTIVE) {
		vbus_allowance = CONTINUOUS;
	} else if (min_allowed_uv == MICRO_5V && max_allowed_uv == MICRO_5V) {
		vbus_allowance = FORCE_5V;
	} else if (min_allowed_uv == MICRO_9V && max_allowed_uv == MICRO_9V) {
		vbus_allowance = FORCE_9V;
	} else if (min_allowed_uv == MICRO_12V && max_allowed_uv == MICRO_12V) {
		vbus_allowance = FORCE_12V;
	} else if (min_allowed_uv < MICRO_12V && max_allowed_uv <= MICRO_12V) {
		vbus_allowance = CONTINUOUS;
	} else {
		smblib_err(chg, "invalid allowed voltage [%d, %d]\n",
				min_allowed_uv, max_allowed_uv);
		return -EINVAL;
	}

	rc = smblib_usb_pd_adapter_allowance_override(chg, vbus_allowance);
	if (rc < 0) {
		smblib_err(chg, "set CONTINUOUS allowance failed, rc=%d\n",
				rc);
		return rc;
	}

	if (vbus_allowance != CONTINUOUS)
		return 0;

	aicl_threshold = min_allowed_uv / 1000 - CONT_AICL_HEADROOM_MV;
	if (chg->adapter_cc_mode)
		aicl_threshold = min(aicl_threshold, AICL_THRESHOLD_MV_IN_CC);

	rc = smblib_set_charge_param(chg, &chg->param.aicl_cont_threshold,
							aicl_threshold);
	if (rc < 0) {
		smblib_err(chg, "set CONT_AICL_THRESHOLD failed, rc=%d\n",
							rc);
		return rc;
	}

	return rc;
}

int smblib_set_aicl_cont_threshold(struct smb_chg_param *param,
				int val_u, u8 *val_raw)
{
	int base = param->min_u;
	int offset = 0;
	int step = param->step_u;

	if (val_u > param->max_u)
		val_u = param->max_u;
	if (val_u < param->min_u)
		val_u = param->min_u;

	if (val_u >= AICL_RANGE2_MIN_MV) {
		base = AICL_RANGE2_MIN_MV;
		step = AICL_RANGE2_STEP_DELTA_MV;
		offset = AICL_RANGE2_OFFSET;
	};

	*val_raw = ((val_u - base) / step) + offset;

	return 0;
}

/********************
 * HELPER FUNCTIONS *
 ********************/
static bool is_cp_available(struct smb_charger *chg)
{
	if (!chg->cp_psy)
		chg->cp_psy = power_supply_get_by_name("charge_pump_master");

	return !!chg->cp_psy;
}

#ifdef OP_SWARP_SUPPORTED
bool is_op_chg_available(struct smb_charger *chg)
{
	if (!chg->op_chg_psy)
		chg->op_chg_psy = power_supply_get_by_name("op_charger");

	return !!chg->op_chg_psy;
}
#endif

static bool is_cp_topo_vbatt(struct smb_charger *chg)
{
	int rc;
	bool is_vbatt;
	union power_supply_propval pval;

	if (!is_cp_available(chg))
		return false;

	rc = power_supply_get_property(chg->cp_psy,
				POWER_SUPPLY_PROP_PARALLEL_OUTPUT_MODE, &pval);
	if (rc < 0)
		return false;

	is_vbatt = (pval.intval == POWER_SUPPLY_PL_OUTPUT_VBAT);

	smblib_dbg(chg, PR_WLS, "%s\n", is_vbatt ? "true" : "false");

	return is_vbatt;
}

#define CP_TO_MAIN_ICL_OFFSET_PC		10
int smblib_get_qc3_main_icl_offset(struct smb_charger *chg, int *offset_ua)
{
	union power_supply_propval pval = {0, };
	int rc;

	/*
	 * Apply ILIM offset to main charger's FCC if all of the following
	 * conditions are met:
	 * - HVDCP3 adapter with CP as parallel charger
	 * - Output connection topology is VBAT
	 */
	if (!is_cp_topo_vbatt(chg) || chg->hvdcp3_standalone_config
		|| ((chg->real_charger_type != POWER_SUPPLY_TYPE_USB_HVDCP_3)
		&& chg->real_charger_type != POWER_SUPPLY_TYPE_USB_HVDCP_3P5))
		return -EINVAL;

	rc = power_supply_get_property(chg->cp_psy, POWER_SUPPLY_PROP_CP_ENABLE,
					&pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get CP ENABLE rc=%d\n", rc);
		return rc;
	}

	if (!pval.intval)
		return -EINVAL;

	rc = power_supply_get_property(chg->cp_psy, POWER_SUPPLY_PROP_CP_ILIM,
					&pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get CP ILIM rc=%d\n", rc);
		return rc;
	}

	*offset_ua = (pval.intval * CP_TO_MAIN_ICL_OFFSET_PC * 2) / 100;

	return 0;
}

int smblib_get_prop_from_bms(struct smb_charger *chg,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int rc;

	if (!chg->bms_psy)
		return -EINVAL;

	rc = power_supply_get_property(chg->bms_psy, psp, val);

	return rc;
}

void smblib_apsd_enable(struct smb_charger *chg, bool enable)
{
	int rc;

	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
				BC1P2_SRC_DETECT_BIT,
				enable ? BC1P2_SRC_DETECT_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "failed to write USBIN_OPTIONS_1_CFG rc=%d\n",
				rc);
}

void smblib_hvdcp_detect_enable(struct smb_charger *chg, bool enable)
{
	int rc;
	u8 mask;

	mask = HVDCP_EN_BIT;
	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG, mask,
						enable ? mask : 0);
	if (rc < 0)
		smblib_err(chg, "failed to write USBIN_OPTIONS_1_CFG rc=%d\n",
				rc);
}

static void smblib_hvdcp_detect_try_enable(struct smb_charger *chg, bool enable)
{
	if (chg->hvdcp_disable || chg->pd_not_supported)
		return;
	smblib_hvdcp_detect_enable(chg, enable);
}

void smblib_hvdcp_hw_inov_enable(struct smb_charger *chg, bool enable)
{
	int rc;

	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
				HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT,
				enable ? HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "failed to write USBIN_OPTIONS_1_CFG rc=%d\n",
				rc);
}

void smblib_hvdcp_exit_config(struct smb_charger *chg)
{
	u8 stat;
	int rc;

	rc = smblib_read(chg, APSD_RESULT_STATUS_REG, &stat);
	if (rc < 0)
		return;

	if (stat & (QC_3P0_BIT | QC_2P0_BIT)) {
		/* force HVDCP to 5V */
		smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
				HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT, 0);
		smblib_write(chg, CMD_HVDCP_2_REG, FORCE_5V_BIT);

		/* rerun APSD */
		smblib_masked_write(chg, CMD_APSD_REG, APSD_RERUN_BIT,
				APSD_RERUN_BIT);
	}
}

static int smblib_request_dpdm(struct smb_charger *chg, bool enable)
{
	int rc = 0;

	if (chg->pr_swap_in_progress)
		return 0;

	/* fetch the DPDM regulator */
	if (!chg->dpdm_reg && of_get_property(chg->dev->of_node,
				"dpdm-supply", NULL)) {
		chg->dpdm_reg = devm_regulator_get(chg->dev, "dpdm");
		if (IS_ERR(chg->dpdm_reg)) {
			rc = PTR_ERR(chg->dpdm_reg);
			smblib_err(chg, "Couldn't get dpdm regulator rc=%d\n",
					rc);
			chg->dpdm_reg = NULL;
			return rc;
		}
	}

	mutex_lock(&chg->dpdm_lock);
	if (enable) {
		if (chg->dpdm_reg && !chg->dpdm_enabled) {
			smblib_dbg(chg, PR_MISC, "enabling DPDM regulator\n");
			rc = regulator_enable(chg->dpdm_reg);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't enable dpdm regulator rc=%d\n",
					rc);
			else
				chg->dpdm_enabled = true;
		}
	} else {
		if (chg->dpdm_reg && chg->dpdm_enabled) {
			smblib_dbg(chg, PR_MISC, "disabling DPDM regulator\n");
			rc = regulator_disable(chg->dpdm_reg);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't disable dpdm regulator rc=%d\n",
					rc);
			else
				chg->dpdm_enabled = false;
		}
	}
	mutex_unlock(&chg->dpdm_lock);

	return rc;
}

void smblib_rerun_apsd(struct smb_charger *chg)
{
	int rc;

	smblib_dbg(chg, PR_MISC, "re-running APSD\n");

	rc = smblib_masked_write(chg, CMD_APSD_REG,
				APSD_RERUN_BIT, APSD_RERUN_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't re-run APSD rc=%d\n", rc);
}

static const struct apsd_result *smblib_update_usb_type(struct smb_charger *chg)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

	if (chg->dash_on) {
		chg->real_charger_type = POWER_SUPPLY_TYPE_DASH;
		chg->usb_psy_desc.type = POWER_SUPPLY_TYPE_DASH;
	} else if (chg->pd_active) {
		/* if PD is active, APSD is disabled so won't have a valid result */
		chg->usb_psy_desc.type = apsd_result->pst;
		chg->real_charger_type = POWER_SUPPLY_TYPE_USB_PD;
#ifdef OP_SWARP_SUPPORTED
		chg->fastchg_type = CHARGER_PD;
#endif
		if (chg->wireless_present)
			notify_pd_in_to_wireless();
	} else if (chg->qc3p5_detected) {
		chg->usb_psy_desc.type = apsd_result->pst;
		chg->real_charger_type = POWER_SUPPLY_TYPE_USB_HVDCP_3P5;
#ifdef OP_SWARP_SUPPORTED
		chg->fastchg_type = CHARGER_QC;
#endif
	} else if (chg->wireless_present) {
		chg->real_charger_type = POWER_SUPPLY_TYPE_WIRELESS;
		chg->usb_psy_desc.type = POWER_SUPPLY_TYPE_WIRELESS;
#ifdef OP_SWARP_SUPPORTED
		chg->fastchg_type = CHARGER_WIRELESS;
#endif

	} else {
		chg->usb_psy_desc.type = apsd_result->pst;
		chg->real_charger_type = apsd_result->pst;
#ifdef OP_SWARP_SUPPORTED
		if ((apsd_result->bit & (QC_2P0_BIT | QC_3P0_BIT)) != 0)
			chg->fastchg_type = CHARGER_QC;
#endif
	}
	smblib_err(chg, "APSD=%s PD=%d dash_on=%d real_charger_type=%d\n",
					apsd_result->name, chg->pd_active,
					chg->dash_on, chg->real_charger_type);
	return apsd_result;
}

static int smblib_notifier_call(struct notifier_block *nb,
		unsigned long ev, void *v)
{
	struct power_supply *psy = v;
	struct smb_charger *chg = container_of(nb, struct smb_charger, nb);

	if (!strcmp(psy->desc->name, "bms")) {
		if (!chg->bms_psy)
			chg->bms_psy = psy;
		if (ev == PSY_EVENT_PROP_CHANGED)
			schedule_work(&chg->bms_update_work);
	}

	if (chg->jeita_configured == JEITA_CFG_NONE)
		schedule_work(&chg->jeita_update_work);

	if (chg->sec_pl_present && !chg->pl.psy
		&& !strcmp(psy->desc->name, "parallel")) {
		chg->pl.psy = psy;
		schedule_work(&chg->pl_update_work);
	}

	if (!strcmp(psy->desc->name, "charge_pump_master")) {
		pm_stay_awake(chg->dev);
		schedule_work(&chg->cp_status_change_work);
	}

	return NOTIFY_OK;
}

static int smblib_register_notifier(struct smb_charger *chg)
{
	int rc;

	chg->nb.notifier_call = smblib_notifier_call;
	rc = power_supply_reg_notifier(&chg->nb);
	if (rc < 0) {
		smblib_err(chg, "Couldn't register psy notifier rc = %d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_mapping_soc_from_field_value(struct smb_chg_param *param,
					     int val_u, u8 *val_raw)
{
	if (val_u > param->max_u || val_u < param->min_u)
		return -EINVAL;

	*val_raw = val_u << 1;

	return 0;
}

int smblib_mapping_cc_delta_to_field_value(struct smb_chg_param *param,
					   u8 val_raw)
{
	int val_u  = val_raw * param->step_u + param->min_u;

	if (val_u > param->max_u)
		val_u -= param->max_u * 2;

	return val_u;
}

int smblib_mapping_cc_delta_from_field_value(struct smb_chg_param *param,
					     int val_u, u8 *val_raw)
{
	if (val_u > param->max_u || val_u < param->min_u - param->max_u)
		return -EINVAL;

	val_u += param->max_u * 2 - param->min_u;
	val_u %= param->max_u * 2;
	*val_raw = val_u / param->step_u;

	return 0;
}

static void smblib_uusb_removal(struct smb_charger *chg)
{
	int rc;
	struct smb_irq_data *data;
	struct storm_watch *wdata;
	int sec_charger;

	sec_charger = chg->sec_pl_present ? POWER_SUPPLY_CHARGER_SEC_PL :
				POWER_SUPPLY_CHARGER_SEC_NONE;
	smblib_select_sec_charger(chg, sec_charger, POWER_SUPPLY_CP_NONE,
					false);

	cancel_delayed_work_sync(&chg->pl_enable_work);

	if (chg->wa_flags & BOOST_BACK_WA) {
		data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
		if (data) {
			wdata = &data->storm_data;
			update_storm_count(wdata, WEAK_CHG_STORM_COUNT);
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					false, 0);
		}
	}
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);

	/* reset both usbin current and voltage votes */
	vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, false, 0);
	vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,  SDP_CURRENT_UA);
	vote(chg->usb_icl_votable, SW_QC3_VOTER, false, 0);
	vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
	vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);
	vote(chg->usb_icl_votable, THERMAL_THROTTLE_VOTER, false, 0);
	vote(chg->limited_irq_disable_votable, CHARGER_TYPE_VOTER,
			true, 0);
	vote(chg->hdc_irq_disable_votable, CHARGER_TYPE_VOTER, true, 0);
	vote(chg->hdc_irq_disable_votable, HDC_IRQ_VOTER, false, 0);

	/* Remove SW thermal regulation WA votes */
	vote(chg->usb_icl_votable, SW_THERM_REGULATION_VOTER, false, 0);
	vote(chg->pl_disable_votable, SW_THERM_REGULATION_VOTER, false, 0);
	vote(chg->dc_suspend_votable, SW_THERM_REGULATION_VOTER, false, 0);
	if (chg->cp_disable_votable)
		vote(chg->cp_disable_votable, SW_THERM_REGULATION_VOTER,
								false, 0);

	/* reset USBOV votes and cancel work */
	cancel_delayed_work_sync(&chg->usbov_dbc_work);
	vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
	chg->dbc_usbov = false;

	chg->voltage_min_uv = MICRO_5V;
	chg->voltage_max_uv = MICRO_5V;
	chg->usbin_forced_max_uv = 0;
	chg->usb_icl_delta_ua = 0;
	chg->pulse_cnt = 0;
	chg->uusb_apsd_rerun_done = false;
	chg->chg_param.forced_main_fcc = 0;

	del_timer_sync(&chg->apsd_timer);
	chg->apsd_ext_timeout = false;

	/* write back the default FLOAT charger configuration */
	rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
				(u8)FLOAT_OPTIONS_MASK, chg->float_cfg);
	if (rc < 0)
		smblib_err(chg, "Couldn't write float charger options rc=%d\n",
			rc);

	/* clear USB ICL vote for USB_PSY_VOTER */
	rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't un-vote for USB ICL rc=%d\n", rc);

	/* clear USB ICL vote for DCP_VOTER */
	rc = vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
	if (rc < 0)
		smblib_err(chg,
			"Couldn't un-vote DCP from USB ICL rc=%d\n", rc);

	/*
	 * if non-compliant charger caused UV, restore original max pulses
	 * and turn SUSPEND_ON_COLLAPSE_USBIN_BIT back on.
	 */
	if (chg->qc2_unsupported_voltage) {
		rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
				HVDCP_PULSE_COUNT_MAX_QC2_MASK,
				chg->qc2_max_pulses);
		if (rc < 0)
			smblib_err(chg, "Couldn't restore max pulses rc=%d\n",
					rc);

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				SUSPEND_ON_COLLAPSE_USBIN_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn on SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		chg->qc2_unsupported_voltage = QC2_COMPLIANT;
		chg->qc2_uv_count = 0;
	}

	chg->qc3p5_detected = false;
	smblib_update_usb_type(chg);
}

void smblib_suspend_on_debug_battery(struct smb_charger *chg)
{
	int rc;
	union power_supply_propval val;

	rc = smblib_get_prop_from_bms(chg,
			POWER_SUPPLY_PROP_DEBUG_BATTERY, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get debug battery prop rc=%d\n", rc);
		return;
	}
	if (chg->suspend_input_on_debug_batt) {
		vote(chg->usb_icl_votable, DEBUG_BOARD_VOTER, val.intval, 0);
		vote(chg->dc_suspend_votable, DEBUG_BOARD_VOTER, val.intval, 0);
		if (val.intval)
			pr_info("Input suspended: Fake battery\n");
	} else {
		vote(chg->chg_disable_votable, DEBUG_BOARD_VOTER,
					val.intval, 0);
	}
}

int smblib_rerun_apsd_if_required(struct smb_charger *chg)
{
	union power_supply_propval val;
	int rc;

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb present rc = %d\n", rc);
		return rc;
	}

	if (!val.intval)
		return 0;

	rc = smblib_request_dpdm(chg, true);
	if (rc < 0)
		smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);

	chg->uusb_apsd_rerun_done = true;
	smblib_rerun_apsd(chg);

	return 0;
}

static int smblib_get_pulse_cnt(struct smb_charger *chg, int *count)
{
	*count = chg->pulse_cnt;
	return 0;
}

#define USBIN_25MA	25000
#define USBIN_100MA	100000
#define USBIN_150MA	150000
#define USBIN_500MA	500000
#define USBIN_900MA	900000
#define USBIN_1000MA	1000000
static int set_sdp_current(struct smb_charger *chg, int icl_ua)
{
	int rc;
	u8 icl_options;
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

#ifdef CONFIG_FORCE_FAST_CHARGE
	if (force_fast_charge > 0 && icl_ua == USBIN_500MA)
	{
		icl_ua = USBIN_900MA;
	}
#endif

	/* power source is SDP */
	switch (icl_ua) {
	case USBIN_100MA:
		/* USB 2.0 100mA */
		icl_options = 0;
		break;
	case USBIN_150MA:
		/* USB 3.0 150mA */
		icl_options = CFG_USB3P0_SEL_BIT;
		break;
	case USBIN_500MA:
		/* USB 2.0 500mA */
		icl_options = USB51_MODE_BIT;
		break;
	case USBIN_900MA:
		/* USB 3.0 900mA */
		icl_options = CFG_USB3P0_SEL_BIT | USB51_MODE_BIT;
		break;
	default:
		return -EINVAL;
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB &&
		apsd_result->pst == POWER_SUPPLY_TYPE_USB_FLOAT) {
		/*
		 * change the float charger configuration to SDP, if this
		 * is the case of SDP being detected as FLOAT
		 */
		rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
			FORCE_FLOAT_SDP_CFG_BIT, FORCE_FLOAT_SDP_CFG_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set float ICL options rc=%d\n",
						rc);
			return rc;
		}
	}

	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
			CFG_USB3P0_SEL_BIT | USB51_MODE_BIT, icl_options);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL options rc=%d\n", rc);
		return rc;
	}

	rc = smblib_icl_override(chg, SW_OVERRIDE_USB51_MODE);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL override rc=%d\n", rc);
		return rc;
	}

	return rc;
}

void op_bus_vote(int disable)
{
	int ret;

	if (!g_chg)
		return;

	ret = msm_bus_scale_client_update_request(g_chg->bus_client, disable);

	if (ret) {
		pr_err("%s: failed: bus_client_handle=0x%x, vote=%d, err=%d\n",
			__func__, g_chg->bus_client, disable, ret);
	}
	pr_info("enable =%d\n", disable);
}

static void op_run_sw_aicl_work(struct work_struct *work)
{
	struct smb_charger *chg = g_chg;
	static int counter;

	if (!chg || !is_usb_present(chg))
		return;

	if (!chg->sw_aicl_prepared) {
		if (!chg->pd_active) {
			// not pd, run sw aicl and check apsd result after 5s.
			chg->sw_aicl_prepared = true;
			schedule_delayed_work(&chg->run_sw_aicl_work,
				msecs_to_jiffies(TIME_5S));
		} else {
			// pd not swarp, wait more times.
			if (counter < 2)
				counter++;
			else {
				counter = 0;
				chg->sw_aicl_prepared = true;
			}
		}
		if (chg->sw_aicl_prepared) {
			if (chg->swarp_supported)
				rerun_election(chg->usb_icl_votable);
		} else
			schedule_delayed_work(&chg->run_sw_aicl_work,
				msecs_to_jiffies(TIME_1000MS));
	} else {
		if (chg->real_charger_type == POWER_SUPPLY_TYPE_UNKNOWN) {
			pr_err("not get type in 6s, rerun apsd.");
			smblib_apsd_enable(chg, true);
			op_rerun_apsd(chg);
		}
	}
}

int op_usb_icl_set(struct smb_charger *chg, int icl_ua)
{
	int rc = 0;
	bool override;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval icl_max = {0, };
#endif

	pr_info("icl_ua=%d\n", icl_ua);

	disable_irq_nosync(chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq);

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			icl_max.intval = icl_ua;
			rc = power_supply_set_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_CURRENT_MAX, &icl_max);
			if (rc)
				pr_err("seting op_chg_psy prop CURRENT_MAX failed rc = %d\n", rc);
		} else {
			pr_err("no op_chg_psy found\n");
			rc = -EINVAL;
		}
		goto enable_icl_changed_interrupt;
	}
#endif

	rc = smblib_set_charge_param(chg, &chg->param.usb_icl,
			icl_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set HC ICL rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

	/* determine if override needs to be enforced */
	override = true;
	/* enforce override */
	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
		USBIN_MODE_CHG_BIT, override ? USBIN_MODE_CHG_BIT : 0);

	rc = smblib_icl_override(chg, SW_OVERRIDE_HC_MODE);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL override rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

	/* unsuspend after configuring current and override */
	rc = smblib_set_usb_suspend(chg, false);
	if (rc < 0) {
		smblib_err(chg, "Couldn't resume input rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

enable_icl_changed_interrupt:
	enable_irq(chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq);

	return rc;
}

int smblib_set_icl_current(struct smb_charger *chg, int icl_ua)
{
	int rc = 0;
	enum icl_override_mode icl_override = HW_AUTO_MODE;
	/* suspend if 25mA or less is requested */
	bool suspend = (icl_ua <= USBIN_25MA);
	int has_suspended = 0;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval icl_max = {0, };
#endif

	pr_info("icl_ua=%d\n", icl_ua);
	smblib_err(chg, "icl_ua=%d, connector_type=%d, real_charger_type=%d\n",
				icl_ua, chg->connector_type, chg->real_charger_type);

	if (chg->chg_param.smb_version == PMI632_SUBTYPE)
		schgm_flash_torch_priority(chg, suspend ? TORCH_BOOST_MODE :
					TORCH_BUCK_MODE);

	/* Do not configure ICL from SW for DAM cables */
	if ((smblib_get_prop_typec_mode(chg) ==
			    POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY)
		&& !chg->qc_configed)
		return 0;

	if (suspend)
		return smblib_set_usb_suspend(chg, true);

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			icl_max.intval = icl_ua;
			rc = smblib_get_usb_suspend(chg, &has_suspended);
			if (rc < 0)
				has_suspended = 0;
			if (chg->sw_aicl_prepared && icl_ua > USBIN_500MA
				&& !has_suspended) {
				rc = power_supply_set_property(chg->op_chg_psy,
						POWER_SUPPLY_PROP_SWAICL, &icl_max);
				if (rc)
					pr_err("seting op_chg_psy prop SWAICL failed rc = %d\n", rc);
			} else {
				icl_max.intval = icl_ua < USBIN_500MA ? icl_ua : USBIN_500MA;
				rc = power_supply_set_property(chg->op_chg_psy,
						POWER_SUPPLY_PROP_CURRENT_MAX, &icl_max);
				if (rc)
					pr_err("seting op_chg_psy prop CURRENT_MAX failed rc = %d\n", rc);
			}
		} else {
			pr_err("no op_chg_psy found\n");
			return -EINVAL;
		}
		// not rerun smb aicl.
		icl_override = SW_OVERRIDE_HC_MODE;
		goto unsuspend;
	}
#endif

	if (icl_ua == INT_MAX)
		goto set_mode;

	/* configure current */
	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB
		&& (chg->typec_legacy
		|| chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
		|| chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)) {
		if (chg->non_std_chg_present) {
			rc = smblib_set_charge_param(chg, &chg->param.usb_icl,
					icl_ua);
			icl_override = HW_AUTO_MODE;
		} else
			rc = set_sdp_current(chg, icl_ua);

		rc = set_sdp_current(chg, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set SDP ICL rc=%d\n", rc);
			goto out;
		}
	} else if (chg->wireless_present) {
		if (icl_ua <= USBIN_500MA) {
			rc = set_sdp_current(chg, icl_ua);
			if (rc >= 0)
				goto unsuspend;
		}

		rc = smblib_set_charge_param(chg, &chg->param.usb_icl, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set wireless ICL rc=%d\n", rc);
			goto out;
		}
		icl_override = SW_OVERRIDE_WIRELESS_MODE;
	} else {
		/*
		 * Try USB 2.0/3,0 option first on USB path when maximum input
		 * current limit is 500mA or below for better accuracy; in case
		 * of error, proceed to use USB high-current mode.
		 */
		if (icl_ua <= USBIN_500MA) {
			rc = set_sdp_current(chg, icl_ua);
			if (rc >= 0)
				goto unsuspend;
		}

		rc = smblib_set_charge_param(chg, &chg->param.usb_icl, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set HC ICL rc=%d\n", rc);
			goto out;
		}
		icl_override = SW_OVERRIDE_HC_MODE;
	}

set_mode:
	rc = smblib_icl_override(chg, icl_override);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL override rc=%d\n", rc);
		goto out;
	}

unsuspend:
	/* unsuspend after configuring current and override */
	rc = smblib_set_usb_suspend(chg, false);
	if (rc < 0) {
		smblib_err(chg, "Couldn't resume input rc=%d\n", rc);
		goto out;
	}

	/* Re-run AICL */
	if (icl_override != SW_OVERRIDE_HC_MODE)
		rc = smblib_run_aicl(chg, RERUN_AICL);
out:
	return rc;
}

int smblib_get_icl_current(struct smb_charger *chg, int *icl_ua)
{
	int rc;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval icl_max = {0, };

	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			rc = power_supply_get_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_CURRENT_MAX, &icl_max);
			if (rc)
				pr_err("geting op_chg_psy prop CURRENT_MAX failed rc = %d\n", rc);
			else
				*icl_ua = icl_max.intval;
		} else {
			pr_err("no op_chg_psy found\n");
			return -EINVAL;
		}
	} else {
#endif
		rc = smblib_get_charge_param(chg,
				&chg->param.icl_max_stat, icl_ua);
		if (rc < 0)
			smblib_err(chg, "Couldn't get HC ICL rc=%d\n", rc);
#ifdef OP_SWARP_SUPPORTED
	}
#endif
	return rc;
}

int smblib_toggle_smb_en(struct smb_charger *chg, int toggle)
{
	int rc = 0;

	if (!toggle)
		return rc;

	rc = smblib_select_sec_charger(chg, chg->sec_chg_selected,
				chg->cp_reason, true);

	return rc;
}

int smblib_get_irq_status(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 reg;

	if (chg->wa_flags & SKIP_MISC_PBS_IRQ_WA) {
		val->intval = 0;
		return 0;
	}

	mutex_lock(&chg->irq_status_lock);
	/* Report and clear cached status */
	val->intval = chg->irq_status;
	chg->irq_status = 0;

	/* get real time status of pulse skip irq */
	rc = smblib_read(chg, MISC_PBS_RT_STS_REG, &reg);
	if (rc < 0)
		smblib_err(chg, "Couldn't read MISC_PBS_RT_STS_REG rc=%d\n",
				rc);
	else
		val->intval |= (reg & PULSE_SKIP_IRQ_BIT);
	mutex_unlock(&chg->irq_status_lock);

	return rc;
}

/****************************
 * uUSB Moisture Protection *
 ****************************/
#define MICRO_USB_DETECTION_ON_TIME_20_MS 0x08
#define MICRO_USB_DETECTION_PERIOD_X_100 0x03
#define U_USB_STATUS_WATER_PRESENT 0x00
static int smblib_set_moisture_protection(struct smb_charger *chg,
				bool enable)
{
	int rc = 0;

	if (chg->moisture_present == enable) {
		smblib_dbg(chg, PR_MISC, "No change in moisture protection status\n");
		return rc;
	}

	if (enable) {
		chg->moisture_present = true;

		/* Disable uUSB factory mode detection */
		rc = smblib_masked_write(chg, TYPEC_U_USB_CFG_REG,
					EN_MICRO_USB_FACTORY_MODE_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable uUSB factory mode detection rc=%d\n",
				rc);
			return rc;
		}

		/* Disable moisture detection and uUSB state change interrupt */
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					TYPEC_WATER_DETECTION_INT_EN_BIT |
					MICRO_USB_STATE_CHANGE_INT_EN_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable moisture detection interrupt rc=%d\n",
			rc);
			return rc;
		}

		/* Set 1% duty cycle on ID detection */
		rc = smblib_masked_write(chg,
				((chg->chg_param.smb_version == PMI632_SUBTYPE)
				? PMI632_TYPEC_U_USB_WATER_PROTECTION_CFG_REG :
				TYPEC_U_USB_WATER_PROTECTION_CFG_REG),
				EN_MICRO_USB_WATER_PROTECTION_BIT |
				MICRO_USB_DETECTION_ON_TIME_CFG_MASK |
				MICRO_USB_DETECTION_PERIOD_CFG_MASK,
				EN_MICRO_USB_WATER_PROTECTION_BIT |
				MICRO_USB_DETECTION_ON_TIME_20_MS |
				MICRO_USB_DETECTION_PERIOD_X_100);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set 1 percent CC_ID duty cycle rc=%d\n",
				rc);
			return rc;
		}

		vote(chg->usb_icl_votable, MOISTURE_VOTER, true, 0);
	} else {
		chg->moisture_present = false;
		vote(chg->usb_icl_votable, MOISTURE_VOTER, false, 0);

		/* Enable moisture detection and uUSB state change interrupt */
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					TYPEC_WATER_DETECTION_INT_EN_BIT |
					MICRO_USB_STATE_CHANGE_INT_EN_BIT,
					TYPEC_WATER_DETECTION_INT_EN_BIT |
					MICRO_USB_STATE_CHANGE_INT_EN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable moisture detection and uUSB state change interrupt rc=%d\n",
				rc);
			return rc;
		}

		/* Disable periodic monitoring of CC_ID pin */
		rc = smblib_write(chg,
				((chg->chg_param.smb_version == PMI632_SUBTYPE)
				? PMI632_TYPEC_U_USB_WATER_PROTECTION_CFG_REG :
				TYPEC_U_USB_WATER_PROTECTION_CFG_REG), 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable 1 percent CC_ID duty cycle rc=%d\n",
				rc);
			return rc;
		}

		/* Enable uUSB factory mode detection */
		rc = smblib_masked_write(chg, TYPEC_U_USB_CFG_REG,
					EN_MICRO_USB_FACTORY_MODE_BIT,
					EN_MICRO_USB_FACTORY_MODE_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable uUSB factory mode detection rc=%d\n",
				rc);
			return rc;
		}
	}

	smblib_dbg(chg, PR_MISC, "Moisture protection %s\n",
			chg->moisture_present ? "enabled" : "disabled");
	return rc;
}

/*********************
 * VOTABLE CALLBACKS *
 *********************/
static int smblib_smb_disable_override_vote_callback(struct votable *votable,
			void *data, int disable_smb, const char *client)
{
	struct smb_charger *chg = data;
	int rc = 0;

	/* Enable/disable SMB_EN pin */
	rc = smblib_masked_write(chg, MISC_SMB_EN_CMD_REG,
			SMB_EN_OVERRIDE_BIT | SMB_EN_OVERRIDE_VALUE_BIT,
			disable_smb ? SMB_EN_OVERRIDE_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't configure SMB_EN, rc=%d\n", rc);

	return rc;
}

static int smblib_dc_suspend_vote_callback(struct votable *votable, void *data,
			int suspend, const char *client)
{
	struct smb_charger *chg = data;

	if (chg->chg_param.smb_version == PMI632_SUBTYPE)
		return 0;

	/* resume input if suspend is invalid */
	if (suspend < 0)
		suspend = 0;

	return smblib_set_dc_suspend(chg, (bool)suspend);
}

static int smblib_otg_toggle_vote_callback(struct votable *votable,
			void *data, int value, const char *client)
{
	struct smb_charger *chg = data;
	int rc = 0;

	if (value < 0)
		value = 0;

	rc = op_set_prop_otg_switch(chg, (bool)value);
	if (rc < 0) {
		smblib_err(chg, "Can not set otg switch,value=%d, rc=%d\n",
			value, rc);
		return rc;
	}

	return rc;
}

static int smblib_awake_vote_callback(struct votable *votable, void *data,
			int awake, const char *client)
{
	struct smb_charger *chg = data;

	pr_info("set awake=%d\n", awake);
	if (awake)
		pm_stay_awake(chg->dev);
	else
		pm_relax(chg->dev);

	return 0;
}

static int smblib_chg_disable_vote_callback(struct votable *votable, void *data,
			int chg_disable, const char *client)
{
	struct smb_charger *chg = data;
	int rc;

	pr_err("set chg_disable=%d\n", chg_disable);
#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		rc = op_charging_en(chg, !chg_disable);
		if (rc < 0)
			pr_err("swarp: %sable charging failed(%d).",
				chg_disable ? "dis" : "en", rc);
		return 0;
	}
#endif
	rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT,
				 chg_disable ? 0 : CHARGING_ENABLE_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't %s charging rc=%d\n",
			chg_disable ? "disable" : "enable", rc);
		return rc;
	}

	return 0;
}

static int smblib_hdc_irq_disable_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		return 0;

	if (chg->irq_info[HIGH_DUTY_CYCLE_IRQ].enabled) {
		if (disable)
			disable_irq_nosync(
				chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
	} else {
		if (!disable)
			enable_irq(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
	}

	chg->irq_info[HIGH_DUTY_CYCLE_IRQ].enabled = !disable;

	return 0;
}

static int smblib_limited_irq_disable_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].irq)
		return 0;

	if (chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].enabled) {
		if (disable)
			disable_irq_nosync(
				chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].irq);
	} else {
		if (!disable)
			enable_irq(
				chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].irq);
	}

	chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].enabled = !disable;

	return 0;
}

static int smblib_icl_irq_disable_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq)
		return 0;

	if (chg->irq_info[USBIN_ICL_CHANGE_IRQ].enabled) {
		if (disable)
			disable_irq_nosync(
				chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq);
	} else {
		if (!disable)
			enable_irq(chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq);
	}

	chg->irq_info[USBIN_ICL_CHANGE_IRQ].enabled = !disable;

	return 0;
}

static int smblib_temp_change_irq_disable_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[TEMP_CHANGE_IRQ].irq)
		return 0;

	if (chg->irq_info[TEMP_CHANGE_IRQ].enabled && disable) {
		if (chg->irq_info[TEMP_CHANGE_IRQ].wake)
			disable_irq_wake(chg->irq_info[TEMP_CHANGE_IRQ].irq);
		disable_irq_nosync(chg->irq_info[TEMP_CHANGE_IRQ].irq);
	} else if (!chg->irq_info[TEMP_CHANGE_IRQ].enabled && !disable) {
		enable_irq(chg->irq_info[TEMP_CHANGE_IRQ].irq);
		if (chg->irq_info[TEMP_CHANGE_IRQ].wake)
			enable_irq_wake(chg->irq_info[TEMP_CHANGE_IRQ].irq);
	}

	chg->irq_info[TEMP_CHANGE_IRQ].enabled = !disable;

	return 0;
}

/*******************
 * VCONN REGULATOR *
 * *****************/

int smblib_vconn_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;
	u8 stat, orientation;

	smblib_dbg(chg, PR_OTG, "enabling VCONN\n");

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return rc;
	}

	/* VCONN orientation is opposite to that of CC */
	orientation =
		stat & TYPEC_CCOUT_VALUE_BIT ? 0 : VCONN_EN_ORIENTATION_BIT;
	rc = smblib_masked_write(chg, TYPE_C_VCONN_CONTROL_REG,
				VCONN_EN_VALUE_BIT | VCONN_EN_ORIENTATION_BIT,
				VCONN_EN_VALUE_BIT | orientation);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_CCOUT_CONTROL_REG rc=%d\n",
			rc);
		return rc;
	}

	return 0;
}

int smblib_vconn_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	smblib_dbg(chg, PR_OTG, "disabling VCONN\n");
	rc = smblib_masked_write(chg, TYPE_C_VCONN_CONTROL_REG,
				 VCONN_EN_VALUE_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable vconn regulator rc=%d\n", rc);

	return 0;
}

int smblib_vconn_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;
	u8 cmd;

	rc = smblib_read(chg, TYPE_C_VCONN_CONTROL_REG, &cmd);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			rc);
		return rc;
	}

	return (cmd & VCONN_EN_VALUE_BIT) ? 1 : 0;
}

/*****************
 * OTG REGULATOR *
 *****************/

int smblib_vbus_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval en_val = {0, };
#endif

	smblib_err(chg, "enabling OTG\n");

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			en_val.intval = 1;
			rc =  power_supply_set_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_OTG_SWITCH, &en_val);
			if (rc) {
				smblib_err(chg, "Couldn't enable op_chg_psy OTG mode rc=%d\n", rc);
				return rc;
			}
		}
	} else {
#endif
		rc = smblib_masked_write(chg, DCDC_CMD_OTG_REG,
				OTG_EN_BIT, OTG_EN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable OTG rc=%d\n", rc);
			return rc;
		}
#ifdef OP_SWARP_SUPPORTED
	}
#endif

	return 0;
}

int smblib_vbus_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval en_val = {0, };
#endif

	smblib_err(chg, "disabling OTG\n");
#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			en_val.intval = 0;
			rc =  power_supply_set_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_OTG_SWITCH, &en_val);
			if (rc) {
				smblib_err(chg, "Couldn't disable op_chg_psy OTG mode rc=%d\n", rc);
				return rc;
			}
		}
	} else {
#endif
		rc = smblib_masked_write(chg, DCDC_CMD_OTG_REG, OTG_EN_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable OTG regulator rc=%d\n",
						rc);
			return rc;
		}
#ifdef OP_SWARP_SUPPORTED
	}
#endif
	return 0;
}

int smblib_vbus_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;
	u8 cmd;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval en_val = {0, };

	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			rc =  power_supply_get_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_OTG_SWITCH, &en_val);
			if (rc)
				return rc;
			rc = en_val.intval;
		}
	} else {
#endif
		rc = smblib_read(chg, DCDC_CMD_OTG_REG, &cmd);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read CMD_OTG rc=%d", rc);
			return rc;
		}

		rc = (cmd & OTG_EN_BIT) ? 1 : 0;
#ifdef OP_SWARP_SUPPORTED
	}
#endif
	return rc;
}

/********************
 * BATT PSY GETTERS *
 ********************/

int smblib_get_prop_input_suspend(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	val->intval
		= (get_client_vote(chg->usb_icl_votable, USER_VOTER) == 0)
		 && get_client_vote(chg->dc_suspend_votable, USER_VOTER);
	return 0;
}

int smblib_get_prop_batt_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATIF_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATIF_INT_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = !(stat & (BAT_THERM_OR_ID_MISSING_RT_STS_BIT
					| BAT_TERMINAL_MISSING_RT_STS_BIT));

	return rc;
}

int smblib_get_prop_batt_capacity(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	int rc = -EINVAL;

	if (chg->fake_capacity >= 0) {
		val->intval = chg->fake_capacity;
		return 0;
	}

	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CAPACITY, val);

	return rc;
}

int smblib_get_prop_batt_capacity_level(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc = -EINVAL;

	if (chg->fake_capacity_level >= 0) {
		val->intval = chg->fake_capacity_level;
		return 0;
	}

	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CAPACITY_LEVEL, val);
	return rc;
}

static bool is_charging_paused(struct smb_charger *chg)
{
	int rc;
	u8 val;

	rc = smblib_read(chg, CHARGING_PAUSE_CMD_REG, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read CHARGING_PAUSE_CMD rc=%d\n", rc);
		return false;
	}

	return val & CHARGING_PAUSE_CMD_BIT;
}

#define CUTOFF_COUNT		3
int smblib_get_prop_batt_status(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	bool usb_online, dc_online;
	u8 stat;
	int rc, suspend = 0, input_present = 0;

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported && is_op_chg_available(chg)) {
		rc = power_supply_get_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_STATUS, val);
		if (rc)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		return rc;
	}
#endif

	if (chg->fake_chg_status_on_debug_batt) {
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_DEBUG_BATTERY, &pval);
		if (rc < 0) {
			pr_err_ratelimited("Couldn't get debug battery prop rc=%d\n",
					rc);
		} else if (pval.intval == 1) {
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
			return 0;
		}
	}

	rc = smblib_get_prop_batt_health(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get batt health rc=%d\n", rc);
		return rc;
	}
	/*
	 * The charger status register shows charging even though the battery
	 * is discharging when the over voltage condition is hit. Report power
	 * supply state as NOT_CHARGING when the battery health reports
	 * over voltage.
	 */
	if (pval.intval == POWER_SUPPLY_HEALTH_OVERVOLTAGE) {
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	}

	/*
	 * If SOC = 0 and we are discharging with input connected, report
	 * the battery status as DISCHARGING.
	 */
	smblib_is_input_present(chg, &input_present);
	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CAPACITY, &pval);
	if (!rc && pval.intval == 0 && input_present) {
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_CURRENT_NOW, &pval);
		if (!rc && pval.intval > 0) {
			if (chg->cutoff_count > CUTOFF_COUNT) {
				val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
				return 0;
			}
			chg->cutoff_count++;
		} else {
			chg->cutoff_count = 0;
		}
	} else {
		chg->cutoff_count = 0;
	}

	if (chg->dbc_usbov) {
		rc = smblib_get_prop_usb_present(chg, &pval);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't get usb present prop rc=%d\n", rc);
			return rc;
		}

		rc = smblib_get_usb_suspend(chg, &suspend);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't get usb suspend rc=%d\n", rc);
			return rc;
		}

		/*
		 * Report charging as long as USBOV is not debounced and
		 * charging path is un-suspended.
		 */
		if (pval.intval && !suspend) {
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			return 0;
		}
	}

	rc = smblib_get_prop_usb_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb online property rc=%d\n",
			rc);
		return rc;
	}
	usb_online = (bool)pval.intval;

	rc = smblib_get_prop_dc_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get dc online property rc=%d\n",
			rc);
		return rc;
	}
	dc_online = (bool)pval.intval;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}
	stat = stat & BATTERY_CHARGER_STATUS_MASK;

	if (!usb_online && !dc_online) {
		switch (stat) {
		case TERMINATE_CHARGE:
		case INHIBIT_CHARGE:
			val->intval = POWER_SUPPLY_STATUS_FULL;
			break;
		default:
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		}
		return rc;
	}

	switch (stat) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
	case FULLON_CHARGE:
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case TERMINATE_CHARGE:
	case INHIBIT_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_FULL;
		break;
	case DISABLE_CHARGE:
	case PAUSE_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	if (is_charging_paused(chg)) {
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		return 0;
	}

	/*
	 * If charge termination WA is active and has suspended charging, then
	 * continue reporting charging status as FULL.
	 */
	if (is_client_vote_enabled_locked(chg->usb_icl_votable,
						CHG_TERMINATION_VOTER)) {
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}

	if (val->intval != POWER_SUPPLY_STATUS_CHARGING)
		return 0;

	if (!usb_online && dc_online
		&& chg->fake_batt_status == POWER_SUPPLY_STATUS_FULL) {
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_5_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
				rc);
			return rc;
	}

	stat &= ENABLE_TRICKLE_BIT | ENABLE_PRE_CHARGING_BIT |
						ENABLE_FULLON_MODE_BIT;

	if (!stat)
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

int smblib_get_prop_batt_charge_type(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}

	switch (stat & BATTERY_CHARGER_STATUS_MASK) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case FULLON_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TAPER;
		break;
	default:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
	}

	return rc;
}

int smblib_get_prop_batt_health(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;

	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "BATTERY_CHARGER_STATUS_2 = 0x%02x\n",
		   stat);

	if (stat & CHARGER_ERROR_STATUS_BAT_OV_BIT) {
		val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_7_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}
	if (stat & BAT_TEMP_STATUS_TOO_COLD_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COLD;
	else if (stat & BAT_TEMP_STATUS_TOO_HOT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (stat & BAT_TEMP_STATUS_COLD_SOFT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COOL;
	else if (stat & BAT_TEMP_STATUS_HOT_SOFT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_WARM;
	else
		val->intval = POWER_SUPPLY_HEALTH_GOOD;

	return rc;
}

int smblib_get_prop_system_temp_level(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->system_temp_level;
	return 0;
}

int smblib_get_prop_system_temp_level_max(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->thermal_levels;
	return 0;
}

int smblib_get_prop_input_current_limited(struct smb_charger *chg,
				union power_supply_propval *val)
{
	u8 stat;
	int rc;

	if (chg->fake_input_current_limited >= 0) {
		val->intval = chg->fake_input_current_limited;
		return 0;
	}

	rc = smblib_read(chg, AICL_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n", rc);
		return rc;
	}
	val->intval = (stat & SOFT_ILIMIT_BIT) || chg->is_hdc;
	return 0;
}

int smblib_get_prop_batt_iterm(struct smb_charger *chg,
		union power_supply_propval *val)
{
	int rc, temp;
	u8 stat, buf[2];

	/*
	 * Currently, only ADC comparator-based termination is supported,
	 * hence read only the threshold corresponding to ADC source.
	 * Proceed only if CHGR_ITERM_USE_ANALOG_BIT is 0.
	 */
	rc = smblib_read(chg, CHGR_ENG_CHARGING_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read CHGR_ENG_CHARGING_CFG_REG rc=%d\n",
				rc);
		return rc;
	}

	if (stat & CHGR_ITERM_USE_ANALOG_BIT) {
		val->intval = -EINVAL;
		return 0;
	}

	rc = smblib_batch_read(chg, CHGR_ADC_ITERM_UP_THD_MSB_REG, buf, 2);

	if (rc < 0) {
		smblib_err(chg, "Couldn't read CHGR_ADC_ITERM_UP_THD_MSB_REG rc=%d\n",
				rc);
		return rc;
	}

	temp = buf[1] | (buf[0] << 8);
	temp = sign_extend32(temp, 15);

	if (chg->chg_param.smb_version == PMI632_SUBTYPE)
		temp = DIV_ROUND_CLOSEST(temp * ITERM_LIMITS_PMI632_MA,
					ADC_CHG_ITERM_MASK);
	else
		temp = DIV_ROUND_CLOSEST(temp * ITERM_LIMITS_PM8150B_MA,
					ADC_CHG_ITERM_MASK);

	val->intval = temp;

	return rc;
}

int smblib_get_prop_batt_temp(struct smb_charger *chg,
					union power_supply_propval *val)
{
	int rc;

	if (!chg->bms_psy)
		return -EINVAL;

	if (chg->use_fake_temp) {
		val->intval = chg->fake_temp;
		return 0;
	}

	rc = power_supply_get_property(chg->bms_psy,
				POWER_SUPPLY_PROP_TEMP, val);
	return rc;
}

int smblib_get_prop_batt_charge_done(struct smb_charger *chg,
					union power_supply_propval *val)
{
	int rc;
	u8 stat;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval pval = {0, };

	if (chg->swarp_supported && is_op_chg_available(chg)) {
		rc = power_supply_get_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_STATUS, &pval);
		if (rc)
			smblib_err(chg, "Couldn't get op_psy charging status rc=%d\n", rc);

		val->intval = (pval.intval == POWER_SUPPLY_STATUS_FULL);
	} else {
#endif
		rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
			return rc;
		}

		stat = stat & BATTERY_CHARGER_STATUS_MASK;
		val->intval = (stat == TERMINATE_CHARGE);
#ifdef OP_SWARP_SUPPORTED
	}
#endif
	return 0;
}

int smblib_get_batt_current_now(struct smb_charger *chg,
					union power_supply_propval *val)
{
	int rc;

	rc = smblib_get_prop_from_bms(chg,
			POWER_SUPPLY_PROP_CURRENT_NOW, val);
	if (!rc)
		val->intval *= 1;

	return rc;
}

/***********************
 * BATTERY PSY SETTERS *
 ***********************/

int smblib_set_prop_input_suspend(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	int rc;

	/* vote 0mA when suspended */
	rc = vote(chg->usb_icl_votable, USER_VOTER, (bool)val->intval, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s USB rc=%d\n",
			(bool)val->intval ? "suspend" : "resume", rc);
		return rc;
	}

	rc = vote(chg->dc_suspend_votable, USER_VOTER, (bool)val->intval, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s DC rc=%d\n",
			(bool)val->intval ? "suspend" : "resume", rc);
		return rc;
	}

	power_supply_changed(chg->batt_psy);
	return rc;
}

int op_set_prop_otg_switch(struct smb_charger *chg,
				  bool enable)
{
	int rc = 0;
	u8 power_role;
	u8 ctrl = 0;
	bool pre_otg_switch;
	bool user_vote_enable;
	int i = 0;
	int wireless_present = 0;
	union power_supply_propval pval;

	pre_otg_switch = chg->otg_switch;
	chg->otg_switch = enable;

	if (!chg->wls_psy)
		chg->wls_psy = power_supply_get_by_name("wireless");
	if (chg->wls_psy) {
		rc = power_supply_get_property(chg->wls_psy,
					       POWER_SUPPLY_PROP_PRESENT,
					       &pval);
		if (rc < 0)
			smblib_err(chg, "Couldn't get wireless present rc=%d\n", rc);
		else
			wireless_present = pval.intval;
	} else {
		smblib_err(chg, "Couldn't find wireless psy\n");
	}

	user_vote_enable = is_client_vote_enabled_locked(chg->otg_toggle_votable, USER_VOTER);
	if ((chg->otg_switch == pre_otg_switch) &&
	    !(user_vote_enable && wireless_present))
		return 0;

	pr_info("set otg_switch=%d\n", chg->otg_switch);
	if (chg->otg_switch) {
		if (wireless_present)
			power_role = EN_DRP_MODE;
		else
			power_role = EN_TRY_SNK_BIT;
	} else {
		power_role = EN_SNK_ONLY_BIT;
	}

	for (i = 0; i < 10; i++) {
		rc = smblib_masked_write(chg,
					TYPE_C_MODE_CFG_REG,
					TYPEC_POWER_ROLE_CMD_MASK, power_role);
		if (rc < 0) {
			smblib_err(chg, "Couldn't write 0x%02x to 0x1544 rc=%d\n",
				power_role, rc);
			return rc;
		}
		usleep_range(30000, 31000);
		ctrl = 0;
		rc = smblib_read(chg,
			TYPE_C_MODE_CFG_REG, &ctrl);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read err=%d\n", rc);
			return rc;
		}
		if ((power_role == EN_TRY_SNK_BIT) && (ctrl & EN_TRY_SNK_BIT))
			break;
		if ((power_role == EN_SNK_ONLY_BIT) && (ctrl & EN_SNK_ONLY_BIT))
			break;
		if ((power_role == EN_DRP_MODE) && ((ctrl & TYPEC_POWER_ROLE_CMD_MASK) == EN_DRP_MODE))
			break;
	}
	pr_info("retry time = %d,ctrl = %d\n", i, ctrl);
	if (i == 10)
		pr_err("retry time over\n");

	if (power_role == EN_DRP_MODE) {
		rc = smblib_masked_write(chg, TYPE_C_EXIT_STATE_CFG_REG,
					 BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT,
					 BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't set exit state cfg rc=%d\n", rc);
	} else {
		rc = smblib_masked_write(chg, TYPE_C_EXIT_STATE_CFG_REG,
					 BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT,
					 0);
		if (rc < 0)
			smblib_err(chg, "Couldn't clear exit state cfg rc=%d\n", rc);
	}

	return rc;

}

int smblib_set_prop_chg_voltage(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	chg->fake_chgvol = val->intval;
	chg->use_fake_chgvol = true;
	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_batt_temp(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	chg->fake_temp = val->intval;
	chg->use_fake_temp = true;
	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_chg_protect_status(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	chg->fake_protect_sts = val->intval;
	chg->use_fake_protect_sts = true;
	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_charge_parameter_set(struct smb_charger *chg)
{
	chg->is_power_changed = true;
	op_check_battery_temp(chg);
	return 0;
}

int smblib_set_prop_batt_capacity(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	chg->fake_capacity = val->intval;

	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_batt_capacity_level(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	pr_info("set fake capacity level:%d", val->intval);
	chg->fake_capacity_level = val->intval;
	power_supply_changed(chg->batt_psy);
	return 0;
}

int smblib_set_prop_batt_status(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	/* Faking battery full */
	if (val->intval == POWER_SUPPLY_STATUS_FULL)
		chg->fake_batt_status = val->intval;
	else
		chg->fake_batt_status = -EINVAL;

	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_system_temp_level(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	if (val->intval < 0)
		return -EINVAL;

	if (chg->thermal_levels <= 0)
		return -EINVAL;

	if (val->intval > chg->thermal_levels)
		return -EINVAL;

	chg->system_temp_level = val->intval;

	if (chg->system_temp_level == chg->thermal_levels)
		return vote(chg->chg_disable_votable,
			THERMAL_DAEMON_VOTER, true, 0);

	vote(chg->chg_disable_votable, THERMAL_DAEMON_VOTER, false, 0);
	if (chg->system_temp_level == 0)
		return vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, false, 0);

	vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, true,
			chg->thermal_mitigation[chg->system_temp_level]);
	return 0;
}

int smblib_set_prop_input_current_limited(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	chg->fake_input_current_limited = val->intval;
	return 0;
}

int smblib_set_prop_rechg_soc_thresh(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;
	u8 new_thr = DIV_ROUND_CLOSEST(val->intval * 255, 100);

	rc = smblib_write(chg, CHARGE_RCHG_SOC_THRESHOLD_CFG_REG,
			new_thr);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write to RCHG_SOC_THRESHOLD_CFG_REG rc=%d\n",
				rc);
		return rc;
	}

	chg->auto_recharge_soc = val->intval;

	return rc;
}

int smblib_run_aicl(struct smb_charger *chg, int type)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
								rc);
		return rc;
	}

	/* USB is suspended so skip re-running AICL */
	if (stat & USBIN_SUSPEND_STS_BIT)
		return rc;

	smblib_dbg(chg, PR_MISC, "re-running AICL\n");
	rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				USBIN_AICL_PERIODIC_RERUN_EN_BIT,
				USBIN_AICL_PERIODIC_RERUN_EN_BIT);

	stat = (type == RERUN_AICL) ? RERUN_AICL_BIT : RESTART_AICL_BIT;
	rc = smblib_masked_write(chg, AICL_CMD_REG, stat, stat);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to AICL_CMD_REG rc=%d\n",
				rc);
	return 0;
}

static int smblib_dp_pulse(struct smb_charger *chg)
{
	int rc;

	/* QC 3.0 increment */
	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, SINGLE_INCREMENT_BIT,
			SINGLE_INCREMENT_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

static int smblib_dm_pulse(struct smb_charger *chg)
{
	int rc;

	/* QC 3.0 decrement */
	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, SINGLE_DECREMENT_BIT,
			SINGLE_DECREMENT_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

int smblib_force_vbus_voltage(struct smb_charger *chg, u8 val)
{
	int rc;

	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, val, val);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

static void smblib_hvdcp_set_fsw(struct smb_charger *chg, int bit)
{
	switch (bit) {
	case QC_5V_BIT:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_5V);
		break;
	case QC_9V_BIT:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_9V);
		break;
	case QC_12V_BIT:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_12V);
		break;
	default:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_removal);
		break;
	}
}

#define QC3_PULSES_FOR_6V	5
#define QC3_PULSES_FOR_9V	20
#define QC3_PULSES_FOR_12V	35
static int smblib_hvdcp3_set_fsw(struct smb_charger *chg)
{
	int pulse_count, rc;

	rc = smblib_get_pulse_cnt(chg, &pulse_count);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
		return rc;
	}

	if (pulse_count < QC3_PULSES_FOR_6V)
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_5V);
	else if (pulse_count < QC3_PULSES_FOR_9V)
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_6V_8V);
	else if (pulse_count < QC3_PULSES_FOR_12V)
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_9V);
	else
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_12V);

	return 0;
}

static void smblib_hvdcp_adaptive_voltage_change(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_CHANGE_STATUS rc=%d\n", rc);
			return;
		}
		smblib_err(chg, "QC_CHANGE_STATUS_REG=0x%02x", stat);

		smblib_hvdcp_set_fsw(chg, stat & QC_2P0_STATUS_MASK);
		vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3
		|| chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5) {
		rc = smblib_hvdcp3_set_fsw(chg);
		if (rc < 0)
			smblib_err(chg, "Couldn't set QC3.0 Fsw rc=%d\n", rc);
	}

	power_supply_changed(chg->usb_main_psy);
}

int smblib_dp_dm(struct smb_charger *chg, int val)
{
	int target_icl_ua, rc = 0;
	union power_supply_propval pval;
	u8 stat;

	switch (val) {
	case POWER_SUPPLY_DP_DM_DP_PULSE:
		/*
		 * Pre-emptively increment pulse count to enable the setting
		 * of FSW prior to increasing voltage.
		 */
		chg->pulse_cnt++;

		rc = smblib_hvdcp3_set_fsw(chg);
		if (rc < 0)
			smblib_err(chg, "Couldn't set QC3.0 Fsw rc=%d\n", rc);

		rc = smblib_dp_pulse(chg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't increase pulse count rc=%d\n",
				rc);
			/*
			 * Increment pulse count failed;
			 * reset to former value.
			 */
			chg->pulse_cnt--;
		}

		smblib_dbg(chg, PR_PARALLEL, "DP_DM_DP_PULSE rc=%d cnt=%d\n",
				rc, chg->pulse_cnt);
		break;
	case POWER_SUPPLY_DP_DM_DM_PULSE:
		rc = smblib_dm_pulse(chg);
		if (!rc && chg->pulse_cnt)
			chg->pulse_cnt--;
		smblib_dbg(chg, PR_PARALLEL, "DP_DM_DM_PULSE rc=%d cnt=%d\n",
				rc, chg->pulse_cnt);
		break;
	case POWER_SUPPLY_DP_DM_ICL_DOWN:
		target_icl_ua = get_effective_result(chg->usb_icl_votable);
		if (target_icl_ua < 0) {
			/* no client vote, get the ICL from charger */
			rc = power_supply_get_property(chg->usb_psy,
					POWER_SUPPLY_PROP_HW_CURRENT_MAX,
					&pval);
			if (rc < 0) {
				smblib_err(chg, "Couldn't get max curr rc=%d\n",
					rc);
				return rc;
			}
			target_icl_ua = pval.intval;
		}

		/*
		 * Check if any other voter voted on USB_ICL in case of
		 * voter other than SW_QC3_VOTER reset and restart reduction
		 * again.
		 */
		if (target_icl_ua != get_client_vote(chg->usb_icl_votable,
							SW_QC3_VOTER))
			chg->usb_icl_delta_ua = 0;

		chg->usb_icl_delta_ua += 100000;
		vote(chg->usb_icl_votable, SW_QC3_VOTER, true,
						target_icl_ua - 100000);
		smblib_dbg(chg, PR_PARALLEL, "ICL DOWN ICL=%d reduction=%d\n",
				target_icl_ua, chg->usb_icl_delta_ua);
		break;
	case POWER_SUPPLY_DP_DM_FORCE_5V:
		rc = smblib_force_vbus_voltage(chg, FORCE_5V_BIT);
		if (rc < 0)
			pr_err("Failed to force 5V\n");
		break;
	case POWER_SUPPLY_DP_DM_FORCE_9V:
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_9V) {
			smblib_err(chg, "Couldn't set 9V: unsupported\n");
			return -EINVAL;
		}

		/* If we are increasing voltage to get to 9V, set FSW first */
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read QC_CHANGE_STATUS_REG rc=%d\n",
					rc);
			break;
		}

		if (stat & QC_5V_BIT) {
			/* Force 1A ICL before requesting higher voltage */
			vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER,
					true, 1000000);
			smblib_hvdcp_set_fsw(chg, QC_9V_BIT);
		}

		rc = smblib_force_vbus_voltage(chg, FORCE_9V_BIT);
		if (rc < 0)
			pr_err("Failed to force 9V\n");
		break;
	case POWER_SUPPLY_DP_DM_FORCE_12V:
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_12V) {
			smblib_err(chg, "Couldn't set 12V: unsupported\n");
			return -EINVAL;
		}

		/* If we are increasing voltage to get to 12V, set FSW first */
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read QC_CHANGE_STATUS_REG rc=%d\n",
					rc);
			break;
		}

		if ((stat & QC_9V_BIT) || (stat & QC_5V_BIT)) {
			/* Force 1A ICL before requesting higher voltage */
			vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER,
					true, 1000000);
			smblib_hvdcp_set_fsw(chg, QC_12V_BIT);
		}

		rc = smblib_force_vbus_voltage(chg, FORCE_12V_BIT);
		if (rc < 0)
			pr_err("Failed to force 12V\n");
		break;
	case POWER_SUPPLY_DP_DM_CONFIRMED_HVDCP3P5:
		chg->qc3p5_detected = true;
		smblib_update_usb_type(chg);
		break;
	case POWER_SUPPLY_DP_DM_ICL_UP:
	default:
		break;
	}

	return rc;
}

int smblib_disable_hw_jeita(struct smb_charger *chg, bool disable)
{
	int rc;
	u8 mask;

	/*
	 * Disable h/w base JEITA compensation if s/w JEITA is enabled
	 */
	mask = JEITA_EN_COLD_SL_FCV_BIT
		| JEITA_EN_HOT_SL_FCV_BIT
		| JEITA_EN_HOT_SL_CCC_BIT
		| JEITA_EN_COLD_SL_CCC_BIT,
	rc = smblib_masked_write(chg, JEITA_EN_CFG_REG, mask,
			disable ? 0 : mask);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure s/w jeita rc=%d\n",
				rc);
		return rc;
	}

	return 0;
}

static int smblib_set_sw_thermal_regulation(struct smb_charger *chg,
						bool enable)
{
	int rc = 0;

	if (!(chg->wa_flags & SW_THERM_REGULATION_WA))
		return rc;

	if (enable) {
		/*
		 * Configure min time to quickly address thermal
		 * condition.
		 */
		rc = smblib_masked_write(chg, SNARL_BARK_BITE_WD_CFG_REG,
			SNARL_WDOG_TIMEOUT_MASK, SNARL_WDOG_TMOUT_62P5MS);
		if (rc < 0) {
			smblib_err(chg, "Couldn't configure snarl wdog tmout, rc=%d\n",
					rc);
			return rc;
		}

		/*
		 * Schedule SW_THERM_REGULATION_WORK directly if USB input
		 * is suspended due to SW thermal regulation WA since WDOG
		 * IRQ won't trigger with input suspended.
		 */
		if (is_client_vote_enabled(chg->usb_icl_votable,
						SW_THERM_REGULATION_VOTER)) {
			vote(chg->awake_votable, SW_THERM_REGULATION_VOTER,
								true, 0);
			schedule_delayed_work(&chg->thermal_regulation_work, 0);
		}
	} else {
		cancel_delayed_work_sync(&chg->thermal_regulation_work);
		vote(chg->awake_votable, SW_THERM_REGULATION_VOTER, false, 0);
	}

	smblib_dbg(chg, PR_MISC, "WDOG SNARL INT %s\n",
				enable ? "Enabled" : "Disabled");

	return rc;
}

static int smblib_update_thermal_readings(struct smb_charger *chg)
{
	union power_supply_propval pval = {0, };
	int rc = 0;

	if (!chg->pl.psy)
		chg->pl.psy = power_supply_get_by_name("parallel");

	rc = smblib_read_iio_channel(chg, chg->iio.die_temp_chan,
				DIV_FACTOR_DECIDEGC, &chg->die_temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DIE TEMP channel, rc=%d\n", rc);
		return rc;
	}

	rc = smblib_read_iio_channel(chg, chg->iio.connector_temp_chan,
				DIV_FACTOR_DECIDEGC, &chg->connector_temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read CONN TEMP channel, rc=%d\n", rc);
		return rc;
	}

	rc = smblib_read_iio_channel(chg, chg->iio.skin_temp_chan,
				DIV_FACTOR_DECIDEGC, &chg->skin_temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SKIN TEMP channel, rc=%d\n", rc);
		return rc;
	}

	if (chg->sec_chg_selected == POWER_SUPPLY_CHARGER_SEC_CP) {
		if (is_cp_available(chg)) {
			rc = power_supply_get_property(chg->cp_psy,
				POWER_SUPPLY_PROP_CP_DIE_TEMP, &pval);
			if (rc < 0) {
				smblib_err(chg, "Couldn't get smb1390 charger temp, rc=%d\n",
					rc);
				return rc;
			}
			chg->smb_temp = pval.intval;
		} else {
			smblib_dbg(chg, PR_MISC, "Coudln't find cp_psy\n");
			chg->smb_temp = -ENODATA;
		}
	} else if (chg->pl.psy && chg->sec_chg_selected ==
					POWER_SUPPLY_CHARGER_SEC_PL) {
		rc = power_supply_get_property(chg->pl.psy,
				POWER_SUPPLY_PROP_CHARGER_TEMP, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get smb1355 charger temp, rc=%d\n",
					rc);
			return rc;
		}
		chg->smb_temp = pval.intval;
	} else {
		chg->smb_temp = -ENODATA;
	}

	return rc;
}

/* SW thermal regulation thresholds in deciDegC */
#define DIE_TEMP_RST_THRESH		1000
#define DIE_TEMP_REG_H_THRESH		800
#define DIE_TEMP_REG_L_THRESH		600

#define CONNECTOR_TEMP_SHDN_THRESH	700
#define CONNECTOR_TEMP_RST_THRESH	600
#define CONNECTOR_TEMP_REG_H_THRESH	550
#define CONNECTOR_TEMP_REG_L_THRESH	500

#define SMB_TEMP_SHDN_THRESH		1400
#define SMB_TEMP_RST_THRESH		900
#define SMB_TEMP_REG_H_THRESH		800
#define SMB_TEMP_REG_L_THRESH		600

#define SKIN_TEMP_SHDN_THRESH		700
#define SKIN_TEMP_RST_THRESH		600
#define SKIN_TEMP_REG_H_THRESH		550
#define SKIN_TEMP_REG_L_THRESH		500

#define THERM_REG_RECHECK_DELAY_1S	1000	/* 1 sec */
#define THERM_REG_RECHECK_DELAY_8S	8000	/* 8 sec */
static int smblib_process_thermal_readings(struct smb_charger *chg)
{
	int rc = 0, wdog_timeout = SNARL_WDOG_TMOUT_8S;
	u32 thermal_status = TEMP_BELOW_RANGE;
	bool suspend_input = false, disable_smb = false;

	/*
	 * Following is the SW thermal regulation flow:
	 *
	 * TEMP_SHUT_DOWN_LEVEL: If either connector temp or skin temp
	 * exceeds their respective SHDN threshold. Need to suspend input
	 * and secondary charger.
	 *
	 * TEMP_SHUT_DOWN_SMB_LEVEL: If smb temp exceed its SHDN threshold
	 * but connector and skin temp are below it. Need to suspend SMB.
	 *
	 * TEMP_ALERT_LEVEL: If die, connector, smb or skin temp exceeds it's
	 * respective RST threshold. Stay put and monitor temperature closely.
	 *
	 * TEMP_ABOVE_RANGE or TEMP_WITHIN_RANGE or TEMP_BELOW_RANGE: If die,
	 * connector, smb or skin temp exceeds it's respective REG_H or REG_L
	 * threshold. Unsuspend input and SMB.
	 */
	if (chg->connector_temp > CONNECTOR_TEMP_SHDN_THRESH ||
		chg->skin_temp > SKIN_TEMP_SHDN_THRESH) {
		thermal_status = TEMP_SHUT_DOWN;
		wdog_timeout = SNARL_WDOG_TMOUT_1S;
		suspend_input = true;
		disable_smb = true;
		goto out;
	}

	if (chg->smb_temp > SMB_TEMP_SHDN_THRESH) {
		thermal_status = TEMP_SHUT_DOWN_SMB;
		wdog_timeout = SNARL_WDOG_TMOUT_1S;
		disable_smb = true;
		goto out;
	}

	if (chg->connector_temp > CONNECTOR_TEMP_RST_THRESH ||
			chg->skin_temp > SKIN_TEMP_RST_THRESH ||
			chg->smb_temp > SMB_TEMP_RST_THRESH ||
			chg->die_temp > DIE_TEMP_RST_THRESH) {
		thermal_status = TEMP_ALERT_LEVEL;
		wdog_timeout = SNARL_WDOG_TMOUT_1S;
		goto out;
	}

	if (chg->connector_temp > CONNECTOR_TEMP_REG_H_THRESH ||
			chg->skin_temp > SKIN_TEMP_REG_H_THRESH ||
			chg->smb_temp > SMB_TEMP_REG_H_THRESH ||
			chg->die_temp > DIE_TEMP_REG_H_THRESH) {
		thermal_status = TEMP_ABOVE_RANGE;
		wdog_timeout = SNARL_WDOG_TMOUT_1S;
		goto out;
	}

	if (chg->connector_temp > CONNECTOR_TEMP_REG_L_THRESH ||
			chg->skin_temp > SKIN_TEMP_REG_L_THRESH ||
			chg->smb_temp > SMB_TEMP_REG_L_THRESH ||
			chg->die_temp > DIE_TEMP_REG_L_THRESH) {
		thermal_status = TEMP_WITHIN_RANGE;
		wdog_timeout = SNARL_WDOG_TMOUT_8S;
	}
out:
	smblib_dbg(chg, PR_MISC, "Current temperatures: \tDIE_TEMP: %d,\tCONN_TEMP: %d,\tSMB_TEMP: %d,\tSKIN_TEMP: %d\nTHERMAL_STATUS: %d\n",
			chg->die_temp, chg->connector_temp, chg->smb_temp,
			chg->skin_temp, thermal_status);

	if (thermal_status != chg->thermal_status) {
		chg->thermal_status = thermal_status;
		/*
		 * If thermal level changes to TEMP ALERT LEVEL, don't
		 * enable/disable main/parallel charging.
		 */
		if (chg->thermal_status == TEMP_ALERT_LEVEL)
			goto exit;

		vote(chg->smb_override_votable, SW_THERM_REGULATION_VOTER,
				disable_smb, 0);

		/*
		 * Enable/disable secondary charger through votables to ensure
		 * that if SMB_EN pin get's toggled somehow, secondary charger
		 * remains enabled/disabled according to SW thermal regulation.
		 */
		if (!chg->cp_disable_votable)
			chg->cp_disable_votable = find_votable("CP_DISABLE");
		if (chg->cp_disable_votable)
			vote(chg->cp_disable_votable, SW_THERM_REGULATION_VOTER,
							disable_smb, 0);

		vote(chg->pl_disable_votable, SW_THERM_REGULATION_VOTER,
							disable_smb, 0);
		smblib_dbg(chg, PR_MISC, "Parallel %s as per SW thermal regulation\n",
				disable_smb ? "disabled" : "enabled");

		/*
		 * If thermal level changes to TEMP_SHUT_DOWN_SMB, don't
		 * enable/disable main charger.
		 */
		if (chg->thermal_status == TEMP_SHUT_DOWN_SMB)
			goto exit;

		/* Suspend input if SHDN threshold reached */
		vote(chg->dc_suspend_votable, SW_THERM_REGULATION_VOTER,
							suspend_input, 0);
		vote(chg->usb_icl_votable, SW_THERM_REGULATION_VOTER,
							suspend_input, 0);
		smblib_dbg(chg, PR_MISC, "USB/DC %s as per SW thermal regulation\n",
				suspend_input ? "suspended" : "unsuspended");
	}
exit:
	/*
	 * On USB suspend, WDOG IRQ stops triggering. To continue thermal
	 * monitoring and regulation until USB is plugged out, reschedule
	 * the SW thermal regulation work without releasing the wake lock.
	 */
	if (is_client_vote_enabled(chg->usb_icl_votable,
					SW_THERM_REGULATION_VOTER)) {
		schedule_delayed_work(&chg->thermal_regulation_work,
				msecs_to_jiffies(THERM_REG_RECHECK_DELAY_1S));
		return 0;
	}

	rc = smblib_masked_write(chg, SNARL_BARK_BITE_WD_CFG_REG,
			SNARL_WDOG_TIMEOUT_MASK, wdog_timeout);
	if (rc < 0)
		smblib_err(chg, "Couldn't set WD SNARL timer, rc=%d\n", rc);

	vote(chg->awake_votable, SW_THERM_REGULATION_VOTER, false, 0);
	return rc;
}

/*******************
 * DC PSY GETTERS *
 *******************/

int smblib_get_prop_voltage_wls_output(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc;

	if (!chg->wls_psy) {
		chg->wls_psy = power_supply_get_by_name("wireless");
		if (!chg->wls_psy)
			return -ENODEV;
	}

	rc = power_supply_get_property(chg->wls_psy,
				POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION,
				val);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't get POWER_SUPPLY_PROP_VOLTAGE_REGULATION, rc=%d\n",
				rc);

	return rc;
}

int smblib_get_prop_dc_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	if (chg->chg_param.smb_version == PMI632_SUBTYPE) {
		val->intval = 0;
		return 0;
	}

	rc = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DCIN_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = (bool)(stat & DCIN_PLUGIN_RT_STS_BIT);
	return 0;
}

int smblib_get_prop_dc_online(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	if (chg->chg_param.smb_version == PMI632_SUBTYPE) {
		val->intval = 0;
		return 0;
	}

	if (get_client_vote(chg->dc_suspend_votable, USER_VOTER)) {
		val->intval = false;
		return rc;
	}

	if (is_client_vote_enabled(chg->dc_suspend_votable,
						CHG_TERMINATION_VOTER)) {
		rc = smblib_get_prop_dc_present(chg, val);
		return rc;
	}

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "POWER_PATH_STATUS = 0x%02x\n",
		   stat);

	val->intval = (stat & USE_DCIN_BIT) &&
		      (stat & VALID_INPUT_POWER_SOURCE_STS_BIT);

	return rc;
}

int smblib_get_prop_dc_current_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	return smblib_get_charge_param(chg, &chg->param.dc_icl, &val->intval);
}

int smblib_get_prop_dc_voltage_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc;
	val->intval = MICRO_12V;

	if (!chg->wls_psy)
		chg->wls_psy = power_supply_get_by_name("wireless");

	if (chg->wls_psy) {
		rc = power_supply_get_property(chg->wls_psy,
				POWER_SUPPLY_PROP_VOLTAGE_MAX,
				val);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't get VOLTAGE_MAX, rc=%d\n",
					rc);
			return rc;
		}
	}

	return 0;
}

int smblib_get_prop_dc_voltage_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc;

	if (!chg->wls_psy) {
		chg->wls_psy = power_supply_get_by_name("wireless");
		if (!chg->wls_psy)
			return -ENODEV;
	}

	rc = power_supply_get_property(chg->wls_psy,
				POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION,
				val);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't get POWER_SUPPLY_PROP_VOLTAGE_REGULATION, rc=%d\n",
				rc);
		return rc;
	}

	return rc;
}

/*******************
 * DC PSY SETTERS *
 *******************/

int smblib_set_prop_dc_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	chg->dcin_icl_user_set = true;
	return smblib_set_charge_param(chg, &chg->param.dc_icl, val->intval);
}

#define DCIN_AICL_RERUN_DELAY_MS	5000
int smblib_set_prop_voltage_wls_output(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;

	if (!chg->wls_psy) {
		chg->wls_psy = power_supply_get_by_name("wireless");
		if (!chg->wls_psy)
			return -ENODEV;
	}

	rc = power_supply_set_property(chg->wls_psy,
				POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION,
				val);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't set POWER_SUPPLY_PROP_VOLTAGE_REGULATION, rc=%d\n",
				rc);

	smblib_dbg(chg, PR_WLS, "%d\n", val->intval);

	/*
	 * When WLS VOUT goes down, the power-constrained adaptor may be able
	 * to supply more current, so allow it to do so - unless userspace has
	 * changed DCIN ICL value already due to thermal considerations.
	 */
	if (!chg->dcin_icl_user_set && (val->intval > 0) &&
			(val->intval < chg->last_wls_vout)) {
		alarm_start_relative(&chg->dcin_aicl_alarm,
				ms_to_ktime(DCIN_AICL_RERUN_DELAY_MS));
	}

	chg->last_wls_vout = val->intval;

	return rc;
}

int smblib_set_prop_dc_reset(struct smb_charger *chg)
{
	int rc;

	rc = vote(chg->dc_suspend_votable, VOUT_VOTER, true, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't suspend DC rc=%d\n", rc);
		return rc;
	}

	rc = smblib_masked_write(chg, DCIN_CMD_IL_REG, DCIN_EN_MASK,
				DCIN_EN_OVERRIDE_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set DCIN_EN_OVERRIDE_BIT rc=%d\n",
			rc);
		return rc;
	}

	rc = smblib_write(chg, DCIN_CMD_PON_REG, DCIN_PON_BIT | MID_CHG_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write %lu to DCIN_CMD_PON_REG rc=%d\n",
			DCIN_PON_BIT | MID_CHG_BIT, rc);
		return rc;
	}

	/* Wait for 10ms to allow the charge to get drained */
	usleep_range(10000, 10010);

	rc = smblib_write(chg, DCIN_CMD_PON_REG, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't clear DCIN_CMD_PON_REG rc=%d\n", rc);
		return rc;
	}

	rc = smblib_masked_write(chg, DCIN_CMD_IL_REG, DCIN_EN_MASK, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't clear DCIN_EN_OVERRIDE_BIT rc=%d\n",
			rc);
		return rc;
	}

	rc = vote(chg->dc_suspend_votable, VOUT_VOTER, false, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't unsuspend  DC rc=%d\n", rc);
		return rc;
	}

	smblib_dbg(chg, PR_MISC, "Wireless charger removal detection successful\n");
	return rc;
}

/*******************
 * USB PSY GETTERS *
 *******************/

int smblib_get_prop_usb_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	return 0;
}

int smblib_get_prop_usb_online(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	if (get_client_vote_locked(chg->usb_icl_votable, USER_VOTER) == 0) {
		val->intval = false;
		return rc;
	}

	if (chg->vbus_present) {
		val->intval = true;
		return rc;
	}
	chg->dash_on = get_prop_fast_chg_started(chg);
	if (chg->dash_on) {
		val->intval = true;
		return rc;
	}
	if (chg->icon_debounce) {
		val->intval = 1;
		return rc;
	}
	if (is_client_vote_enabled_locked(chg->usb_icl_votable,
					CHG_TERMINATION_VOTER)) {
		rc = smblib_get_prop_usb_present(chg, val);
		return rc;
	}

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "POWER_PATH_STATUS = 0x%02x\n",
		   stat);

	val->intval = (stat & USE_USBIN_BIT) &&
		      (stat & VALID_INPUT_POWER_SOURCE_STS_BIT);
	return rc;
}

int smblib_get_usb_online(struct smb_charger *chg,
			union power_supply_propval *val)
{
	int rc;

	rc = smblib_get_prop_usb_online(chg, val);
	if (!val->intval)
		goto exit;

	if (((chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) ||
		(chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_MEDIUM) ||
		(chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH) ||
		(chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB))
		&& ((chg->real_charger_type == POWER_SUPPLY_TYPE_USB) ||
		(chg->real_charger_type == POWER_SUPPLY_TYPE_USB_CDP)))
		val->intval = 0;
	else
		val->intval = 1;

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_UNKNOWN &&
		!chg->pd_active && !chg->icon_debounce)
		val->intval = 0;
	if (chg->wireless_present)
		val->intval = 0;

exit:
	return rc;
}

int smblib_get_prop_usb_voltage_max_design(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_9V) {
			val->intval = MICRO_5V;
			break;
		} else if (chg->qc2_unsupported_voltage ==
				QC2_NON_COMPLIANT_12V) {
			val->intval = MICRO_9V;
			break;
		}
		/* else, fallthrough */
	case POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
	case POWER_SUPPLY_TYPE_USB_PD:
		if (chg->chg_param.smb_version == PMI632_SUBTYPE)
			val->intval = MICRO_9V;
		else
			val->intval = MICRO_12V;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_usb_voltage_max(struct smb_charger *chg,
					union power_supply_propval *val)
{
	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_9V) {
			val->intval = MICRO_5V;
			break;
		} else if (chg->qc2_unsupported_voltage ==
				QC2_NON_COMPLIANT_12V) {
			val->intval = MICRO_9V;
			break;
		}
		val->intval = MICRO_9V;
		break;
		/* else, fallthrough */
	case POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		if (chg->chg_param.smb_version == PMI632_SUBTYPE)
			val->intval = MICRO_9V;
		else
			val->intval = MICRO_12V;
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		val->intval = chg->voltage_max_uv;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

#define HVDCP3_STEP_UV	200000
#define HVDCP3P5_STEP_UV	20000
static int smblib_estimate_adaptor_voltage(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	int step_uv = HVDCP3_STEP_UV;

	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
		val->intval = MICRO_12V;
		break;
	case POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
		step_uv = HVDCP3P5_STEP_UV;
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		val->intval = MICRO_5V + (step_uv * chg->pulse_cnt);
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		/* Take the average of min and max values */
		val->intval = chg->voltage_min_uv +
			((chg->voltage_max_uv - chg->voltage_min_uv) / 2);
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}
	val->intval = val->intval / 1000;
	return 0;
}

static int smblib_read_mid_voltage_chan(struct smb_charger *chg,
					union power_supply_propval *val)
{
	int rc;

	if (!chg->iio.mid_chan)
		return -ENODATA;

	rc = iio_read_channel_processed(chg->iio.mid_chan, &val->intval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read MID channel rc=%d\n", rc);
		return rc;
	}

	/*
	 * If MID voltage < 1V, it is unreliable.
	 * Figure out voltage from registers and calculations.
	 */
	if (val->intval < 1000000)
		smblib_estimate_adaptor_voltage(chg, val);
	val->intval = val->intval / 1000;

	return 0;
}

static int smblib_read_usbin_voltage_chan(struct smb_charger *chg,
				     union power_supply_propval *val)
{
	int rc;

	if (!chg->iio.usbin_v_chan)
		return -ENODATA;

	rc = iio_read_channel_processed(chg->iio.usbin_v_chan, &val->intval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN channel rc=%d\n", rc);
		return rc;
	}
	val->intval = val->intval / 1000;

	return 0;
}

int smblib_get_prop_usb_voltage_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	int rc, ret = 0;
	u8 reg, adc_ch_reg = 0;

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported && is_op_chg_available(chg)) {
		rc = power_supply_get_property(chg->op_chg_psy,
			POWER_SUPPLY_PROP_CHARGE_NOW, val);
		if (rc < 0)
			val->intval = 0;
		else
			val->intval /= 1000;
		return rc;
	}
#endif
	mutex_lock(&chg->adc_lock);

	if (chg->wa_flags & USBIN_ADC_WA) {
		/* Store ADC channel config in order to restore later */
		rc = smblib_read(chg, BATIF_ADC_CHANNEL_EN_REG, &adc_ch_reg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read ADC config rc=%d\n", rc);
			ret = rc;
			goto unlock;
		}

		/* Disable all ADC channels except IBAT channel */
		rc = smblib_write(chg, BATIF_ADC_CHANNEL_EN_REG,
						IBATT_CHANNEL_EN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable ADC channels rc=%d\n",
						rc);
			ret = rc;
			goto unlock;
		}
	}

	rc = smblib_get_prop_usb_present(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb presence status rc=%d\n", rc);
		ret = -ENODATA;
		goto restore_adc_config;
	}

	if (!pval.intval) {
		val->intval = 0;
		goto unlock;
	}

	/*
	 * Skip reading voltage only if USB is not present and we are not in
	 * OTG mode.
	 */
	if (!pval.intval) {
		rc = smblib_read(chg, DCDC_CMD_OTG_REG, &reg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read CMD_OTG rc=%d", rc);
			goto restore_adc_config;
		}

		if (!(reg & OTG_EN_BIT))
			goto restore_adc_config;
	}

	/*
	 * For PM8150B, use MID_CHG ADC channel because overvoltage is observed
	 * to occur randomly in the USBIN channel, particularly at high
	 * voltages.
	 */
	if (chg->chg_param.smb_version == PM8150B_SUBTYPE && pval.intval
			&& !chg->low_voltage_charger)
		rc = smblib_read_mid_voltage_chan(chg, val);
	else
		rc = smblib_read_usbin_voltage_chan(chg, val);
	if (rc < 0) {
		smblib_err(chg, "Failed to read USBIN over vadc, rc=%d\n", rc);
		ret = rc;
	}

restore_adc_config:
	 /* Restore ADC channel config */
	if (chg->wa_flags & USBIN_ADC_WA) {
		rc = smblib_write(chg, BATIF_ADC_CHANNEL_EN_REG, adc_ch_reg);
		if (rc < 0)
			smblib_err(chg, "Couldn't write ADC config rc=%d\n",
						rc);
	}

unlock:
	mutex_unlock(&chg->adc_lock);

	return ret;
}

int smblib_get_prop_vph_voltage_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc;

	if (!chg->iio.vph_v_chan)
		return -ENODATA;

	rc = iio_read_channel_processed(chg->iio.vph_v_chan, &val->intval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read vph channel rc=%d\n", rc);
		return rc;
	}

	return 0;
}

bool smblib_rsbux_low(struct smb_charger *chg, int r_thr)
{
	int r_sbu1, r_sbu2;
	bool ret = false;
	int rc;

	if (!chg->iio.sbux_chan)
		return false;

	/* disable crude sensors */
	rc = smblib_masked_write(chg, TYPE_C_CRUDE_SENSOR_CFG_REG,
			EN_SRC_CRUDE_SENSOR_BIT | EN_SNK_CRUDE_SENSOR_BIT,
			0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable crude sensor rc=%d\n", rc);
		return false;
	}

	/* select SBU1 as current source */
	rc = smblib_write(chg, TYPE_C_SBU_CFG_REG, SEL_SBU1_ISRC_VAL);
	if (rc < 0) {
		smblib_err(chg, "Couldn't select SBU1 rc=%d\n", rc);
		goto cleanup;
	}

	rc = iio_read_channel_processed(chg->iio.sbux_chan, &r_sbu1);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SBU1 rc=%d\n", rc);
		goto cleanup;
	}

	pr_info("r_sbu1:(%d)uv, r_thr:(%d)uv\n", r_sbu1, r_thr);

	if (r_sbu1 < r_thr) {
		ret = true;
		goto cleanup;
	}

	/* select SBU2 as current source */
	rc = smblib_write(chg, TYPE_C_SBU_CFG_REG, SEL_SBU2_ISRC_VAL);
	if (rc < 0) {
		smblib_err(chg, "Couldn't select SBU1 rc=%d\n", rc);
		goto cleanup;
	}

	rc = iio_read_channel_processed(chg->iio.sbux_chan, &r_sbu2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SBU1 rc=%d\n", rc);
		goto cleanup;
	}

	pr_info("r_sbu1:(%d)uv, r_thr:(%d)uv\n", r_sbu1, r_thr);

	if (r_sbu2 < r_thr)
		ret = true;
cleanup:
	/* enable crude sensors */
	rc = smblib_masked_write(chg, TYPE_C_CRUDE_SENSOR_CFG_REG,
			EN_SRC_CRUDE_SENSOR_BIT | EN_SNK_CRUDE_SENSOR_BIT,
			EN_SRC_CRUDE_SENSOR_BIT | EN_SNK_CRUDE_SENSOR_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable crude sensor rc=%d\n", rc);

	/* disable current source */
	rc = smblib_write(chg, TYPE_C_SBU_CFG_REG, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't select SBU1 rc=%d\n", rc);

	return ret;
}

int smblib_get_prop_charger_temp(struct smb_charger *chg,
				 union power_supply_propval *val)
{
	int temp, rc;
	int input_present;

	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0)
		return rc;

	if (input_present == INPUT_NOT_PRESENT)
		return -ENODATA;

	if (chg->iio.temp_chan) {
		rc = iio_read_channel_processed(chg->iio.temp_chan,
				&temp);
		if (rc < 0) {
			pr_err("Error in reading temp channel, rc=%d\n", rc);
			return rc;
		}
		val->intval = temp / 100;
	} else {
		return -ENODATA;
	}

	return rc;
}

int smblib_get_prop_typec_cc_orientation(struct smb_charger *chg,
					 union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	val->intval = 0;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		return 0;

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_STATUS_4 = 0x%02x\n", stat);

	if (stat & CC_ATTACHED_BIT)
		val->intval = (bool)(stat & CC_ORIENTATION_BIT) + 1;

	return rc;
}

static const char * const smblib_typec_mode_name[] = {
	[POWER_SUPPLY_TYPEC_NONE]		  = "NONE",
	[POWER_SUPPLY_TYPEC_SOURCE_DEFAULT]	  = "SOURCE_DEFAULT",
	[POWER_SUPPLY_TYPEC_SOURCE_MEDIUM]	  = "SOURCE_MEDIUM",
	[POWER_SUPPLY_TYPEC_SOURCE_HIGH]	  = "SOURCE_HIGH",
	[POWER_SUPPLY_TYPEC_NON_COMPLIANT]	  = "NON_COMPLIANT",
	[POWER_SUPPLY_TYPEC_SINK]		  = "SINK",
	[POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE]   = "SINK_POWERED_CABLE",
	[POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY] = "SINK_DEBUG_ACCESSORY",
	[POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER]   = "SINK_AUDIO_ADAPTER",
	[POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY]   = "POWERED_CABLE_ONLY",
};

static int smblib_get_prop_ufp_mode(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_SNK_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_1 rc=%d\n", rc);
		return POWER_SUPPLY_TYPEC_NONE;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_STATUS_1 = 0x%02x\n", stat);

	switch (stat & DETECTED_SRC_TYPE_MASK) {
	case SNK_RP_STD_BIT:
	case SNK_DAM_500MA_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_DEFAULT;
	case SNK_RP_1P5_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_MEDIUM;
	case SNK_RP_3P0_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_HIGH;
	case SNK_RP_SHORT_BIT:
		return POWER_SUPPLY_TYPEC_NON_COMPLIANT;
	case SNK_DAM_1500MA_BIT:
	case SNK_DAM_3000MA_BIT:
		return POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY;
	default:
		break;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

static int smblib_get_prop_dfp_mode(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	if (chg->lpd_stage == LPD_STAGE_COMMIT)
		return POWER_SUPPLY_TYPEC_NONE;

	rc = smblib_read(chg, TYPE_C_SRC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_SRC_STATUS_REG rc=%d\n",
				rc);
		return POWER_SUPPLY_TYPEC_NONE;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_SRC_STATUS_REG = 0x%02x\n", stat);

	switch (stat & DETECTED_SNK_TYPE_MASK) {
	case AUDIO_ACCESS_RA_RA_BIT:
		chg->is_audio_adapter = true;
		return POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER;
	case SRC_DEBUG_ACCESS_BIT:
		return POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY;
	case SRC_RD_RA_VCONN_BIT:
		return POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE;
	case SRC_RD_OPEN_BIT:
		return POWER_SUPPLY_TYPEC_SINK;
	default:
		break;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

static int smblib_get_prop_typec_mode(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n",
				rc);
		return 0;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_MISC_STATUS_REG = 0x%02x\n", stat);

	if (stat & SNK_SRC_MODE_BIT)
		rc = smblib_get_prop_dfp_mode(chg);
	else
		rc = smblib_get_prop_ufp_mode(chg);

	pr_info("typec mode is %d\n", rc);

	return rc;
}

inline int smblib_get_usb_prop_typec_mode(struct smb_charger *chg,
				union power_supply_propval *val)
{
	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB ||
	    chg->disconnect_pd)
		val->intval = POWER_SUPPLY_TYPEC_NONE;
	else
		val->intval = chg->typec_mode;

	return 0;
}

int smblib_get_prop_typec_power_role(struct smb_charger *chg,
				     union power_supply_propval *val)
{
	int rc = 0;
	u8 ctrl;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		return 0;
	}

	rc = smblib_read(chg, TYPE_C_MODE_CFG_REG, &ctrl);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_MODE_CFG_REG rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_MODE_CFG_REG = 0x%02x\n",
		   ctrl);

	if (ctrl & TYPEC_DISABLE_CMD_BIT) {
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		return rc;
	}

	switch (ctrl & (EN_SRC_ONLY_BIT | EN_SNK_ONLY_BIT)) {
	case 0:
		val->intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		break;
	case EN_SRC_ONLY_BIT:
		val->intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
		break;
	case EN_SNK_ONLY_BIT:
		val->intval = POWER_SUPPLY_TYPEC_PR_SINK;
		break;
	default:
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		smblib_err(chg, "unsupported power role 0x%02lx\n",
			ctrl & (EN_SRC_ONLY_BIT | EN_SNK_ONLY_BIT));
		return -EINVAL;
	}

	chg->power_role = val->intval;
	return rc;
}

static inline bool typec_in_src_mode(struct smb_charger *chg)
{
	return (chg->typec_mode > POWER_SUPPLY_TYPEC_NONE &&
		chg->typec_mode < POWER_SUPPLY_TYPEC_SOURCE_DEFAULT);
}

int smblib_get_prop_typec_select_rp(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc, rp;
	u8 stat;

	if (!typec_in_src_mode(chg))
		return -ENODATA;

	rc = smblib_read(chg, TYPE_C_CURRSRC_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_CURRSRC_CFG_REG rc=%d\n",
				rc);
		return rc;
	}

	switch (stat & TYPEC_SRC_RP_SEL_MASK) {
	case TYPEC_SRC_RP_STD:
		rp = POWER_SUPPLY_TYPEC_SRC_RP_STD;
		break;
	case TYPEC_SRC_RP_1P5A:
		rp = POWER_SUPPLY_TYPEC_SRC_RP_1P5A;
		break;
	case TYPEC_SRC_RP_3A:
	case TYPEC_SRC_RP_3A_DUPLICATE:
		rp = POWER_SUPPLY_TYPEC_SRC_RP_3A;
		break;
	default:
		return -EINVAL;
	}

	val->intval = rp;

	return 0;
}

int smblib_get_prop_usb_current_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	int rc = 0, buck_scale = 1, boost_scale = 1;

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (!is_op_chg_available(chg)) {
			val->intval = 0;
			return -ENODEV;
		}
		rc =  power_supply_get_property(chg->op_chg_psy,
				POWER_SUPPLY_PROP_CURRENT_NOW, &pval);
		if (!rc)
			val->intval = pval.intval;
		else {
			smblib_err(chg, "Couldn't get input current, rc=%d\n", rc);
			val->intval = 0;
		}
	} else {
#endif
		if (chg->iio.usbin_i_chan) {
			rc = iio_read_channel_processed(chg->iio.usbin_i_chan,
					&val->intval);
			if (rc < 0) {
				pr_err("Error in reading USBIN_I channel, rc=%d\n", rc);
				return rc;
			}

			/*
			 * For PM8150B, scaling factor = reciprocal of
			 * 0.2V/A in Buck mode, 0.4V/A in Boost mode.
			 * For PMI632, scaling factor = reciprocal of
			 * 0.4V/A in Buck mode, 0.8V/A in Boost mode.
			 */
			switch (chg->chg_param.smb_version) {
			case PMI632_SUBTYPE:
				buck_scale = 40;
				boost_scale = 80;
				break;
			default:
				buck_scale = 20;
				boost_scale = 40;
				break;
			}

			if (chg->otg_present || smblib_get_prop_dfp_mode(chg) !=
					POWER_SUPPLY_TYPEC_NONE) {
				val->intval = DIV_ROUND_CLOSEST(val->intval * 100,
									boost_scale);
				return rc;
			}

			rc = smblib_get_prop_usb_present(chg, &pval);
			if (rc < 0) {
				smblib_err(chg, "Couldn't get usb present status,rc=%d\n",
					rc);
				return -ENODATA;
			}

			/* If USB is not present, return 0 */
			if (!pval.intval)
				val->intval = 0;
			else
				val->intval = DIV_ROUND_CLOSEST(val->intval * 100,
									buck_scale);
		} else {
			val->intval = 0;
			rc = -ENODATA;
		}
#ifdef OP_SWARP_SUPPORTED
	}
#endif

	return rc;
}

int smblib_get_prop_low_power(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	int rc;
	u8 stat;

	if (chg->sink_src_mode != SRC_MODE)
		return -ENODATA;

	rc = smblib_read(chg, TYPE_C_SRC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_SRC_STATUS_REG rc=%d\n",
				rc);
		return rc;
	}

	val->intval = !(stat & SRC_HIGH_BATT_BIT);

	return 0;
}

int smblib_get_prop_input_current_settled(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	return smblib_get_charge_param(chg, &chg->param.icl_stat, &val->intval);
}

int smblib_get_prop_input_current_max(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	int icl_ua = 0, rc;

	rc = smblib_get_charge_param(chg, &chg->param.usb_icl, &icl_ua);
	if (rc < 0)
		return rc;

	if (is_override_vote_enabled_locked(chg->usb_icl_votable) &&
					icl_ua < USBIN_1000MA) {
		val->intval = USBIN_1000MA;
		return 0;
	}

	return smblib_get_charge_param(chg, &chg->param.icl_stat, &val->intval);
}

int smblib_get_prop_input_voltage_settled(struct smb_charger *chg,
						union power_supply_propval *val)
{
	int rc, pulses;
	int step_uv = HVDCP3_STEP_UV;

	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP_3P5:
		step_uv = HVDCP3P5_STEP_UV;
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		rc = smblib_get_pulse_cnt(chg, &pulses);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
			return 0;
		}
		val->intval = MICRO_5V + step_uv * pulses;
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		val->intval = chg->voltage_min_uv;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_pd_in_hard_reset(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	val->intval = chg->pd_hard_reset;
	return 0;
}

int smblib_get_pe_start(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	val->intval = chg->ok_to_pd;
	return 0;
}

int smblib_get_prop_smb_health(struct smb_charger *chg)
{
	int rc;
	int input_present;
	union power_supply_propval prop = {0, };

	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0)
		return rc;

	if ((input_present == INPUT_NOT_PRESENT) || (!is_cp_available(chg)))
		return POWER_SUPPLY_HEALTH_UNKNOWN;

	rc = power_supply_get_property(chg->cp_psy,
				POWER_SUPPLY_PROP_CP_DIE_TEMP, &prop);
	if (rc < 0)
		return rc;

	if (prop.intval > SMB_TEMP_RST_THRESH)
		return POWER_SUPPLY_HEALTH_OVERHEAT;

	if (prop.intval > SMB_TEMP_REG_H_THRESH)
		return POWER_SUPPLY_HEALTH_HOT;

	if (prop.intval > SMB_TEMP_REG_L_THRESH)
		return POWER_SUPPLY_HEALTH_WARM;

	return POWER_SUPPLY_HEALTH_COOL;
}

int smblib_get_prop_die_health(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	int input_present;

	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0)
		return rc;

	if (input_present == INPUT_NOT_PRESENT)
		return POWER_SUPPLY_HEALTH_UNKNOWN;

	if (chg->wa_flags & SW_THERM_REGULATION_WA) {
		if (chg->die_temp == -ENODATA)
			return POWER_SUPPLY_HEALTH_UNKNOWN;

		if (chg->die_temp > DIE_TEMP_RST_THRESH)
			return POWER_SUPPLY_HEALTH_OVERHEAT;

		if (chg->die_temp > DIE_TEMP_REG_H_THRESH)
			return POWER_SUPPLY_HEALTH_HOT;

		if (chg->die_temp > DIE_TEMP_REG_L_THRESH)
			return POWER_SUPPLY_HEALTH_WARM;

		return POWER_SUPPLY_HEALTH_COOL;
	}

	rc = smblib_read(chg, DIE_TEMP_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DIE_TEMP_STATUS_REG, rc=%d\n",
				rc);
		return POWER_SUPPLY_HEALTH_UNKNOWN;
	}

	if (stat & DIE_TEMP_RST_BIT)
		return POWER_SUPPLY_HEALTH_OVERHEAT;

	if (stat & DIE_TEMP_UB_BIT)
		return POWER_SUPPLY_HEALTH_HOT;

	if (stat & DIE_TEMP_LB_BIT)
		return POWER_SUPPLY_HEALTH_WARM;

	return POWER_SUPPLY_HEALTH_COOL;
}

int smblib_get_die_health(struct smb_charger *chg,
			union power_supply_propval *val)
{
	if (chg->die_health == -EINVAL)
		val->intval = smblib_get_prop_die_health(chg);
	else
		val->intval = chg->die_health;

	return 0;
}

int smblib_get_prop_scope(struct smb_charger *chg,
			union power_supply_propval *val)
{
	int rc;
	union power_supply_propval pval;

	val->intval = POWER_SUPPLY_SCOPE_UNKNOWN;
	rc = smblib_get_prop_usb_present(chg, &pval);
	if (rc < 0)
		return rc;

	val->intval = pval.intval ? POWER_SUPPLY_SCOPE_DEVICE
		: chg->otg_present ? POWER_SUPPLY_SCOPE_SYSTEM
		: POWER_SUPPLY_SCOPE_UNKNOWN;

	return 0;
}

static int smblib_get_typec_connector_temp_status(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	if (chg->connector_health != -EINVAL)
		return chg->connector_health;

	if (chg->wa_flags & SW_THERM_REGULATION_WA) {
		if (chg->connector_temp == -ENODATA)
			return POWER_SUPPLY_HEALTH_UNKNOWN;

		if (chg->connector_temp > CONNECTOR_TEMP_RST_THRESH)
			return POWER_SUPPLY_HEALTH_OVERHEAT;

		if (chg->connector_temp > CONNECTOR_TEMP_REG_H_THRESH)
			return POWER_SUPPLY_HEALTH_HOT;

		if (chg->connector_temp > CONNECTOR_TEMP_REG_L_THRESH)
			return POWER_SUPPLY_HEALTH_WARM;

		return POWER_SUPPLY_HEALTH_COOL;
	}

	rc = smblib_read(chg, CONNECTOR_TEMP_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read CONNECTOR_TEMP_STATUS_REG, rc=%d\n",
				rc);
		return POWER_SUPPLY_HEALTH_UNKNOWN;
	}

	if (stat & CONNECTOR_TEMP_RST_BIT)
		return POWER_SUPPLY_HEALTH_OVERHEAT;

	if (stat & CONNECTOR_TEMP_UB_BIT)
		return POWER_SUPPLY_HEALTH_HOT;

	if (stat & CONNECTOR_TEMP_LB_BIT)
		return POWER_SUPPLY_HEALTH_WARM;

	return POWER_SUPPLY_HEALTH_COOL;
}

int smblib_get_skin_temp_status(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	if (!chg->en_skin_therm_mitigation)
		return POWER_SUPPLY_HEALTH_UNKNOWN;

	rc = smblib_read(chg, SKIN_TEMP_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SKIN_TEMP_STATUS_REG, rc=%d\n",
				rc);
		return POWER_SUPPLY_HEALTH_UNKNOWN;
	}

	if (stat & SKIN_TEMP_RST_BIT)
		return POWER_SUPPLY_HEALTH_OVERHEAT;

	if (stat & SKIN_TEMP_UB_BIT)
		return POWER_SUPPLY_HEALTH_HOT;

	if (stat & SKIN_TEMP_LB_BIT)
		return POWER_SUPPLY_HEALTH_WARM;

	return POWER_SUPPLY_HEALTH_COOL;
}

int smblib_get_prop_connector_health(struct smb_charger *chg)
{
	bool dc_present, usb_present;
	int input_present;
	int rc;

	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0)
		return POWER_SUPPLY_HEALTH_UNKNOWN;

	dc_present = input_present & INPUT_PRESENT_DC;
	usb_present = input_present & INPUT_PRESENT_USB;

	if (usb_present)
		return smblib_get_typec_connector_temp_status(chg);

	/*
	 * In PM8150B, SKIN channel measures Wireless charger receiver
	 * temp, used to regulate DC ICL.
	 */
	if (chg->chg_param.smb_version == PM8150B_SUBTYPE && dc_present)
		return smblib_get_skin_temp_status(chg);

	return POWER_SUPPLY_HEALTH_COOL;
}

#define PD_PANELON_CURRENT_UA		3000000
#define PD_PANELOFF_CURRENT_UA		3000000
#define DCP_PANELOFF_CURRENT_UA		2000000
static int get_rp_based_dcp_current(struct smb_charger *chg, int typec_mode)
{
	int rp_ua;

	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		if (chg->oem_lcd_is_on)
			rp_ua = PD_PANELON_CURRENT_UA;
		else
			rp_ua = TYPEC_HIGH_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
	/* fall through */
	default:
		if (chg->oem_lcd_is_on)
			rp_ua = DCP_CURRENT_UA;
		else
			rp_ua = DCP_PANELOFF_CURRENT_UA;
	}

	return rp_ua;
}

/*******************
 * USB PSY SETTERS *
 * *****************/

int smblib_set_prop_pd_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, icl;

	if (chg->pd_active) {
		icl = get_client_vote(chg->usb_icl_votable, PD_VOTER);
		rc = vote(chg->usb_icl_votable, PD_VOTER, true, val->intval);
		if (val->intval != icl)
			power_supply_changed(chg->usb_psy);
	} else {
		rc = -EPERM;
	}

	if (chg->oem_lcd_is_on)
		rc = vote(chg->usb_icl_votable,
			SW_ICL_MAX_VOTER, true, PD_PANELON_CURRENT_UA);

	return rc;
}

static int smblib_handle_usb_current(struct smb_charger *chg,
					int usb_current)
{
	int rc = 0, rp_ua, typec_mode;
	union power_supply_propval val = {0, };

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_FLOAT) {
		if (usb_current == -ETIMEDOUT) {
			if ((chg->float_cfg & FLOAT_OPTIONS_MASK)
						== FORCE_FLOAT_SDP_CFG_BIT) {
				/*
				 * Confiugure USB500 mode if Float charger is
				 * configured for SDP mode.
				 */
				rc = vote(chg->usb_icl_votable,
					SW_ICL_MAX_VOTER, true, USBIN_500MA);
				if (rc < 0)
					smblib_err(chg,
						"Couldn't set SDP ICL rc=%d\n",
						rc);
				return rc;
			}

			if (chg->connector_type ==
					POWER_SUPPLY_CONNECTOR_TYPEC) {
				/*
				 * Valid FLOAT charger, report the current
				 * based of Rp.
				 */
				typec_mode = smblib_get_prop_typec_mode(chg);
				rp_ua = get_rp_based_dcp_current(chg,
								typec_mode);
				rc = vote(chg->usb_icl_votable,
						SW_ICL_MAX_VOTER, true, rp_ua);
				if (rc < 0)
					return rc;
			} else {
				rc = vote(chg->usb_icl_votable,
					SW_ICL_MAX_VOTER, true, DCP_CURRENT_UA);
				if (rc < 0)
					return rc;
			}
		} else {
			/*
			 * FLOAT charger detected as SDP by USB driver,
			 * charge with the requested current and update the
			 * real_charger_type
			 */
			chg->real_charger_type = POWER_SUPPLY_TYPE_USB;
			rc = vote(chg->usb_icl_votable, USB_PSY_VOTER,
						true, usb_current);
			if (rc < 0)
				return rc;
			rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER,
							false, 0);
			if (rc < 0)
				return rc;
		}
	} else {
		rc = smblib_get_prop_usb_present(chg, &val);
		if (!rc && !val.intval)
			return 0;

		/* if flash is active force 500mA */
		if ((usb_current < SDP_CURRENT_UA) && is_flash_active(chg))
			usb_current = SDP_CURRENT_UA;

		rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, true,
							usb_current);
		if (rc < 0) {
			pr_err("Couldn't vote ICL USB_PSY_VOTER rc=%d\n", rc);
			return rc;
		}

		rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
		if (rc < 0) {
			pr_err("Couldn't remove SW_ICL_MAX vote rc=%d\n", rc);
			return rc;
		}

	}

	return 0;
}

int smblib_set_prop_sdp_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	union power_supply_propval pval;
	int rc = 0;

	pr_err("set usb current_max=%d\n", val->intval);
	if (!chg->pd_active) {
		rc = smblib_get_prop_usb_present(chg, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get usb present rc = %d\n",
						rc);
			return rc;
		}

		/* handle the request only when USB is present */
		if (pval.intval)
			rc = smblib_handle_usb_current(chg, val->intval);
	} else if (chg->system_suspend_supported) {
		if (val->intval <= USBIN_25MA)
			rc = vote(chg->usb_icl_votable,
				PD_SUSPEND_SUPPORTED_VOTER, true, val->intval);
		else
			rc = vote(chg->usb_icl_votable,
				PD_SUSPEND_SUPPORTED_VOTER, false, 0);
	}
	return rc;
}

int smblib_set_prop_boost_current(struct smb_charger *chg,
					const union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher,
				val->intval <= chg->boost_threshold_ua ?
				chg->chg_freq.freq_below_otg_threshold :
				chg->chg_freq.freq_above_otg_threshold);
	if (rc < 0) {
		dev_err(chg->dev, "Error in setting freq_boost rc=%d\n", rc);
		return rc;
	}

	chg->boost_current_ua = val->intval;
	return rc;
}

int smblib_set_prop_usb_voltage_max_limit(struct smb_charger *chg,
					const union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };

	/* Exit if same value is re-configured */
	if (val->intval == chg->usbin_forced_max_uv)
		return 0;

	smblib_get_prop_usb_voltage_max_design(chg, &pval);

	if (val->intval >= MICRO_5V && val->intval <= pval.intval) {
		chg->usbin_forced_max_uv = val->intval;
		smblib_dbg(chg, PR_MISC, "Max VBUS limit changed to: %d\n",
				val->intval);
	} else if (chg->usbin_forced_max_uv) {
		chg->usbin_forced_max_uv = 0;
	} else {
		return 0;
	}

	power_supply_changed(chg->usb_psy);

	return 0;
}

void smblib_typec_irq_config(struct smb_charger *chg, bool en)
{
	if (en == chg->typec_irq_en)
		return;

	if (en) {
		enable_irq(
			chg->irq_info[TYPEC_ATTACH_DETACH_IRQ].irq);
		enable_irq(
			chg->irq_info[TYPEC_CC_STATE_CHANGE_IRQ].irq);
		enable_irq(
			chg->irq_info[TYPEC_OR_RID_DETECTION_CHANGE_IRQ].irq);
	} else {
		disable_irq_nosync(
			chg->irq_info[TYPEC_ATTACH_DETACH_IRQ].irq);
		disable_irq_nosync(
			chg->irq_info[TYPEC_CC_STATE_CHANGE_IRQ].irq);
		disable_irq_nosync(
			chg->irq_info[TYPEC_OR_RID_DETECTION_CHANGE_IRQ].irq);
	}

	chg->typec_irq_en = en;
}

int smblib_set_prop_reset_rd(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;
	u8 stat;

	if (!(chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH)) {
		smblib_err(chg, "Not in source high mode, do not reset rd!\n");
		return -EINVAL;
	}

	rc = smblib_masked_write(chg, TYPE_C_CC_CURRSRC_CONTROL_REG,
			TYPE_C_CC_CURRSRC_CONTROL_MASK,
			val->intval);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to TYPE_C_CC_CURRSRC_CONTROL_REG rc=%d\n",
				rc);

	rc = smblib_read(chg, TYPE_C_CC_CURRSRC_CONTROL_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_CC_CURRSRC_CONTROL_REG rc=%d\n",
				rc);
		return rc;
	}
	smblib_err(chg, "0x155C value is 0x%04x\n", stat);
	schedule_delayed_work(&chg->reset_rd_work, msecs_to_jiffies(500));

	return rc;
}

static void op_reset_rd_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						reset_rd_work.work);
	int rc;
	u8 stat = 0;

	rc = smblib_masked_write(chg, TYPE_C_CC_CURRSRC_CONTROL_REG,
			TYPE_C_CC_CURRSRC_CONTROL_MASK,
			0);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to TYPE_C_CC_CURRSRC_CONTROL_REG rc=%d\n",
				rc);

	rc = smblib_read(chg, TYPE_C_CC_CURRSRC_CONTROL_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_CC_CURRSRC_CONTROL_REG rc=%d\n",
				rc);
	}
	smblib_err(chg, "0x155C value is 0x%04x\n", stat);
}

static void op_reset_chg_disable_flag_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						disable_chg_flag_work.work);

	chg->ignore_irq_flag = false;
	pr_info("ignore_irq_flag:false");
}

static void op_icon_debounce_unplug_check_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						icon_debounce_unplug_check.work);

	chg->icon_debounce = false;
	power_supply_changed(chg->usb_psy);
	pr_info("icon_debounce: false");
}

#define PR_LOCK_TIMEOUT_MS	1000
int smblib_set_prop_typec_power_role(struct smb_charger *chg,
				     const union power_supply_propval *val)
{
	int rc = 0;
	u8 power_role;
	enum power_supply_typec_mode typec_mode;
	bool snk_attached = false, src_attached = false, is_pr_lock = false;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		return 0;

	smblib_dbg(chg, PR_MISC, "power role change: %d --> %d!",
			chg->power_role, val->intval);

	if (chg->power_role == val->intval) {
		smblib_dbg(chg, PR_MISC, "power role already in %d, ignore!",
				chg->power_role);
		return 0;
	}

	typec_mode = smblib_get_prop_typec_mode(chg);
	if (typec_mode >= POWER_SUPPLY_TYPEC_SINK &&
			typec_mode <= POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER)
		snk_attached = true;
	else if (typec_mode >= POWER_SUPPLY_TYPEC_SOURCE_DEFAULT &&
			typec_mode <= POWER_SUPPLY_TYPEC_SOURCE_HIGH)
		src_attached = true;

	/*
	 * If current power role is in DRP, and type-c is already in the
	 * mode (source or sink) that's being requested, it means this is
	 * a power role locking request from USBPD driver. Disable type-c
	 * related interrupts for locking power role to avoid the redundant
	 * notifications.
	 */
	if ((chg->power_role == POWER_SUPPLY_TYPEC_PR_DUAL) &&
		((src_attached && val->intval == POWER_SUPPLY_TYPEC_PR_SINK) ||
		(snk_attached && val->intval == POWER_SUPPLY_TYPEC_PR_SOURCE)))
		is_pr_lock = true;

	smblib_dbg(chg, PR_MISC, "snk_attached = %d, src_attached = %d, is_pr_lock = %d\n",
			snk_attached, src_attached, is_pr_lock);
	cancel_delayed_work(&chg->pr_lock_clear_work);
	spin_lock(&chg->typec_pr_lock);
	if (!chg->pr_lock_in_progress && is_pr_lock) {
		smblib_dbg(chg, PR_MISC, "disable type-c interrupts for power role locking\n");
		smblib_typec_irq_config(chg, false);
		schedule_delayed_work(&chg->pr_lock_clear_work,
					msecs_to_jiffies(PR_LOCK_TIMEOUT_MS));
	} else if (chg->pr_lock_in_progress && !is_pr_lock) {
		smblib_dbg(chg, PR_MISC, "restore type-c interrupts after exit power role locking\n");
		smblib_typec_irq_config(chg, true);
	}

	chg->pr_lock_in_progress = is_pr_lock;
	spin_unlock(&chg->typec_pr_lock);

	switch (val->intval) {
	case POWER_SUPPLY_TYPEC_PR_NONE:
		power_role = TYPEC_DISABLE_CMD_BIT;
		break;
	case POWER_SUPPLY_TYPEC_PR_DUAL:
		power_role = chg->typec_try_mode;
		break;
	case POWER_SUPPLY_TYPEC_PR_SINK:
		power_role = EN_SNK_ONLY_BIT;
		break;
	case POWER_SUPPLY_TYPEC_PR_SOURCE:
		power_role = EN_SRC_ONLY_BIT;
		break;
	default:
		smblib_err(chg, "power role %d not supported\n", val->intval);
		return -EINVAL;
	}
	if (!chg->otg_switch)
		power_role = EN_SNK_ONLY_BIT;

	rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
				TYPEC_POWER_ROLE_CMD_MASK | TYPEC_TRY_MODE_MASK,
				power_role);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			power_role, rc);
		return rc;
	}

	chg->power_role = val->intval;
	return rc;
}

int smblib_set_prop_typec_select_rp(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;

	if (!typec_in_src_mode(chg)) {
		smblib_err(chg, "Couldn't set curr src: not in SRC mode\n");
		return -EINVAL;
	}

	if (val->intval < TYPEC_SRC_RP_MAX_ELEMENTS) {
		rc = smblib_masked_write(chg, TYPE_C_CURRSRC_CFG_REG,
				TYPEC_SRC_RP_SEL_MASK,
				val->intval);
		if (rc < 0)
			smblib_err(chg, "Couldn't write to TYPE_C_CURRSRC_CFG rc=%d\n",
					rc);
		return rc;
	}

	return -EINVAL;
}

int smblib_set_prop_pd_voltage_min(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, min_uv;

	min_uv = min(val->intval, chg->voltage_max_uv);
	if (chg->voltage_min_uv == min_uv)
		return 0;

	rc = smblib_set_usb_pd_allowed_voltage(chg, min_uv,
					       chg->voltage_max_uv);
	if (rc < 0) {
		smblib_err(chg, "invalid min voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	chg->voltage_min_uv = min_uv;
	power_supply_changed(chg->usb_main_psy);

	return rc;
}

int smblib_set_prop_pd_voltage_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, max_uv;

	max_uv = max(val->intval, chg->voltage_min_uv);
	if (chg->voltage_max_uv == max_uv)
		return 0;

	rc = smblib_set_usb_pd_fsw(chg, max_uv);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set FSW for voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	rc = smblib_set_usb_pd_allowed_voltage(chg, chg->voltage_min_uv,
					       max_uv);
	if (rc < 0) {
		smblib_err(chg, "invalid max voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	chg->voltage_max_uv = max_uv;
	power_supply_changed(chg->usb_main_psy);

	return rc;
}

int smblib_set_prop_pd_active(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);

	int rc = 0;
	int sec_charger, typec_mode;
	int temp_region = 0;

	pr_debug("pd_active=%d, val=%d\n", chg->pd_active, val->intval);
	/*
	 * Ignore repetitive notification while PD is active, which
	 * is caused by hard reset.
	 */
	if (chg->pd_active && chg->pd_active == val->intval) {
		op_usbpd_send_svdm(USBPD_SID, USBPD_SVDM_DISCOVER_IDENTITY,
			SVDM_CMD_TYPE_INITIATOR, 0, NULL, 0);

		cancel_delayed_work(&chg->pd_status_check_work);
		schedule_delayed_work(&chg->pd_status_check_work,
				msecs_to_jiffies(2000));
		return 0;
	}

	chg->pd_active = val->intval;

	if (chg->swarp_online && chg->switch_on_fastchg)
		return 0;

	smblib_apsd_enable(chg, !chg->pd_active);

	update_sw_icl_max(chg, apsd->pst);

	if (chg->pd_active) {

		if (chg->swarp_supported) {
			schedule_work(&chg->op_pd_active_work);
		} else {
			smblib_set_prop_charge_parameter_set(chg);
			temp_region = op_battery_temp_region_get(chg);
			if (temp_region != BATT_TEMP_COLD
				&& temp_region != BATT_TEMP_HOT
				&& chg->typec_mode != POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE) {
				op_charging_en(chg, true);
			}
		}
		vote(chg->limited_irq_disable_votable, CHARGER_TYPE_VOTER,
				false, 0);
		vote(chg->hdc_irq_disable_votable, CHARGER_TYPE_VOTER,
				false, 0);

		if (!chg->wireless_present)
			vote(chg->usb_icl_votable, PD_VOTER, true, USBIN_100MA);
		vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);

		vote(chg->fcc_votable, FCC_STEPPER_VOTER, false, 0);
		vote(chg->fcc_votable, HW_LIMIT_VOTER, false, 0);
		vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
		vote(chg->fcc_votable, BATT_PROFILE_VOTER, false, 0);
		set_property_on_fg(chg,
			POWER_SUPPLY_PROP_SET_ALLOW_READ_EXTERN_FG_IIC, true);

		if (chg->pd_active == POWER_SUPPLY_PD_PPS_ACTIVE
						&& chg->sec_cp_present) {
			rc = smblib_select_sec_charger(chg,
						POWER_SUPPLY_CHARGER_SEC_CP,
						POWER_SUPPLY_CP_PPS, false);
			if (rc < 0)
				dev_err(chg->dev, "Couldn't enable secondary charger rc=%d\n",
					rc);
		}

		if (!chg->swarp_online) {
			op_usbpd_send_svdm(USBPD_SID, USBPD_SVDM_DISCOVER_IDENTITY,
				SVDM_CMD_TYPE_INITIATOR, 0, NULL, 0);
			pr_info("PPS type, Discover SVID!\n");
		}
		schedule_delayed_work(&chg->pd_status_check_work,
				msecs_to_jiffies(2000));
	} else {
		vote(chg->usb_icl_votable, PD_VOTER, false, 0);
		vote(chg->limited_irq_disable_votable, CHARGER_TYPE_VOTER,
				true, 0);
		vote(chg->hdc_irq_disable_votable, CHARGER_TYPE_VOTER,
				true, 0);

		sec_charger = chg->sec_pl_present ?
						POWER_SUPPLY_CHARGER_SEC_PL :
						POWER_SUPPLY_CHARGER_SEC_NONE;
		rc = smblib_select_sec_charger(chg, sec_charger,
						POWER_SUPPLY_CP_NONE, false);
		if (rc < 0)
			dev_err(chg->dev,
				"Couldn't enable secondary charger rc=%d\n",
					rc);
	}

	smblib_usb_pd_adapter_allowance_override(chg,
			!!chg->pd_active ? FORCE_5V : FORCE_NULL);
	smblib_update_usb_type(chg);

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB &&
			!chg->ok_to_pd) {
		typec_mode = smblib_get_prop_typec_mode(chg);
		if (typec_rp_med_high(chg, typec_mode))
			vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	}

	power_supply_changed(chg->usb_psy);
	return rc;
}

int smblib_set_prop_ship_mode(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;

	smblib_dbg(chg, PR_MISC, "Set ship mode: %d!!\n", !!val->intval);

	rc = smblib_masked_write(chg, SHIP_MODE_REG, SHIP_MODE_EN_BIT,
			!!val->intval ? SHIP_MODE_EN_BIT : 0);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't %s ship mode, rc=%d\n",
				!!val->intval ? "enable" : "disable", rc);

	return rc;
}

int smblib_set_prop_pd_in_hard_reset(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc = 0;

	if (chg->pd_hard_reset == val->intval)
		return rc;

	chg->pd_hard_reset = val->intval;
	rc = smblib_masked_write(chg, TYPE_C_EXIT_STATE_CFG_REG,
			EXIT_SNK_BASED_ON_CC_BIT,
			(chg->pd_hard_reset) ? EXIT_SNK_BASED_ON_CC_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set EXIT_SNK_BASED_ON_CC rc=%d\n",
				rc);

	return rc;
}

#define JEITA_SOFT			0
#define JEITA_HARD			1
static int smblib_update_jeita(struct smb_charger *chg, u32 *thresholds,
								int type)
{
	int rc;
	u16 temp, base;

	base = CHGR_JEITA_THRESHOLD_BASE_REG(type);

	temp = thresholds[1] & 0xFFFF;
	temp = ((temp & 0xFF00) >> 8) | ((temp & 0xFF) << 8);
	rc = smblib_batch_write(chg, base, (u8 *)&temp, 2);
	if (rc < 0) {
		smblib_err(chg,
			"Couldn't configure Jeita %s hot threshold rc=%d\n",
			(type == JEITA_SOFT) ? "Soft" : "Hard", rc);
		return rc;
	}

	temp = thresholds[0] & 0xFFFF;
	temp = ((temp & 0xFF00) >> 8) | ((temp & 0xFF) << 8);
	rc = smblib_batch_write(chg, base + 2, (u8 *)&temp, 2);
	if (rc < 0) {
		smblib_err(chg,
			"Couldn't configure Jeita %s cold threshold rc=%d\n",
			(type == JEITA_SOFT) ? "Soft" : "Hard", rc);
		return rc;
	}

	smblib_dbg(chg, PR_MISC, "%s Jeita threshold configured\n",
				(type == JEITA_SOFT) ? "Soft" : "Hard");

	return 0;
}

static int smblib_charge_inhibit_en(struct smb_charger *chg, bool enable)
{
	int rc;

	rc = smblib_masked_write(chg, CHGR_CFG2_REG,
					CHARGER_INHIBIT_BIT,
					enable ? CHARGER_INHIBIT_BIT : 0);
	return rc;
}

static int smblib_soft_jeita_arb_wa(struct smb_charger *chg)
{
	union power_supply_propval pval;
	int rc = 0;
	bool soft_jeita;

	rc = smblib_get_prop_batt_health(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get battery health rc=%d\n", rc);
		return rc;
	}

	/* Do nothing on entering hard JEITA condition */
	if (pval.intval == POWER_SUPPLY_HEALTH_COLD ||
		pval.intval == POWER_SUPPLY_HEALTH_HOT)
		return 0;

	if (chg->jeita_soft_fcc[0] < 0 || chg->jeita_soft_fcc[1] < 0 ||
		chg->jeita_soft_fv[0] < 0 || chg->jeita_soft_fv[1] < 0)
		return 0;

	soft_jeita = (pval.intval == POWER_SUPPLY_HEALTH_COOL) ||
			(pval.intval == POWER_SUPPLY_HEALTH_WARM);

	/* Do nothing on entering soft JEITA from hard JEITA */
	if (chg->jeita_arb_flag && soft_jeita)
		return 0;

	/* Do nothing, initial to health condition */
	if (!chg->jeita_arb_flag && !soft_jeita)
		return 0;

	/* Entering soft JEITA from normal state */
	if (!chg->jeita_arb_flag && soft_jeita) {
		vote(chg->chg_disable_votable, JEITA_ARB_VOTER, true, 0);

		rc = smblib_charge_inhibit_en(chg, true);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable charge inhibit rc=%d\n",
					rc);

		rc = smblib_update_jeita(chg, chg->jeita_soft_hys_thlds,
					JEITA_SOFT);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't configure Jeita soft threshold rc=%d\n",
				rc);

		if (pval.intval == POWER_SUPPLY_HEALTH_COOL) {
			vote(chg->fcc_votable, JEITA_ARB_VOTER, true,
						chg->jeita_soft_fcc[0]);
			vote(chg->fv_votable, JEITA_ARB_VOTER, true,
						chg->jeita_soft_fv[0]);
		} else {
			vote(chg->fcc_votable, JEITA_ARB_VOTER, true,
						chg->jeita_soft_fcc[1]);
			vote(chg->fv_votable, JEITA_ARB_VOTER, true,
						chg->jeita_soft_fv[1]);
		}

		vote(chg->chg_disable_votable, JEITA_ARB_VOTER, false, 0);
		chg->jeita_arb_flag = true;
	} else if (chg->jeita_arb_flag && !soft_jeita) {
		/* Exit to health state from soft JEITA */

		vote(chg->chg_disable_votable, JEITA_ARB_VOTER, true, 0);

		rc = smblib_charge_inhibit_en(chg, false);
		if (rc < 0)
			smblib_err(chg, "Couldn't disable charge inhibit rc=%d\n",
					rc);

		rc = smblib_update_jeita(chg, chg->jeita_soft_thlds,
							JEITA_SOFT);
		if (rc < 0)
			smblib_err(chg, "Couldn't configure Jeita soft threshold rc=%d\n",
				rc);

		vote(chg->fcc_votable, JEITA_ARB_VOTER, false, 0);
		vote(chg->fv_votable, JEITA_ARB_VOTER, false, 0);
		vote(chg->chg_disable_votable, JEITA_ARB_VOTER, false, 0);
		chg->jeita_arb_flag = false;
	}

	smblib_dbg(chg, PR_MISC, "JEITA ARB status %d, soft JEITA status %d\n",
			chg->jeita_arb_flag, soft_jeita);
	return rc;
}

/************************
 * USB MAIN PSY GETTERS *
 ************************/
int smblib_get_prop_fcc_delta(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc, jeita_cc_delta_ua = 0;

	if (chg->sw_jeita_enabled) {
		val->intval = 0;
		return 0;
	}

	rc = smblib_get_jeita_cc_delta(chg, &jeita_cc_delta_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get jeita cc delta rc=%d\n", rc);
		jeita_cc_delta_ua = 0;
	}

	val->intval = jeita_cc_delta_ua;
	return 0;
}

/************************
 * USB MAIN PSY SETTERS *
 ************************/
int smblib_get_charge_current(struct smb_charger *chg,
				int *total_current_ua)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);
	union power_supply_propval val = {0, };
	int rc = 0, typec_source_rd, current_ua;
	bool non_compliant;
	u8 stat;

	if (chg->pd_active) {
		*total_current_ua =
			get_client_vote_locked(chg->usb_icl_votable, PD_VOTER);
		return rc;
	}

	rc = smblib_read(chg, LEGACY_CABLE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_5 rc=%d\n", rc);
		return rc;
	}
	non_compliant = stat & TYPEC_NONCOMP_LEGACY_CABLE_STATUS_BIT;

	/* get settled ICL */
	rc = smblib_get_prop_input_current_settled(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get settled ICL rc=%d\n", rc);
		return rc;
	}

	typec_source_rd = smblib_get_prop_ufp_mode(chg);

	/* QC 2.0/3.0 adapter */
	if (apsd_result->bit & (QC_3P0_BIT | QC_2P0_BIT)) {
		*total_current_ua = HVDCP_CURRENT_UA;
		return 0;
	}

	if (non_compliant && !chg->typec_legacy_use_rp_icl) {
		switch (apsd_result->bit) {
		case CDP_CHARGER_BIT:
			current_ua = CDP_CURRENT_UA;
			break;
		case DCP_CHARGER_BIT:
		case OCP_CHARGER_BIT:
		case FLOAT_CHARGER_BIT:
			current_ua = DCP_CURRENT_UA;
			break;
		default:
			current_ua = 0;
			break;
		}

		*total_current_ua = max(current_ua, val.intval);
		return 0;
	}

	switch (typec_source_rd) {
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		switch (apsd_result->bit) {
		case CDP_CHARGER_BIT:
			current_ua = CDP_CURRENT_UA;
			break;
		case DCP_CHARGER_BIT:
		case OCP_CHARGER_BIT:
		case FLOAT_CHARGER_BIT:
			current_ua = chg->default_icl_ua;
			break;
		default:
			current_ua = 0;
			break;
		}
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		current_ua = TYPEC_MEDIUM_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		current_ua = TYPEC_HIGH_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
	case POWER_SUPPLY_TYPEC_NONE:
	default:
		current_ua = 0;
		break;
	}

	*total_current_ua = max(current_ua, val.intval);
	return 0;
}

#define IADP_OVERHEAT_UA	500000
int smblib_set_prop_thermal_overheat(struct smb_charger *chg,
						int therm_overheat)
{
	int icl_ua = 0;

	if (chg->thermal_overheat == !!therm_overheat)
		return 0;

	/* Configure ICL to 500mA in case system health is Overheat */
	if (therm_overheat)
		icl_ua = IADP_OVERHEAT_UA;

	if (!chg->cp_disable_votable)
		chg->cp_disable_votable = find_votable("CP_DISABLE");

	if (chg->cp_disable_votable) {
		vote(chg->cp_disable_votable, OVERHEAT_LIMIT_VOTER,
							therm_overheat, 0);
		vote(chg->usb_icl_votable, OVERHEAT_LIMIT_VOTER,
							therm_overheat, icl_ua);
	}

	chg->thermal_overheat = !!therm_overheat;
	return 0;
}

/**********************
 * INTERRUPT HANDLERS *
 **********************/

irqreturn_t default_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	return IRQ_HANDLED;
}

irqreturn_t smb_en_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc, input_present;

	if (!chg->cp_disable_votable) {
		chg->cp_disable_votable = find_votable("CP_DISABLE");
		if (!chg->cp_disable_votable)
			return IRQ_HANDLED;
	}

	if (chg->pd_hard_reset) {
		vote(chg->cp_disable_votable, BOOST_BACK_VOTER, true, 0);
		return IRQ_HANDLED;
	}

	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0) {
		pr_err("Couldn't get usb presence status rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	if (input_present) {
		/*
		 * Add some delay to enable SMB1390 switcher after SMB_EN
		 * pin goes high
		 */
		usleep_range(1000, 1100);
		vote(chg->cp_disable_votable, BOOST_BACK_VOTER, false, 0);
	}

	return IRQ_HANDLED;
}

irqreturn_t sdam_sts_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	mutex_lock(&chg->irq_status_lock);
	chg->irq_status |= PULSE_SKIP_IRQ_BIT;
	mutex_unlock(&chg->irq_status_lock);

	power_supply_changed(chg->usb_main_psy);

	return IRQ_HANDLED;
}

#define CHG_TERM_WA_ENTRY_DELAY_MS		300000		/* 5 min */
#define CHG_TERM_WA_EXIT_DELAY_MS		60000		/* 1 min */
static void smblib_eval_chg_termination(struct smb_charger *chg, u8 batt_status)
{
	union power_supply_propval pval = {0, };
	int rc = 0;

	rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_REAL_CAPACITY, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SOC value, rc=%d\n", rc);
		return;
	}

	/*
	 * Post charge termination, switch to BSM mode triggers the risk of
	 * over charging as BATFET opening may take some time post the necessity
	 * of staying in supplemental mode, leading to unintended charging of
	 * battery. Trigger the charge termination WA once charging is completed
	 * to prevent overcharing.
	 */
	if ((batt_status == TERMINATE_CHARGE) && (pval.intval == 100) && (!chg->is_aging_test)) {
		chg->cc_soc_ref = 0;
		chg->last_cc_soc = 0;
		chg->term_vbat_uv = 0;
		alarm_start_relative(&chg->chg_termination_alarm,
				ms_to_ktime(CHG_TERM_WA_ENTRY_DELAY_MS));
	} else if (pval.intval < 100) {
		/*
		 * Reset CC_SOC reference value for charge termination WA once
		 * we exit the TERMINATE_CHARGE state and soc drops below 100%
		 */
		chg->cc_soc_ref = 0;
		chg->last_cc_soc = 0;
		chg->term_vbat_uv = 0;
	}
}

irqreturn_t chg_state_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;
	if (stat == TERMINATE_CHARGE) {
		chg->chg_done = true;
		pr_info("TERMINATE_CHARGE: chg_done: CAP=%d (Q:%d), VBAT=%d (Q:%d), IBAT=%d (Q:%d), BAT_TEMP=%d\n",
			get_prop_batt_capacity(chg),
			get_prop_fg_capacity(chg),
			get_prop_batt_voltage_now(chg) / 1000,
			get_prop_fg_voltage_now(chg) / 1000,
			get_prop_batt_current_now(chg) / 1000,
			get_prop_fg_current_now(chg) / 1000,
			get_prop_batt_temp(chg));
		op_charging_en(chg, false);
	}

	if (chg->wa_flags & CHG_TERMINATION_WA)
		smblib_eval_chg_termination(chg, stat);

	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

irqreturn_t batt_temp_changed_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if (chg->jeita_configured != JEITA_CFG_COMPLETE)
		return IRQ_HANDLED;

	rc = smblib_soft_jeita_arb_wa(chg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't fix soft jeita arb rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

	return IRQ_HANDLED;
}

irqreturn_t batt_psy_changed_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

#define AICL_STEP_MV		200
#define MAX_AICL_THRESHOLD_MV	4800
irqreturn_t usbin_uv_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	struct storm_watch *wdata;
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);
	int rc;
	u8 stat = 0, max_pulses = 0;

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK ||
			chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE) {
			pr_err("%s, Sink detected\n", __func__);
			return IRQ_HANDLED;
		}
	}
#endif
	chg->dash_on = get_prop_fast_chg_started(chg);
	if (chg->dash_on) {
		pr_err("return directly because dash is online\n");
		return IRQ_HANDLED;
	}
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if ((chg->wa_flags & WEAK_ADAPTER_WA)
			&& is_storming(&irq_data->storm_data)) {

		if (chg->aicl_max_reached) {
			smblib_dbg(chg, PR_MISC,
					"USBIN_UV storm at max AICL threshold\n");
			return IRQ_HANDLED;
		}

		smblib_dbg(chg, PR_MISC, "USBIN_UV storm at threshold %d\n",
				chg->aicl_5v_threshold_mv);

		/* suspend USBIN before updating AICL threshold */
		vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER, true, 0);

		/* delay for VASHDN deglitch */
		msleep(20);

		if (chg->aicl_5v_threshold_mv > MAX_AICL_THRESHOLD_MV) {
			/* reached max AICL threshold */
			chg->aicl_max_reached = true;
			goto unsuspend_input;
		}

		/* Increase AICL threshold by 200mV */
		rc = smblib_set_charge_param(chg, &chg->param.aicl_5v_threshold,
				chg->aicl_5v_threshold_mv + AICL_STEP_MV);
		if (rc < 0)
			dev_err(chg->dev,
				"Error in setting AICL threshold rc=%d\n", rc);
		else
			chg->aicl_5v_threshold_mv += AICL_STEP_MV;

		rc = smblib_set_charge_param(chg,
				&chg->param.aicl_cont_threshold,
				chg->aicl_cont_threshold_mv + AICL_STEP_MV);
		if (rc < 0)
			dev_err(chg->dev,
				"Error in setting AICL threshold rc=%d\n", rc);
		else
			chg->aicl_cont_threshold_mv += AICL_STEP_MV;

unsuspend_input:
		/* Force torch in boost mode to ensure it works with low ICL */
		if (chg->chg_param.smb_version == PMI632_SUBTYPE)
			schgm_flash_torch_priority(chg, TORCH_BOOST_MODE);

		if (chg->aicl_max_reached) {
			smblib_dbg(chg, PR_MISC,
				"Reached max AICL threshold resctricting ICL to 100mA\n");
			vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER,
					true, USBIN_100MA);
			smblib_run_aicl(chg, RESTART_AICL);
		} else {
			smblib_run_aicl(chg, RESTART_AICL);
			vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER,
					false, 0);
		}

		wdata = &chg->irq_info[USBIN_UV_IRQ].irq_data->storm_data;
		reset_storm_count(wdata);
	}

	if (!chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data)
		return IRQ_HANDLED;

	wdata = &chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data->storm_data;
	reset_storm_count(wdata);
	smblib_err(chg, "DEBUG: RESET STORM COUNT FOR POWER_OK\n");

	/* Workaround for non-QC2.0-compliant chargers follows */
	if (!chg->qc2_unsupported_voltage &&
			apsd->pst == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read CHANGE_STATUS_REG rc=%d\n", rc);

		if (stat & QC_5V_BIT)
			return IRQ_HANDLED;

		rc = smblib_read(chg, HVDCP_PULSE_COUNT_MAX_REG, &max_pulses);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read QC2 max pulses rc=%d\n", rc);

		chg->qc2_uv_count++;
		if (chg->qc2_uv_count < 2)
			return IRQ_HANDLED;

		chg->qc2_max_pulses = (max_pulses &
				HVDCP_PULSE_COUNT_MAX_QC2_MASK);

		if (stat & QC_12V_BIT) {
			chg->qc2_unsupported_voltage = QC2_NON_COMPLIANT_12V;
			chg->qc2_uv_count = 0;
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_9V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 9V rc=%d\n",
						rc);

		} else if (stat & QC_9V_BIT) {
			chg->qc2_unsupported_voltage = QC2_NON_COMPLIANT_9V;
			chg->qc2_uv_count = 0;
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_5V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 5V rc=%d\n",
						rc);

		}

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				0);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn off SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		smblib_rerun_apsd(chg);
	}

	return IRQ_HANDLED;
}

char dump_val[2048];
static inline void op_dump_reg(struct smb_charger *chip,
	u16 addr_start, u16 addr_end)
{
	u8 reg = 0;
	u16 addr;
	char reg_val[19];
	int rc;

	memset(dump_val, 0, sizeof(dump_val));
	for (addr = addr_start; addr <= addr_end; addr++) {
		memset(reg_val, 0, sizeof(reg_val));
		rc = smblib_read(chip, addr, &reg);
		if (rc < 0)
			smblib_err(chip, "op_dump_reg  read error rc=%d\n",
					rc);
		scnprintf(reg_val,
			sizeof(reg_val), "%x=%0x;", addr, reg);
		strlcat(dump_val, reg_val, sizeof(dump_val));
	}
	 pr_info("%s\n", dump_val);
}

static void op_dump_regs(struct smb_charger *chip)
{
	u16 addr, count;

	count = 0x80;
	for (addr = 0x1000; addr <= 0x1700; addr += count)
		op_dump_reg(chip, addr, (addr + count));
}


#define USB_WEAK_INPUT_UA	1400000
#define ICL_CHANGE_DELAY_MS	1000
irqreturn_t icl_change_irq_handler(int irq, void *data)
{
	u8 stat;
	int rc, settled_ua, delay = ICL_CHANGE_DELAY_MS;
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	if (chg->mode == PARALLEL_MASTER) {
		/*
		 * Ignore if change in ICL is due to DIE temp mitigation.
		 * This is to prevent any further ICL split.
		 */
		if (chg->hw_die_temp_mitigation) {
			rc = smblib_read(chg, DIE_TEMP_STATUS_REG, &stat);
			if (rc < 0) {
				smblib_err(chg,
					"Couldn't read DIE_TEMP rc=%d\n", rc);
				return IRQ_HANDLED;
			}
			if (stat & (DIE_TEMP_UB_BIT | DIE_TEMP_LB_BIT)) {
				smblib_dbg(chg, PR_PARALLEL,
					"skip ICL change DIE_TEMP %x\n", stat);
				return IRQ_HANDLED;
			}
		}

		rc = smblib_read(chg, AICL_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n",
					rc);
			return IRQ_HANDLED;
		}

		rc = smblib_get_charge_param(chg, &chg->param.icl_stat,
					&settled_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
			return IRQ_HANDLED;
		}

		/* If AICL settled then schedule work now */
		if (settled_ua == get_effective_result(chg->usb_icl_votable))
			delay = 0;

		cancel_delayed_work_sync(&chg->icl_change_work);
		schedule_delayed_work(&chg->icl_change_work,
						msecs_to_jiffies(delay));
	}

	return IRQ_HANDLED;
}

static int smblib_role_switch_failure(struct smb_charger *chg)
{
	int rc = 0;
	union power_supply_propval pval = {0, };

	if (!chg->use_extcon)
		return 0;

	rc = smblib_get_prop_usb_present(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb presence status rc=%d\n",
				rc);
		return rc;
	}

	/*
	 * When role switch fails notify the
	 * current charger state to usb driver.
	 */
	if (pval.intval) {
		smblib_dbg(chg, PR_MISC, " Role reversal failed, notifying device mode to usb driver.\n");
		smblib_notify_device_mode(chg, true);
	}

	return rc;
}

static int typec_partner_register(struct smb_charger *chg)
{
	int typec_mode, rc = 0;

	mutex_lock(&chg->typec_lock);

	if (!chg->typec_port || chg->pr_swap_in_progress)
		goto unlock;

	if (!chg->typec_partner) {
		if (chg->sink_src_mode == AUDIO_ACCESS_MODE)
			chg->typec_partner_desc.accessory =
					TYPEC_ACCESSORY_AUDIO;
		else
			chg->typec_partner_desc.accessory =
					TYPEC_ACCESSORY_NONE;

		chg->typec_partner = typec_register_partner(chg->typec_port,
				&chg->typec_partner_desc);
		if (IS_ERR(chg->typec_partner)) {
			rc = PTR_ERR(chg->typec_partner);
			pr_err("failed to register typec_partner rc=%d\n", rc);
			goto unlock;
		}
	}

	typec_mode = smblib_get_prop_typec_mode(chg);

	if (typec_mode >= POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
			|| typec_mode == POWER_SUPPLY_TYPEC_NONE) {

		if (chg->typec_role_swap_failed) {
			rc = smblib_role_switch_failure(chg);
			if (rc < 0)
				smblib_err(chg, "Failed to role switch rc=%d\n",
					rc);
			chg->typec_role_swap_failed = false;
		}

		typec_set_data_role(chg->typec_port, TYPEC_DEVICE);
		typec_set_pwr_role(chg->typec_port, TYPEC_SINK);
	} else {
		typec_set_data_role(chg->typec_port, TYPEC_HOST);
		typec_set_pwr_role(chg->typec_port, TYPEC_SOURCE);
	}

unlock:
	mutex_unlock(&chg->typec_lock);
	return rc;
}

static void typec_partner_unregister(struct smb_charger *chg)
{
	mutex_lock(&chg->typec_lock);

	if (!chg->typec_port)
		goto unlock;

	if (chg->typec_partner && !chg->pr_swap_in_progress) {
		smblib_dbg(chg, PR_MISC, "Un-registering typeC partner\n");
		typec_unregister_partner(chg->typec_partner);
		chg->typec_partner = NULL;
	}

unlock:
	mutex_unlock(&chg->typec_lock);
}

static void smblib_micro_usb_plugin(struct smb_charger *chg, bool vbus_rising)
{
	int rc = 0;

	if (!vbus_rising) {
		smblib_update_usb_type(chg);
		smblib_notify_device_mode(chg, false);
		smblib_uusb_removal(chg);
		typec_partner_unregister(chg);
	} else {
		rc = typec_partner_register(chg);
		if (rc < 0)
			smblib_err(chg, "Couldn't register partner rc =%d\n",
					rc);
	}
}

void smblib_usb_plugin_hard_reset_locked(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	bool vbus_rising;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	bool last_vbus_present;

	last_vbus_present = chg->vbus_present;
	chg->dash_on = get_prop_fast_chg_started(chg);
	if (chg->dash_on) {
		pr_err("return directly because dash is online\n");
		return;
	}

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}

	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	chg->vbus_present = vbus_rising;

	if (vbus_rising) {
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER, false, 0);
		if (gpio_is_valid(chg->vbus_ctrl))
			schedule_delayed_work(&chg->connecter_check_work,
				msecs_to_jiffies(200));
		if (chg->wireless_present) {
			vote(chg->usb_icl_votable, OTG_VOTER, false, 0);
			if (chg->pd_active)
				vote(chg->usb_icl_votable, PD_VOTER, false, 0);
		}
		if (chg->swarp_supported) {
			chg->sw_aicl_prepared = false;
			cancel_delayed_work(&chg->run_sw_aicl_work);
			schedule_delayed_work(&chg->run_sw_aicl_work,
					msecs_to_jiffies(TIME_1000MS));
		}
	} else {
		if (chg->wa_flags & BOOST_BACK_WA) {
			data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				update_storm_count(wdata,
						WEAK_CHG_STORM_COUNT);
				vote(chg->usb_icl_votable, BOOST_BACK_VOTER,
						false, 0);
				vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
						false, 0);
			}
		}

		/* Force 1500mA FCC on USB removal if fcc stepper is enabled */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
							true, 1500000);
		if (!chg->wireless_present) {
			rc = smblib_request_dpdm(chg, false);
			if (rc < 0)
				smblib_err(chg, "Couldn't disable DPDM rc=%d\n", rc);
		} else {
			if ((chg->sink_src_mode == SRC_MODE) && !chg->is_audio_adapter)
				vote(chg->usb_icl_votable, OTG_VOTER, true, 0);
		}
		if (last_vbus_present != chg->vbus_present) {
			pr_info("vbus_rising 0");
			if (chg->swarp_online) {
				cancel_delayed_work(&chg->check_switch_dash_work);
				if (!get_prop_fast_chg_started(chg)) {
					if (chg->swarp_supported)
						(void)opchg_mcu_action(ACTION_MODE_RESET_SLEEP);
					else
						set_mcu_active(0);
				}
			}
			op_handle_usb_removal(chg);
		}
	}
	power_supply_changed(chg->usb_psy);
	power_supply_changed(chg->batt_psy);
	if (last_vbus_present != chg->vbus_present) {
		if (chg->vbus_present) {
			if (!chg->chg_wake_lock_on) {
				pr_info("acquire chg_wake_lock\n");
				__pm_stay_awake(chg->chg_wake_lock);
				chg->chg_wake_lock_on = true;
			} else {
				pr_err("chg_wake_lock is already stay awake.");
			}
		} else {
			if (chg->chg_wake_lock_on) {
				pr_info("release chg_wake_lock\n");
				__pm_relax(chg->chg_wake_lock);
				chg->chg_wake_lock_on = false;
			} else {
				pr_err("chg_wake_lock is already relax.");
			}
		}
	}
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: usbin-plugin %s\n",
					vbus_rising ? "attached" : "detached");
}

#define PL_DELAY_MS	30000
void smblib_usb_plugin_locked(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	bool vbus_rising;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	union power_supply_propval vbus_val;
	bool last_vbus_present;

	last_vbus_present = chg->vbus_present;
	chg->dash_on = get_prop_fast_chg_started(chg);
	if (chg->dash_on) {
		pr_err("return directly because dash is online\n");
		return;
	}

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}

	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	if (chg->wireless_present) {
		if (chg->wireless_high_vol_mode)
			smblib_set_opt_switcher_freq(chg, vbus_rising ? chg->chg_freq.freq_12V :
						     chg->chg_freq.freq_removal);
		else
			smblib_set_opt_switcher_freq(chg, vbus_rising ? chg->chg_freq.freq_6V_8V :
						     chg->chg_freq.freq_removal);
	} else {
		smblib_set_opt_switcher_freq(chg, vbus_rising ? chg->chg_freq.freq_5V :
					     chg->chg_freq.freq_removal);
	}

	smblib_hvdcp_detect_enable(chg, false);

	chg->vbus_present = vbus_rising;
	if (vbus_rising) {
		cancel_delayed_work_sync(&chg->pr_swap_detach_work);
		vote(chg->awake_votable, DETACH_DETECT_VOTER, false, 0);

		if (!chg->wireless_present) {
			rc = smblib_request_dpdm(chg, true);
			if (rc < 0)
				smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);
		} else {
			vote(chg->usb_icl_votable, OTG_VOTER, false, 0);
			if (chg->pd_active)
				vote(chg->usb_icl_votable, PD_VOTER, false, 0);
		}

		if (chg->charger_collpse) {
			op_set_collapse_fet(chg, 0);
			chg->charger_collpse = false;
		}
		chg->switch_on_fastchg = false;
		schedule_delayed_work(&chg->op_check_apsd_work,
				msecs_to_jiffies(TIME_1000MS));
		if (gpio_is_valid(chg->vbus_ctrl))
			schedule_delayed_work(&chg->connecter_check_work,
					msecs_to_jiffies(200));
		if (chg->check_slow_charge)
			schedule_delayed_work(
				&chg->slow_chg_check_work,
				msecs_to_jiffies(chg->probe_done ? 400 : 10000));
		/* Enable SW Thermal regulation */
		rc = smblib_set_sw_thermal_regulation(chg, true);
		if (rc < 0)
			smblib_err(chg, "Couldn't start SW thermal regulation WA, rc=%d\n",
				rc);

		/* Remove FCC_STEPPER 1.5A init vote to allow FCC ramp up */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER, false, 0);

		/* Schedule work to enable parallel charger */
		vote(chg->awake_votable, PL_DELAY_VOTER, true, 0);
		schedule_delayed_work(&chg->pl_enable_work,
					msecs_to_jiffies(PL_DELAY_MS));
		if (chg->swarp_supported) {
			chg->sw_aicl_prepared = false;
			schedule_delayed_work(&chg->run_sw_aicl_work,
					msecs_to_jiffies(TIME_1000MS));
		}
		if (chg->tried_fastchg) {
			chg->tried_fastchg = false;
			if (++chg->reconnect_count > 3) {
				pr_err("disconnect and reconnect too much, diasble switch fastchg.");
				chg->fastchg_switch_disable = true;
			}
			pr_info("reconnect %d times.", chg->reconnect_count);
		}
	} else {
		/* Disable SW Thermal Regulation */
		rc = smblib_set_sw_thermal_regulation(chg, false);
		if (rc < 0)
			smblib_err(chg, "Couldn't stop SW thermal regulation WA, rc=%d\n",
				rc);

		if (chg->wa_flags & BOOST_BACK_WA) {
			data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				update_storm_count(wdata,
						WEAK_CHG_STORM_COUNT);
				vote(chg->usb_icl_votable, BOOST_BACK_VOTER,
						false, 0);
				vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
						false, 0);
			}
		}

		/* Force 1500mA FCC on removal if fcc stepper is enabled */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
							true, 1500000);

		if (chg->wa_flags & WEAK_ADAPTER_WA) {
			chg->aicl_5v_threshold_mv =
					chg->default_aicl_5v_threshold_mv;
			chg->aicl_cont_threshold_mv =
					chg->default_aicl_cont_threshold_mv;

			smblib_set_charge_param(chg,
					&chg->param.aicl_5v_threshold,
					chg->aicl_5v_threshold_mv);
			smblib_set_charge_param(chg,
					&chg->param.aicl_cont_threshold,
					chg->aicl_cont_threshold_mv);
			chg->aicl_max_reached = false;

			if (chg->chg_param.smb_version == PMI632_SUBTYPE)
				schgm_flash_torch_priority(chg,
						TORCH_BUCK_MODE);

			data = chg->irq_info[USBIN_UV_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				reset_storm_count(wdata);
			}
			vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER,
					false, 0);
		}

		if (!chg->wireless_present) {
			rc = smblib_request_dpdm(chg, false);
			if (rc < 0)
				smblib_err(chg, "Couldn't disable DPDM rc=%d\n", rc);
		} else {
			if ((chg->sink_src_mode == SRC_MODE) && !chg->is_audio_adapter)
				vote(chg->usb_icl_votable, OTG_VOTER, true, 0);
		}
		if (last_vbus_present != chg->vbus_present) {
			op_handle_usb_removal(chg);
			if (chg->hw_detect) {
				chg->icon_debounce = true;
				schedule_delayed_work(&chg->icon_debounce_unplug_check,
					msecs_to_jiffies(TIME_1000MS));
			}
		}
		smblib_update_usb_type(chg);
	}

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		smblib_micro_usb_plugin(chg, vbus_rising);

	vote(chg->temp_change_irq_disable_votable, DEFAULT_VOTER,
						!vbus_rising, 0);

	power_supply_changed(chg->usb_psy);
	power_supply_changed(chg->batt_psy);
	if (last_vbus_present != chg->vbus_present) {
		if (chg->vbus_present) {
			if (!chg->chg_wake_lock_on) {
				pr_info("acquire chg_wake_lock\n");
				__pm_stay_awake(chg->chg_wake_lock);
				chg->chg_wake_lock_on = true;
			} else {
				pr_err("chg_wake_lock is already stay awake.");
			}
		} else {
			if (chg->chg_wake_lock_on) {
				pr_info("release chg_wake_lock\n");
				__pm_relax(chg->chg_wake_lock);
				chg->chg_wake_lock_on = false;
			} else {
				pr_err("chg_wake_lock is already relax.");
			}
		}
	}
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: usbin-plugin %s\n",
					vbus_rising ? "attached" : "detached");
	if (!vbus_rising) {
		rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
		if (rc < 0) {
			pr_err("V  fail rc=%d\n", rc);
		} else {
			if (vbus_val.intval > 3000) {
				pr_err("unplg,Vbus=%d", vbus_val.intval);
				op_dump_regs(chg);
			}
		}
	}
	pr_err("IRQ: %s\n",
			vbus_rising ? "attached" : "detached");
}

irqreturn_t usb_plugin_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK ||
			chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE) {
			pr_info("%s, Sink detected\n", __func__);
			return IRQ_HANDLED;
		}
	}
#endif
	if (chg->count_run)
		chg->count_run = 0;
	if (chg->pd_hard_reset)
		smblib_usb_plugin_hard_reset_locked(chg);
	else
		smblib_usb_plugin_locked(chg);

	return IRQ_HANDLED;
}

void op_handle_usb_plugin(struct smb_charger *chg)
{
	mutex_lock(&chg->smb_lock);
	smblib_usb_plugin_locked(chg);
	op_typec_state_change_irq_handler();
	mutex_unlock(&chg->smb_lock);
}

static void smblib_handle_slow_plugin_timeout(struct smb_charger *chg,
					      bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: slow-plugin-timeout %s\n",
		   rising ? "rising" : "falling");
}

static void smblib_handle_sdp_enumeration_done(struct smb_charger *chg,
					       bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: sdp-enumeration-done %s\n",
		   rising ? "rising" : "falling");
}

#define APSD_EXTENDED_TIMEOUT_MS	400
/* triggers when HVDCP 3.0 authentication has finished */
static void smblib_handle_hvdcp_3p0_auth_done(struct smb_charger *chg,
					      bool rising)
{
	const struct apsd_result *apsd_result;
	int rc;

	if (!rising)
		return;

	if (chg->mode == PARALLEL_MASTER)
		vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, true, 0);

	/* the APSD done handler will set the USB supply type */
	apsd_result = smblib_get_apsd_result(chg);

	if (apsd_result->bit & QC_3P0_BIT) {
		/* for QC3, switch to CP if present */
		if (chg->sec_cp_present) {
			rc = smblib_select_sec_charger(chg,
				POWER_SUPPLY_CHARGER_SEC_CP,
				POWER_SUPPLY_CP_HVDCP3, false);
			if (rc < 0)
				dev_err(chg->dev,
				"Couldn't enable secondary chargers  rc=%d\n",
					rc);
		}

		/* QC3.5 detection timeout */
		if (!chg->apsd_ext_timeout &&
				!timer_pending(&chg->apsd_timer)) {
			smblib_dbg(chg, PR_MISC,
				"APSD Extented timer started at %u\n",
				jiffies_to_msecs(jiffies));

			mod_timer(&chg->apsd_timer,
				msecs_to_jiffies(APSD_EXTENDED_TIMEOUT_MS)
				+ jiffies);
		}
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp-3p0-auth-done rising; %s detected\n",
		   apsd_result->name);
}

static void smblib_handle_hvdcp_check_timeout(struct smb_charger *chg,
					      bool rising, bool qc_charger)
{
	u32 hvdcp_ua = 0;

	if (rising) {

		if (qc_charger) {
			hvdcp_ua = (chg->real_charger_type ==
					POWER_SUPPLY_TYPE_USB_HVDCP) ?
					chg->chg_param.hvdcp2_max_icl_ua :
					HVDCP_CURRENT_UA;

			/* enable HDC and ICL irq for QC2/3 charger */
			vote(chg->limited_irq_disable_votable,
					CHARGER_TYPE_VOTER, false, 0);
			vote(chg->hdc_irq_disable_votable,
					CHARGER_TYPE_VOTER, false, 0);
			vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					hvdcp_ua);
		} else {
			/* A plain DCP, enforce DCP ICL if specified */
			vote(chg->usb_icl_votable, DCP_VOTER,
				chg->dcp_icl_ua != -EINVAL, chg->dcp_icl_ua);
		}
	}

	if (chg->wireless_present) {
		vote(chg->limited_irq_disable_votable, CHARGER_TYPE_VOTER,
		     false, 0);
		vote(chg->hdc_irq_disable_votable, CHARGER_TYPE_VOTER, false,
		     0);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
		     HVDCP_CURRENT_UA);
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s %s\n", __func__,
		   rising ? "rising" : "falling");
}

/* triggers when HVDCP is detected */
static void smblib_handle_hvdcp_detect_done(struct smb_charger *chg,
					    bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp-detect-done %s\n",
		   rising ? "rising" : "falling");
}

static void update_sw_icl_max(struct smb_charger *chg, int pst)
{
	int typec_mode;
	int rp_ua;

	/* while PD is active it should have complete ICL control */
	if (chg->pd_active)
		return;

	if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER) {
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, 500000);
		return;
	}

	/*
	 * HVDCP 2/3, handled separately
	 */
	if (pst == POWER_SUPPLY_TYPE_USB_HVDCP
			|| pst == POWER_SUPPLY_TYPE_USB_HVDCP_3)
		return;

#ifdef OP_SWARP_SUPPORTED
	if (is_charger_not_match(chg) && pst != POWER_SUPPLY_TYPE_UNKNOWN) {
		smblib_err(chg, "swarp prj with warp or dash adapter, 15W");
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
			DEFAULT_WARP_FOR_SWARP_MA * 1000);
		return;
	}
#endif

	/* TypeC rp med or high, use rp value */
	typec_mode = smblib_get_prop_typec_mode(chg);
	if (typec_rp_med_high(chg, typec_mode)) {
		rp_ua = get_rp_based_dcp_current(chg, typec_mode);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, rp_ua);
		return;
	}

	if (chg->wireless_present) {
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, 1500000);
		return;
	}

	/* rp-std or legacy, USB BC 1.2 */
	switch (pst) {
	case POWER_SUPPLY_TYPE_USB:
		/*
		 * USB_PSY will vote to increase the current to 500/900mA once
		 * enumeration is done.
		 */
		if (!is_client_vote_enabled(chg->usb_icl_votable, USB_PSY_VOTER))
			vote(chg->usb_icl_votable, USB_PSY_VOTER, true, 500000);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
		break;
	case POWER_SUPPLY_TYPE_USB_CDP:
		if (!chg->swarp_supported)
			vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					CDP_CURRENT_UA);
		else
			vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					CDP_DUAL_CELL_CURRENT_UA);
		break;
	case POWER_SUPPLY_TYPE_USB_DCP:
		rp_ua = get_rp_based_dcp_current(chg, typec_mode);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, rp_ua);
		break;
	case POWER_SUPPLY_TYPE_USB_FLOAT:
		/*
		 * limit ICL to 100mA, the USB driver will enumerate to check
		 * if this is a SDP and appropriately set the current
		 */
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					1500000);
		break;
	case POWER_SUPPLY_TYPE_UNKNOWN:
	default:
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					 SDP_CURRENT_UA);
		break;
	}
}

static void smblib_handle_apsd_done(struct smb_charger *chg, bool rising)
{
	const struct apsd_result *apsd_result;
	int temp_region = 0, current_limit_ua = 0;

	if (!rising)
		return;

	apsd_result = smblib_update_usb_type(chg);

	update_sw_icl_max(chg, apsd_result->pst);

	switch (apsd_result->bit) {
	case SDP_CHARGER_BIT:
	case CDP_CHARGER_BIT:
	case FLOAT_CHARGER_BIT:
		if (chg->use_extcon)
			smblib_notify_device_mode(chg, true);
		break;
	case OCP_CHARGER_BIT:
	case DCP_CHARGER_BIT:
		if (!chg->hvdcp_check)
			smblib_hvdcp_detect_enable(chg, false);
		break;
	default:
		break;
	}

	if ((apsd_result->bit) == SDP_CHARGER_BIT)
		current_limit_ua = DEFAULT_SDP_MA*1000;
	else if ((apsd_result->bit) == CDP_CHARGER_BIT) {
		if (!chg->swarp_supported)
			current_limit_ua = DEFAULT_CDP_MA*1000;
		else
			current_limit_ua = CDP_DUAL_CELL_CURRENT_UA;
	}
	else if ((apsd_result->bit) == DCP_CHARGER_BIT)
		current_limit_ua = DEFAULT_DCP_MA*1000;
	else if ((apsd_result->bit) == FLOAT_CHARGER_BIT) {
		if (chg->usb_type_redet_done)
			current_limit_ua = DEFAULT_DCP_MA*1000;
		else
			current_limit_ua = TYPEC_DEFAULT_CURRENT_UA*1000;
	} else if ((apsd_result->bit) == OCP_CHARGER_BIT)
		current_limit_ua = DEFAULT_DCP_MA*1000;
	else
		current_limit_ua = DEFAULT_DCP_MA*1000;

	if (chg->is_aging_test)
		current_limit_ua = DEFAULT_AGAING_CHG_MA*1000;

	if (chg->wireless_present)
		current_limit_ua = DEFAULT_DCP_MA * 1000;

#ifdef OP_SWARP_SUPPORTED
	if (is_charger_not_match(chg)) {
		smblib_err(chg, "swarp prj whith warp or dash adapter");
		current_limit_ua = DEFAULT_WARP_FOR_SWARP_MA * 1000;
	}
#endif

	vote(chg->usb_icl_votable,
		DCP_VOTER, true, current_limit_ua);
	vote(chg->usb_icl_votable,
		USB_PSY_VOTER, true, current_limit_ua);

	smblib_set_prop_charge_parameter_set(chg);
	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_COLD
		&& temp_region != BATT_TEMP_HOT) {
		op_charging_en(chg, true);
	}

	pr_info("apsd result=0x%x, name=%s, psy_type=%d\n",
		apsd_result->bit, apsd_result->name, apsd_result->pst);
	pr_info("apsd done,current_now=%d\n",
		(get_prop_batt_current_now(chg) / 1000));
	if (!chg->fastchg_switch_disable) {
		if (apsd_result->bit != SDP_CHARGER_BIT &&
			apsd_result->bit != CDP_CHARGER_BIT &&
			apsd_result->bit) {
			schedule_delayed_work(&chg->check_switch_dash_work,
						msecs_to_jiffies(50));
		} else {
			if (!chg->usb_type_redet_done) {
				if (!chg->boot_usb_present && chg->probe_done)
					schedule_delayed_work(&chg->re_det_work,
						msecs_to_jiffies(TIME_1000MS));
			} else {
				schedule_delayed_work(
				&chg->non_standard_charger_check_work,
				msecs_to_jiffies(TIME_1000MS));
			}
		}
	}
	chg->op_apsd_done = true;

	/* set allow read extern fg IIC */
	set_property_on_fg(chg,
		POWER_SUPPLY_PROP_SET_ALLOW_READ_EXTERN_FG_IIC, true);

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: apsd-done rising; %s detected\n",
		   apsd_result->name);
}

irqreturn_t usb_source_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc = 0;
	u8 stat;

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK ||
			chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE) {
			pr_info("%s, Sink detected\n", __func__);
			return IRQ_HANDLED;
		}
	}
#endif
	chg->dash_on = get_prop_fast_chg_started(chg);
	if (g_chg->dash_on || chg->switch_on_fastchg) {
		pr_err("return directly because dash is dash_on\n");
		return IRQ_HANDLED;
	}

	/* PD session is ongoing, ignore BC1.2 and QC detection */
	if (chg->pd_active)
		return IRQ_HANDLED;

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	pr_info("APSD_STATUS=0x%02x\n", stat);

	smblib_dbg(chg, PR_INTERRUPT, "APSD_STATUS = 0x%02x\n", stat);

	if ((chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		&& (stat & APSD_DTC_STATUS_DONE_BIT)
		&& !chg->uusb_apsd_rerun_done) {
		/*
		 * Force re-run APSD to handle slow insertion related
		 * charger-mis-detection.
		 */
		chg->uusb_apsd_rerun_done = true;
		smblib_rerun_apsd_if_required(chg);
		return IRQ_HANDLED;
	}

	smblib_handle_apsd_done(chg,
		(bool)(stat & APSD_DTC_STATUS_DONE_BIT));

	smblib_handle_hvdcp_detect_done(chg,
		(bool)(stat & QC_CHARGER_BIT));

	smblib_handle_hvdcp_check_timeout(chg,
		(bool)(stat & HVDCP_CHECK_TIMEOUT_BIT),
		(bool)(stat & QC_CHARGER_BIT));

	smblib_handle_hvdcp_3p0_auth_done(chg,
		(bool)(stat & QC_AUTH_DONE_STATUS_BIT));

	smblib_handle_sdp_enumeration_done(chg,
		(bool)(stat & ENUMERATION_DONE_BIT));

	smblib_handle_slow_plugin_timeout(chg,
		(bool)(stat & SLOW_PLUGIN_TIMEOUT_BIT));

	smblib_hvdcp_adaptive_voltage_change(chg);

	power_supply_changed(chg->usb_psy);

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	smblib_dbg(chg, PR_INTERRUPT, "APSD_STATUS = 0x%02x\n", stat);

	if (chg->support_hvdcp)
		schedule_delayed_work(&chg->heartbeat_work, 0);

	return IRQ_HANDLED;
}

enum alarmtimer_restart smblib_lpd_recheck_timer(struct alarm *alarm,
						ktime_t time)
{
	union power_supply_propval pval;
	struct smb_charger *chg = container_of(alarm, struct smb_charger,
							lpd_recheck_timer);
	int rc;

	if (chg->lpd_reason == LPD_MOISTURE_DETECTED) {
		pval.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		rc = smblib_set_prop_typec_power_role(chg, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
				pval.intval, rc);
			return ALARMTIMER_NORESTART;
		}
		chg->moisture_present = false;
		power_supply_changed(chg->usb_psy);
	} else {
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					TYPEC_WATER_DETECTION_INT_EN_BIT,
					TYPEC_WATER_DETECTION_INT_EN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set TYPE_C_INTERRUPT_EN_CFG_2_REG rc=%d\n",
					rc);
			return ALARMTIMER_NORESTART;
		}
	}

	chg->lpd_stage = LPD_STAGE_NONE;
	chg->lpd_reason = LPD_NONE;

	return ALARMTIMER_NORESTART;
}

#define RSBU_K_300K_UV	3000000
static bool smblib_src_lpd(struct smb_charger *chg)
{
	union power_supply_propval pval;
	bool lpd_flag = false;
	u8 stat;
	int rc;

	if (chg->lpd_disabled)
		return false;

	rc = smblib_read(chg, TYPE_C_SRC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_SRC_STATUS_REG rc=%d\n",
				rc);
		return false;
	}

	switch (stat & DETECTED_SNK_TYPE_MASK) {
	case SRC_DEBUG_ACCESS_BIT:
		if (smblib_rsbux_low(chg, RSBU_K_300K_UV))
			lpd_flag = true;
		break;
	case SRC_RD_RA_VCONN_BIT:
	case SRC_RD_OPEN_BIT:
	case AUDIO_ACCESS_RA_RA_BIT:
	default:
		break;
	}

	if (lpd_flag) {
		chg->lpd_stage = LPD_STAGE_COMMIT;
		pval.intval = POWER_SUPPLY_TYPEC_PR_SINK;
		rc = smblib_set_prop_typec_power_role(chg, &pval);
		if (rc < 0)
			smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
				pval.intval, rc);
		chg->lpd_reason = LPD_MOISTURE_DETECTED;
		chg->moisture_present =  true;
		vote(chg->usb_icl_votable, LPD_VOTER, true, 0);
		alarm_start_relative(&chg->lpd_recheck_timer,
						ms_to_ktime(60000));
		power_supply_changed(chg->usb_psy);
	} else {
		chg->lpd_reason = LPD_NONE;
		chg->typec_mode = smblib_get_prop_typec_mode(chg);
	}

	return lpd_flag;
}

static void typec_src_fault_condition_cfg(struct smb_charger *chg, bool src)
{
	int rc;
	u8 mask = USBIN_MID_COMP_FAULT_EN_BIT | USBIN_COLLAPSE_FAULT_EN_BIT;

	rc = smblib_masked_write(chg, OTG_FAULT_CONDITION_CFG_REG, mask,
					src ? 0 : mask);
	if (rc < 0)
		smblib_err(chg, "Couldn't write OTG_FAULT_CONDITION_CFG_REG rc=%d\n",
			rc);
}

static void typec_sink_insertion(struct smb_charger *chg)
{
	int rc;
	if (!chg->is_audio_adapter && !chg->wireless_present)
		vote(chg->usb_icl_votable, OTG_VOTER, true, 0);
	typec_src_fault_condition_cfg(chg, true);
	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher,
					chg->chg_freq.freq_above_otg_threshold);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_boost rc=%d\n", rc);

	if (chg->use_extcon) {
		smblib_notify_usb_host(chg, true);
		chg->otg_present = true;
	}

	if (!chg->pr_swap_in_progress)
		chg->ok_to_pd = (!(chg->pd_disabled) || chg->early_usb_attach)
					&& !chg->pd_not_supported;
}

static void typec_src_insertion(struct smb_charger *chg)
{
	int rc = 0;
	u8 stat;

	if (chg->pr_swap_in_progress) {
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
		return;
	}

	rc = smblib_read(chg, LEGACY_CABLE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATE_MACHINE_STATUS_REG rc=%d\n",
			rc);
		return;
	}

	chg->typec_legacy = stat & TYPEC_LEGACY_CABLE_STATUS_BIT;
	chg->ok_to_pd = (!(chg->typec_legacy || chg->pd_disabled)
			|| chg->early_usb_attach) && !chg->pd_not_supported;

	/* allow apsd proceed to detect QC2/3 */
	if (!chg->ok_to_pd)
		smblib_hvdcp_detect_try_enable(chg, true);
}

static void typec_ra_ra_insertion(struct smb_charger *chg)
{
	vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, 500000);
	vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	chg->ok_to_pd = false;
}

static const char * const dr_mode_text[] = {
	"ufp", "dfp", "none"
};

static int smblib_force_dr_mode(struct smb_charger *chg, int mode)
{
	int rc = 0;

	switch (mode) {
	case TYPEC_PORT_SNK:
		rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
			TYPEC_POWER_ROLE_CMD_MASK, EN_SNK_ONLY_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable snk, rc=%d\n", rc);
			return rc;
		}
		break;
	case TYPEC_PORT_SRC:
		rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
			TYPEC_POWER_ROLE_CMD_MASK, EN_SRC_ONLY_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable src, rc=%d\n", rc);
			return rc;
		}
		break;
	case TYPEC_PORT_DRP:
		rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
			TYPEC_POWER_ROLE_CMD_MASK, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable DRP, rc=%d\n", rc);
			return rc;
		}
		break;
	default:
		smblib_err(chg, "Power role %d not supported\n", mode);
		return -EINVAL;
	}

	chg->dr_mode = mode;

	return rc;
}

int smblib_typec_port_type_set(const struct typec_capability *cap,
					enum typec_port_type type)
{
	struct smb_charger *chg = container_of(cap,
					struct smb_charger, typec_caps);
	int rc = 0;

	mutex_lock(&chg->typec_lock);

	if ((chg->pr_swap_in_progress) || (type == TYPEC_PORT_DRP)) {
		smblib_dbg(chg, PR_MISC, "Ignoring port type request type = %d swap_in_progress = %d\n",
				type, chg->pr_swap_in_progress);
		goto unlock;
	}

	chg->pr_swap_in_progress = true;

	rc = smblib_force_dr_mode(chg, type);
	if (rc < 0) {
		chg->pr_swap_in_progress = false;
		smblib_err(chg, "Failed to force mode, rc=%d\n", rc);
		goto unlock;
	}

	smblib_dbg(chg, PR_MISC, "Requested role %s\n",
				type ? "SINK" : "SOURCE");

	/*
	 * As per the hardware requirements,
	 * schedule the work with required delay.
	 */
	if (!(delayed_work_pending(&chg->role_reversal_check))) {
		cancel_delayed_work_sync(&chg->role_reversal_check);
		schedule_delayed_work(&chg->role_reversal_check,
			msecs_to_jiffies(ROLE_REVERSAL_DELAY_MS));
		vote(chg->awake_votable, TYPEC_SWAP_VOTER, true, 0);
	}

unlock:
	mutex_unlock(&chg->typec_lock);
	return rc;
}

static void smblib_typec_role_check_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
					role_reversal_check.work);
	int rc = 0;

	mutex_lock(&chg->typec_lock);

	switch (chg->dr_mode) {
	case TYPEC_PORT_SNK:
		if (chg->typec_mode < POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) {
			smblib_dbg(chg, PR_MISC, "Role reversal not latched to UFP in %d msecs. Resetting to DRP mode\n",
						ROLE_REVERSAL_DELAY_MS);
			rc = smblib_force_dr_mode(chg, TYPEC_PORT_DRP);
			if (rc < 0)
				smblib_err(chg, "Failed to set DRP mode, rc=%d\n",
						rc);
		} else {
			chg->power_role = POWER_SUPPLY_TYPEC_PR_SINK;
			typec_set_pwr_role(chg->typec_port, TYPEC_SINK);
			typec_set_data_role(chg->typec_port, TYPEC_DEVICE);
			smblib_dbg(chg, PR_MISC, "Role changed successfully to SINK");
		}
		break;
	case TYPEC_PORT_SRC:
		if (chg->typec_mode >= POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
			|| chg->typec_mode == POWER_SUPPLY_TYPEC_NONE) {
			smblib_dbg(chg, PR_MISC, "Role reversal not latched to DFP in %d msecs. Resetting to DRP mode\n",
						ROLE_REVERSAL_DELAY_MS);
			chg->pr_swap_in_progress = false;
			chg->typec_role_swap_failed = true;
			rc = smblib_force_dr_mode(chg, TYPEC_PORT_DRP);
			if (rc < 0)
				smblib_err(chg, "Failed to set DRP mode, rc=%d\n",
					rc);
		} else {
			chg->power_role = POWER_SUPPLY_TYPEC_PR_SOURCE;
			typec_set_pwr_role(chg->typec_port, TYPEC_SOURCE);
			typec_set_data_role(chg->typec_port, TYPEC_HOST);
			smblib_dbg(chg, PR_MISC, "Role changed successfully to SOURCE");
		}
		break;
	default:
		pr_debug("Already in DRP mode\n");
		break;
	}

	chg->pr_swap_in_progress = false;
	vote(chg->awake_votable, TYPEC_SWAP_VOTER, false, 0);
	mutex_unlock(&chg->typec_lock);
}

static void typec_sink_removal(struct smb_charger *chg)
{
	int rc;

	vote(chg->usb_icl_votable, OTG_VOTER, false, 0);
	typec_src_fault_condition_cfg(chg, false);
	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher,
					chg->chg_freq.freq_removal);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_removal rc=%d\n", rc);

	if (chg->use_extcon) {
		if (chg->otg_present)
			smblib_notify_usb_host(chg, false);
		chg->otg_present = false;
	}

	typec_partner_unregister(chg);

	chg->is_audio_adapter = false;
}

static void typec_src_removal(struct smb_charger *chg)
{
	int rc;
	struct smb_irq_data *data;
	struct storm_watch *wdata;
	int sec_charger;
	u8 val[2] = {0};

	sec_charger = chg->sec_pl_present ? POWER_SUPPLY_CHARGER_SEC_PL :
				POWER_SUPPLY_CHARGER_SEC_NONE;

	rc = smblib_select_sec_charger(chg, sec_charger, POWER_SUPPLY_CP_NONE,
					false);
	if (rc < 0)
		dev_err(chg->dev,
			"Couldn't disable secondary charger rc=%d\n", rc);

	chg->qc3p5_detected = false;
	typec_src_fault_condition_cfg(chg, false);
	smblib_hvdcp_detect_try_enable(chg, false);
	smblib_update_usb_type(chg);

	if (chg->wa_flags & BOOST_BACK_WA) {
		data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
		if (data) {
			wdata = &data->storm_data;
			update_storm_count(wdata, WEAK_CHG_STORM_COUNT);
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					false, 0);
		}
	}

	cancel_delayed_work_sync(&chg->pl_enable_work);

	/* reset input current limit voters */
	vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, 500000);
	vote(chg->usb_icl_votable, PD_VOTER, false, 0);
	vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
	vote(chg->usb_icl_votable, SW_QC3_VOTER, false, 0);
	vote(chg->usb_icl_votable, OTG_VOTER, false, 0);
	vote(chg->usb_icl_votable, CTM_VOTER, false, 0);
	vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
	vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);
	vote(chg->usb_icl_votable, THERMAL_THROTTLE_VOTER, false, 0);
	vote(chg->usb_icl_votable, LPD_VOTER, false, 0);

	/* reset usb irq voters */
	vote(chg->limited_irq_disable_votable, CHARGER_TYPE_VOTER,
			true, 0);
	vote(chg->hdc_irq_disable_votable, CHARGER_TYPE_VOTER, true, 0);
	vote(chg->hdc_irq_disable_votable, HDC_IRQ_VOTER, false, 0);

	/* reset parallel voters */
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);
	vote(chg->pl_disable_votable, PL_FCC_LOW_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, false, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);

	/* Remove SW thermal regulation WA votes */
	vote(chg->usb_icl_votable, SW_THERM_REGULATION_VOTER, false, 0);
	vote(chg->pl_disable_votable, SW_THERM_REGULATION_VOTER, false, 0);
	vote(chg->dc_suspend_votable, SW_THERM_REGULATION_VOTER, false, 0);
	if (chg->cp_disable_votable)
		vote(chg->cp_disable_votable, SW_THERM_REGULATION_VOTER,
								false, 0);

	/* reset USBOV votes and cancel work */
	cancel_delayed_work_sync(&chg->usbov_dbc_work);
	vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
	chg->dbc_usbov = false;

	chg->pulse_cnt = 0;
	chg->usb_icl_delta_ua = 0;
	chg->voltage_min_uv = MICRO_5V;
	chg->voltage_max_uv = MICRO_5V;
	chg->usbin_forced_max_uv = 0;
	chg->chg_param.forced_main_fcc = 0;

	/* Reset all CC mode votes */
	vote(chg->fcc_main_votable, MAIN_FCC_VOTER, false, 0);
	chg->adapter_cc_mode = 0;
	chg->thermal_overheat = 0;
	vote_override(chg->fcc_votable, CC_MODE_VOTER, false, 0);
	vote_override(chg->usb_icl_votable, CC_MODE_VOTER, false, 0);
	vote(chg->cp_disable_votable, OVERHEAT_LIMIT_VOTER, false, 0);
	vote(chg->usb_icl_votable, OVERHEAT_LIMIT_VOTER, false, 0);

	/* write back the default FLOAT charger configuration */
	rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
				(u8)FLOAT_OPTIONS_MASK, chg->float_cfg);
	if (rc < 0)
		smblib_err(chg, "Couldn't write float charger options rc=%d\n",
			rc);

	if (chg->sdam_base) {
		rc = smblib_write(chg,
			chg->sdam_base + SDAM_QC_DET_STATUS_REG, 0);
		if (rc < 0)
			pr_err("Couldn't clear SDAM QC status rc=%d\n", rc);

		rc = smblib_batch_write(chg,
			chg->sdam_base + SDAM_QC_ADC_LSB_REG, val, 2);
		if (rc < 0)
			pr_err("Couldn't clear SDAM ADC status rc=%d\n", rc);
	}

	if (!chg->pr_swap_in_progress) {
		rc = smblib_usb_pd_adapter_allowance_override(chg, FORCE_NULL);
		if (rc < 0)
			smblib_err(chg, "Couldn't set FORCE_NULL rc=%d\n", rc);

		rc = smblib_set_charge_param(chg,
				&chg->param.aicl_cont_threshold,
				chg->default_aicl_cont_threshold_mv);
		if (rc < 0)
			smblib_err(chg, "Couldn't restore aicl_cont_threshold, rc=%d",
					rc);
	}
	/*
	 * if non-compliant charger caused UV, restore original max pulses
	 * and turn SUSPEND_ON_COLLAPSE_USBIN_BIT back on.
	 */
	if (chg->qc2_unsupported_voltage) {
		rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
				HVDCP_PULSE_COUNT_MAX_QC2_MASK,
				chg->qc2_max_pulses);
		if (rc < 0)
			smblib_err(chg, "Couldn't restore max pulses rc=%d\n",
					rc);

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				SUSPEND_ON_COLLAPSE_USBIN_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn on SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		chg->qc2_unsupported_voltage = QC2_COMPLIANT;
		chg->qc2_uv_count = 0;
	}

	if (chg->use_extcon)
		smblib_notify_device_mode(chg, false);

	chg->typec_legacy = false;

	del_timer_sync(&chg->apsd_timer);
	chg->apsd_ext_timeout = false;
}

static void typec_mode_unattached(struct smb_charger *chg)
{
	vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, USBIN_100MA);
}

static void smblib_handle_rp_change(struct smb_charger *chg, int typec_mode)
{
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);

	/*
	 * We want the ICL vote @ 100mA for a FLOAT charger
	 * until the detection by the USB stack is complete.
	 * Ignore the Rp changes unless there is a
	 * pre-existing valid vote or FLOAT is configured for
	 * SDP current.
	 */
	if (apsd->pst == POWER_SUPPLY_TYPE_USB_FLOAT) {
		if (get_client_vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER)
					<= USBIN_100MA
			|| (chg->float_cfg & FLOAT_OPTIONS_MASK)
					== FORCE_FLOAT_SDP_CFG_BIT)
			return;
	}

	update_sw_icl_max(chg, apsd->pst);

	smblib_dbg(chg, PR_MISC, "CC change old_mode=%d new_mode=%d\n",
						chg->typec_mode, typec_mode);
}

static void smblib_lpd_launch_ra_open_work(struct smb_charger *chg)
{
	u8 stat;
	int rc;

	if (chg->lpd_disabled)
		return;

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n",
			rc);
		return;
	}

	if (!(stat & TYPEC_TCCDEBOUNCE_DONE_STATUS_BIT)
			&& chg->lpd_stage == LPD_STAGE_NONE) {
		chg->lpd_stage = LPD_STAGE_FLOAT;
		cancel_delayed_work_sync(&chg->lpd_ra_open_work);
		vote(chg->awake_votable, LPD_VOTER, true, 0);
		schedule_delayed_work(&chg->lpd_ra_open_work,
						msecs_to_jiffies(300));
	}
}

irqreturn_t typec_or_rid_detection_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		if (chg->uusb_moisture_protection_enabled) {
			/*
			 * Adding pm_stay_awake as because pm_relax is called
			 * on exit path from the work routine.
			 */
			pm_stay_awake(chg->dev);
			schedule_work(&chg->moisture_protection_work);
		}

		cancel_delayed_work_sync(&chg->uusb_otg_work);
		/*
		 * Skip OTG enablement if RID interrupt triggers with moisture
		 * protection still enabled.
		 */
		if (!chg->moisture_present) {
			vote(chg->awake_votable, OTG_DELAY_VOTER, true, 0);
			smblib_dbg(chg, PR_INTERRUPT, "Scheduling OTG work\n");
			schedule_delayed_work(&chg->uusb_otg_work,
				msecs_to_jiffies(chg->otg_delay_ms));
		}

		goto out;
	}

	if (chg->pr_swap_in_progress || chg->pd_hard_reset)
		goto out;

	smblib_lpd_launch_ra_open_work(chg);

	if (chg->usb_psy)
		power_supply_changed(chg->usb_psy);

out:
	return IRQ_HANDLED;
}

static void op_typec_state_change_irq_handler(void)
{
	int typec_mode;

	if (g_chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		smblib_dbg(g_chg, PR_INTERRUPT,
				"Ignoring for micro USB\n");
		return;
	}

	typec_mode = smblib_get_prop_typec_mode(g_chg);
	if (typec_mode != g_chg->typec_mode) {
		smblib_handle_rp_change(g_chg, typec_mode);
		g_chg->typec_mode = typec_mode;
	}

	smblib_dbg(g_chg, PR_INTERRUPT,
				 "%s: cc-state-change; Type-C %s detected\n",
				__func__,
				smblib_typec_mode_name[g_chg->typec_mode]);

	power_supply_changed(g_chg->usb_psy);
}

irqreturn_t typec_state_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int typec_mode;
	int rc;
	int wireless_present = 0;
	union power_supply_propval pval;
	chg->dash_on = get_prop_fast_chg_started(chg);
	if (chg->dash_on) {
		pr_info("chg->dash_on = %d update typec state!\n",
			chg->dash_on);
		return IRQ_HANDLED;
	}

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		smblib_dbg(chg, PR_INTERRUPT,
				"Ignoring for micro USB\n");
		return IRQ_HANDLED;
	}

	if (!chg->wls_psy)
		chg->wls_psy = power_supply_get_by_name("wireless");
	if (chg->wls_psy) {
		rc = power_supply_get_property(chg->wls_psy,
					       POWER_SUPPLY_PROP_PRESENT,
					       &pval);
		if (rc < 0)
			smblib_err(chg, "Couldn't get wireless present rc=%d\n", rc);
		else
			wireless_present = pval.intval;
	} else {
		smblib_err(chg, "Couldn't find wireless psy\n");
	}

	typec_mode = smblib_get_prop_typec_mode(chg);
	if (chg->sink_src_mode != UNATTACHED_MODE
			&& (typec_mode != chg->typec_mode))
		smblib_handle_rp_change(chg, typec_mode);
	chg->typec_mode = typec_mode;
	if (wireless_present) {
		rc = smblib_masked_write(g_chg, TYPE_C_EXIT_STATE_CFG_REG,
					 BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT,
					 0);
		if (rc < 0)
			smblib_err(g_chg, "Couldn't set exit state cfg rc=%d\n", rc);
	}

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK ||
			chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE) {
			pr_info("%s: Sink detected, Disable APSD!\n", __func__);
			smblib_apsd_enable(chg, false);
		}
	}
#endif
	smblib_err(chg, "IRQ: cc-state-change; Type-C %s detected\n",
				smblib_typec_mode_name[chg->typec_mode]);

	power_supply_changed(chg->usb_psy);

	return IRQ_HANDLED;
}

static void smblib_lpd_clear_ra_open_work(struct smb_charger *chg)
{
	if (chg->lpd_disabled)
		return;

	cancel_delayed_work_sync(&chg->lpd_detach_work);
	chg->lpd_stage = LPD_STAGE_FLOAT_CANCEL;
	cancel_delayed_work_sync(&chg->lpd_ra_open_work);
	vote(chg->awake_votable, LPD_VOTER, false, 0);
}

#define TYPEC_DETACH_DETECT_DELAY_MS 2000
irqreturn_t typec_attach_detach_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	bool attached = false;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	chg->dash_on = get_prop_fast_chg_started(chg);
	if (chg->dash_on) {
		pr_err("return directly because dash is online\n");
		return IRQ_HANDLED;
	}

	rc = smblib_read(chg, TYPE_C_STATE_MACHINE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATE_MACHINE_STATUS_REG rc=%d\n",
			rc);
		return IRQ_HANDLED;
	}

	attached = !!(stat & TYPEC_ATTACH_DETACH_STATE_BIT);

	if (attached) {
		smblib_lpd_clear_ra_open_work(chg);

		rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n",
				rc);
			return IRQ_HANDLED;
		}

		if (smblib_get_prop_dfp_mode(chg) ==
				POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER) {
			chg->sink_src_mode = AUDIO_ACCESS_MODE;
			typec_ra_ra_insertion(chg);
		} else if (stat & SNK_SRC_MODE_BIT) {
			if (smblib_src_lpd(chg))
				return IRQ_HANDLED;
			chg->sink_src_mode = SRC_MODE;
			typec_sink_insertion(chg);
		} else {
			chg->sink_src_mode = SINK_MODE;
			typec_src_insertion(chg);
		}

		rc = typec_partner_register(chg);
		if (rc < 0)
			smblib_err(chg, "failed to register partner rc =%d\n",
					rc);
	} else {
		switch (chg->sink_src_mode) {
		case SRC_MODE:
			typec_sink_removal(chg);
			break;
		case SINK_MODE:
		case AUDIO_ACCESS_MODE:
			typec_src_removal(chg);
			break;
		case UNATTACHED_MODE:
		default:
			typec_mode_unattached(chg);
			break;
		}

		if (!chg->pr_swap_in_progress) {
			chg->ok_to_pd = false;
			chg->sink_src_mode = UNATTACHED_MODE;
			chg->early_usb_attach = false;
			smblib_apsd_enable(chg, true);
		}

		/*
		 * Restore DRP mode on type-C cable disconnect if role
		 * swap is not in progress, to ensure forced sink or src
		 * mode configuration is reset properly.
		 */
		mutex_lock(&chg->typec_lock);

		if (chg->typec_port && !chg->pr_swap_in_progress) {

			/*
			 * Schedule the work to differentiate actual removal
			 * of cable and detach interrupt during role swap,
			 * unregister the partner only during actual cable
			 * removal.
			 */
			cancel_delayed_work(&chg->pr_swap_detach_work);
			vote(chg->awake_votable, DETACH_DETECT_VOTER, true, 0);
			schedule_delayed_work(&chg->pr_swap_detach_work,
				msecs_to_jiffies(TYPEC_DETACH_DETECT_DELAY_MS));
			smblib_force_dr_mode(chg, TYPEC_PORT_DRP);
			/*
			 * To handle cable removal during role
			 * swap failure.
			 */
			chg->typec_role_swap_failed = false;
		}

		mutex_unlock(&chg->typec_lock);

		if (chg->lpd_stage == LPD_STAGE_FLOAT_CANCEL)
			schedule_delayed_work(&chg->lpd_detach_work,
					msecs_to_jiffies(1000));
	}

	rc = smblib_masked_write(chg, USB_CMD_PULLDOWN_REG,
			EN_PULLDOWN_USB_IN_BIT,
			attached ?  0 : EN_PULLDOWN_USB_IN_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't configure pulldown on USB_IN rc=%d\n",
				rc);

	power_supply_changed(chg->usb_psy);

	return IRQ_HANDLED;
}

static void dcin_aicl(struct smb_charger *chg)
{
	int rc, icl, icl_save;
	int input_present;
	bool aicl_done = true;

	/*
	 * Hold awake votable to prevent pm_relax being called prior to
	 * completion of this work.
	 */
	vote(chg->awake_votable, DCIN_AICL_VOTER, true, 0);

increment:
	mutex_lock(&chg->dcin_aicl_lock);

	rc = smblib_get_charge_param(chg, &chg->param.dc_icl, &icl);
	if (rc < 0)
		goto err;

	if (icl == chg->wls_icl_ua) {
		/* Upper limit reached; do nothing */
		smblib_dbg(chg, PR_WLS, "hit max ICL: stop\n");

		rc = smblib_is_input_present(chg, &input_present);
		if (rc < 0 || !(input_present & INPUT_PRESENT_DC))
			aicl_done = false;

		goto unlock;
	}

	icl = min(chg->wls_icl_ua, icl + DCIN_ICL_STEP_UA);
	icl_save = icl;

	rc = smblib_set_charge_param(chg, &chg->param.dc_icl, icl);
	if (rc < 0)
		goto err;

	mutex_unlock(&chg->dcin_aicl_lock);

	smblib_dbg(chg, PR_WLS, "icl: %d mA\n", (icl / 1000));

	/* Check to see if DC is still present before and after sleep */
	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0 || !(input_present & INPUT_PRESENT_DC)) {
		aicl_done = false;
		goto unvote;
	}

	/*
	 * Wait awhile to check for any DCIN_UVs (the UV handler reduces the
	 * ICL). If the adaptor collapses, the ICL read after waking up will be
	 * lesser, indicating that the AICL process is complete.
	 */
	msleep(500);

	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0 || !(input_present & INPUT_PRESENT_DC)) {
		aicl_done = false;
		goto unvote;
	}

	mutex_lock(&chg->dcin_aicl_lock);

	rc = smblib_get_charge_param(chg, &chg->param.dc_icl, &icl);
	if (rc < 0)
		goto err;

	if (icl < icl_save) {
		smblib_dbg(chg, PR_WLS, "done: icl: %d mA\n", (icl / 1000));
		goto unlock;
	}

	mutex_unlock(&chg->dcin_aicl_lock);

	goto increment;

err:
	aicl_done = false;
unlock:
	mutex_unlock(&chg->dcin_aicl_lock);
unvote:
	vote(chg->awake_votable, DCIN_AICL_VOTER, false, 0);
	chg->dcin_aicl_done = aicl_done;
}

static void dcin_aicl_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						dcin_aicl_work);
	dcin_aicl(chg);
}

static void dump_register_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						dump_reg_work);
	op_dump_regs(chg);
}


static enum alarmtimer_restart dcin_aicl_alarm_cb(struct alarm *alarm,
							ktime_t now)
{
	struct smb_charger *chg = container_of(alarm, struct smb_charger,
					dcin_aicl_alarm);

	smblib_dbg(chg, PR_WLS, "rerunning DCIN AICL\n");

	pm_stay_awake(chg->dev);
	schedule_work(&chg->dcin_aicl_work);

	return ALARMTIMER_NORESTART;
}

static void dcin_icl_decrement(struct smb_charger *chg)
{
	int rc, icl;
	ktime_t now = ktime_get();

	rc = smblib_get_charge_param(chg, &chg->param.dc_icl, &icl);
	if (rc < 0) {
		smblib_err(chg, "reading DCIN ICL failed: %d\n", rc);
		return;
	}

	if (icl == DCIN_ICL_MIN_UA) {
		/* Cannot possibly decrease ICL any further - do nothing */
		smblib_dbg(chg, PR_WLS, "hit min ICL: stop\n");
		return;
	}

	/* Reduce ICL by 100 mA if 3 UVs happen in a row */
	if (ktime_us_delta(now, chg->dcin_uv_last_time) > (200 * 1000)) {
		chg->dcin_uv_count = 0;
	} else if (chg->dcin_uv_count >= 3) {
		icl -= DCIN_ICL_STEP_UA;

		smblib_dbg(chg, PR_WLS, "icl: %d mA\n", (icl / 1000));
		rc = smblib_set_charge_param(chg, &chg->param.dc_icl, icl);
		if (rc < 0) {
			smblib_err(chg, "setting DCIN ICL failed: %d\n", rc);
			return;
		}

		chg->dcin_uv_count = 0;
	}

	chg->dcin_uv_last_time = now;
}

irqreturn_t dcin_uv_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	mutex_lock(&chg->dcin_aicl_lock);

	chg->dcin_uv_count++;
	smblib_dbg(chg, (PR_WLS | PR_INTERRUPT), "DCIN UV count: %d\n",
			chg->dcin_uv_count);
	dcin_icl_decrement(chg);

	mutex_unlock(&chg->dcin_aicl_lock);

	return IRQ_HANDLED;
}

irqreturn_t dc_plugin_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	union power_supply_propval pval;
	int input_present;
	bool dcin_present, vbus_present;
	int rc, wireless_vout = 0, wls_set = 0;
	int sec_charger;

	if (chg->wireless_present) {
		pr_info("Do not use the following process when wireless charging\n");
		return IRQ_HANDLED;
	}

	rc = smblib_get_prop_vph_voltage_now(chg, &pval);
	if (rc < 0)
		return IRQ_HANDLED;

	/* 2*VPH, with a granularity of 100mV */
	wireless_vout = ((pval.intval * 2) / 100000) * 100000;

	rc = smblib_is_input_present(chg, &input_present);
	if (rc < 0)
		return IRQ_HANDLED;

	dcin_present = input_present & INPUT_PRESENT_DC;
	vbus_present = input_present & INPUT_PRESENT_USB;

	if (!chg->cp_ilim_votable)
		chg->cp_ilim_votable = find_votable("CP_ILIM");

	if (dcin_present && !vbus_present) {
		cancel_work_sync(&chg->dcin_aicl_work);

		/* Reset DCIN ICL to 100 mA */
		mutex_lock(&chg->dcin_aicl_lock);
		rc = smblib_set_charge_param(chg, &chg->param.dc_icl,
				DCIN_ICL_MIN_UA);
		mutex_unlock(&chg->dcin_aicl_lock);
		if (rc < 0)
			return IRQ_HANDLED;

		smblib_dbg(chg, (PR_WLS | PR_INTERRUPT), "reset: icl: 100 mA\n");

		/*
		 * Remove USB's CP ILIM vote - inapplicable for wireless
		 * parallel charging.
		 */
		if (chg->cp_ilim_votable)
			vote(chg->cp_ilim_votable, ICL_CHANGE_VOTER, false, 0);

		if (chg->sec_cp_present) {
			/*
			 * If CP output topology is VBATT, limit main charger's
			 * FCC share and let the CPs handle the rest.
			 */
			if (is_cp_topo_vbatt(chg))
				vote(chg->fcc_main_votable,
					WLS_PL_CHARGING_VOTER, true, 800000);

			rc = smblib_get_prop_batt_status(chg, &pval);
			if (rc < 0)
				smblib_err(chg, "Couldn't read batt status rc=%d\n",
						rc);

			wls_set = (pval.intval == POWER_SUPPLY_STATUS_FULL) ?
				MICRO_5V : wireless_vout;

			pval.intval = wls_set;
			rc = smblib_set_prop_voltage_wls_output(chg, &pval);
			if (rc < 0)
				dev_err(chg->dev, "Couldn't set dc voltage to 2*vph  rc=%d\n",
					rc);

			rc = smblib_select_sec_charger(chg,
					POWER_SUPPLY_CHARGER_SEC_CP,
					POWER_SUPPLY_CP_WIRELESS, false);
			if (rc < 0)
				dev_err(chg->dev, "Couldn't enable secondary chargers  rc=%d\n",
					rc);
		} else {
			/*
			 * If no secondary charger is present, commence
			 * wireless charging at 5 V by default.
			 */
			pval.intval = 5000000;
			rc = smblib_set_prop_voltage_wls_output(chg, &pval);
			if (rc < 0)
				dev_err(chg->dev, "Couldn't set dc voltage to 5 V rc=%d\n",
					rc);
		}

		schedule_work(&chg->dcin_aicl_work);
	} else {
		if (chg->cp_reason == POWER_SUPPLY_CP_WIRELESS) {
			sec_charger = chg->sec_pl_present ?
					POWER_SUPPLY_CHARGER_SEC_PL :
					POWER_SUPPLY_CHARGER_SEC_NONE;
			rc = smblib_select_sec_charger(chg, sec_charger,
					POWER_SUPPLY_CP_NONE, false);
			if (rc < 0)
				dev_err(chg->dev, "Couldn't disable secondary charger rc=%d\n",
					rc);
		}

		vote(chg->dc_suspend_votable, CHG_TERMINATION_VOTER, false, 0);
		vote(chg->fcc_main_votable, WLS_PL_CHARGING_VOTER, false, 0);

		chg->last_wls_vout = 0;
		chg->dcin_aicl_done = false;
		chg->dcin_icl_user_set = false;
	}

	/*
	 * Vote for 1500mA FCC upon WLS detach and remove vote upon attach if
	 * FCC stepper is enabled.
	 */
	if (chg->fcc_stepper_enable && !vbus_present)
		vote(chg->fcc_votable, FCC_STEPPER_VOTER, !dcin_present,
				dcin_present ? 0 : 1500000);

	if (chg->dc_psy)
		power_supply_changed(chg->dc_psy);

	smblib_dbg(chg, (PR_WLS | PR_INTERRUPT), "dcin_present= %d, usbin_present= %d, cp_reason = %d\n",
			dcin_present, vbus_present, chg->cp_reason);

	return IRQ_HANDLED;
}

irqreturn_t high_duty_cycle_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	chg->is_hdc = true;
	/*
	 * Disable usb IRQs after the flag set and re-enable IRQs after
	 * the flag cleared in the delayed work queue, to avoid any IRQ
	 * storming during the delays
	 */
	vote(chg->hdc_irq_disable_votable, HDC_IRQ_VOTER, true, 0);

	schedule_delayed_work(&chg->clear_hdc_work, msecs_to_jiffies(60));

	return IRQ_HANDLED;
}

static void smblib_bb_removal_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						bb_removal_work.work);

	vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
	vote(chg->awake_votable, BOOST_BACK_VOTER, false, 0);
}

#define BOOST_BACK_UNVOTE_DELAY_MS		750
#define BOOST_BACK_STORM_COUNT			3
#define WEAK_CHG_STORM_COUNT			8
irqreturn_t switcher_power_ok_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc, usb_icl;
	u8 stat;
	union power_supply_propval vbus_val;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	chg->dash_on = get_prop_fast_chg_started(chg);
	if (chg->dash_on) {
		pr_err("return directly because dash is online\n");
		return IRQ_HANDLED;
	}

	if (!(chg->wa_flags & BOOST_BACK_WA))
		return IRQ_HANDLED;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	/* skip suspending input if its already suspended by some other voter */
	usb_icl = get_effective_result(chg->usb_icl_votable);
	if ((stat & USE_USBIN_BIT) && usb_icl >= 0 && usb_icl <= USBIN_25MA)
		return IRQ_HANDLED;

	if (stat & USE_DCIN_BIT)
		return IRQ_HANDLED;

	if (is_storming(&irq_data->storm_data)) {
		smblib_err(chg, "Reverse boost detected\n");
		rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
		if (rc < 0)
			pr_err("fail to read usb_voltage rc=%d\n", rc);
		else if (vbus_val.intval >= 2500)
			pr_err("vbus_val.intval=%d\n", vbus_val.intval);
		chg->revert_boost_trigger = true;
		vote(chg->usb_icl_votable, BOOST_BACK_VOTER, true, 0);
		schedule_delayed_work(&chg->recovery_suspend_work,
				msecs_to_jiffies(TIME_100MS));
	}

	return IRQ_HANDLED;
}

irqreturn_t wdog_snarl_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if (chg->wa_flags & SW_THERM_REGULATION_WA) {
		cancel_delayed_work_sync(&chg->thermal_regulation_work);
		vote(chg->awake_votable, SW_THERM_REGULATION_VOTER, true, 0);
		schedule_delayed_work(&chg->thermal_regulation_work, 0);
	}

	power_supply_changed(chg->batt_psy);

	return IRQ_HANDLED;
}

irqreturn_t wdog_bark_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_write(chg, BARK_BITE_WDOG_PET_REG, BARK_BITE_WDOG_PET_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't pet the dog rc=%d\n", rc);

	if (chg->step_chg_enabled)
		power_supply_changed(chg->batt_psy);

	return IRQ_HANDLED;
}

static void smblib_die_rst_icl_regulate(struct smb_charger *chg)
{
	int rc;
	u8 temp;

	rc = smblib_read(chg, DIE_TEMP_STATUS_REG, &temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DIE_TEMP_STATUS_REG rc=%d\n",
				rc);
		return;
	}

	/* Regulate ICL on die temp crossing DIE_RST threshold */
	vote(chg->usb_icl_votable, DIE_TEMP_VOTER,
				temp & DIE_TEMP_RST_BIT, 500000);
}

/*
 * triggered when DIE or SKIN or CONNECTOR temperature across
 * either of the _REG_L, _REG_H, _RST, or _SHDN thresholds
 */
irqreturn_t temp_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_die_rst_icl_regulate(chg);

	return IRQ_HANDLED;
}

static void op_get_aicl_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							get_aicl_work);
	int rc, settled_ua;

	rc = smblib_get_charge_param(chg, &chg->param.icl_stat,
				&settled_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
		return;
	}

	pr_err("AICL result=%dmA\n", settled_ua / 1000);
}

#define THIRD_LOOP_ENTER_MINI_THRESHOLD 35
#define THIRD_LOOP_ENTER_MAX_THRESHOLD 60
#define THIRD_INTERVAL_MINI_THRESHOLD 8
#define THIRD_INTERVAL_MAX_THRESHOLD 20
static void op_connecter_temp_check_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
				struct smb_charger, connecter_check_work);
	int batt_temp = 0, interval_temp = 0;
	int i = 0, loop_enter_temp = 0, third_interval_temp = 0;

	if (!chg->vbus_present)
		return;

	chg->connecter_temp = get_usb_temp(chg);
	batt_temp = get_prop_batt_temp(chg)/10;
	interval_temp = chg->connecter_temp - batt_temp;

	/* online config*/
	if ((*chg->usb_connector_temp >= THIRD_LOOP_ENTER_MINI_THRESHOLD) &&
		(*chg->usb_connector_temp <= THIRD_LOOP_ENTER_MAX_THRESHOLD))
		loop_enter_temp = *chg->usb_connector_temp;
	else
		loop_enter_temp = chg->third_protect_loop_temp;

	if ((*chg->usb_interval_temp >= THIRD_INTERVAL_MINI_THRESHOLD) &&
		(*chg->usb_interval_temp <= THIRD_INTERVAL_MAX_THRESHOLD))
		third_interval_temp = *chg->usb_interval_temp;
	else
		third_interval_temp = chg->third_protect_interval_temp;

	if (!chg->count_run) {/*count run state keep count_total not change*/
		if (chg->connecter_temp >= chg->fast_check_threshold_temp) {// 33
			chg->count_total =
			chg->high_temp_short_check_timeout / chg->fast_check_interval_period;
			smblib_dbg(chg, PR_FAST_DEBUG, "> %d! chg->count_total:%d",
					chg->fast_check_threshold_temp,
					chg->count_total);
		} else {
			chg->count_total =
			chg->high_temp_short_check_timeout / chg->normal_check_interval_period;
			smblib_dbg(chg, PR_FAST_DEBUG, "<= %d! chg->count_total:%d",
					chg->fast_check_threshold_temp,
					chg->count_total);
		}
	}

	smblib_dbg(chg, PR_FAST_DEBUG, "connecter_temp:%d,batt_temp:%d,interval_temp:%d,count_total:%d,count_run:%d,connector_short:%d\n",
			chg->connecter_temp,
			batt_temp,
			interval_temp,
			chg->count_total,
			chg->count_run,
			chg->connector_short);
	/*error:EOC bit not set! cause connector_temp=125 other 75*/
	if (chg->connecter_temp == 125 || chg->connecter_temp == 75) {
		for (i = 0; i <= 9; i++) {
			msleep(100);
			smblib_dbg(chg, PR_FAST_DEBUG,
			"EOC error!temp abormal delay count:%d\n",
					i);
		}
		schedule_delayed_work(&chg->connecter_check_work,
				msecs_to_jiffies(100));
		return; /*rerun check again*/
	}

	if (chg->connecter_temp >= chg->first_protect_connecter_temp) { /* >=60 */
		pr_info("connecter_temp=%d,connector_short=%d\n",
				chg->connecter_temp,
				chg->connector_short);
		op_disconnect_vbus(chg, true);
	} else {/*20<= ? < 60*/
		if ((interval_temp >= chg->second_protect_interval_temp)
			&& (chg->connecter_temp >=
			chg->second_protect_connecter_temp)) {
			/*interval > 14 && connecter > 45*/
			pr_info("interval_temp=%d,connecter_temp=%d\n",
					interval_temp,
					chg->connecter_temp);
			op_disconnect_vbus(chg, true);
			return;
		} else if (((interval_temp >= third_interval_temp) &&
			(chg->connecter_temp >= chg->third_protect_base_temp)) ||
			(chg->connecter_temp >= loop_enter_temp)) {
		/*interval >=8 && connecter >=20  or connecter >= 40 enter*/
			if (chg->count_run <= chg->count_total) {
			/*time out count*/
				if (chg->count_run == 0)
					chg->pre_temp = chg->connecter_temp;

				/* time out check MAX=count_total*/
				if (chg->count_run > 0) {
					chg->current_temp = chg->connecter_temp;
					if ((chg->current_temp -
						chg->pre_temp) >=
						chg->third_protect_rise_rate) { /* 3 degree per 1.5 senconds*/
						chg->connector_short = true;
						pr_info("cout_run=%d,short=%d\n",
							chg->count_run,
							chg->connector_short);
						op_disconnect_vbus(chg, true);
						return;
					}
				}

				chg->count_run++;/*count ++*/

				if (chg->count_run > chg->count_total) {
					chg->count_run = 0;
					smblib_dbg(chg, PR_FAST_DEBUG, "count reset!\n");
				}
			}
		} else {/*connecter <20 or connecter < 40 && interval < 8*/
			if (chg->count_run)/* high temp cold down count reset.*/
				chg->count_run = 0;
			smblib_dbg(chg, PR_FAST_DEBUG,
					"chg->dash_on:%d,connecter_temp:%d\n",
					chg->dash_on,
					chg->connecter_temp);
		}
	}

	if (chg->connecter_temp < chg->fast_check_threshold_temp)
		schedule_delayed_work(&chg->connecter_check_work,
			msecs_to_jiffies(chg->normal_check_interval_period));
	else if ((chg->connecter_temp >= chg->fast_check_threshold_temp)
		&& (chg->connecter_temp < chg->first_protect_connecter_temp))
		/*time need optimize depend on test*/
		schedule_delayed_work(&chg->connecter_check_work,
			msecs_to_jiffies(chg->fast_check_interval_period));
	else
		smblib_dbg(chg, PR_FAST_DEBUG, "connecter_temp:%d\n",
				chg->connecter_temp);
}

#define CONNECTER_RECOVERY_CHECK_INTERVAL 3000 //ms
static void op_connecter_recovery_charge_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
				struct smb_charger, connecter_recovery_work);
	int batt_temp = 0, interval_temp = 0;

	if (!chg->disconnect_vbus)
		return;

	chg->connecter_temp = get_usb_temp(chg);
	batt_temp = get_prop_batt_temp(chg)/10;
	interval_temp = chg->connecter_temp - batt_temp;

	pr_info("disconnect_vbus=%d, hw_detect=%d, connecter_temp=%d, interval_temp=%d",
			chg->disconnect_vbus, chg->hw_detect, chg->connecter_temp, interval_temp);

	if ((chg->hw_detect == 0) && (chg->connecter_temp <= 50) && (interval_temp <= 10)) {
		pr_info("Recovery Charge!\n");
		op_disconnect_vbus(chg, false);
		chg->connector_short = false;
		return;
	}

	schedule_delayed_work(&chg->connecter_recovery_work,
		msecs_to_jiffies(CONNECTER_RECOVERY_CHECK_INTERVAL));
}

static void norchg_check_skin_thermal_temp(struct smb_charger *chg)
{
	bool call_on;
	bool skin_high, skin_warm, skin_med;
	int ua = 0, icl = 0;
	union power_supply_propval psp_val = {0, };
	int vbus_mv = 0;
	int typec_mode = 0;
	int rp_ua = 0;
	int rc = 0;

	if (!chg->enable_nor_current_adjust) {
		pr_info("skin thermal for nor-chg path not enable.");
		return;
	}
	if (!chg->pd_active && !chg->qc_configed
		&& !is_charger_not_match(chg)) {
		if (chg->need_check_icl_lcdon) {
			chg->need_check_icl_lcdon = false;
			typec_mode = smblib_get_prop_typec_mode(chg);
			if (typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH ||
				typec_mode == POWER_SUPPLY_TYPEC_SOURCE_MEDIUM ||
				typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) {
				rp_ua = get_rp_based_dcp_current(chg, typec_mode);
				vote(chg->usb_icl_votable,
						SW_ICL_MAX_VOTER, true, rp_ua);
			}
		}
		return;
	}
	call_on = check_call_on_status() || check_video_call_on_status();
	skin_high = check_skin_thermal_high();
	skin_warm = check_skin_thermal_warm();
	skin_med = check_skin_thermal_medium();
	if (call_on)
		ua = chg->nor_call_on_current_ma * 1000;
	else if (skin_high)
		ua = chg->nor_skin_thermal_hi_current_ma * 1000;
	else if (skin_warm)
		ua = chg->nor_skin_thermal_wrm_current_ma * 1000;
	else if (skin_med)
		ua = chg->nor_skin_thermal_med_current_ma * 1000;

	if (call_on || skin_high || skin_warm || skin_med) {
		if (!call_on && (chg->qc_configed
			|| (chg->pd_9vsupported && chg->pd_active))) {
			rc = smblib_get_prop_usb_voltage_now(chg, &psp_val);
			if (rc < 0) {
				pr_err("get usb voltage error, rc=%d\r\n", rc);
				vbus_mv = 0;
			} else {
				vbus_mv = psp_val.intval;
			}
			if (vbus_mv > 7500)
				ua *= 2;
		}
		icl = get_client_vote(chg->fcc_votable, SKIN_THERMAL_FCC);
		if (ua != icl) {
			pr_info("NOR: skin_thermal_temp=(%d), skin_high(%d), skin_warm(%d), skin_med(%d), call_on(%d), ua(%d)\n",
				chg->skin_thermal_temp, skin_high, skin_warm,
				skin_med, call_on, ua);
				vote(chg->fcc_votable, SKIN_THERMAL_FCC, true, ua);
		}
	} else {
		if (is_client_vote_enabled(chg->fcc_votable, SKIN_THERMAL_FCC))
			vote(chg->fcc_votable, SKIN_THERMAL_FCC, false, 0);
	}
}

static void op_set_charger_vsys_threshold(struct smb_charger *chg, int val)
{
	int rc = 0;
	union power_supply_propval thld = {0, };

	thld.intval = val;
	if (is_op_chg_available(chg)) {
		rc = power_supply_set_property(chg->op_chg_psy,
				POWER_SUPPLY_PROP_VSYS_THD, &thld);
		if (rc)
			pr_err("op_chg psy does not allow set prop VSYS_THD=0x%02x rc = %d\n",
			val, rc);
	} else
		pr_err("no op_chg psy found\n");
}

static void op_en_charger_burst_mode(struct smb_charger *chg, bool en)
{
	int rc = 0;
	union power_supply_propval enable = {0, };

	enable.intval = en ? 1 : 0;
	if (is_op_chg_available(chg)) {
		rc = power_supply_set_property(chg->op_chg_psy,
				POWER_SUPPLY_PROP_EN_BURST, &enable);
		if (rc)
			pr_err("op_chg psy does not allow set prop EN_BURST=%d rc = %d\n",
			enable.intval, rc);
	} else
		pr_err("no op_chg psy found\n");
}

#define PD_CHECK_INTERVAL_PERIOD 5000
#define PD_9V_VBATT_THRESHOLD 4000
#define PD_5V_VBATT_THRESHOLD 4100
#define PD_9V_ENGINEER_MODE_VBATT_THRESHOLD 4100
static void pd_charge_status_check_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
				struct smb_charger, pd_status_check_work);
	static int count;
	int ret = 0;
	int rc, vbat_mv, vbus_mv;
	bool should_check_fastchg = false;
	union power_supply_propval pval = {0, };

	if (!chg->pd_active) {
		pr_err("PD not detected, exit!\n");
		return;
	}
	if (!chg->chg_enabled || chg->chg_disabled || chg->chg_done) {
		pr_info("charge disabled or chg_done, exit!\n");
		return;
	}
	if (chg->is_aging_test) {
		pr_info("is_aging_test exit!\n");
		return;
	}
	if ((chg->swarp_online || chg->dash_present)
		&& !chg->disallow_fast_high_temp
		&& !chg->fastchg_switch_disable) {
		pr_err("Fastcharge detected in normal temp region, exit!!\n");
		return;
	}

	if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE ||
		chg->typec_mode == POWER_SUPPLY_TYPEC_SINK) {
		pr_err("Sink powered cable connected, do not select PDO!");
		return;
	}

	if (op_battery_status_get(chg) != BATT_STATUS_GOOD) {
		pr_err("battery is not good, prevent pdo select");
		return;
	}

	if (chg->usb_enum_status && chg->pd_9vsupported) {
		vote(chg->usb_icl_votable,
				SW_ICL_MAX_VOTER, true, 1000000);
		pr_info("Enumeration done, do not select PDO!");
		return;
	}
	vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
	rc = smblib_get_prop_usb_voltage_now(chg, &pval);
	if (rc < 0) {
		pr_err("get usb voltage error, rc=%d\r\n", rc);
		vbus_mv = 0;
	} else {
		vbus_mv = pval.intval;
	}

	if (chg->pd_9vsupported) {
		if (vbat_mv < PD_9V_VBATT_THRESHOLD)
			chg->select_pdo_9v = 1;
		else if ((vbat_mv > PD_5V_VBATT_THRESHOLD) && chg->select_pdo_9v)
			chg->select_pdo_9v = 0;
		else if ((vbat_mv < PD_5V_VBATT_THRESHOLD) && chg->select_pdo_9v)
			chg->select_pdo_9v = 1;
		else
			chg->select_pdo_9v = 0;

		if (chg->engineer_mode && (vbat_mv < PD_9V_ENGINEER_MODE_VBATT_THRESHOLD)
			&& chg->pd_9vsupported) {
			chg->select_pdo_9v = 1;
			pr_err("Engineer mode:%d, vbat:%d, select 9v pdo\n", chg->engineer_mode, vbat_mv);
		} else if (chg->engineer_mode && vbat_mv >= PD_9V_ENGINEER_MODE_VBATT_THRESHOLD) {
			chg->select_pdo_9v = 0;
			pr_err("Engineer mode:%d, vbat:%d, select 5v pdo\n", chg->engineer_mode, vbat_mv);
		}

		if (chg->select_pdo_9v && vbus_mv < 7500) {
			chg->suspend_by_voltage_switch = true;
			(void)smblib_set_usb_suspend(chg, true);
			//set Vsys Skip threshold 104%
			op_set_charger_vsys_threshold(chg, 0x02);
			op_en_charger_burst_mode(chg, false);
			ret = op_pdo_select(9000, 3000);
			if (ret == 0) {
				pr_info("PD: Select 9V pdo.\n");
				vote(chg->usb_icl_votable,
					SW_ICL_MAX_VOTER, true, 3000000);
				count = 0;
				chg->is_power_changed = true;
				should_check_fastchg = true;
			} else {
				chg->select_pdo_9v = 0;
				if (count++ > 2)
					should_check_fastchg = true;
			}
			msleep(300);
			chg->suspend_by_voltage_switch = false;
			(void)smblib_set_usb_suspend(chg, false);
			op_en_charger_burst_mode(chg, true);
			rerun_election(chg->usb_icl_votable);
		} else if (!chg->select_pdo_9v && vbus_mv > 7500) {
			// force icl=500mA before switch to 5v.
			smblib_set_icl_current(chg, USBIN_500MA);
			chg->suspend_by_voltage_switch = true;
			smblib_set_usb_suspend(chg, true);
			//set Vsys Skip threshold 101%
			op_set_charger_vsys_threshold(chg, 0x03);
			ret = op_pdo_select(5000, 3000);
			if (ret == 0) {
				pr_info("PD: Select 5V pdo.\n");
				vote(chg->usb_icl_votable,
					SW_ICL_MAX_VOTER, true, 3000000);
				chg->is_power_changed = true;
			} else {
				chg->select_pdo_9v = 1;
			}
			msleep(300);
			chg->suspend_by_voltage_switch = false;
			smblib_set_usb_suspend(chg, false);
			op_en_charger_burst_mode(chg, true);
			rerun_election(chg->usb_icl_votable);
			if (chg->engineer_mode) {
				pr_info("Engineer mode exit 9v pdo.\n");
				return;
			}
		} else if (count > 0 && chg->select_pdo_9v && vbus_mv > 7500) {
			count = 0;
			chg->is_power_changed = true;
		}
		pr_info("PD: vbat_mv: %d, pdo_9v: %d, vbus_mv: %d, ret: %d\n",
			vbat_mv, chg->select_pdo_9v, vbus_mv, ret);
	} else {
		pr_info("not support pd 9v.");
		should_check_fastchg = true;
	}
	if (!chg->swarp_online
		&& !chg->pd_tried_warp_switch
		&& (should_check_fastchg
		|| vbat_mv > PD_9V_VBATT_THRESHOLD)) {
		// try to switch fastchg handshake
		chg->pd_tried_warp_switch = true;
		cancel_delayed_work(&chg->check_switch_dash_work);
		schedule_delayed_work(&chg->check_switch_dash_work, msecs_to_jiffies(1000));
	}
	if (count > 3) {
		pr_err("9V pdo select failed exceed 3times, exit!\n");
		count = 0;
		return;
	}
	if (!chg->pd_9vsupported) {
		pr_info("not support 9v pd, exit.");
		return;
	}
	schedule_delayed_work(&chg->pd_status_check_work,
			msecs_to_jiffies(PD_CHECK_INTERVAL_PERIOD));
}

static void op_set_pd_active_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
				struct smb_charger, op_pd_active_work);
	enum temp_region_type temp_region_now = BATT_TEMP_INVALID;

	if (!chg) {
		pr_err("smb_charge is not ready, return.");
		return;
	}
	smblib_set_prop_charge_parameter_set(chg);
	temp_region_now = op_battery_temp_region_get(chg);
	if (temp_region_now != BATT_TEMP_COLD
		&& temp_region_now != BATT_TEMP_HOT
		&& chg->typec_mode != POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE) {
		op_charging_en(chg, true);
	}
}

void op_set_swarp_charger(struct smb_charger *chg)
{
	chg->real_charger_type = POWER_SUPPLY_TYPE_DASH;
	enhance_dash_type_set(CHARGER_SWARP_65W);
}

static void pps_usbpd_connect_cb(struct usbpd_svid_handler *hdlr,
		bool peer_usb_comm)
{
	struct smb_charger *chg = g_chg;

	chg->swarp_online = 1;
	op_set_fast_chg_allow(chg, false);
	if (chg->chg_enabled) {
		if (!chg->fastchg_switch_disable && !chg->dash_present) {
			cancel_delayed_work(&chg->check_switch_dash_work);
			// delay 500ms, waiting for current greater than 200mA.
			schedule_delayed_work(&chg->check_switch_dash_work, msecs_to_jiffies(500));
		}
		op_set_swarp_charger(chg);
	}
	pr_info("SWARP adapter connected!\n");
}

static void pps_usbpd_disconnect_cb(struct usbpd_svid_handler *hdlr)
{

	//Reserve this function for now.
}

static void op_register_pps_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
				struct smb_charger, register_pps_work);
	struct op_pps *swarp_pps = &op_pps_chg;
	int rc = 0;
	const char *pd_phandle = "qcom,op-pps-usbpd-detection";
	struct usbpd *pd = NULL;

	pd = devm_usbpd_get_by_phandle(chg->dev, pd_phandle);
	if (IS_ERR(pd)) {
		pr_err("op pps usbpd phandle failed (%ld)\n", PTR_ERR(pd));
		rc = PTR_ERR(pd);
		schedule_delayed_work(&chg->register_pps_work, msecs_to_jiffies(1000));
	} else {
		swarp_pps->pd = pd;
		swarp_pps->svid_handler.svid = OP_SVID;
		swarp_pps->svid_handler.vdm_received = NULL;
		swarp_pps->svid_handler.connect = pps_usbpd_connect_cb;
		swarp_pps->svid_handler.svdm_received = NULL;
		swarp_pps->svid_handler.disconnect = pps_usbpd_disconnect_cb;
		rc = usbpd_register_svid(swarp_pps->pd, &swarp_pps->svid_handler);
		if (rc)
			pr_err("pps pd registration failed\n");
		pr_err("pps pd registration success\n");
	}

}

irqreturn_t smblib_handle_aicl_done(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	cancel_work_sync(&chg->get_aicl_work);
	schedule_work(&chg->get_aicl_work);

	return IRQ_HANDLED;
}

int op_get_aicl_result(struct smb_charger *chg)
{
	int icl_ma, rc;

	rc = smblib_get_charge_param(chg,
				&chg->param.icl_stat, &icl_ma);
	if (rc < 0) {
		pr_err("Couldn't get ICL status rc=%d\n", rc);
		return  -EINVAL;
	}

	pr_info("AICL result=%d\n", icl_ma);
	return icl_ma;
}
static bool op_get_fast_chg_status_is_ok(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->fast_chg_status_is_ok)
		return fast_charger->fast_chg_status_is_ok();
	pr_err("no fast_charger register found\n");
	return true;
}

static int get_property_from_fg(struct smb_charger *chg,
		enum power_supply_property prop, int *val)
{
	int rc;
	union power_supply_propval ret = {0, };

	if (!chg->bms_psy)
		chg->bms_psy = power_supply_get_by_name("bms");

	if (chg->bms_psy) {
		rc = power_supply_get_property(chg->bms_psy, prop, &ret);
		if (rc) {
			pr_err("bms psy doesn't support reading prop %d rc = %d\n",
				prop, rc);
			return rc;
		}
		*val = ret.intval;
	} else {
		pr_err("no bms psy found\n");
		return -EINVAL;
	}

	return rc;
}

static int set_property_on_fg(struct smb_charger *chg,
		enum power_supply_property prop, int val)
{
	int rc;
	union power_supply_propval ret = {0, };

	if (!chg->bms_psy)
		chg->bms_psy = power_supply_get_by_name("bms");

	if (chg->bms_psy) {
		ret.intval = val;
		rc = power_supply_set_property(chg->bms_psy, prop, &ret);
		if (rc)
			pr_err("bms psy does not allow updating prop %d rc = %d\n",
				prop, rc);
	} else {
		pr_err("no bms psy found\n");
		return -EINVAL;
	}

	return rc;
}

int op_charging_en(struct smb_charger *chg, bool en)
{
	int rc;
	union power_supply_propval en_chg = {0, };

	pr_info("enable=%d\n", en);
	if (chg->chg_disabled && en) {
		pr_info("chg_disabled just return\n");
		return 0;
	}

	if (chg->swarp_supported) {
		if (chg->disable_normal_chg_for_dash && en) {
			pr_info("chg disabled by fastchg, return.");
			return 0;
		}
		if (en && (op_battery_status_get(chg) == BATT_STATUS_BAD_VOL_DIFF)) {
			pr_err("battery dual-cell vol diff too large.");
			return -ENODEV;
		}
		if (is_op_chg_available(chg)) {
			en_chg.intval = en ? 1 : 0;
			rc = power_supply_set_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_CHARGE_ENABLED, &en_chg);
			if (rc)
				pr_err("can't set op_chg_psy prop CHARGE_ENABLED rc = %d\n", rc);
		} else {
			pr_err("no op_chg psy found\n");
			return -EINVAL;
		}
	} else
		rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT,
				 en ? CHARGING_ENABLE_CMD_BIT : 0);
	if (rc < 0) {
		pr_err("Couldn't %s charging rc=%d\n",
			en ? "enable" : "disable", rc);
		return rc;
	}

	return 0;
}

static bool is_usb_present(struct smb_charger *chg)
{
	int rc = 0;
	u8 stat;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		pr_err("Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return rc;
	}
	pr_debug("TYPE_C_STATUS_4 = 0x%02x\n", stat);
	return (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
}

static bool op_get_fast_low_temp_full(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->get_fast_low_temp_full)
		return fast_charger->get_fast_low_temp_full();
	pr_err("no fast_charger register found\n");
	return false;
}

static bool get_fastchg_firmware_updated_status(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->get_fastchg_firmware_already_updated)
		return fast_charger->get_fastchg_firmware_already_updated();
	pr_err("no fast_charger register found\n");
	return false;
}

static bool get_prop_fast_switch_to_normal(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->fast_switch_to_normal)
		return fast_charger->fast_switch_to_normal();
	pr_err("no fast_charger register found\n");
	return false;
}

bool is_fastchg_allowed(struct smb_charger *chg)
{
	int temp;
	int vbat_mv = 0, cap = 0;
	static int pre_temp;
	static bool pre_switch_to_normal;
	bool low_temp_full, switch_to_normal, fw_updated;

	temp = get_prop_batt_temp(chg);
	low_temp_full = op_get_fast_low_temp_full(chg);
	fw_updated = get_fastchg_firmware_updated_status(chg);
	vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
	cap = get_prop_batt_capacity(chg);

	if (!chg->chg_enabled)
		return false;
	if (chg->chg_disabled)
		return false;
	if (!fw_updated)
		return false;
	if (chg->usb_enum_status)
		return false;
	if (chg->support_hvdcp && chg->hvdcp_check) {
		pr_info("%s:check qc charger, return\n", __func__);
		return false;
	}
	if (temp < DASH_VALID_TEMP_LOW_THRESHOLD
			|| temp > DASH_VALID_TEMP_HIG_THRESHOLD) {
		if (temp != pre_temp)
			pr_err("temp=%d is not allow to switch fastchg\n", temp);
		pre_temp = temp;
		chg->disallow_fast_high_temp = true;
		return false;
	}

	if (chg->swarp_supported
		&& vbat_mv <= DASH_VALID_VBAT_LOW_THRE_MV) {
		pr_err("battery is too low to switch fastchg.");
		chg->disallow_fast_bad_vol = true;
		return false;
	}

	if (chg->swarp_online
		&& cap > DASH_VALID_CAPACITY_HIG_THRESHOLD) {
		pr_err("capacity high, swarp adapter.");
		return false;
	}

	switch_to_normal = get_prop_fast_switch_to_normal(chg);
	if (pre_switch_to_normal != switch_to_normal)
		pr_info("switch_to_normal =%d\n", switch_to_normal);
	if (switch_to_normal)
		return false;

	return true;
}

#ifdef OP_SWARP_SUPPORTED
bool is_charger_not_match(struct smb_charger *chg)
{
	if ((chg->swarp_supported)
		&& ((chg->fastchg_type == CHARGER_DASH)
		|| (chg->fastchg_type == CHARGER_WARP)
		|| (chg->fastchg_type == CHARGER_CAR)))
		return true;
	else
		return false;
}

void op_warp_config_for_swarp(void)
{
	struct smb_charger *chg = g_chg;

	if (is_charger_not_match(chg)) {
		vote(chg->fv_votable, DEFAULT_VOTER, true,
			chg->vbatmax[BATT_TEMP_NORMAL] * 1000);
		vote(chg->fcc_votable, DEFAULT_VOTER, true,
			chg->dash_ibatmax[BATT_TEMP_NORMAL] * 1000);
		vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
		vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
		vote(chg->usb_icl_votable, PD_VOTER, false, 0);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
			DEFAULT_WARP_FOR_SWARP_MA * 1000);
		chg->ffc_status = FFC_DEFAULT;
		chg->is_power_changed = true;
		chg->warp_charging_dual_cell = true;
	}
}

void op_pd_config_switch_normal(void)
{
	struct smb_charger *chg = g_chg;

	if (chg->pd_active) {
		op_pdo_select(5000, 3000);
		chg->is_power_changed = true;
	}
}

void op_set_hw_term_charging(bool en)
{
	struct smb_charger *chg = g_chg;
	union power_supply_propval en_term = {0, };
	int rc = 0;

	if (is_op_chg_available(chg)) {
		en_term.intval = en ? 1 : 0;
		rc = power_supply_set_property(chg->op_chg_psy,
				POWER_SUPPLY_PROP_HWTERM, &en_term);
		if (rc)
			pr_err("can't set op_chg_psy prop HWTERM rc = %d\n", rc);
	} else {
		pr_err("no op_chg psy found\n");
	}
}

void update_aicl_point_val(int bat_mv)
{
	struct smb_charger *chg = g_chg;
	union power_supply_propval bat_vol = {0, };
	int rc = 0;

	if (is_op_chg_available(chg)) {
		bat_vol.intval = bat_mv;
		rc = power_supply_set_property(chg->op_chg_psy,
				POWER_SUPPLY_PROP_VOLTAGE_MIN, &bat_vol);
		if (rc)
			pr_err("can't set op_chg_psy prop vimdpm rc = %d\n", rc);
	} else {
		pr_err("no op_chg psy found\n");
	}
}

#endif

void op_switch_normal_set(void)
{
	bool support_4p45;
	struct smb_charger *chg = g_chg;

	if (!chg)
		return;

	support_4p45 = get_4p45_battery_support();
	pr_info("switch_normal_set,support_4p45:%d\n",
			support_4p45);
#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported && chg->pd_active) {
		op_pdo_select(5000, 3000);
		msleep(300);
	}
#endif
	vote(chg->usb_icl_votable,
		DCP_VOTER, true, 2000 * 1000);

	if (support_4p45)
		vote(chg->fv_votable,
			DEFAULT_VOTER, true, 4550 * 1000);
	else
		vote(chg->fv_votable,
			DEFAULT_VOTER, true, 4500 * 1000);

	if (chg->wireless_present) {
		vote(chg->usb_icl_votable, WLCH_FFC_VOTER, true, 1000000);
		vote(chg->fcc_votable, DEFAULT_VOTER, true, 1500 * 1000);
	} else {
#ifdef OP_SWARP_SUPPORTED
		if (chg->swarp_supported)
			vote(chg->fcc_votable, DEFAULT_VOTER, true, chg->FFC_FAST_FCC * 1000);
		else
#endif
			vote(chg->fcc_votable, DEFAULT_VOTER, true, 1400 * 1000);
	}
	chg->ffc_status = FFC_FAST;
}

bool get_oem_charge_done_status(void)
{
#ifdef	CONFIG_OP_DEBUG_CHG
	return false;
#else
	if (!g_chg || g_chg->is_aging_test)
		return false;
#endif
	return (g_chg->chg_done || g_chg->recharge_status);
}
static bool ff_force;

static void op_handle_usb_removal(struct smb_charger *chg)
{
	op_set_fast_chg_allow(chg, false);
	set_prop_fast_switch_to_normal_false(chg);
	set_usb_switch(chg, false);
	set_dash_charger_present(false);
	op_clean_dash_status();
	ff_force = 0;

	chg->chg_ovp = false;
	chg->vbus_uovp_satus = false;
	chg->dash_on = false;
	chg->chg_done = false;
	chg->time_out = false;
	chg->recharge_status = false;
	chg->usb_enum_status = false;
	chg->non_std_chg_present = false;
	chg->usb_type_redet_done = false;
	chg->boot_usb_present = false;
	chg->revert_boost_trigger = false;
	chg->is_audio_adapter = false;
	chg->apsd_not_done = false;
	chg->slow_charger = false;
	chg->slow_chg_count = 0;
	chg->ffc_status = FFC_DEFAULT;
	chg->non_stand_chg_current = 0;
	chg->non_stand_chg_count = 0;
	chg->redet_count = 0;
	chg->dump_count = 0;
	chg->op_apsd_done = 0;
	chg->ck_dash_count = 0;
	chg->re_trigr_dash_done = 0;
	chg->recovery_boost_count = 0;
	chg->ck_unplug_count = 0;
	chg->count_run = 0;
	chg->chg_disabled = 0;
	chg->qc_configed = false;
	chg->qc2_uv_count = 0;
	chg->fastchg_switch_disable = false;
	chg->fastchg_present_wait_count = 0;
	chg->disable_normal_chg_for_dash = false;
	chg->suspend_by_voltage_switch = false;
	chg->hvdcp_check = false;
	chg->not_check_hvdcp_count = 0;
	chg->adapter_sid = 0;
	chg->ignore_irq_flag = false;
	chg->fastchg_type = CHARGER_DEFAULT;
	chg->disallow_fast_high_temp = false;
	chg->disallow_fast_bad_vol = false;
	chg->select_pdo_9v = 0;
	chg->swarp_online = 0;
	chg->warp_charging_dual_cell = false;
	chg->hw_term_enabled = false;
	chg->sw_aicl_prepared = false;
	chg->pd_tried_warp_switch = false;
	op_set_hw_term_charging(false);
	cancel_delayed_work_sync(&chg->pd_status_check_work);
#ifdef CONFIG_FORCE_FAST_CHARGE
	chg->ffc_count = 0;
	set_sdp_current(chg, USBIN_500MA);
#endif
	vote(chg->fcc_votable,
		DEFAULT_VOTER, true, SDP_CURRENT_UA);
	vote(chg->chg_disable_votable,
		FORCE_RECHARGE_VOTER, false, 0);
	vote(chg->usb_icl_votable,
		AICL_RERUN_VOTER, false, 0);
	vote(chg->fcc_votable,
		SKIN_THERMAL_FCC, false, 0);
	vote(chg->usb_icl_votable, QC_VOTER, false, 0);
	vote(chg->fcc_votable, QC_VOTER, false, 0);
	op_battery_temp_region_set(chg, BATT_TEMP_INVALID);
}

int update_dash_unplug_status(void)
{
	int rc;
	union power_supply_propval vbus_val;

	if (g_chg == NULL || (g_chg->is_aging_test && g_chg->ignore_irq_flag))
		return 0;

	rc = smblib_get_prop_usb_voltage_now(g_chg, &vbus_val);
	if (rc < 0)
		pr_err("failed to read usb_voltage rc=%d\n", rc);
	else if (vbus_val.intval <= 2500) {
		if (is_charger_not_match(g_chg))
			op_clean_dash_status();
		op_handle_usb_plugin(g_chg);
		smblib_update_usb_type(g_chg);
		power_supply_changed(g_chg->usb_psy);
	}

	return 0;
}
#define USBIN_AICL_RERUN_EN_BIT			BIT(4)

void op_check_charger_collapse_rerun_aicl(void)
{
	int rc;

	rc = smblib_masked_write(g_chg, USBIN_AICL_OPTIONS_CFG_REG,
	SUSPEND_ON_COLLAPSE_USBIN_BIT
	|USBIN_AICL_START_AT_MAX_BIT
	| USBIN_AICL_ADC_EN_BIT
	|USBIN_AICL_RERUN_EN_BIT, USBIN_AICL_RERUN_EN_BIT);
	if (rc < 0)
		dev_err(g_chg->dev,
			"Couldn't configure AICL rc=%d\n", rc);
	smblib_run_aicl(g_chg, RERUN_AICL);
	smblib_err(g_chg, "%s done\n", __func__);
}
static int op_set_collapse_fet(struct smb_charger *chg, bool on)
{
	int rc = 0;
	u8 stat;

	rc = smblib_masked_write(chg, USBIN_5V_AICL_THRESHOLD_CFG_REG,
		BIT(0) | BIT(1), on ? 0 : BIT(0) | BIT(1));
	if (rc < 0) {
		smblib_err(chg, "Couldn't write %s to 0x%x rc=%d\n",
			on ? "on" : "off", USBIN_5V_AICL_THRESHOLD_CFG_REG, rc);
		return rc;
	}

	rc = smblib_read(chg, USBIN_5V_AICL_THRESHOLD_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read 0x%x rc=%d\n",
			USBIN_5V_AICL_THRESHOLD_CFG_REG, rc);
		return rc;
	}
	pr_info("USBIN_5V_AICL_THRESHOLD_CFG_REG(0x%x)=0x%x\n",
			USBIN_5V_AICL_THRESHOLD_CFG_REG, stat);

	rc = smblib_masked_write(chg, USBIN_CONT_AICL_THRESHOLD_CFG_REG,
		BIT(0) | BIT(1), on ? 0 : BIT(0) | BIT(1));
	if (rc < 0) {
		smblib_err(chg, "Couldn't write %s to 0x%x rc=%d\n",
			on ? "on" : "off", USBIN_CONT_AICL_THRESHOLD_CFG_REG,
			rc);
		return rc;
	}

	rc = smblib_read(chg, USBIN_CONT_AICL_THRESHOLD_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read 0x%x rc=%d\n",
			USBIN_CONT_AICL_THRESHOLD_CFG_REG, rc);
		return rc;
	}
	pr_info("USBIN_CONT_AICL_THRESHOLD_CFG_REG(0x%x)=0x%x\n",
			USBIN_CONT_AICL_THRESHOLD_CFG_REG, stat);

	rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
					SUSPEND_ON_COLLAPSE_USBIN_BIT
					| USBIN_HV_COLLAPSE_RESPONSE_BIT
					| USBIN_LV_COLLAPSE_RESPONSE_BIT
					| USBIN_AICL_PERIODIC_RERUN_EN_BIT,
					on ? 0 : SUSPEND_ON_COLLAPSE_USBIN_BIT
					| USBIN_HV_COLLAPSE_RESPONSE_BIT
					| USBIN_LV_COLLAPSE_RESPONSE_BIT);
	if (rc < 0) {
		smblib_err(chg,
			"Couldn't write %s to 0x%x rc=%d\n",
			on ? "on" : "off", USBIN_AICL_OPTIONS_CFG_REG, rc);
		return rc;
	}

	rc = smblib_read(chg, USBIN_AICL_OPTIONS_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read 0x%x rc=%d\n",
			USBIN_AICL_OPTIONS_CFG_REG, rc);
		return rc;
	}
	pr_info("USBIN_AICL_OPTIONS_CFG_REG(0x%x)=0x%x\n",
		USBIN_AICL_OPTIONS_CFG_REG, stat);

	rc = smblib_masked_write(chg, USBIN_LOAD_CFG_REG, BIT(0)
			| BIT(1), on ? 0 : BIT(0) | BIT(1));
	if (rc < 0) {
		smblib_err(chg, "Couldn't write %s to 0x%x rc=%d\n",
			on ? "on" : "off", USBIN_LOAD_CFG_REG, rc);
		return rc;
	}

	rc = smblib_read(chg, USBIN_LOAD_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read 0x%x rc=%d\n",
			USBIN_LOAD_CFG_REG, rc);
		return rc;
	}
	pr_info("USBIN_LOAD_CFG_REG(0x%x)=0x%x\n",
			USBIN_LOAD_CFG_REG, stat);

	return rc;
}

int op_handle_switcher_power_ok(void)
{
	int rc;
	u8 stat;
	union power_supply_propval vbus_val;

	if (!g_chg)
		return 0;
	if (!(g_chg->wa_flags & BOOST_BACK_WA))
		return 0;
	rc = smblib_read(g_chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(g_chg,
			"Couldn't read POWER_PATH_STATUS rc=%d\n", rc);
		return 0;
	}
	smblib_err(g_chg, "POWER_PATH_STATUS stat=0x%x\n", stat);

	if ((stat & USE_USBIN_BIT) &&
		get_effective_result(g_chg->usb_icl_votable)
		< USBIN_25MA)
		return 0;

	if (stat & USE_DCIN_BIT)
		return 0;
	usleep_range(50000, 50002);
	rc = smblib_get_prop_usb_voltage_now(g_chg, &vbus_val);
	if (rc < 0) {
		pr_err("fail to read usb_voltage rc=%d\n", rc);
	} else if (vbus_val.intval >= 2500) {
		op_dump_regs(g_chg);
		pr_err("vbus_val.intval=%d\n", vbus_val.intval);
		vote(g_chg->usb_icl_votable, BOOST_BACK_VOTER, true, 0);
		schedule_delayed_work(&g_chg->recovery_suspend_work,
				msecs_to_jiffies(TIME_100MS));
		smblib_err(g_chg, "OP Reverse boost detected\n");
	}

	return 0;
}

int op_contrl(int enable, bool check_power_ok)
{
	pr_info("en=%d\n", enable);
	if (!g_chg)
		return 0;
	if (enable) {
		if (check_power_ok)
			op_handle_switcher_power_ok();
	} else {
		op_set_collapse_fet(g_chg, enable);
	}
	return 0;
}

static int op_suspend_disable_norchg_by_warp(bool enable)
{
	struct smb_charger *chg = g_chg;

	pr_info("en=%d\n", enable);
	if (!chg)
		return 0;
	if (!chg->disable_normal_chg_for_dash && enable) {
		op_charging_en(chg, false);
		smblib_set_usb_suspend(chg, true);
		chg->disable_normal_chg_for_dash = true;
		chg->tried_fastchg = true;
	}
	return 0;
}

int get_prop_fast_adapter_update(struct smb_charger *chg)
{
	int update_status;

	if (fast_charger && fast_charger->get_adapter_update)
		update_status = fast_charger->get_adapter_update();
	else {
		pr_err("no fast_charger register found\n");
		update_status = ADAPTER_FW_UPDATE_NONE;
	}
	return update_status;
}

bool get_prop_fast_chg_started(struct smb_charger *chg)
{
	if (get_prop_fast_adapter_update(chg)
			== ADAPTER_FW_NEED_UPDATE)
		return true;
	if (chg->ignore_irq_flag)
		return true;
	if (fast_charger && fast_charger->fast_chg_started)
		return fast_charger->fast_chg_started();
	pr_err("no fast_charger register found\n");
	return false;
}

void update_fast_switch_off_status(void)
{
	if (g_chg->switch_on_fastchg)
		g_chg->switch_on_fastchg = false;
	pr_info("switch_on_fastchg is(%d)\n",
				g_chg->switch_on_fastchg);
}

void update_disconnect_pd_status(bool en)
{
	if (g_chg == NULL) {
		pr_err("smbchg not found\n");
		return;
	}

	g_chg->disconnect_pd = en;
	power_supply_changed(g_chg->usb_psy);
	pr_info("disconnect_pd is(%d)\n", g_chg->disconnect_pd);
}

static bool set_prop_fast_switch_to_normal_false(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->set_switch_to_noraml_false)
		return fast_charger->set_switch_to_noraml_false();
	pr_err("no fast_charger register found\n");
	return false;
}

bool op_get_fastchg_ing(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->get_fast_chg_ing)
		return fast_charger->get_fast_chg_ing();
	pr_err("no fast_charger register found\n");
	return false;
}

bool op_set_fast_chg_allow(struct smb_charger *chg, bool enable)
{
	if (fast_charger && fast_charger->set_fast_chg_allow)
		return fast_charger->set_fast_chg_allow(enable);
	pr_err("no fast_charger register found\n");
	return false;
}

static void op_clean_dash_status(void)
{
	if (fast_charger && fast_charger->clean)
		return fast_charger->clean();
	pr_err("no fast_charger register found\n");
}

static bool op_get_fast_chg_allow(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->get_fast_chg_allow)
		return fast_charger->get_fast_chg_allow();
	pr_err("no fast_charger register found\n");
	return false;
}

static bool op_is_usb_switch_on(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->is_usb_switch_on)
		return fast_charger->is_usb_switch_on();

	pr_err("no fast_charger register found\n");
	return false;
}

static enum batt_status_type op_battery_status_get(struct smb_charger *chg)
{
	return chg->battery_status;
}

static enum temp_region_type op_battery_temp_region_get(struct smb_charger *chg)
{
	return chg->mBattTempRegion;
}

int fuelgauge_battery_temp_region_get(void)
{
	if (!g_chg)
		return BATT_TEMP_NORMAL;

	return op_battery_temp_region_get(g_chg);
}

static void op_battery_status_set(struct smb_charger *chg,
		enum batt_status_type battery_status)
{
	chg->battery_status = battery_status;
}

static void op_battery_temp_region_set(struct smb_charger *chg,
		enum temp_region_type batt_temp_region)
{
	chg->mBattTempRegion = batt_temp_region;
	pr_err("set temp_region=%d\n", chg->mBattTempRegion);
	if (batt_temp_region == BATT_TEMP_INVALID) {
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
	}
}

static void set_prop_batt_health(struct smb_charger *chg, int batt_health)
{
	chg->batt_health = batt_health;
}

static void set_usb_switch(struct smb_charger *chg, bool enable)
{
	int retrger_time;

	if (!fast_charger) {
		pr_err("no fast_charger register found\n");
		return;
	}
	if (chg->wireless_present) {
		pr_info("wireless charge, don't set usb switch\n");
		return;
	}

	if (enable) {
		if (chg->support_hvdcp && chg->hvdcp_check) {
			pr_info("%s:check qc charger, return\n", __func__);
			return;
		}
		if (chg->support_hvdcp)
			smblib_hvdcp_detect_enable(chg, false);
		pr_err("switch on fastchg\n");
		chg->switch_on_fastchg = true;
		chg->disconnect_pd = true;
		if (chg->boot_usb_present && chg->re_trigr_dash_done) {
			vote(chg->usb_icl_votable, AICL_RERUN_VOTER,
					true, 0);
			usleep_range(500000, 510000);
			vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
			vote(chg->usb_icl_votable, AICL_RERUN_VOTER,
					true, DEFAULT_DCP_MA*1000);
		}
		if (chg->swarp_supported) {
			(void)opchg_mcu_action(ACTION_MODE_SWITCH_WARP);
		} else {
			set_mcu_active(0);
			usleep_range(10000, 10002);
			usb_sw_gpio_set(1);
			usleep_range(10000, 10002);
			mcu_en_gpio_set(0);
		}
		if (chg->boot_usb_present)
			retrger_time = TIME_3S;
		else
			retrger_time = TIME_200MS;
		if (!chg->re_trigr_dash_done)
			schedule_delayed_work(&chg->rechk_sw_dsh_work,
					msecs_to_jiffies(retrger_time));
		if (!chg->swarp_online)
			chg->tried_fastchg = true;
	} else {
		pr_err("switch off fastchg\n");
		chg->switch_on_fastchg = false;
		update_disconnect_pd_status(false);
		if (chg->swarp_supported) {
			(void)opchg_mcu_action(ACTION_MODE_SWITCH_NORMAL);
		} else {
			usb_sw_gpio_set(0);
			mcu_en_gpio_set(1);
		}
	}
}

static void switch_fast_chg(struct smb_charger *chg)
{
	bool fastchg_allowed, is_allowed;
	static bool pre_fastchg_allowed, pre_is_allowed;

	mutex_lock(&chg->sw_dash_lock);
	if (op_is_usb_switch_on(chg)) {
		mutex_unlock(&chg->sw_dash_lock);
		return;
	}
	if (!is_usb_present(chg)) {
		mutex_unlock(&chg->sw_dash_lock);
		return;
	}

	fastchg_allowed = op_get_fast_chg_allow(chg);
	if (pre_fastchg_allowed != fastchg_allowed) {
		pre_fastchg_allowed = fastchg_allowed;
		pr_info("fastchg_allowed = %d\n", fastchg_allowed);
	}
	if (!fastchg_allowed) {
		is_allowed = is_fastchg_allowed(chg);
		if (pre_is_allowed != is_allowed) {
			pre_is_allowed = is_allowed;
			pr_info("is_allowed = %d\n", is_allowed);
		}
		if (is_allowed) {
			set_usb_switch(chg, true);
			op_set_fast_chg_allow(chg, true);
		}
	}
	mutex_unlock(&chg->sw_dash_lock);
}

static void op_re_kick_allowed_voltage(struct smb_charger  *chg)
{
	const struct apsd_result *apsd_result;

	if (!is_usb_present(chg))
		return;

	apsd_result = smblib_get_apsd_result(chg);
	if (apsd_result->bit == SDP_CHARGER_BIT)
		return;

	pr_info("re-kick allowed voltage\n");
	smblib_set_usb_pd_allowed_voltage(chg, MICRO_9V, MICRO_9V);
	msleep(500);
	smblib_set_usb_pd_allowed_voltage(chg, MICRO_5V, MICRO_5V);
}

static void op_re_kick_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			re_kick_work.work);

	if (chg->vbus_present) {
		op_re_kick_allowed_voltage(chg);
		schedule_delayed_work(&chg->check_switch_dash_work,
				msecs_to_jiffies(500));
	}
}

static void op_usb_remove_check_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			unplug_check_work.work);

	if (!chg->vbus_present) {
		chg->ck_unplug_count = 0;
		return;
	}
	if (chg->ck_unplug_count >= 4) {
		pr_info("op usb remove checked\n");
		update_dash_unplug_status();
	} else {
		chg->ck_unplug_count++;
		schedule_delayed_work(&chg->unplug_check_work,
				msecs_to_jiffies(TIME_100MS));
	}
}

static void retrigger_dash_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			rechk_sw_dsh_work.work);
	pr_debug("chg->ck_dash_count=%d\n", chg->ck_dash_count);
	if (chg->usb_enum_status)
		return;
	if (chg->dash_present) {
		chg->ck_dash_count = 0;
		return;
	}
	if (!chg->vbus_present) {
		chg->ck_dash_count = 0;
		return;
	}
	if (chg->chg_disabled) {
		chg->ck_dash_count = 0;
		return;
	}
	if (chg->wireless_present) {
		pr_info("wireless charge, return retrigger_dash\n");
		chg->ck_dash_count = 0;
		return;
	}

	if (op_battery_status_get(chg) != BATT_STATUS_GOOD) {
		pr_err("battery is not good, prevent retrigger_dash");
		chg->ck_dash_count = 0;
		return;
	}

	if (chg->support_hvdcp && chg->hvdcp_check) {
		pr_info("check qc charger, return\n");
		chg->ck_dash_count = 0;
		return;
	}

	if (chg->ck_dash_count == DASH_CHECK_COUNT) {
		pr_info("retrger dash\n");
		chg->re_trigr_dash_done = true;
		set_usb_switch(chg, false);
		set_usb_switch(chg, true);
	} else if (chg->ck_dash_count == 2 * DASH_CHECK_COUNT) {
		set_usb_switch(chg, false);
	} else if (chg->ck_dash_count > 2 * DASH_CHECK_COUNT) {
		chg->fastchg_switch_disable = true;
		if (chg->support_hvdcp &&
		    (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP ||
		     chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3 ||
		     chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5 ||
		     chg->real_charger_type == POWER_SUPPLY_TYPE_USB_DCP)) {
			chg->hvdcp_check = true;
			smblib_hvdcp_detect_enable(chg, true);
			smblib_request_dpdm(chg, true);
			smblib_rerun_apsd(chg);
		}
		if (chg->swarp_online && chg->pd_9vsupported) {
			cancel_delayed_work(&chg->pd_status_check_work);
			schedule_delayed_work(&chg->pd_status_check_work,
					msecs_to_jiffies(2000));
		}
		if (chg->swarp_supported)
			rerun_election(chg->usb_icl_votable);
		return;
	}

	chg->ck_dash_count++;
	schedule_delayed_work(&chg->rechk_sw_dsh_work,
			msecs_to_jiffies(TIME_200MS));
}

static void op_check_slow_charge_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			slow_chg_check_work.work);
	union power_supply_propval vbus_val;
	int rc, ibat, temp_region, soc;
	int suspend;
	const struct apsd_result *apsd_result;

	pr_info("chg->slow_chg_count=%d\n", chg->slow_chg_count);
	soc = get_prop_batt_capacity(chg);
	if (chg->wireless_present
		|| chg->usb_enum_status
		|| soc > 80
		|| chg->chg_done
		|| chg->chg_disabled
		|| chg->dash_present) {
		chg->slow_chg_count = 0;
		chg->slow_charger = false;
		return;
	}
	temp_region = op_battery_temp_region_get(chg);
	if (temp_region == BATT_TEMP_COLD
		|| temp_region == BATT_TEMP_HOT
		|| temp_region == BATT_TEMP_LITTLE_COLD) {
		chg->slow_chg_count = 0;
		chg->slow_charger = false;
		return;
	}
	apsd_result = smblib_update_usb_type(chg);
	if (apsd_result->bit == SDP_CHARGER_BIT
			|| apsd_result->bit == CDP_CHARGER_BIT) {
		chg->slow_chg_count = 0;
		chg->slow_charger = false;
		return;
	}
	rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
	if (rc < 0) {
		chg->slow_chg_count = 0;
		chg->slow_charger = false;
		pr_info("failed to read usb_voltage rc=%d\n", rc);
		return;
	}
	if (vbus_val.intval < 2500) {
		pr_info("vbus less 2.5v\n");
		chg->slow_chg_count = 0;
		chg->slow_charger = false;
		return;
	}
	rc = smblib_get_usb_suspend(chg, &suspend);
	if (rc < 0) {
		pr_info("%s,Couldn't get usb suspend rc=%d\n", __func__, rc);
		return;
	}
	if (suspend) {
		chg->slow_chg_count = 0;
		chg->slow_charger = false;
		pr_info("usb suspend, return\n");
		return;
	}
	ibat = get_prop_batt_current_now(chg);
	if (-ibat > USBIN_500MA) {
		chg->slow_chg_count = 0;
		chg->slow_charger = false;
		pr_info("ibat is %d\n", ibat);
		return;
	}
	if (chg->slow_chg_count >= SLOW_CHG_CHECK_COUTNT) {
		pr_info("slow charge detected\n");
		chg->slow_chg_count = 0;
		chg->slow_charger = true;
		power_supply_changed(chg->batt_psy);
	} else {
		chg->slow_chg_count++;
		schedule_delayed_work(&chg->slow_chg_check_work,
				msecs_to_jiffies(400));
	}
}

static void op_chek_apsd_done_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			op_check_apsd_work.work);
	union power_supply_propval vbus_val;
	int rc;
	const struct apsd_result *apsd_result = NULL;

	pr_debug("chg->ck_apsd_count=%d\n", chg->ck_apsd_count);
	if (chg->pd_active) {
		chg->ck_apsd_count = 0;
		pr_info("pd_active  return\n");
		return;
	}
	if (chg->wireless_present || !chg->check_slow_charge) {
		chg->ck_apsd_count = 0;
		return;
	}
	apsd_result = smblib_get_apsd_result(chg);
	if (chg->usb_enum_status || (apsd_result->bit && chg->typec_mode)) {
		chg->ck_apsd_count = 0;
		return;
	}
	rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
	if (rc < 0) {
		chg->ck_apsd_count = 0;
		pr_info("failed to read usb_voltage rc=%d\n", rc);
		return;
	}
	if (vbus_val.intval < 2500) {
		pr_info("vbus less 2.5v\n");
		chg->ck_apsd_count = 0;
		return;
	}
	if (apsd_result->bit && chg->typec_mode) {
		chg->ck_apsd_count = 0;
		return;
	}

	if (chg->ck_apsd_count >= APSD_CHECK_COUTNT) {
		pr_info("apsd done error\n");
		chg->ck_apsd_count = 0;
		op_dump_regs(chg);
		chg->apsd_not_done = true;
		if (!apsd_result->bit)
			op_rerun_apsd(chg);
	} else {
		chg->ck_apsd_count++;
		schedule_delayed_work(&chg->op_check_apsd_work,
				msecs_to_jiffies(TIME_1000MS));
	}
}

static void op_recovery_usb_suspend_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			recovery_suspend_work.work);
		int effect_result;

		if (chg->recovery_boost_count >= BOOST_BACK_COUNT
			|| (!is_usb_present(chg))) {
			pr_info("recovery revert boost\n");
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			if (is_usb_present(chg)) {
				effect_result =
				get_effective_result(chg->usb_icl_votable);
				pr_info("effect_result=%d\n", effect_result);
				if (effect_result
					> DEFAULT_AGAING_CHG_MA*1000) {
					vote(chg->usb_icl_votable, DCP_VOTER,
					true, (effect_result - USBIN_150MA));
					smblib_run_aicl(chg, RERUN_AICL);
				}
			}
			msleep(1000);
			chg->revert_boost_trigger = false;
		} else {
			chg->recovery_boost_count++;
			schedule_delayed_work(&chg->recovery_suspend_work,
					msecs_to_jiffies(TIME_100MS));
		}
}

static void op_check_allow_switch_dash_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
			struct smb_charger, check_switch_dash_work);
	const struct apsd_result *apsd_result;

	pr_debug("iusb_present=%d, usb_enum_status=%d, wireless_present=%d, apsd_delayed=%d, fastchg_switch_disable=%d, pd_active=%d\n",
		 is_usb_present(chg), chg->usb_enum_status,
		 chg->wireless_present, chg->apsd_delayed,
		 chg->fastchg_switch_disable, chg->pd_active);

	if (!is_usb_present(chg))
		return;
	if (chg->usb_enum_status)
		return;
	if (chg->wireless_present)
		return;
	if (chg->apsd_delayed)
		return;
	if (chg->fastchg_switch_disable)
		return;
	if (op_battery_status_get(chg) != BATT_STATUS_GOOD) {
		pr_err("battery is not good, prevent fastchg");
		return;
	}

	if (chg->pd_active) {
		switch_fast_chg(chg);
		return;
	}

	apsd_result = smblib_get_apsd_result(chg);
	if (((apsd_result->bit != SDP_CHARGER_BIT
		&& apsd_result->bit != CDP_CHARGER_BIT)
		&& apsd_result->bit)
		|| chg->non_std_chg_present)
		switch_fast_chg(chg);
}

int check_allow_switch_dash(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	if (val->intval < 0)
		return -EINVAL;

	schedule_delayed_work(&chg->check_switch_dash_work,
				msecs_to_jiffies(500));
	return 0;
}

#define DEFAULT_WALL_CHG_MA	1800
static int set_dash_charger_present(int status)
{
	int charger_present;
	bool pre_dash_present;

	if (g_chg) {
		pre_dash_present = g_chg->dash_present;
		charger_present = is_usb_present(g_chg);
		g_chg->dash_present = status && charger_present;
		if (g_chg->dash_present && !pre_dash_present) {
			pr_err("set dash online\n");
			g_chg->usb_psy_desc.type = POWER_SUPPLY_TYPE_DASH;
			vote(g_chg->usb_icl_votable, PD_VOTER, true,
					DEFAULT_WALL_CHG_MA * 1000);
			g_chg->slow_chg_count = 0;
			g_chg->slow_charger = false;
			g_chg->apsd_not_done = false;
		}
		if (g_chg->dash_present) {
			g_chg->real_charger_type = POWER_SUPPLY_TYPE_DASH;
			g_chg->usb_psy_desc.type = POWER_SUPPLY_TYPE_DASH;
		}
		power_supply_changed(g_chg->batt_psy);
		pr_info("dash_present = %d, charger_present = %d\n",
				g_chg->dash_present, charger_present);
	} else {
		pr_err("set dash charger present error\n");
	}

	return 0;
}

#ifdef OP_SWARP_SUPPORTED
static int update_fastchg_type(enum fast_charger_type type)
{
	if (g_chg)
		g_chg->fastchg_type = type;
	else
		pr_err("update fast charger type error\n");
	return 0;
}
#endif

static int update_adapter_sid(unsigned int sid)
{
	if (g_chg == NULL) {
		pr_err("smbchg not found\n");
		return -ENODEV;
	}

	g_chg->adapter_sid = sid;
	return 0;
}

static void op_otg_icl_contrl(struct smb_charger *chg)
{
	int cap, rc = 0;
	static int icl_pre, icl;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval pval = {0, };
#endif

	if (!chg->OTG_ICL_CTRL)
		return;
	cap = get_prop_batt_capacity(chg);
	if (cap <= chg->OTG_LOW_BAT) {
		icl =  chg->OTG_LOW_BAT_ICL;
	} else if (cap > chg->OTG_LOW_BAT
		&& icl_pre == chg->OTG_LOW_BAT_ICL) {
		icl =  chg->OTG_NORMAL_BAT_ICL;
	}

	if (icl_pre == icl)
		return;
	pr_info("cap=%d,icl=%d,icl_pre=%d\n", cap, icl, icl_pre);
#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			pval.intval = icl;
			rc =  power_supply_set_property(chg->op_chg_psy,
				POWER_SUPPLY_PROP_OTG_OCL, &pval);
			if (rc)
				smblib_err(chg, "Couldn't set OTG_ICL property, rc=%d\n", rc);
		} else {
			smblib_err(chg, "It's no op_psy.\n");
		}
	} else {
#endif
		rc = smblib_set_charge_param(chg, &chg->param.otg_cl, icl);
		if (rc < 0)
			smblib_err(chg, "Couldn't set otg icl rc=%d\n", rc);
#ifdef OP_SWARP_SUPPORTED
	}
#endif
	icl_pre = icl;
}

#ifndef CONFIG_OP_DEBUG_CHG
static void op_check_charge_timeout(struct smb_charger *chg)
{
	static int batt_status, count;

	if (chg->chg_done || chg->is_aging_test)
		return;

	batt_status = get_prop_batt_status(chg);
	if (chg->vbus_present
			&& batt_status == POWER_SUPPLY_STATUS_CHARGING)
		count++;
	else
		count = 0;

	if (count > CHG_TIMEOUT_COUNT) {
		pr_err("chg timeout! stop chaging now\n");
		op_charging_en(chg, false);
		chg->time_out = true;
	}
}
#endif

static int get_prop_batt_present(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATIF_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		pr_err("Couldn't read BATIF_INT_RT_STS rc=%d\n", rc);
		return rc;
	}

	return !(stat & (BAT_THERM_OR_ID_MISSING_RT_STS_BIT
					| BAT_TERMINAL_MISSING_RT_STS_BIT));
}

#define DEFAULT_BATT_CAPACITY	50
static int get_prop_batt_capacity(struct smb_charger *chg)
{
	int capacity, rc;

	if (chg->fake_capacity >= 0)
		return chg->fake_capacity;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_CAPACITY, &capacity);
	if (rc) {
		pr_err("Couldn't get capacity rc=%d\n", rc);
		capacity = DEFAULT_BATT_CAPACITY;
	}

	return capacity;
}

#define DEFAULT_BATT_TEMP		200
static int get_prop_batt_temp(struct smb_charger *chg)
{
	int temp, rc;

	if (chg->use_fake_temp)
		return chg->fake_temp;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_TEMP, &temp);
	if (rc) {
		pr_err("Couldn't get temperature rc=%d\n", rc);
		temp = DEFAULT_BATT_TEMP;
	}

	return temp;
}

#define DEFAULT_BATT_CURRENT_NOW	0
static int get_prop_batt_current_now(struct smb_charger *chg)
{
	int ua, rc;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_CURRENT_NOW, &ua);
	if (rc) {
		pr_err("Couldn't get current rc=%d\n", rc);
		ua = DEFAULT_BATT_CURRENT_NOW;
	}
	if (-ua > USBIN_500MA)
		chg->slow_charger = false;
	return ua;
}

#define DEFAULT_BATT_VOLTAGE_NOW	0
static int get_prop_batt_voltage_now(struct smb_charger *chg)
{
	int uv, rc;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_VOLTAGE_NOW, &uv);
	if (rc) {
		pr_err("Couldn't get voltage rc=%d\n", rc);
		uv = DEFAULT_BATT_VOLTAGE_NOW;
	}

	return uv;
}

static int get_prop_fg_capacity(struct smb_charger *chg)
{
	int capacity = 0, rc;

	if (chg->fake_capacity >= 0)
		return chg->fake_capacity;

	rc = get_property_from_fg(chg,
		POWER_SUPPLY_PROP_FG_CAPACITY, &capacity);
	if (rc) {
		pr_err("Couldn't get capacity rc=%d\n", rc);
		capacity = DEFAULT_BATT_CAPACITY;
	}

	return capacity;
}

static int get_prop_fg_current_now(struct smb_charger *chg)
{
	int ua = 0, rc;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_FG_CURRENT_NOW, &ua);
	if (rc) {
		pr_err("Couldn't get current rc=%d\n", rc);
		ua = DEFAULT_BATT_CURRENT_NOW;
	}

	return ua;
}

static int get_prop_fg_voltage_now(struct smb_charger *chg)
{
	int uv, rc;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_FG_VOLTAGE_NOW, &uv);
	if (rc) {
		pr_err("Couldn't get voltage rc=%d\n", rc);
		uv = DEFAULT_BATT_VOLTAGE_NOW;
	}

	return uv;
}

int get_prop_batt_status(struct smb_charger *chg)
{
	int capacity, batt_status, rc;
	enum temp_region_type temp_region;
	union power_supply_propval pval = {0, };
	bool wlchg_disable_batt_charge = false;
	bool wlchg_online = false;

	if (!chg->wls_psy)
		chg->wls_psy = power_supply_get_by_name("wireless");
	if (chg->wls_psy) {
		rc = power_supply_get_property(chg->wls_psy,
					       POWER_SUPPLY_PROP_OP_DISABLE_CHARGE,
					       &pval);
		if (rc < 0)
			smblib_err(chg, "Couldn't get wlchg batt status rc=%d\n", rc);
		else
			wlchg_disable_batt_charge = pval.intval;
		pval.intval = 0;

		rc = power_supply_get_property(chg->wls_psy,
						POWER_SUPPLY_PROP_ONLINE,
						&pval);
		if (rc < 0)
			smblib_err(chg, "Couldn't get wlchg online status rc=%d\n", rc);
		else
			wlchg_online = pval.intval;
		pval.intval = 0;
	}

	temp_region = op_battery_temp_region_get(chg);
	capacity = get_prop_batt_capacity(chg);
	chg->dash_on = get_prop_fast_chg_started(chg);
	if ((chg->chg_done || chg->recharge_status)
			&& (temp_region == BATT_TEMP_COOL
			|| temp_region == BATT_TEMP_LITTLE_COOL
			|| temp_region == BATT_TEMP_PRE_NORMAL
			|| temp_region == BATT_TEMP_NORMAL)
			&&	capacity == 100) {
		return POWER_SUPPLY_STATUS_FULL;
	} else if (chg->dash_on && chg->dash_present) {
		return POWER_SUPPLY_STATUS_CHARGING;
	}
	if (chg->revert_boost_trigger && chg->vbus_present)
		return POWER_SUPPLY_STATUS_CHARGING;
	if ((chg->wireless_present || wlchg_online) && !wlchg_disable_batt_charge)
		return POWER_SUPPLY_STATUS_CHARGING;

	rc = smblib_get_prop_batt_status(chg, &pval);
	if (rc)
		batt_status = 0;
	else
		batt_status = pval.intval;

	return batt_status;
}

int op_get_batt_status(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	bool usb_online, dc_online;
	u8 stat;
	int rc, suspend = 0;

	if (chg->fake_chg_status_on_debug_batt) {
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_DEBUG_BATTERY, &pval);
		if (rc < 0) {
			pr_err_ratelimited("Couldn't get debug battery prop rc=%d\n",
					rc);
		} else if (pval.intval == 1) {
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
			return 0;
		}
	}

	rc = smblib_get_prop_batt_health(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get batt health rc=%d\n", rc);
		return rc;
	}
	/*
	 * The charger status register shows charging even though the battery
	 * is discharging when the over voltage condition is hit. Report power
	 * supply state as NOT_CHARGING when the battery health reports
	 * over voltage.
	 */
	if (pval.intval == POWER_SUPPLY_HEALTH_OVERVOLTAGE) {
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	}

	if (chg->dbc_usbov) {
		rc = smblib_get_prop_usb_present(chg, &pval);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't get usb present prop rc=%d\n", rc);
			return rc;
		}

		rc = smblib_get_usb_suspend(chg, &suspend);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't get usb suspend rc=%d\n", rc);
			return rc;
		}

		/*
		 * Report charging as long as USBOV is not debounced and
		 * charging path is un-suspended.
		 */
		if (pval.intval && !suspend) {
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			return 0;
		}
	}

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported && is_op_chg_available(chg)) {
		rc = power_supply_get_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_STATUS, val);
		if (rc)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		return rc;
	}
#endif

	rc = smblib_get_prop_usb_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb online property rc=%d\n",
			rc);
		return rc;
	}
	usb_online = (bool)pval.intval;

	rc = smblib_get_prop_dc_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get dc online property rc=%d\n",
			rc);
		return rc;
	}
	dc_online = (bool)pval.intval;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}
	stat = stat & BATTERY_CHARGER_STATUS_MASK;

	if (!usb_online && !dc_online) {
		switch (stat) {
		case TERMINATE_CHARGE:
		case INHIBIT_CHARGE:
			val->intval = POWER_SUPPLY_STATUS_FULL;
			break;
		default:
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		}
		return rc;
	}

	switch (stat) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
	case FULLON_CHARGE:
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case TERMINATE_CHARGE:
	case INHIBIT_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_FULL;
		break;
	case DISABLE_CHARGE:
	case PAUSE_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	if (is_charging_paused(chg)) {
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		return 0;
	}

	/*
	 * If charge termination WA is active and has suspended charging, then
	 * continue reporting charging status as FULL.
	 */
	if (is_client_vote_enabled_locked(chg->usb_icl_votable,
						CHG_TERMINATION_VOTER)) {
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}

	if (val->intval != POWER_SUPPLY_STATUS_CHARGING)
		return 0;

	if (!usb_online && dc_online
		&& chg->fake_batt_status == POWER_SUPPLY_STATUS_FULL) {
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_5_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
				rc);
			return rc;
	}

	stat &= ENABLE_TRICKLE_BIT | ENABLE_PRE_CHARGING_BIT |
						ENABLE_FULLON_MODE_BIT;

	if (!stat)
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

int get_charging_status(void)
{
	int rc;
	union power_supply_propval pval = {0, };

	if (!g_chg)
		return POWER_SUPPLY_STATUS_DISCHARGING;

	rc = op_get_batt_status(g_chg, &pval);
	if (rc)
		return POWER_SUPPLY_STATUS_UNKNOWN;

	return pval.intval;
}

void set_chg_ibat_vbat_max(
	struct smb_charger *chg, int ibat, int vfloat)
{
	enum temp_region_type temp_region;

	pr_err("set ibatmax=%d and set vbatmax=%d\n",
			ibat, vfloat);
	if (chg->ffc_status != FFC_DEFAULT)
		return;

	vote(chg->fv_votable,
			DEFAULT_VOTER, true, vfloat * 1000);
	temp_region = op_battery_temp_region_get(chg);
	if (chg->wireless_present &&
	    temp_region != BATT_TEMP_COLD &&
	    temp_region != BATT_TEMP_LITTLE_COLD &&
	    temp_region != BATT_TEMP_HOT) {
		vote(chg->fcc_votable, DEFAULT_VOTER, false, 0);
	} else if ((!chg->pd_9vsupported && chg->pd_active) &&
		temp_region == BATT_TEMP_NORMAL) {
		vote(chg->fcc_votable,
				DEFAULT_VOTER, true, PD_PANELOFF_CURRENT_UA);
	} else {
		vote(chg->fcc_votable,
			DEFAULT_VOTER, true, ibat * 1000);
	}

	if (chg->swarp_supported)
		rerun_election(chg->usb_icl_votable);
	/* set cc to cv 100mv lower than vfloat */
	set_property_on_fg(chg, POWER_SUPPLY_PROP_CC_TO_CV_POINT, vfloat - 100);
}

static void op_temp_region_charging_en(struct smb_charger *chg, int vbatmax)
{
	int vbat_mv = 0;

	vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
	pr_info("%s vbat_mv =%d\n", __func__, vbat_mv);
	if (vbat_mv < vbatmax)
		return;
	op_charging_en(chg, false);
	chg->chg_done = true;
}

/* Tbatt < -3C */
static int handle_batt_temp_cold(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_COLD || chg->is_power_changed) {
		pr_err("triggered\n");
		chg->is_power_changed = false;

		op_charging_en(chg, false);
		op_battery_temp_region_set(chg, BATT_TEMP_COLD);

		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0 + BATT_TEMP_HYST;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
		set_prop_batt_health(chg, POWER_SUPPLY_HEALTH_COLD);
	}

	return 0;
}

/* -3C <= Tbatt <= 0C */
static int handle_batt_temp_little_cold(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_LITTLE_COLD
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->recharge_pending = false;
		chg->is_power_changed = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);
		op_battery_temp_region_set(chg, BATT_TEMP_LITTLE_COLD);
		op_temp_region_charging_en(chg,
				chg->vbatmax[BATT_TEMP_LITTLE_COLD]);
#ifdef OP_SWARP_SUPPORTED
		if (is_charger_not_match(chg) || chg->qc_configed
			|| (chg->pd_9vsupported && chg->pd_active))
			set_chg_ibat_vbat_max(chg,
				chg->setting_ibatmax[BATT_TEMP_LITTLE_COLD],
				chg->vbatmax[BATT_TEMP_LITTLE_COLD]);
		else
#endif
			set_chg_ibat_vbat_max(chg,
				chg->ibatmax[BATT_TEMP_LITTLE_COLD],
				chg->vbatmax[BATT_TEMP_LITTLE_COLD]);
		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1 + BATT_TEMP_HYST;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
		set_prop_batt_health(chg, POWER_SUPPLY_HEALTH_GOOD);
	}

	return 0;
}

/* 0C < Tbatt <= 5C*/
static int handle_batt_temp_cool(struct smb_charger *chg)
{
	enum temp_region_type temp_region;
	int  vbat_mv;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_COOL
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->recharge_pending = false;
		chg->is_power_changed = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);
		op_battery_temp_region_set(chg, BATT_TEMP_COOL);
#ifdef OP_SWARP_SUPPORTED
		if (is_charger_not_match(chg) || chg->qc_configed
			|| (chg->pd_9vsupported && chg->pd_active)) {
			set_chg_ibat_vbat_max(chg,
				chg->setting_ibatmax[BATT_TEMP_COOL],
				chg->vbatmax[BATT_TEMP_COOL]);
		} else {
#endif
			vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
			if (vbat_mv > chg->temp_cool_voltage) {
				set_chg_ibat_vbat_max(chg, chg->temp_cool_current,
						chg->vbatmax[BATT_TEMP_COOL]);
				chg->temp_littel_cool_set_current_0_point_25c = false;
			} else {
				set_chg_ibat_vbat_max(chg,
						chg->ibatmax[BATT_TEMP_COOL],
						chg->vbatmax[BATT_TEMP_COOL]);
				chg->temp_littel_cool_set_current_0_point_25c = true;
			}
#ifdef OP_SWARP_SUPPORTED
		}
#endif
		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2 + BATT_TEMP_HYST;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
		set_prop_batt_health(chg, POWER_SUPPLY_HEALTH_GOOD);
	}

	return 0;
}
/* 5C < Tbatt <= 12C */
static int handle_batt_temp_little_cool(struct smb_charger *chg)
{
	int temp_region, vbat_mv;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_LITTLE_COOL
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->recharge_pending = false;
		chg->is_power_changed = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);
		op_battery_temp_region_set(chg, BATT_TEMP_LITTLE_COOL);
#ifdef OP_SWARP_SUPPORTED
		if (is_charger_not_match(chg) || chg->qc_configed
			|| (chg->pd_9vsupported && chg->pd_active)) {
			set_chg_ibat_vbat_max(chg,
				chg->setting_ibatmax[BATT_TEMP_LITTLE_COOL],
				chg->vbatmax[BATT_TEMP_LITTLE_COOL]);
		} else {
#endif
			vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
			if (vbat_mv > chg->temp_littel_cool_voltage) {
				set_chg_ibat_vbat_max(chg, chg->temp_littel_cool_current,
						chg->vbatmax[BATT_TEMP_LITTLE_COOL]);
				chg->temp_littel_cool_set_current_0_point_25c = false;
			} else {
				set_chg_ibat_vbat_max(chg,
						chg->ibatmax[BATT_TEMP_LITTLE_COOL],
						chg->vbatmax[BATT_TEMP_LITTLE_COOL]);
				chg->temp_littel_cool_set_current_0_point_25c = true;
			}
#ifdef OP_SWARP_SUPPORTED
		}
#endif
		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3 + BATT_TEMP_HYST;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
		set_prop_batt_health(chg, POWER_SUPPLY_HEALTH_GOOD);
	}

	return 0;
}

/* 12C < Tbatt < 22C */
static int handle_batt_temp_prenormal(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_PRE_NORMAL
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->recharge_pending = false;
		chg->is_power_changed = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);
		op_battery_temp_region_set(chg, BATT_TEMP_PRE_NORMAL);
#ifdef OP_SWARP_SUPPORTED
		if (is_charger_not_match(chg) || chg->qc_configed
			|| (chg->pd_9vsupported && chg->pd_active))
			set_chg_ibat_vbat_max(chg,
				chg->setting_ibatmax[BATT_TEMP_PRE_NORMAL],
				chg->vbatmax[BATT_TEMP_PRE_NORMAL]);
		else
#endif
			set_chg_ibat_vbat_max(chg,
				chg->ibatmax[BATT_TEMP_PRE_NORMAL],
				chg->vbatmax[BATT_TEMP_PRE_NORMAL]);
		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4 + BATT_TEMP_HYST;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
		set_prop_batt_health(chg, POWER_SUPPLY_HEALTH_GOOD);
	}

	return 0;
}

/* 15C < Tbatt < 45C */
static int handle_batt_temp_normal(struct smb_charger *chg)
{
	enum temp_region_type temp_region;
	int vbat_mv = 0;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if ((temp_region != BATT_TEMP_NORMAL)
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->recharge_pending = false;
		chg->is_power_changed = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);
		else if (temp_region == BATT_TEMP_WARM && chg->chg_done) {
			vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
			if (vbat_mv < chg->vbatmax[BATT_TEMP_NORMAL]) {
				chg->chg_done = false;
				op_charging_en(chg, true);
			}
		}
		op_battery_temp_region_set(chg, BATT_TEMP_NORMAL);
#ifdef OP_SWARP_SUPPORTED
		if (is_charger_not_match(chg) || chg->qc_configed
			|| (chg->pd_9vsupported && chg->pd_active))
			set_chg_ibat_vbat_max(chg,
				chg->setting_ibatmax[BATT_TEMP_NORMAL],
				chg->vbatmax[BATT_TEMP_NORMAL]);
		else
#endif
			set_chg_ibat_vbat_max(chg,
				chg->ibatmax[BATT_TEMP_NORMAL],
				chg->vbatmax[BATT_TEMP_NORMAL]);
		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
		set_prop_batt_health(chg, POWER_SUPPLY_HEALTH_GOOD);
	}

	return 0;
}

/* 45C <= Tbatt <= 55C */
static int handle_batt_temp_warm(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if ((temp_region != BATT_TEMP_WARM)
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->is_power_changed = false;
		chg->recharge_pending = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);
		op_battery_temp_region_set(chg, BATT_TEMP_WARM);
		op_temp_region_charging_en(chg,
				chg->vbatmax[BATT_TEMP_WARM]);
#ifdef OP_SWARP_SUPPORTED
		if (is_charger_not_match(chg) || chg->qc_configed
			|| (chg->pd_9vsupported && chg->pd_active))
			set_chg_ibat_vbat_max(chg,
				chg->setting_ibatmax[BATT_TEMP_WARM],
				chg->vbatmax[BATT_TEMP_WARM]);
		else
#endif
			set_chg_ibat_vbat_max(chg,
				chg->ibatmax[BATT_TEMP_WARM],
				chg->vbatmax[BATT_TEMP_WARM]);
		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5 - BATT_TEMP_HYST;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
		set_prop_batt_health(chg, POWER_SUPPLY_HEALTH_GOOD);
	}

	return 0;
}

/* 55C < Tbatt */
static int handle_batt_temp_hot(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	temp_region = op_battery_temp_region_get(chg);
	if ((temp_region != BATT_TEMP_HOT)
			|| chg->is_power_changed) {
		pr_err("triggered\n");
		chg->is_power_changed = false;
		op_charging_en(chg, false);
		op_battery_temp_region_set(chg, BATT_TEMP_HOT);

		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 =
			chg->BATT_TEMP_T6 - BATT_TEMP_HYST;
		/* from hot to warm */
		set_prop_batt_health(chg, POWER_SUPPLY_HEALTH_OVERHEAT);
	}

	return 0;
}

#ifdef OP_SWARP_SUPPORTED
static void op_other_fast_check_temp_region(struct smb_charger *chg, int temp)
{
	int rc = 0;
	int vbat_mv = 0;
	int vbus_mv;
	union power_supply_propval pval = {0, };

	// Warp/Dash 1.8A Max
	if (is_charger_not_match(chg)) {
		if (!chg->setting_ibatmax || chg->is_power_changed) {
			vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
			if (vbat_mv <= chg->dash_vbat_thr_mv) {
				pr_err("switch setting_ibatmax to dash_ibatmax");
				chg->setting_ibatmax = chg->dash_ibatmax;
				chg->dash_need_check_vbat = true;
			} else {
				pr_err("switch setting_ibatmax to dash_ibatmax_thr_ma");
				chg->setting_ibatmax = chg->dash_ibatmax_thr_ma;
				chg->dash_need_check_vbat = false;
			}
		}
	}
	// QC 18W Max
	if (chg->qc_configed &&
	    (!chg->setting_ibatmax || chg->is_power_changed)) {
		rc = smblib_get_prop_usb_voltage_now(chg, &pval);
		if (rc < 0) {
			pr_err("get usb voltage error, rc=%d\r\n", rc);
			vbus_mv = 0;
		} else {
			vbus_mv = pval.intval;
		}
		if (vbus_mv > 7500) {
			pr_err("switch setting_ibatmax to qc_9v_ibatmax");
			chg->setting_ibatmax = chg->qc_9v_ibatmax;
		} else {
			pr_err("switch setting_ibatmax to qc_9v_ibatmax_thr_ma");
			chg->setting_ibatmax = chg->qc_9v_ibatmax_thr_ma;
		}
	}
	// PD 27W Max
	if ((chg->pd_9vsupported && chg->pd_active) &&
	    (!chg->setting_ibatmax || chg->is_power_changed)) {
		rc = smblib_get_prop_usb_voltage_now(chg, &pval);
		if (rc < 0) {
			pr_err("get usb voltage error, rc=%d\r\n", rc);
			vbus_mv = 0;
		} else {
			vbus_mv = pval.intval;
		}
		if (vbus_mv > 7500) {
			pr_err("switch setting_ibatmax to pd_9v_ibatmax");
			chg->setting_ibatmax = chg->pd_9v_ibatmax;
		} else {
			pr_err("switch setting_ibatmax to pd_9v_ibatmax_thr_ma");
			chg->setting_ibatmax = chg->pd_9v_ibatmax_thr_ma;
		}
	}

}
#endif

static int op_check_battery_temp(struct smb_charger *chg)
{
	int temp, rc = -1;

	if (!chg->vbus_present)
		return rc;

	temp = get_prop_batt_temp(chg);
#ifdef OP_SWARP_SUPPORTED
	op_other_fast_check_temp_region(chg, temp);
#endif
	if (temp < chg->mBattTempBoundT0) /* COLD */
		rc = handle_batt_temp_cold(chg);
	else if (temp >=  chg->mBattTempBoundT0 &&
			temp < chg->mBattTempBoundT1) /* LITTLE_COLD */
		rc = handle_batt_temp_little_cold(chg);
	else if (temp >=  chg->mBattTempBoundT1 &&
			temp < chg->mBattTempBoundT2) /* COOL */
		rc = handle_batt_temp_cool(chg);
	else if (temp >= chg->mBattTempBoundT2 &&
			temp < chg->mBattTempBoundT3) /* LITTLE_COOL */
		rc = handle_batt_temp_little_cool(chg);
	else if (temp >= chg->mBattTempBoundT3 &&
			temp < chg->mBattTempBoundT4) /* PRE_NORMAL */
		rc = handle_batt_temp_prenormal(chg);
	else if (temp >= chg->mBattTempBoundT4 &&
			temp < chg->mBattTempBoundT5) /* NORMAL */
		rc = handle_batt_temp_normal(chg);
	else if (temp >= chg->mBattTempBoundT5 &&
			temp <=  chg->mBattTempBoundT6) /* WARM */
		rc = handle_batt_temp_warm(chg);
	else if (temp > chg->mBattTempBoundT6) /* HOT */
		rc = handle_batt_temp_hot(chg);

	return rc;
}

void op_charge_info_init(struct smb_charger *chg)
{
	op_battery_temp_region_set(chg, BATT_TEMP_NORMAL);

	chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
	chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
	chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
	chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
	chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
	chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
	chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
	chg->chg_ovp = false;
	chg->is_power_changed = false;
	chg->chg_done = false;
	chg->recharge_pending = false;
	chg->recharge_status = false;
	chg->temp_littel_cool_set_current_0_point_25c = false;
	chg->dash_need_check_vbat = false;
	chg->disallow_fast_high_temp = false;
	chg->disallow_fast_bad_vol = false;
	chg->oem_lcd_is_on = false;
	chg->time_out = false;
	chg->battery_status = BATT_STATUS_GOOD;
	chg->disable_normal_chg_for_dash = false;
	chg->suspend_by_voltage_switch = false;
	chg->usb_enum_status = false;
	chg->non_std_chg_present = false;
	chg->is_audio_adapter = false;
	chg->init_irq_done = false;
	chg->wireless_present = false;
	chg->apsd_delayed = false;
	chg->apsd_not_done = false;
	chg->pd_tried_warp_switch = false;
}

static int op_handle_battery_uovp(struct smb_charger *chg)
{
	pr_err("vbat is over voltage, stop charging\n");
	set_prop_batt_health(chg, POWER_SUPPLY_HEALTH_OVERVOLTAGE);
	op_charging_en(chg, false);

	return 0;
}

static int op_handle_battery_restore_from_uovp(struct smb_charger *chg)
{
	pr_err("vbat is back to normal, start charging\n");
	/* restore charging form battery ovp */
	op_charging_en(chg, true);
	set_prop_batt_health(chg, POWER_SUPPLY_HEALTH_GOOD);

	return 0;
}

static void op_check_battery_uovp(struct smb_charger *chg)
{
	int vbat_mv = 0;
	enum batt_status_type battery_status_pre;

	if (!chg->vbus_present)
		return;

	battery_status_pre = op_battery_status_get(chg);
	vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
	pr_debug("bat vol:%d\n", vbat_mv);
	if (vbat_mv > BATT_SOFT_OVP_MV) {
		if (battery_status_pre == BATT_STATUS_GOOD) {
			pr_err("BATTERY_SOFT_OVP_VOLTAGE\n");
			op_battery_status_set(chg, BATT_STATUS_BAD);
			op_handle_battery_uovp(chg);
		}
	} else {
		if (battery_status_pre == BATT_STATUS_BAD) {
			pr_err("battery_restore_from_uovp\n");
			op_battery_status_set(chg, BATT_STATUS_GOOD);
			op_handle_battery_restore_from_uovp(chg);
		}
	}

	if (chg->swarp_supported) {
		if (!chg->hw_term_enabled
			&& vbat_mv > chg->vbatmax[BATT_TEMP_NORMAL]) {
			chg->hw_term_enabled = true;
			op_set_hw_term_charging(true);
		}
		update_aicl_point_val(vbat_mv);
	}
}

#ifdef OP_SWARP_SUPPORTED
#define ALLOW_DIFF_VALUE 1000
static void op_check_battery_vol_diff(struct smb_charger *chg)
{
	int rc = 0;
	int vbat_cell_max = 0;
	int vbat_cell_min = 0;
	bool dash_on = false;
	enum batt_status_type battery_status_pre;
	static int counter;

	battery_status_pre = op_battery_status_get(chg);
	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_VBAT_CELL_MAX, &vbat_cell_max);
	if (rc) {
		pr_err("Couldn't get vbat_max rc=%d\n", rc);
		vbat_cell_max = 3800;
	}
	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_VBAT_CELL_MIN, &vbat_cell_min);
	if (rc) {
		pr_err("Couldn't get vbat_min rc=%d\n", rc);
		vbat_cell_min = 3800;
	}
	pr_err("bat vol:(%d,%d)\n", vbat_cell_max, vbat_cell_min);
	if ((vbat_cell_max - vbat_cell_min) > ALLOW_DIFF_VALUE) {
		counter++;
		if (counter > 10 && battery_status_pre == BATT_STATUS_GOOD) {
			pr_err("BATTERY_SOFT_DIFF_VOLTAGE\n");
			dash_on = get_prop_fast_chg_started(chg);
			if (dash_on || is_charger_not_match(chg)) {
				switch_mode_to_normal();
				op_set_fast_chg_allow(chg, false);
			}
			op_battery_status_set(chg, BATT_STATUS_BAD_VOL_DIFF);
			op_handle_battery_uovp(chg);
		}
	} else
		counter = 0;
}
#endif

int op_get_charg_en(struct smb_charger *chg, int *chg_enabled)
{
	int rc = 0;
	u8 temp;
#ifdef OP_SWARP_SUPPORTED
	union power_supply_propval en_chg = {0, };

	if (chg->swarp_supported) {
		if (is_op_chg_available(chg)) {
			rc = power_supply_get_property(chg->op_chg_psy,
					POWER_SUPPLY_PROP_CHARGE_ENABLED, &en_chg);
			if (rc)
				pr_err("can't get op_chg_psy prop CHARGE_ENABLED rc = %d\n", rc);
			else
				*chg_enabled = en_chg.intval;
		} else {
			pr_err("no op_chg psy found\n");
			return -EINVAL;
		}
	} else {
#endif
		rc = smblib_read(chg, CHARGING_ENABLE_CMD_REG, &temp);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read chg en rc=%d\n", rc);
			return rc;
		}
		*chg_enabled = temp & CHARGING_ENABLE_CMD_BIT;
#ifdef OP_SWARP_SUPPORTED
	}
#endif

	return rc;
}

static void op_check_charger_collapse(struct smb_charger *chg)
{
	int rc, is_usb_supend, curr, chg_en;
	u8 stat = 0, chger_stat = 0, pwer_source_stats = 0;

	if (!chg->vbus_present)
		return;
	if (chg->dash_present)
		return;
	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &chger_stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
	}
	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &pwer_source_stats);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read AICL_STATUS_REG rc=%d\n",
			rc);
	}
	rc = smblib_get_usb_suspend(chg, &is_usb_supend);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb suspend rc=%d\n", rc);
		return;
	}
	op_get_charg_en(chg, &chg_en);
	pr_debug("chger_stat=0x%x, aicl_stats =0x%x, chg_en =%d\n",
		chger_stat, pwer_source_stats, chg_en);
	curr = get_prop_batt_current_now(chg) / 1000;
	stat = !chg->chg_done
			&& !is_usb_supend
			&& (curr > 20)
			&& chg_en
			&& ((ZERO_CHARGE_CURRENT_BIT & chger_stat)
				|| (pwer_source_stats == 0x72));

	if (stat && !chg->charger_collpse) {
		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
		SUSPEND_ON_COLLAPSE_USBIN_BIT
		|USBIN_AICL_START_AT_MAX_BIT
		|USBIN_AICL_ADC_EN_BIT
		|USBIN_AICL_PERIODIC_RERUN_EN_BIT,
		USBIN_AICL_PERIODIC_RERUN_EN_BIT);
		if (rc < 0)
			dev_err(chg->dev,
			"Couldn't configure AICL rc=%d\n", rc);
	smblib_run_aicl(chg, RERUN_AICL);
	chg->charger_collpse = true;
	schedule_delayed_work(&chg->op_re_set_work,
				msecs_to_jiffies(TIME_1000MS));
	smblib_err(chg, "op check charger collapse done\n");
	}
}

static void op_check_charger_uovp(struct smb_charger *chg, int vchg_mv)
{
	static int over_volt_count, not_over_volt_count;
	static bool pre_uovp_satus;
	int detect_time = 3; /* 3 x 6s = 18s */
	int v_allow_max = 0;

	if (!chg->vbus_present)
		return;

	if (chg->wireless_present)
		return;

	pr_err("charger_voltage=%d charger_ovp=%d pd_active=%d\n",
			vchg_mv, chg->chg_ovp, chg->pd_active);

#ifdef OP_SWARP_SUPPORTED
	if (chg->swarp_supported)
		v_allow_max = SWARP_CHG_SOFT_OVP_MV;
	else
#endif
		v_allow_max = CHG_SOFT_OVP_MV;

	pre_uovp_satus = chg->vbus_uovp_satus;
	if (!chg->chg_ovp) {
		if (vchg_mv > v_allow_max || vchg_mv <= CHG_SOFT_UVP_MV) {
			pr_err("charger is over voltage, count=%d\n",
				over_volt_count);
			chg->vbus_uovp_satus = true;
			if (pre_uovp_satus)
				over_volt_count++;
			else
				over_volt_count = 0;

			pr_err("uovp_satus=%d,pre_uovp_satus=%d,over_volt_count=%d\n",
				chg->vbus_uovp_satus, pre_uovp_satus, over_volt_count);
			rerun_election(chg->usb_icl_votable);
			if (detect_time <= over_volt_count) {
				/* vchg continuous higher than 5.8v */
				pr_err("charger is over voltage, stop charging\n");
				op_charging_en(chg, false);
				chg->chg_ovp = true;
			}
		}
	} else {
		if (vchg_mv < v_allow_max - 100
				&& vchg_mv > CHG_SOFT_UVP_MV + 100) {
			chg->vbus_uovp_satus = false;
			if (!pre_uovp_satus)
				not_over_volt_count++;
			else
				not_over_volt_count = 0;

			pr_err("uovp_satus=%d, pre_uovp_satus=%d,not_over_volt_count=%d\n",
				chg->vbus_uovp_satus, pre_uovp_satus,
					not_over_volt_count);
			if (detect_time <= not_over_volt_count) {
				/* vchg continuous lower than 5.7v */
				pr_err("charger voltage is back to normal\n");
				op_charging_en(chg, true);
				chg->chg_ovp = false;
				op_check_battery_temp(chg);
				smblib_run_aicl(chg, RERUN_AICL);
			}
		}
	}
}

#define CHECK_HVDCP_COUNT 10
static void op_check_qc_charge_config(struct smb_charger *chg)
{
	int vbat_mv, vbus_mv;
	union power_supply_propval pval = {0, };
	int rc;

	if (!chg->hvdcp_check) {
		if (chg->not_check_hvdcp_count < CHECK_HVDCP_COUNT) {
			chg->not_check_hvdcp_count++;
			chg->qc_configed = false;
		} else if (!chg->dash_on && !chg->swarp_online
			&& chg->not_check_hvdcp_count == CHECK_HVDCP_COUNT) {
			chg->not_check_hvdcp_count++;
			if (chg->support_hvdcp &&
				(chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP ||
				chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3 ||
				chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5 ||
				chg->real_charger_type == POWER_SUPPLY_TYPE_USB_DCP)) {
				chg->hvdcp_check = true;
				smblib_hvdcp_detect_enable(chg, true);
				smblib_request_dpdm(chg, true);
				smblib_rerun_apsd(chg);
			}
		}
		return;
	} else {
		if (chg->not_check_hvdcp_count > CHECK_HVDCP_COUNT &&
			chg->real_charger_type != POWER_SUPPLY_TYPE_USB_HVDCP) {
			chg->hvdcp_check = false;
			return;
		}
	}

	if (chg->dash_on || chg->real_charger_type != POWER_SUPPLY_TYPE_USB_HVDCP) {
		chg->qc_configed = false;
		return;
	}

	vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
	rc = smblib_get_prop_usb_voltage_now(chg, &pval);
	if (rc < 0) {
		pr_err("get usb voltage error, rc=%d\r\n", rc);
		vbus_mv = 0;
	} else {
		vbus_mv = pval.intval;
	}

	if (!chg->qc_configed && chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP) {
		vote(chg->fcc_votable, QC_VOTER, true, 2000000);
		vote(chg->usb_icl_votable, QC_VOTER, true, 2000000);
		if (chg->qcpd_9v_vbat_thr > 0 && vbus_mv < 7500 &&
		    vbat_mv < chg->qcpd_9v_vbat_thr) {
			if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_9V) {
				pr_info("the adapter does not support output 9V\n");
				chg->qc_configed = true;
				chg->is_power_changed = true;
				return;
			}
			smblib_hvdcp_hw_inov_enable(chg, false);
			pr_info("force the adapter to output 9V\n");
			// before request 9v, need to force 5v first
			rc = smblib_dp_dm(chg, POWER_SUPPLY_DP_DM_FORCE_5V);
			if (rc < 0)
				return;
			msleep(300);
			chg->suspend_by_voltage_switch = true;
			(void)smblib_set_usb_suspend(chg, true);
			//set Vsys Skip threshold 104%
			op_set_charger_vsys_threshold(chg, 0x02);
			op_en_charger_burst_mode(chg, false);
			rc = smblib_dp_dm(chg, POWER_SUPPLY_DP_DM_FORCE_9V);
			if (rc < 0)
				return;
			msleep(300);
			chg->suspend_by_voltage_switch = false;
			(void)smblib_set_usb_suspend(chg, false);
			chg->qc_configed = true;
			chg->is_power_changed = true;
			rerun_election(chg->usb_icl_votable);
			op_en_charger_burst_mode(chg, true);
		} else {
			pr_info("set adapter to output 5V\n");
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_5V);
			if (rc < 0) {
				pr_err("can't set adapter to output 5V\n");
				return;
			}
			smblib_hvdcp_hw_inov_enable(chg, true);
			chg->qc_configed = true;
			chg->is_power_changed = true;
		}
		return;
	}

	if (chg->qc_configed && chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP) {
		if (vbus_mv > 7500 && chg->qcpd_to_5v_vbat_thr > 0 &&
		    vbat_mv > chg->qcpd_to_5v_vbat_thr) {
			pr_info("Exit high-voltage charging\n");
			// force icl=500mA before switch to 5v.
			smblib_set_icl_current(chg, USBIN_500MA);
			chg->suspend_by_voltage_switch = true;
			(void)smblib_set_usb_suspend(chg, true);
			//set Vsys Skip threshold 101%
			op_set_charger_vsys_threshold(chg, 0x03);
			(void)smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_5V);
			smblib_hvdcp_hw_inov_enable(chg, true);
			msleep(300);
			chg->suspend_by_voltage_switch = false;
			(void)smblib_set_usb_suspend(chg, false);
			chg->is_power_changed = true;
			rerun_election(chg->usb_icl_votable);
			op_en_charger_burst_mode(chg, true);
		}
	}
}

static void op_dcdc_vph_track_sel(struct smb_charger *chg)
{
	int rc = 0;

	pr_debug("lcd_is_on:%d\n", chg->oem_lcd_is_on);

	if (chg->vbus_present && chg->chg_done && !chg->vph_sel_disable) {
		if (chg->oem_lcd_is_on && !chg->vph_set_flag) {
			pr_info("vbus present,LCD on set dcdc vph 300mv\n");
			/* config the DCDC_VPH_TRACK_SEL 300mv */
			rc = smblib_masked_write(chg, DCDC_VPH_TRACK_SEL,
					VPH_TRACK_SEL_MASK, SEL_300MV);
			if (rc < 0)
				pr_err("Couldn't set  DCDC_VPH_TRACK_SEL rc=%d\n",
						rc);
			chg->vph_set_flag = true;
		} else if (!chg->oem_lcd_is_on && chg->vph_set_flag) {
			pr_info("vbus present,LCD off set dcdc vph 100mv\n");
			/* config the DCDC_VPH_TRACK_SEL 100mv */
			rc = smblib_masked_write(chg, DCDC_VPH_TRACK_SEL,
					VPH_TRACK_SEL_MASK, 0);
			if (rc < 0)
				pr_err("Couldn't set  DCDC_VPH_TRACK_SEL rc=%d\n",
						rc);
			chg->vph_set_flag = false;
		}
	}
}

#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	struct smb_charger *chip =
		container_of(self, struct smb_charger, fb_notif);

	if (evdata && evdata->data && chip) {
		if (event == FB_EVENT_BLANK) {
			blank = evdata->data;
			if (*blank == FB_BLANK_UNBLANK) {
				if (!chip->oem_lcd_is_on)
					set_property_on_fg(chip,
					POWER_SUPPLY_PROP_UPDATE_LCD_IS_OFF, 0);
				chip->oem_lcd_is_on = true;
			} else if (*blank == FB_BLANK_POWERDOWN) {
				if (chip->oem_lcd_is_on != false)
					set_property_on_fg(chip,
					POWER_SUPPLY_PROP_UPDATE_LCD_IS_OFF, 1);
				chip->oem_lcd_is_on = false;
			}
		}

	}

	return 0;
}
#elif defined(CONFIG_MSM_RDM_NOTIFY)
static int msm_drm_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct drm_panel_notifier *evdata = data;
	struct smb_charger *chip =
		container_of(self, struct smb_charger, msm_drm_notifier);
	int *blank;
	int typec_mode;
	int rp_ua;

	if ((evdata == NULL) || (event != DRM_PANEL_EARLY_EVENT_BLANK))
		return 0;

	typec_mode = smblib_get_prop_typec_mode(chip);

	if (evdata && evdata->data && chip) {
		blank = evdata->data;
		if (*blank == DRM_PANEL_BLANK_UNBLANK ||
			*blank == DRM_PANEL_BLANK_UNBLANK_CHARGE) {
			if (!chip->oem_lcd_is_on)
				set_property_on_fg(chip,
				POWER_SUPPLY_PROP_UPDATE_LCD_IS_OFF, 0);
			chip->oem_lcd_is_on = true;
			chip->lcd_st_debounce_expire = jiffies + 60 * HZ; // 1min
			op_dcdc_vph_track_sel(chip);
		} else if (*blank == DRM_PANEL_BLANK_POWERDOWN ||
				*blank == DRM_PANEL_BLANK_POWERDOWN_CHARGE) {
			if (chip->oem_lcd_is_on != false)
				set_property_on_fg(chip,
				POWER_SUPPLY_PROP_UPDATE_LCD_IS_OFF, 1);
			chip->oem_lcd_is_on = false;
			chip->lcd_st_debounce_expire = jiffies + 60 * HZ; // 1min
			op_dcdc_vph_track_sel(chip);
		} else if (*blank == DRM_PANEL_BLANK_AOD) {
			chip->oem_lcd_is_on = false;
			pr_info("AOD is on, treat it as screen off.");
		}

		if (chip->swarp_supported) {
			pr_info("dual-cell prj, not change with lcd status here.");
			chip->need_check_icl_lcdon = true;
			return 0;
		}

		/* add to set pd charging current 2.0A when panel on */
		if (typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH ||
			typec_mode == POWER_SUPPLY_TYPEC_SOURCE_MEDIUM ||
			typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) {
			rp_ua = get_rp_based_dcp_current(chip, typec_mode);
			if (!chip->pd_active) {
				vote(chip->usb_icl_votable,
						SW_ICL_MAX_VOTER, true, rp_ua);
			}
		}
	}

	return 0;
}
#endif

#ifdef OP_SWARP_SUPPORTED
static void swarp_ffc_check(struct smb_charger *chg)
{
	int icharging, batt_volt, temp;
	int next_status = 0;
	int pre_ffc_status = 0;
	int fv = 0, fc = 0;
	int chg_en = 0;
	bool need_check = false;

	if (chg->ffc_status == FFC_DEFAULT) {
		chg->ffc_count = 0;
		chg->ffc_fv_count = 0;
		return;
	}
	batt_volt = get_prop_batt_voltage_now(chg) / 1000;
	icharging = get_prop_batt_current_now(chg) / 1000;
	temp = get_prop_batt_temp(chg);

	switch (chg->ffc_status) {
	case FFC_FAST:
		if (chg->disable_normal_chg_for_dash)
			break;
		if (temp > chg->FFC_TEMP_T1
			&& temp < chg->FFC_TEMP_T2) {
			fv = chg->FFC_FAST_FV;
		}  else if (temp >= chg->FFC_TEMP_T2
			&& temp < chg->FFC_TEMP_T3) {
			fv = chg->FFC_FAST_WARM_FV;
		} else {
			// < FFC_TEMP_T1(12 Celsius) or > FFC_TEMP_T3(40 Celsius), not do FFC.
			chg->ffc_status = FFC_IDLE;
			break;
		}
		fc = chg->FFC_FAST_CUTOFF;
		next_status = FFC_TAPER;
		need_check = true;
		break;
	// Fall-through.
	case FFC_TAPER:
	case FFC_NOR_TAPER:
	case FFC_WARM_TAPER:
		pre_ffc_status = chg->ffc_status;
		if (temp > chg->FFC_TEMP_T1
			&& temp < chg->FFC_TEMP_T2) {
			chg->ffc_status = FFC_NOR_TAPER;
			vote(g_chg->fcc_votable,
				DEFAULT_VOTER, true, chg->FFC_NOR_FCC * 1000);
		} else if (temp >= chg->FFC_TEMP_T2
			&& temp < chg->FFC_TEMP_T3) {
			chg->ffc_status = FFC_WARM_TAPER;
			vote(g_chg->fcc_votable,
				DEFAULT_VOTER, true, chg->FFC_WARM_FCC * 1000);
		} else {
			chg->ffc_count = 0;
			chg->ffc_fv_count = 0;
			chg->ffc_status = FFC_IDLE;
			break;
		}

		if (pre_ffc_status == FFC_TAPER) {
			chg->ffc_count = 0;
			chg->ffc_fv_count = 0;
			break;
		}

		if (chg->ffc_status == FFC_NOR_TAPER) {
			fc = chg->FFC_NORMAL_CUTOFF;
			fv = chg->FFC_VBAT_FULL;
		} else if (chg->ffc_status == FFC_WARM_TAPER) {
			fc = chg->FFC_WARM_CUTOFF;
			fv = chg->FFC_WARM_VBAT_FULL;
		}

		next_status = FFC_IDLE;
		need_check = true;
		break;
	case FFC_IDLE:
		op_get_charg_en(chg, &chg_en);
		if (chg_en != 0)
			op_charging_en(chg, false);
		chg->ffc_count++;
		if (chg->ffc_count > 5) {
			chg->ffc_status = FFC_DEFAULT;
			smblib_set_prop_charge_parameter_set(chg);
			op_charging_en(chg, true);
			rerun_election(chg->usb_icl_votable);
		}
		break;
	default:
		chg->ffc_count = 0;
		chg->ffc_fv_count = 0;
		chg->ffc_status = FFC_DEFAULT;
		break;
	}

	if (need_check) {
		// vbat grand total 4 times
		if (batt_volt >= fv)
			chg->ffc_fv_count++;

		// current 2 consecutive times
		if ((-1) * icharging < fc)
			chg->ffc_count++;
		else
			chg->ffc_count = 0;

		if (chg->ffc_fv_count >= 4 || chg->ffc_count >= 2) {
			chg->ffc_count = 0;
			chg->ffc_fv_count = 0;
			chg->ffc_status = next_status;
		}
	}
}
#endif

static void ffc_exit(struct smb_charger *chg)
{
	int icharging, batt_volt, temp;
	int usbin_curr = 0;
	int usbin_curr_max = 0;
	union power_supply_propval pval = {0, };
	int rc;

	if (chg->ffc_status == FFC_DEFAULT) {
		chg->ffc_count = 0;
		return;
	}
	batt_volt = get_prop_batt_voltage_now(chg) / 1000;
	icharging = get_prop_batt_current_now(chg) / 1000;
	temp = get_prop_batt_temp(chg);

	if (chg->ffc_status == FFC_NOR_TAPER
		|| chg->ffc_status == FFC_WARM_TAPER) {
		if (temp > chg->FFC_TEMP_T1
			&& temp < chg->FFC_TEMP_T2) {
			chg->ffc_status = FFC_NOR_TAPER;
			if (g_chg->wireless_present)
				vote(g_chg->usb_icl_votable,
				     WLCH_FFC_VOTER, true, 800000);
			vote(g_chg->fcc_votable,
				DEFAULT_VOTER, true, chg->FFC_NOR_FCC * 1000);
		} else if (temp >= chg->FFC_TEMP_T2
			&& temp < chg->FFC_TEMP_T3) {
			chg->ffc_status = FFC_WARM_TAPER;
			if (g_chg->wireless_present)
				vote(g_chg->usb_icl_votable,
				     WLCH_FFC_VOTER, true, 800000);
			vote(g_chg->fcc_votable,
				DEFAULT_VOTER, true, chg->FFC_WARM_FCC * 1000);
		} else {
			chg->ffc_count = 0;
			chg->ffc_status = FFC_IDLE;
		}

		if (chg->wireless_present) {
			rc = smblib_get_prop_usb_current_now(chg, &pval);
			if (rc < 0) {
				pr_err("get usb input current error, rc=%d\r\n", rc);
				usbin_curr = 0;
			} else {
				usbin_curr = pval.intval / 1000;
			}
			usbin_curr_max = get_effective_result(chg->usb_icl_votable);
			if (usbin_curr_max < 0)
				usbin_curr_max = 0;
			else
				usbin_curr_max = usbin_curr_max / 1000;
		}
	}

	if (chg->ffc_status == FFC_FAST) {
		if (batt_volt >= chg->FFC_VBAT_FULL)
			chg->ffc_count++;
		else
			chg->ffc_count = 0;
		if (chg->ffc_count >= 2) {
			chg->ffc_count = 0;
			chg->ffc_status = FFC_TAPER;
			pr_info("ffc one done\n");
		}
	} else if (chg->ffc_status == FFC_TAPER) {
		if (temp > chg->FFC_TEMP_T1
			&& temp < chg->FFC_TEMP_T2) {
			chg->ffc_status = FFC_NOR_TAPER;
			if (g_chg->wireless_present)
				vote(g_chg->usb_icl_votable,
				     WLCH_FFC_VOTER, true, 800000);
			vote(g_chg->fcc_votable,
				DEFAULT_VOTER, true, chg->FFC_NOR_FCC * 1000);
		} else if (temp >= chg->FFC_TEMP_T2
			&& temp < chg->FFC_TEMP_T3) {
			chg->ffc_status = FFC_WARM_TAPER;
			if (g_chg->wireless_present)
				vote(g_chg->usb_icl_votable,
				     WLCH_FFC_VOTER, true, 800000);
			vote(g_chg->fcc_votable,
				DEFAULT_VOTER, true, chg->FFC_WARM_FCC * 1000);
		} else {
			chg->ffc_count = 0;
			chg->ffc_status = FFC_IDLE;
		}
	} else if (chg->ffc_status == FFC_NOR_TAPER) {
		if (icharging <= (-1)*chg->FFC_NORMAL_CUTOFF
			&& (batt_volt >= chg->FFC_VBAT_FULL)) {
			chg->ffc_count = 0;
			chg->ffc_status = FFC_IDLE;
		} else if (icharging > (-1)*chg->FFC_NORMAL_CUTOFF) {
			if (chg->wireless_present && (usbin_curr != 0) &&
			    (usbin_curr_max != 0)) {
				if (usbin_curr > usbin_curr_max - 100)
					chg->ffc_count = 0;
				else
					chg->ffc_count++;
			} else {
				chg->ffc_count++;
			}
		} else {
			chg->ffc_count = 0;
		}
		if (chg->ffc_count >= 2) {
			chg->ffc_count = 0;
			chg->ffc_status = FFC_IDLE;
			pr_info("ffc nor taper done\n");
		}
	} else if (chg->ffc_status == FFC_WARM_TAPER) {
		if (icharging <= (-1)*chg->FFC_WARM_CUTOFF
			&& (batt_volt >= chg->FFC_VBAT_FULL)) {
			chg->ffc_count = 0;
			chg->ffc_status = FFC_IDLE;
		} else if (icharging > (-1)*chg->FFC_WARM_CUTOFF) {
			if (chg->wireless_present && (usbin_curr != 0) &&
			    (usbin_curr_max != 0)) {
				if (usbin_curr > usbin_curr_max - 100)
					chg->ffc_count = 0;
				else
					chg->ffc_count++;
			} else {
				chg->ffc_count++;
			}
		} else {
			chg->ffc_count = 0;
		}
		if (chg->ffc_count >= 2) {
			chg->ffc_count = 0;
			chg->ffc_status = FFC_IDLE;
			pr_info("ffc normal taper done\n");
		}
	} else if (chg->ffc_status == FFC_IDLE) {
		chg->ffc_count++;
		op_charging_en(chg, false);
		if (chg->ffc_count > 5) {
			chg->ffc_status = FFC_DEFAULT;
			smblib_set_prop_charge_parameter_set(chg);
			op_charging_en(chg, true);
		}
	} else {
		chg->ffc_count = 0;
		chg->ffc_status = FFC_DEFAULT;
	}
}

#define FULL_COUNTS_SW		5
#define FULL_COUNTS_HW		3

static bool op_check_vbat_is_full_by_sw(struct smb_charger *chg)
{
	static bool ret_sw;
	static bool ret_hw;
	static int vbat_counts_sw;
	static int vbat_counts_hw;
	int vbatt_full_vol_sw;
	int vbatt_full_vol_hw;
	int term_current;
	int tbatt_status, icharging, batt_volt;

	if (!chg->check_batt_full_by_sw)
		return false;
	if (chg->ffc_status != FFC_DEFAULT)
		return false;
	if (!chg->vbus_present) {
		vbat_counts_sw = 0;
		vbat_counts_hw = 0;
		ret_sw = false;
		ret_hw = false;
		return false;
	}

	tbatt_status = op_battery_temp_region_get(chg);
	vbatt_full_vol_hw = chg->vbatmax[tbatt_status];
	if (tbatt_status == BATT_TEMP_LITTLE_COLD)
		vbatt_full_vol_sw = chg->vbatmax[tbatt_status] - chg->fv_offset_voltage_mv;
	else if (tbatt_status == BATT_TEMP_COOL)
		vbatt_full_vol_sw = chg->vbatmax[tbatt_status] - chg->fv_offset_voltage_mv;
	else if (tbatt_status == BATT_TEMP_LITTLE_COOL)
		vbatt_full_vol_sw = chg->vbatmax[tbatt_status] - chg->fv_offset_voltage_mv;
	else if (tbatt_status == BATT_TEMP_PRE_NORMAL)
		vbatt_full_vol_sw = chg->vbatmax[tbatt_status] - chg->fv_offset_voltage_mv;
	else if (tbatt_status == BATT_TEMP_NORMAL)
		vbatt_full_vol_sw = chg->vbatmax[tbatt_status] - chg->fv_offset_voltage_mv;
	else if (tbatt_status == BATT_TEMP_WARM)
		vbatt_full_vol_sw = chg->vbatmax[tbatt_status] - chg->fv_offset_voltage_mv;
	else {
		vbat_counts_sw = 0;
		vbat_counts_hw = 0;
		ret_sw = 0;
		ret_hw = 0;
		return false;
	}
	if (chg->little_cold_iterm_ma > 0
		&& (tbatt_status == BATT_TEMP_LITTLE_COLD))
		term_current = chg->little_cold_iterm_ma;
	else
		term_current =  chg->sw_iterm_ma;

	batt_volt = get_prop_batt_voltage_now(chg) / 1000;
	icharging = get_prop_batt_current_now(chg) / 1000;
	/* use SW Vfloat to check */
	if (batt_volt > vbatt_full_vol_sw) {
		if (icharging < 0 && (icharging * -1) <= term_current) {
			vbat_counts_sw++;
			if (vbat_counts_sw > FULL_COUNTS_SW * chg->full_count_sw_num) {
				vbat_counts_sw = 0;
				ret_sw = true;
			}
		} else if (icharging >= 0) {
			vbat_counts_sw++;
			if (vbat_counts_sw > FULL_COUNTS_SW * 2) {
				vbat_counts_sw = 0;
				ret_sw = true;
				pr_info("[BATTERY] Battery full by sw when icharging>=0!!\n");
			}
		} else {
			vbat_counts_sw = 0;
			ret_sw = false;
		}
	} else {
		vbat_counts_sw = 0;
		ret_sw = false;
	}

	/* use HW Vfloat to check */
	if (batt_volt >= vbatt_full_vol_hw + 18) {
		vbat_counts_hw++;
		if (vbat_counts_hw >= FULL_COUNTS_HW) {
			vbat_counts_hw = 0;
			ret_hw = true;
		}
	} else {
		vbat_counts_hw = 0;
		ret_hw = false;
	}

	if (ret_sw == true || ret_hw == true) {
		pr_info("[BATTERY] Battery full by sw[%s] !!\n",
			(ret_sw == true) ? "S" : "H");
		ret_sw = ret_hw = false;
		return true;
	} else {
		return false;
	}
}

void checkout_term_current(struct smb_charger *chg)
{
	bool chg_full;

	if (chg->chg_done)
		return;
	chg_full = op_check_vbat_is_full_by_sw(chg);
	if (chg_full) {
		chg->chg_done = true;
		op_charging_en(chg, false);
		op_dcdc_vph_track_sel(chg);
		pr_info("chg_done:CAP=%d (Q:%d),VBAT=%d (Q:%d),IBAT=%d (Q:%d),BAT_TEMP=%d\n",
				get_prop_batt_capacity(chg),
				get_prop_fg_capacity(chg),
				get_prop_batt_voltage_now(chg) / 1000,
				get_prop_fg_voltage_now(chg) / 1000,
				get_prop_batt_current_now(chg) / 1000,
				get_prop_fg_current_now(chg) / 1000,
				get_prop_batt_temp(chg));
	}
}


static int usb_enum_check(const char *val, const struct kernel_param *kp)
{
	const struct apsd_result *apsd_result;
	struct smb_charger *chg = g_chg;
	unsigned long usb_sw_reset = 0;
	int ret = 0;

	pr_info("Check usb enum when boot\n");
	if (chg->usb_enum_status)
		return 0;

	ret = kstrtoul(val, 10, &usb_sw_reset);
	if (ret)
		return ret;

	if (!usb_sw_reset || !is_usb_present(chg))
		return 0;

	apsd_result = smblib_get_apsd_result(chg);
	if ((apsd_result->bit) != SDP_CHARGER_BIT &&
		(apsd_result->bit) != CDP_CHARGER_BIT)
		return 0;

	pr_info("usb don't enum for longtime in boot\n");
	op_handle_usb_removal(chg);
	chg->non_stand_chg_count = 0;
	schedule_delayed_work(&chg->re_det_work,
		msecs_to_jiffies(TIME_1000MS));
	schedule_delayed_work(
		&chg->non_standard_charger_check_work,
		msecs_to_jiffies(TIME_1000MS));

	return 0;
}

static void check_non_standard_charger_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
	struct smb_charger, non_standard_charger_check_work);
	bool charger_present;
	const struct apsd_result *apsd_result;
	int aicl_result, rc;

	pr_debug("chg->non_stand_chg_count=%d\n",
		chg->non_stand_chg_count);

	charger_present = is_usb_present(chg);
	if (!charger_present) {
		pr_info("chk_non_std_chger,charger_present\n");
		chg->non_stand_chg_count = 0;
		return;
	}
	if (chg->usb_enum_status) {
		pr_info("chk_non_std_chger,usb_enum_status\n");
		chg->non_stand_chg_count = 0;
		return;
	}

	if (chg->wireless_present) {
		pr_info("chk_non_std_chger,wireless_present\n");
		chg->non_stand_chg_count = 0;
		return;
	}
	if (chg->apsd_delayed) {
		pr_info("chk_non_std_chger,apsd_delayed\n");
		chg->non_stand_chg_count = 0;
		return;
	}
	if (!chg->chg_enabled) {
		pr_info("chg_enabled is false\n");
		chg->non_stand_chg_count = 0;
		return;
	}
	if (chg->swarp_supported) {
		pr_info("dual-cell prj, not check it.");
		chg->non_stand_chg_count = 0;
		return;
	}
	if (chg->non_stand_chg_count
		>= NON_STANDARD_CHARGER_CHECK_S) {
		apsd_result = smblib_update_usb_type(chg);
		if (apsd_result->bit == DCP_CHARGER_BIT
			|| apsd_result->bit == OCP_CHARGER_BIT)
			return;
		if (!chg->chg_enabled)
			return;
		rc = smblib_run_aicl(chg, RERUN_AICL);
		if (rc < 0)
			smblib_err(chg, "Couldn't re-run AICL rc=%d\n", rc);
		msleep(500);
		aicl_result = op_get_aicl_result(chg);
		chg->non_stand_chg_current = aicl_result;
		chg->usb_psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
		if (chg->is_aging_test)
			op_usb_icl_set(chg, DEFAULT_AGAING_CHG_MA*1000);
		else if (aicl_result >= 700*1000)
			op_usb_icl_set(chg, aicl_result - 200*1000);
		else
			op_usb_icl_set(chg, 1200*1000);
		power_supply_changed(chg->batt_psy);
		chg->is_power_changed = true;
		chg->non_std_chg_present = true;
		pr_err("non-standard_charger detected,aicl_result=%d\n",
			aicl_result);
	} else {
		chg->non_stand_chg_count++;
		schedule_delayed_work(
			&chg->non_standard_charger_check_work,
			msecs_to_jiffies(TIME_1000MS));
	}
}

static void smbchg_re_det_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			re_det_work.work);

	pr_debug("chg->redet_count=%d\n", chg->redet_count);
	if (chg->usb_enum_status) {
		pr_info("re_det, usb_enum_status\n");
		chg->redet_count = 0;
		return;
	}
	if (!chg->vbus_present) {
		pr_info("re_det, vbus_no_present\n");
		chg->redet_count = 0;
		return;
	}
	if (chg->support_hvdcp &&
	    (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP ||
	     chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3 ||
	     chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5)) {
		pr_info("%s() qc charger, return\n", __func__);
		chg->redet_count = 0;
		return;
	}

	if (chg->redet_count >= REDET_COUTNT) {
		op_rerun_apsd(chg);
		chg->usb_type_redet_done = true;
	} else {
		chg->redet_count++;
		schedule_delayed_work(&chg->re_det_work,
				msecs_to_jiffies(TIME_1000MS));
	}
}

static void op_recovery_set_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			op_re_set_work.work);
	int rc = 0;

	pr_debug("chg->reset_count=%d\n", chg->reset_count);
	if (!chg->charger_collpse) {
		chg->reset_count = 0;
		return;
	}
	if (!chg->vbus_present) {
		chg->reset_count = 0;
		return;
	}

	if (chg->reset_count >= 13) {
		pr_err("op_set_collapse_fet\n");
#ifdef OP_SWARP_SUPPORTED
		if (chg->swarp_supported)
			rc = smblib_write(chg, USBIN_AICL_OPTIONS_CFG_REG, 0xc3);
		else
#endif
		rc = smblib_write(chg, USBIN_AICL_OPTIONS_CFG_REG, 0xc7);
		if (rc < 0)
			smblib_err(chg,
			"Couldn't enable OTG regulator rc=%d\n", rc);
		chg->charger_collpse = false;
		chg->reset_count = 0;
	} else {
		chg->reset_count++;
		schedule_delayed_work(&chg->op_re_set_work,
				msecs_to_jiffies(TIME_1000MS));
	}
}

void aging_test_check_aicl(struct smb_charger *chg)
{
	int aicl_result = 0, vbat = 0;

	if (chg->usb_enum_status)
		return;
	vbat = get_prop_fg_voltage_now(chg) / 1000;
	aicl_result = op_get_aicl_result(chg);
	if (aicl_result < 800*1000) {
		if (vbat < 4000) {
			pr_info("set icl 900mA\n");
			vote(chg->usb_icl_votable, AICL_RERUN_VOTER,
				true, 900*1000);
			vote(chg->usb_icl_votable, AICL_RERUN_VOTER, false, 0);
		}
	}
}

#define SKIN_THERMAL_DEFAULT_TEMP 25
static int op_get_skin_thermal_temp(struct smb_charger *chg)
{
	int ret = 0, result = 0, thermal_temp = 0;
	bool lcd_status;

	if (chg->iio.op_skin_therm_chan) {
		ret = iio_read_channel_processed(
				chg->iio.op_skin_therm_chan,
				&result);
		if (ret < 0) {
			smblib_err(chg, "Error in reading IIO channel data, rc=%d\n",
					ret);
			return ret;
		}
		thermal_temp = result/1000;
		if ((thermal_temp >= 0) && (thermal_temp <= 60)) //filter to valid value
			chg->skin_thermal_temp = thermal_temp;
		else
			chg->skin_thermal_temp = SKIN_THERMAL_DEFAULT_TEMP;
	}  else {
		pr_err("op_skin_therm_chan no found!\n");
		return -ENODATA;
	}

	lcd_status = check_lcd_on_status();
	if (lcd_status) {
		if (op_get_fastchg_ing(chg)) {
			if (chg->skin_thermal_temp >= chg->skin_thermal_medium_threshold)
				chg->is_skin_thermal_medium = true;
			else if (chg->skin_thermal_temp < chg->skin_thermal_normal_threshold)
				chg->is_skin_thermal_medium = false;

			if (chg->skin_thermal_temp >= chg->skin_thermal_warm_threshold)
				chg->is_skin_thermal_warm = true;
			else if (chg->skin_thermal_temp < chg->skin_thermal_pre_warm_threshold)
				chg->is_skin_thermal_warm = false;

			if (chg->skin_thermal_temp >= chg->skin_thermal_high_threshold)
				chg->is_skin_thermal_high = true;
			else if (chg->skin_thermal_temp < chg->skin_thermal_pre_high_threshold)
				chg->is_skin_thermal_high = false;
		} else {
			if (chg->skin_thermal_temp >= chg->nor_skin_thermal_high_threshold)
				chg->is_skin_thermal_high = true;
			else if (chg->skin_thermal_temp < chg->nor_skin_thermal_pre_high_threshold)
				chg->is_skin_thermal_high = false;

			if (chg->skin_thermal_temp >= chg->nor_skin_thermal_warm_threshold)
				chg->is_skin_thermal_warm = true;
			else if (chg->skin_thermal_temp < chg->nor_skin_thermal_pre_warm_threshold)
				chg->is_skin_thermal_warm = false;

			if (chg->skin_thermal_temp >= chg->nor_skin_thermal_medium_threshold)
				chg->is_skin_thermal_medium = true;
			else if (chg->skin_thermal_temp < chg->nor_skin_thermal_normal_threshold)
				chg->is_skin_thermal_medium = false;
		}
	} else {
		if (op_get_fastchg_ing(chg)) {
			if (chg->skin_thermal_temp >= chg->skin_thermal_lcdoff_medium_threshold)
				chg->is_skin_thermal_medium = true;
			else if (chg->skin_thermal_temp < chg->skin_thermal_lcdoff_normal_threshold)
				chg->is_skin_thermal_medium = false;

			if (chg->skin_thermal_temp >= chg->skin_thermal_lcdoff_high_threshold)
				chg->is_skin_thermal_high = true;
			else if (chg->skin_thermal_temp < chg->skin_thermal_lcdoff_pre_high_threshold)
				chg->is_skin_thermal_high = false;
		} else {
			if (chg->skin_thermal_temp >= chg->nor_skin_thermal_lcdoff_high_threshold)
				chg->is_skin_thermal_high = true;
			else if (chg->skin_thermal_temp < chg->nor_skin_thermal_lcdoff_pre_high_threshold)
				chg->is_skin_thermal_high = false;

			if (chg->skin_thermal_temp >= chg->nor_skin_thermal_lcdoff_warm_threshold)
				chg->is_skin_thermal_warm = true;
			else if (chg->skin_thermal_temp < chg->nor_skin_thermal_lcdoff_pre_warm_threshold)
				chg->is_skin_thermal_warm = false;

			if (chg->skin_thermal_temp >= chg->nor_skin_thermal_lcdoff_medium_threshold)
				chg->is_skin_thermal_medium = true;
			else if (chg->skin_thermal_temp < chg->nor_skin_thermal_lcdoff_normal_threshold)
				chg->is_skin_thermal_medium = false;
		}
	}

	pr_info("skin_thermal_temp=(%d), skin_medium(%d), skin_warm(%d), skin_high(%d), lcd(%d), pd_active(%d)\n",
			chg->skin_thermal_temp,
			chg->is_skin_thermal_medium,
			chg->is_skin_thermal_warm,
			chg->is_skin_thermal_high,
			lcd_status, chg->pd_active);

	return chg->skin_thermal_temp;
}

bool check_skin_thermal_high(void)
{
	int thermal_temp;

	if (!g_chg->enable_dash_current_adjust
		&& !g_chg->enable_nor_current_adjust)
		return false;

	thermal_temp = op_get_skin_thermal_temp(g_chg);
	if (thermal_temp >= 0)
		return g_chg->is_skin_thermal_high;
	else
		return false;
}

bool check_skin_thermal_warm(void)
{
	int thermal_temp;

	if (!g_chg->enable_dash_current_adjust
		&& !g_chg->enable_nor_current_adjust)
		return false;

	thermal_temp = op_get_skin_thermal_temp(g_chg);
	if (thermal_temp >= 0)
		return g_chg->is_skin_thermal_warm;
	else
		return false;
}

bool check_skin_thermal_medium(void)
{
	int thermal_temp;

	if (!g_chg->enable_dash_current_adjust
		&& !g_chg->enable_nor_current_adjust)
		return false;

	thermal_temp = op_get_skin_thermal_temp(g_chg);
	if (thermal_temp >= 0)
		return g_chg->is_skin_thermal_medium;
	else
		return false;
}

bool check_call_on_status(void)
{
	bool is_call_on;

	if (!g_chg->enable_dash_current_adjust
		&& !g_chg->enable_nor_current_adjust)
		return false;

	is_call_on = *g_chg->call_on;

	if (is_call_on == 1) {
		pr_info("is_call_on=(%d)\n",
				is_call_on);
		return true;
	}

	pr_info("is_call_on=(%d)\n",
				is_call_on);
	return false;
}

bool check_video_call_on_status(void)
{
	bool is_video_call_on;

	if (!g_chg->enable_dash_current_adjust
		&& !g_chg->enable_nor_current_adjust)
		return false;

	is_video_call_on = *g_chg->video_call_on;

	if (is_video_call_on > 0) {
		pr_info("is_video_call_on=(%d)\n",
				is_video_call_on);
		return true;
	}

	pr_info("is_video_call_on=(%d)\n",
				is_video_call_on);
	return false;
}

bool check_lcd_on_status(void)
{
	static bool pre_lcd_status;

	if (!g_chg)
		return false;
	if (time_is_before_jiffies(g_chg->lcd_st_debounce_expire))
		pre_lcd_status = g_chg->oem_lcd_is_on;
	pr_info("lcd_status: pre=%d, now=%d", pre_lcd_status,
		g_chg->oem_lcd_is_on);
	return pre_lcd_status;
}

int cur_max[] = {0x06, 0x05, 0x04, 0x03};
int op_get_allowed_current_max(void)
{
	int cur_stage = 0;

	if (!g_chg)
		return cur_max[0];
	cur_stage = g_chg->reconnect_count;
	if (cur_stage > 3)
		cur_stage = 3;
	else if (cur_stage < 0)
		cur_stage = 0;
	return cur_max[cur_stage];
}
#define USB_CONNECTOR_DEFAULT_TEMP 25
static int get_usb_temp(struct smb_charger *chg)
{
	int ret, i, result, temp = 0, step_value = 0, temp_1 = 0, temp_2 = 0;

	if (chg->iio.op_connector_temp_chan) {
		ret = iio_read_channel_processed(
				chg->iio.op_connector_temp_chan,
				&result);
		if (ret < 0) {
			smblib_err(chg, "Error in reading IIO channel data, rc=%d\n",
					ret);
			return USB_CONNECTOR_DEFAULT_TEMP;
		}
		chg->connecter_voltage = result/1000;
	} else {
		pr_err("op_connector_temp_chan no found!\n");
		return USB_CONNECTOR_DEFAULT_TEMP;
	}
	for (i = ARRAY_SIZE(con_volt_30k) - 1; i >= 0; i--) {
		if (con_volt_30k[i] >= chg->connecter_voltage)
			break;
		else if (i == 0)
			break;
	}

	smblib_dbg(chg, PR_FAST_DEBUG, "connecter(vol:%d,temp:%d),Ibatt:%d,batt_temp:%d\n",
				chg->connecter_voltage, con_temp_30k[i],
				get_prop_batt_current_now(chg) / 1000,
				get_prop_batt_temp(chg) / 10);

	smblib_dbg(chg, PR_FAST_DEBUG, "connector_voltage_01:%04dmv,connector_temp_01:%03d\n",
				chg->connecter_voltage,
				con_temp_30k[i]);
	temp_1 = con_temp_30k[i];
	chg->connecter_temp_1 = temp_1;

	if (chg->iio.op_connector_temp_chan_sec) {
		ret = iio_read_channel_processed(
				chg->iio.op_connector_temp_chan_sec,
				&result);
		chg->connecter_voltage = result/1000;
	} else {
		pr_err("op_connector_temp_chan no found!\n");
		return -ENODATA;
	}
	for (i = ARRAY_SIZE(con_volt_30k) - 1; i >= 0; i--) {
		if (con_volt_30k[i] >= chg->connecter_voltage)
			break;
		else if (i == 0)
			break;
	}

	smblib_dbg(chg, PR_FAST_DEBUG, "connector_voltage_02:%04dmv,connector_temp_02:%03d\n",
				chg->connecter_voltage,
				con_temp_30k[i]);
	temp_2 = con_temp_30k[i];
	chg->connecter_temp_2 = temp_2;

	if (temp_1 >= temp_2)
		temp = temp_1;
	else
		temp = temp_2;

	step_value = temp - chg->connecter_temp;

	/*WR for temperature value(70~85) and steep filter, use last value*/
	if (((temp >= 70) && (temp <= 85)) || (step_value >= 10)) {
		chg->filter_count++;
		if (chg->filter_count <= 3) {
			pr_info("con_temp=(%d) pre_temp=(%d) filter_count(%d),filter not report!\n",
				temp,
				chg->connecter_temp,
				chg->filter_count);
			return chg->connecter_temp;
		}
		chg->filter_count = 0;
	} else
		chg->filter_count = 0;

	return temp;

}

void op_disconnect_vbus(struct smb_charger *chg, bool enable)
{
	char *recovered[2] = { "USB_CONTAMINANT=RECOVERED", NULL };
	char *detected[2] = { "USB_CONTAMINANT=DETECTED", NULL };

	if (*chg->disable_connector_protect) {
		pr_info("disable usb connector protect, return!\n");
		return;
	}
#ifdef	CONFIG_OP_DEBUG_CHG
/* *#806# aging test not need Vbus disconnect feature*/
	return;
#else
	if (chg->is_aging_test)
		return;
#endif
	if (!gpio_is_valid(chg->vbus_ctrl))
		return;
	if (!enable) {
		gpio_set_value(chg->vbus_ctrl, 0);
		chg->disconnect_vbus = false;
		kobject_uevent_env(&chg->dev->kobj, KOBJ_CHANGE, recovered);
		pr_info("usb connecter cool(%d), Vbus connected! Sent uevent %s\n",
				chg->connecter_temp, recovered[0]);
		return;
	}
	kobject_uevent_env(&chg->dev->kobj, KOBJ_CHANGE, detected);
	pr_info("usb connecter hot(%d),Vbus disconnected! Sent uevent %s\n",
			chg->connecter_temp, detected[0]);
	chg->dash_on = get_prop_fast_chg_started(chg);
	if (chg->dash_on) {
		if (!chg->swarp_supported) {
			mcu_en_reset();
			usleep_range(2000, 2001);
			switch_mode_to_normal();
		} else
			(void)opchg_mcu_action(ACTION_MODE_RESET_SLEEP);
		op_set_fast_chg_allow(chg, false);
	}
	smblib_set_usb_suspend(chg, true);
	msleep(20);
	gpio_set_value(chg->vbus_ctrl, 1);
	chg->disconnect_vbus = true;

	/* CC set sink only mode */
	vote(chg->otg_toggle_votable, HOT_PROTECT_VOTER, 0, 0);

	/* Charge recovery monitor */
	schedule_delayed_work(&chg->connecter_recovery_work,
		msecs_to_jiffies(1000));
}

int plugin_update(struct smb_charger *chg)
{
	int rc = 0;
	char *connected_str[2] = { "USBCable=CONNECTED", NULL};
	char *disconnected_str[2] = { "USBCable=DISCONNECTED", NULL};

	rc = kobject_uevent_env(&chg->dev->kobj, KOBJ_CHANGE,
		chg->hw_detect ? connected_str : disconnected_str);

	return rc;
}

/*usb connector hw auto detection*/
static void op_otg_switch(struct work_struct *work)
{
	bool usb_pluged;
	bool user_vote_enable;
	int wireless_present = 0;
	union power_supply_propval pval;
	int rc;

	if (!g_chg)
		return;

	if (!g_chg->wls_psy)
		g_chg->wls_psy = power_supply_get_by_name("wireless");
	if (g_chg->wls_psy) {
		rc = power_supply_get_property(g_chg->wls_psy,
					       POWER_SUPPLY_PROP_PRESENT,
					       &pval);
		if (rc < 0)
			smblib_err(g_chg, "Couldn't get wireless present rc=%d\n", rc);
		else
			wireless_present = pval.intval;
	} else {
		smblib_err(g_chg, "Couldn't find wireless psy\n");
	}

	user_vote_enable = is_client_vote_enabled(g_chg->otg_toggle_votable, USER_VOTER);
	usb_pluged = gpio_get_value(g_chg->plug_irq) ? false : true;
	if (usb_pluged == g_chg->pre_cable_pluged) {
		pr_info("same status,return;usb_present:%d\n", usb_pluged);
		if (usb_pluged) {
			vote(g_chg->otg_toggle_votable, HW_DETECT_VOTER, 1, 0);
			if (user_vote_enable && wireless_present)
				rerun_election(g_chg->otg_toggle_votable);
		} else {
			vote(g_chg->otg_toggle_votable, HW_DETECT_VOTER, 0, 0);
		}
		return;
	}

	if (usb_pluged) {
		vote(g_chg->otg_toggle_votable, HW_DETECT_VOTER, 1, 0);
		if (user_vote_enable && wireless_present)
			rerun_election(g_chg->otg_toggle_votable);
		g_chg->hw_detect = 1;
	} else {
		/* For GCE-2351 issue Debug patch, do not set cc mode to
		 * sink-only mode when type-c disconnect
		 */
		vote(g_chg->otg_toggle_votable, HW_DETECT_VOTER, 0, 0);
		g_chg->hw_detect = 0;
		schedule_delayed_work(&g_chg->unplug_check_work,
				msecs_to_jiffies(TIME_200MS));
	}
	rc = plugin_update(g_chg);
	pr_info("%s:hw_detect=%d and report rc: %d\n",
					__func__, g_chg->hw_detect, rc);
	g_chg->pre_cable_pluged = usb_pluged;
	g_chg->reconnect_count = 0;
	g_chg->tried_fastchg = false;
	g_chg->icon_debounce = false;
}
static void op_check_charger_wakup_source(struct smb_charger *chg, int vchg_mv)
{

	struct wakeup_source *ws;

	ws = chg->chg_wake_lock;
	if (chg->vbus_present)
		return;
	if (ws->active && vchg_mv <= 200) {
		pr_info("%s release chg_wake_lock\n", __func__);
		__pm_relax(chg->chg_wake_lock);
	}

}

static void op_heartbeat_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
			struct smb_charger, heartbeat_work);
	enum temp_region_type temp_region;
	int batt_temp = 0;
	bool charger_present = 0;
	bool fast_charging = 0;
	static int vbat_mv;
	union power_supply_propval vbus_val = {0, };
	union power_supply_propval pval = {0, };
	int vbatdet_mv;
	int rc;

	op_otg_icl_contrl(chg);
#ifndef CONFIG_OP_DEBUG_CHG
	op_check_charge_timeout(chg);
#endif
	if (chg->wireless_present && !chg->wls_psy)
		chg->wls_psy = power_supply_get_by_name("wireless");

#ifdef CONFIG_OP_DEBUG_CHG
	rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
	if (rc < 0) {
		pr_err("failed to read usb_voltage rc=%d\n", rc);
		vbus_val.intval = CHG_VOLTAGE_NORMAL;
	}
#endif
	charger_present = is_usb_present(chg);
	if (!charger_present)
		goto out;
	/* charger present */
	power_supply_changed(chg->batt_psy);

	if (chg->wlchg_fast)
		goto out;

	if (chg->swarp_supported)
		op_check_battery_vol_diff(chg);

	chg->dash_on = get_prop_fast_chg_started(chg);
	if (chg->dash_on) {
		if (chg->chg_disabled) {
			set_usb_switch(chg, false);
			goto out;
		}
		switch_fast_chg(chg);
		pr_info("fast chg started, usb_switch=%d\n",
				op_is_usb_switch_on(chg));
		/* add for disable normal charge */
		fast_charging = op_get_fastchg_ing(chg);
		if (fast_charging) {
			if (!chg->disable_normal_chg_for_dash) {
				op_charging_en(chg, false);
				smblib_set_usb_suspend(chg, true);
			}
			chg->disable_normal_chg_for_dash = true;
			chg->fastchg_present_wait_count = 0;
		}
		goto out;
	} else {
		if (chg->disable_normal_chg_for_dash) {
			chg->fastchg_present_wait_count++;
			if (chg->fastchg_present_wait_count >= FULL_DELAY_COUNT) {
				chg->disable_normal_chg_for_dash = false;
				smblib_set_usb_suspend(chg, false);
				op_charging_en(chg, true);
				chg->fastchg_present_wait_count = 0;
				rerun_election(chg->usb_icl_votable);
			}
		}
	}
	rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
	if (rc < 0) {
		pr_err("failed to read usb_voltage rc=%d\n", rc);
		vbus_val.intval = CHG_VOLTAGE_NORMAL;
	}
	op_check_charger_wakup_source(chg, vbus_val.intval);

	if (chg->support_hvdcp && !chg->pd_active)
		op_check_qc_charge_config(chg);
	op_check_charger_uovp(chg, vbus_val.intval);
	op_check_battery_uovp(chg);
	norchg_check_skin_thermal_temp(chg);
	if (vbus_val.intval > 4500)
		op_check_charger_collapse(chg);

	vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
	temp_region = op_battery_temp_region_get(chg);
	if (temp_region == BATT_TEMP_LITTLE_COOL
		|| temp_region ==  BATT_TEMP_COOL) {
		if (vbat_mv > chg->temp_littel_cool_voltage
		&& chg->temp_littel_cool_set_current_0_point_25c) {
			chg->is_power_changed = true;
		} else if (vbat_mv < chg->temp_littel_cool_voltage - 200
		&& !chg->temp_littel_cool_set_current_0_point_25c) {
			chg->is_power_changed = true;
		}
	}

	if ((chg->disallow_fast_high_temp || chg->disallow_fast_bad_vol)
		&& !chg->hvdcp_check) {
		batt_temp = get_prop_batt_temp(chg);
		if ((batt_temp >= DASH_VALID_TEMP_LOW_THRESHOLD
			&& batt_temp <=  DASH_VALID_TEMP_HIG_THRESHOLD)
			&& vbat_mv > DASH_VALID_VBAT_LOW_THRE_MV) {
			chg->disallow_fast_high_temp = false;
			chg->disallow_fast_bad_vol = false;
			switch_fast_chg(chg);
			pr_info("try switch to fastchg again(since last bad status).");
		}
	}

	if (is_charger_not_match(chg)) {
		if ((chg->dash_need_check_vbat
			&& vbat_mv > chg->dash_vbat_thr_mv)
			|| (!chg->dash_need_check_vbat
			&& vbat_mv < chg->dash_vbat_thr_mv - 200))
			chg->is_power_changed = true;
	}
	if (chg->swarp_supported)
		swarp_ffc_check(chg);
	else
		ffc_exit(chg);

	checkout_term_current(chg);
	if (chg->wireless_present) {
		rc = power_supply_get_property(chg->wls_psy,
					       POWER_SUPPLY_PROP_VBATDET,
					       &pval);
		if (rc < 0) {
			pr_err("can't get wireless charge vbatdet\n");
			vbatdet_mv = 0;
		} else {
			vbatdet_mv = pval.intval;
		}
	} else {
		if (temp_region > BATT_TEMP_COLD && temp_region < BATT_TEMP_HOT)
			vbatdet_mv = chg->vbatdet[temp_region];
		else
			vbatdet_mv = 0;
	}
	if (!chg->chg_ovp && chg->chg_done
			&& temp_region > BATT_TEMP_COLD
			&& temp_region < BATT_TEMP_HOT
			&& vbatdet_mv >= vbat_mv) {
		chg->chg_done = false;
		chg->recharge_pending = true;
		chg->recharge_status = true;

		op_charging_en(chg, true);
		pr_debug("temp_region=%d, recharge_pending\n", temp_region);
	}

	if (!chg->chg_ovp && chg->battery_status == BATT_STATUS_GOOD
			&& !chg->time_out) {
		op_check_battery_temp(chg);
	}
#ifdef CONFIG_OP_DEBUG_CHG
	chg->dump_count++;
	if (chg->dump_count == 600) {
		chg->dump_count = 0;
		if ((get_prop_batt_current_now(chg) / 1000) > 0) {
			op_dump_regs(chg);
			aging_test_check_aicl(chg);
		}
	}
#else
	if (chg->is_aging_test) {
		chg->dump_count++;
		if (chg->dump_count == 600) {
			chg->dump_count = 0;
			if ((get_prop_batt_current_now(chg) / 1000) > 0) {
				op_dump_regs(chg);
				aging_test_check_aicl(chg);
			}
		}
	}

#endif
out:
		smblib_dbg(chg, PR_OP_DEBUG, "CAP=%d (Q:%d), VBAT=%d (Q:%d), IBAT=%d (Q:%d), BAT_TEMP=%d, CONNECTOR_TEMP=%d, CHG_TYPE=%d, VBUS=%d AICL:%d\n",
				get_prop_batt_capacity(chg),
				get_prop_fg_capacity(chg),
				get_prop_batt_voltage_now(chg) / 1000,
				get_prop_fg_voltage_now(chg) / 1000,
				get_prop_batt_current_now(chg) / 1000,
				get_prop_fg_current_now(chg) / 1000,
				get_prop_batt_temp(chg),
				chg->connecter_temp,
				chg->usb_psy_desc.type,
				vbus_val.intval,
				op_get_aicl_result(chg));
	/*update time 6s*/
	schedule_delayed_work(&chg->heartbeat_work,
			round_jiffies_relative(msecs_to_jiffies
				(HEARTBEAT_INTERVAL_MS)));
}


static int op_sdram_read(struct smb_charger *chg, u16 addr, u8 *val)
{
	unsigned int temp = 0;
	int rc = 0;

	pr_info("%s enter,temp:0x%x rc:0x%x\n", __func__, temp, rc);
	if (sdram_regmap) {
		rc = regmap_read(sdram_regmap, addr, &temp);
		if (rc >= 0)
			*val = (u8)temp;
	}
	pr_info("%s end,temp:0x%x,rc:0x%x\n", __func__, temp, rc);
	return rc;
}

static int op_sdram_write(struct smb_charger *chg, u16 addr, u8 val)
{
	int rc = 0;

	if (sdram_regmap)
		rc = regmap_write(sdram_regmap, addr, val);
	return rc;
}

static int op_read(struct smb_charger *chg, u16 addr, u8 *val)
{
	unsigned int temp = 0;
	int rc = 0;

	pr_info("%s enter,temp:0x%x rc:0x%x\n", __func__, temp, rc);
	if (pm_regmap) {
		rc = regmap_read(pm_regmap, addr, &temp);
		if (rc >= 0)
			*val = (u8)temp;
	}
	pr_info("%s end,temp:0x%x,rc:0x%x\n", __func__, temp, rc);
	return rc;
}

static int op_write(struct smb_charger *chg, u16 addr, u8 val)
{
	int rc = 0;

	if (pm_regmap)
		rc = regmap_write(pm_regmap, addr, val);
	return rc;
}


/*Modify for backup soc  Begin  */
int op_masked_write(struct smb_charger *chg, u16 addr, u8 mask, u8 val)
{
	int rc = 0;

	if (pm_regmap)
		rc = regmap_update_bits(pm_regmap, addr, mask, val);
	else
		pr_err("pm_pon is NULL\n");
	return rc;
}

void op_write_backup_flag(struct smb_charger *chg, bool bk_flag)
{
	int rc = 0;

	if (chg->support_sdram_store_soc) {
		if (pm8150_regmap)
			rc = regmap_update_bits(pm8150_regmap,
						SOC_DATA_FLAG_SDRAM, SOC_FLAG_BIT, bk_flag ? SOC_FLAG_BIT : 0);
		else
			pr_err("pm_pon is NULL\n");
	} else {
		rc = op_masked_write(chg, SOC_FLAG_REG,
			BIT(0), bk_flag ? BIT(0):0);
		if (rc) {
			pr_err("failed to clean PM addr[0x%x], rc=%d\n",
				SOC_FLAG_REG, rc);
		}
	}
}

int op_read_backup_flag(struct smb_charger *chg)
{
	u8 flag = 0;
	int rc = 0;
	unsigned int temp = 0;

	if (chg->support_sdram_store_soc) {
		if (pm8150_regmap) {
			rc = regmap_read(pm8150_regmap, SOC_DATA_FLAG_SDRAM, &temp);
			if (rc < 0) {
				pr_info("%s read wrong temp:0x%x,rc:0x%x\n", __func__, temp, rc);
				temp = 0;
			}
		}
		flag = temp & SOC_FLAG_BIT;
		pr_info("%s end,temp:0x%x,rc:0x%x\n", __func__, temp, rc);
	} else {
			pr_info("%s enter,flag:0x%x,rc:0x%x\n", __func__, flag, rc);
			rc = op_read(chg, SOC_FLAG_REG, &flag);
			if (rc) {
				pr_err("failed to read PM addr[0x%x], rc=%d\n",
					SOC_FLAG_REG, rc);
				return 0;
			}
			pr_info("%s end,flag:0x%x,rc:0x%x\n", __func__, flag, rc);
			flag = flag & BIT(0);
	}
	return flag;
}

static int load_data(struct smb_charger *chip)
{
	u8 stored_soc = 0;
	int rc = 0, shutdown_soc = 0;

	if (!chip) {
		pr_err("chip is NULL !\n");
		return SOC_INVALID;
	}
	if (!op_read_backup_flag(chip))
		return SOC_INVALID;
	if (chip->support_sdram_store_soc) {
		rc = op_sdram_read(chip, SOC_DATA_REG_SDRAM, &stored_soc);
		if (rc) {
			pr_err("failed to read addr[0x%x], rc=%d\n",
				SOC_DATA_REG_SDRAM, rc);
			return SOC_INVALID;
		}
	} else {
			rc = op_read(chip, SOC_DATA_REG_0, &stored_soc);
			if (rc) {
				pr_err("failed to read addr[0x%x], rc=%d\n",
					SOC_DATA_REG_0, rc);
				return SOC_INVALID;
			}
	}
	shutdown_soc = (stored_soc >> 1); /* get data from bit1~bit7 */
	pr_info("stored_soc[0x%x], shutdown_soc[%d]\n",
		stored_soc, shutdown_soc);
	return shutdown_soc;
}

int load_soc(void)
{
	int soc = 0;

	soc = load_data(g_chg);
	if (soc == SOC_INVALID || soc < 0 || soc > 100)
		return -EINVAL;
	return soc;
}

static void clear_backup_soc(struct smb_charger *chip)
{
	int rc = 0;
	u8 soc_temp = 0;

	if (chip->support_sdram_store_soc) {
		rc = op_sdram_write(chip, SOC_DATA_REG_SDRAM, soc_temp);
		if (rc)
			pr_err("failed to clean addr[0x%x], rc=%d\n",
					SOC_DATA_REG_SDRAM, rc);
		op_write_backup_flag(chip, false);
	} else {
			rc = op_write(chip, SOC_DATA_REG_0, soc_temp);
			if (rc)
				pr_err("failed to clean addr[0x%x], rc=%d\n",
						SOC_DATA_REG_0, rc);
			op_write_backup_flag(chip, false);
	}
}

void clean_backup_soc_ex(void)
{
	if (g_chg)
		clear_backup_soc(g_chg);
}

static void backup_soc(struct smb_charger *chip, int soc)
{
	int rc = 0;
	u8 invalid_soc = SOC_INVALID;
	u8 soc_temp = (soc << 1); /* store data in bit1~bit7 */

	if (!chip || soc < 0 || soc > 100) {
		pr_err("chip or soc invalid, store an invalid soc\n");
		if (chip) {
			if (chip->support_sdram_store_soc) {
				rc = op_sdram_write(chip, SOC_DATA_REG_SDRAM, invalid_soc);
				if (rc)
					pr_err("failed to write addr[0x%x], rc=%d\n",
							SOC_DATA_REG_SDRAM, rc);
			} else {
					rc = op_write(chip, SOC_DATA_REG_0, invalid_soc);
					if (rc)
						pr_err("failed to write addr[0x%x], rc=%d\n",
								SOC_DATA_REG_0, rc);
			}
		}
		return;
	}

	pr_err("backup soc[%d]\n", soc);
	if (chip->support_sdram_store_soc) {
			rc = op_sdram_write(chip, SOC_DATA_REG_SDRAM, soc_temp);
			if (rc)
				pr_err("failed to write addr[0x%x], rc=%d\n",
						SOC_DATA_REG_SDRAM, rc);
			op_write_backup_flag(chip, true);
	} else {
			rc = op_write(chip, SOC_DATA_REG_0, soc_temp);
			if (rc)
				pr_err("failed to write addr[0x%x], rc=%d\n",
						SOC_DATA_REG_0, rc);
			op_write_backup_flag(chip, true);
	}
}
bool get_prop_fastchg_is_ok(struct smb_charger *chg)
{
	if (chg)
		return op_get_fast_chg_status_is_ok(chg);
	return true;
}
void backup_soc_ex(int soc)
{
	if (g_chg)
		backup_soc(g_chg, soc);
}
/* Modify for backup soc  End*/

enum chg_protect_status_type {
	PROTECT_CHG_OVP = 1, /* 1: VCHG > 5.8V     */
	PROTECT_BATT_MISSING, /* 2: battery missing */
	PROTECT_CHG_OVERTIME, /* 3: charge overtime */
	PROTECT_BATT_OVP, /* 4: vbat >= 4.5     */
	PROTECT_BATT_TEMP_REGION__HOT,/* 5: 55 < t          */
	PROTECT_BATT_TEMP_REGION_COLD,/* 6:      t <= -3    */
	PROTECT_BATT_TEMP_REGION_LITTLE_COLD, /* 7: -3 < t <=  0  */
	PROTECT_BATT_TEMP_REGION_COOL,/* 8:  0 < t <=  5    */
	PROTECT_BATT_TEMP_REGION_WARM,/* 9: 45 < t <= 55   */
	PROTECT_INVALID_CHARGER,/*10:invalid charger or slow charger*/
	PROTECT_BATT_VOL_DIFF_TOO_LARGE/*11:dual-cell battery voltage diff too large*/
};

int get_prop_chg_protect_status(struct smb_charger *chg)
{
	int temp = 0, rc = 0;
	bool batt_present = 0;
	enum temp_region_type temp_region;
	union power_supply_propval vbus_val;

	if (chg->use_fake_protect_sts)
		return chg->fake_protect_sts;

	if (!is_usb_present(chg))
		return 0;

	rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
	if (rc < 0) {
		pr_err("failed to read usb_voltage rc=%d\n", rc);
		vbus_val.intval = CHG_VOLTAGE_NORMAL;
	}

	temp = get_prop_batt_temp(chg);
	batt_present = get_prop_batt_present(chg);
	temp_region = op_battery_temp_region_get(chg);
	if (chg->chg_ovp && vbus_val.intval >= CHG_SOFT_OVP_MV - 100)
		return PROTECT_CHG_OVP;
	else if (!batt_present || BATT_REMOVE_TEMP > temp)
		return  PROTECT_BATT_MISSING;
	else if (chg->battery_status == BATT_STATUS_BAD)
		return PROTECT_BATT_OVP;
	else if (true == chg->time_out)
		return PROTECT_CHG_OVERTIME;
	else if (temp_region == BATT_TEMP_HOT)
		return PROTECT_BATT_TEMP_REGION__HOT;
	else if (temp_region == BATT_TEMP_COLD)
		return PROTECT_BATT_TEMP_REGION_COLD;
	else if (temp_region == BATT_TEMP_LITTLE_COLD
			&& (chg->chg_done || chg->recharge_status))
		return PROTECT_BATT_TEMP_REGION_LITTLE_COLD;
	else if (temp_region == BATT_TEMP_WARM
			&& (chg->chg_done || chg->recharge_status))
		return PROTECT_BATT_TEMP_REGION_WARM;
	else if (chg->slow_charger)
		return PROTECT_INVALID_CHARGER;
	else if (chg->battery_status == BATT_STATUS_BAD_VOL_DIFF)
		return PROTECT_BATT_VOL_DIFF_TOO_LARGE;
	else
		return 0;
}

bool get_prop_fastchg_status(struct smb_charger *chg)
{
	int capacity = 0;

	if (chg->dash_present)
		return true;

	if (chg->hvdcp_present) {
		capacity = get_prop_batt_capacity(chg);
		if (capacity >= 1 && capacity <= 85)
			return true;
	}

	return false;
}

static struct notify_dash_event notify_unplug_event  = {
	.notify_event	= update_dash_unplug_status,
	.op_contrl		= op_contrl,
	.notify_dash_charger_present
		= set_dash_charger_present,
	.update_dash_type = update_fastchg_type,
	.update_adapter_sid = update_adapter_sid,
	.suspend_disable_nor_charge = op_suspend_disable_norchg_by_warp,
};

void op_pm8998_regmap_register(struct regmap *regmap)
{
	if (pm_regmap) {
		pm_regmap = regmap;
		pr_err("multiple battery gauge called\n");
	} else {
		pm_regmap = regmap;
	}
}

void op_pm8150_regmap_register(struct regmap *regmap)
{
	if (pm8150_regmap) {
		pm8150_regmap = regmap;
		pr_err("op_pm8150 multiple battery gauge called\n");
	} else {
		pm8150_regmap = regmap;
	}
}

void op_sdram_regmap_register(struct regmap *regmap)
{
	if (sdram_regmap) {
		sdram_regmap = regmap;
		pr_err("sdram multiple battery gauge called\n");
	} else {
		sdram_regmap = regmap;
	}
}


void fastcharge_information_register(struct external_battery_gauge *fast_chg)
{
	if (fast_charger) {
		fast_charger = fast_chg;
		pr_err("multiple battery gauge called\n");
	} else {
		fast_charger = fast_chg;
	}
}
EXPORT_SYMBOL(fastcharge_information_register);

void fastcharge_information_unregister(struct external_battery_gauge *fast_chg)
{
	fast_charger = NULL;
}
EXPORT_SYMBOL(fastcharge_information_unregister);

static int notify_usb_enumeration_function(int status)
{
	pr_info("status=%d\n", status);
	g_chg->usb_enum_status = status;

	return g_chg->usb_enum_status;
}

static struct notify_usb_enumeration_status usb_enumeration  = {
	.notify_usb_enumeration		= notify_usb_enumeration_function,
};
static void smblib_usbov_dbc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						usbov_dbc_work.work);

	smblib_dbg(chg, PR_MISC, "Resetting USBOV debounce\n");
	chg->dbc_usbov = false;
	vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
}

#define USB_OV_DBC_PERIOD_MS		1000
irqreturn_t usbin_ov_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if (!(chg->wa_flags & USBIN_OV_WA))
		return IRQ_HANDLED;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	/*
	 * On specific PMICs, OV IRQ triggers for very small duration in
	 * interim periods affecting charging status reflection. In order to
	 * differentiate between OV IRQ glitch and real OV_IRQ, add a debounce
	 * period for evaluation.
	 */
	if (stat & USBIN_OV_RT_STS_BIT) {
		chg->dbc_usbov = true;
		vote(chg->awake_votable, USBOV_DBC_VOTER, true, 0);
		schedule_delayed_work(&chg->usbov_dbc_work,
				msecs_to_jiffies(USB_OV_DBC_PERIOD_MS));
	} else {
		cancel_delayed_work_sync(&chg->usbov_dbc_work);
		chg->dbc_usbov = false;
		vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
	}

	smblib_dbg(chg, PR_MISC, "USBOV debounce status %d\n",
				chg->dbc_usbov);
	return IRQ_HANDLED;
}

/**************
 * Additional USB PSY getters/setters
 * that call interrupt functions
 ***************/

int smblib_get_prop_pr_swap_in_progress(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->pr_swap_in_progress;
	return 0;
}

#define DETACH_DETECT_DELAY_MS 20
int smblib_set_prop_pr_swap_in_progress(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;
	u8 stat = 0, orientation;

	smblib_dbg(chg, PR_MISC, "Requested PR_SWAP %d\n", val->intval);
	chg->pr_swap_in_progress = val->intval;

	/* check for cable removal during pr_swap */
	if (!chg->pr_swap_in_progress) {
		cancel_delayed_work_sync(&chg->pr_swap_detach_work);
		vote(chg->awake_votable, DETACH_DETECT_VOTER, true, 0);
		schedule_delayed_work(&chg->pr_swap_detach_work,
				msecs_to_jiffies(DETACH_DETECT_DELAY_MS));
	}

	rc = smblib_masked_write(chg, TYPE_C_DEBOUNCE_OPTION_REG,
			REDUCE_TCCDEBOUNCE_TO_2MS_BIT,
			val->intval ? REDUCE_TCCDEBOUNCE_TO_2MS_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set tCC debounce rc=%d\n", rc);

	rc = smblib_masked_write(chg, TYPE_C_EXIT_STATE_CFG_REG,
			BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT,
			val->intval ? BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set exit state cfg rc=%d\n", rc);

	if (chg->pr_swap_in_progress) {
		rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n",
				rc);
		}

		orientation =
			stat & CC_ORIENTATION_BIT ? TYPEC_CCOUT_VALUE_BIT : 0;
		rc = smblib_masked_write(chg, TYPE_C_CCOUT_CONTROL_REG,
			TYPEC_CCOUT_SRC_BIT | TYPEC_CCOUT_BUFFER_EN_BIT
					| TYPEC_CCOUT_VALUE_BIT,
			TYPEC_CCOUT_SRC_BIT | TYPEC_CCOUT_BUFFER_EN_BIT
					| orientation);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_CCOUT_CONTROL_REG rc=%d\n",
				rc);
		}
	} else {
		rc = smblib_masked_write(chg, TYPE_C_CCOUT_CONTROL_REG,
			TYPEC_CCOUT_SRC_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_CCOUT_CONTROL_REG rc=%d\n",
				rc);
			return rc;
		}

		if (!chg->hw_detect)
			return 0;

		/* enable DRP */
		rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
				 TYPEC_POWER_ROLE_CMD_MASK, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable DRP rc=%d\n", rc);
			return rc;
		}
		chg->power_role = POWER_SUPPLY_TYPEC_PR_DUAL;
		smblib_dbg(chg, PR_MISC, "restore power role: %d\n",
				chg->power_role);
	}

	return 0;
}

/***************
 * Work Queues *
 ***************/
static void smblib_pr_lock_clear_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						pr_lock_clear_work.work);

	spin_lock(&chg->typec_pr_lock);
	if (chg->pr_lock_in_progress) {
		smblib_dbg(chg, PR_MISC, "restore type-c interrupts\n");
		smblib_typec_irq_config(chg, true);
		chg->pr_lock_in_progress = false;
	}
	spin_unlock(&chg->typec_pr_lock);
}

static void smblib_pr_swap_detach_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						pr_swap_detach_work.work);
	int rc;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_STATE_MACHINE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read STATE_MACHINE_STS rc=%d\n", rc);
		goto out;
	}
	smblib_dbg(chg, PR_REGISTER, "STATE_MACHINE_STS %x\n", stat);
	if (!(stat & TYPEC_ATTACH_DETACH_STATE_BIT)) {
		rc = smblib_request_dpdm(chg, false);
		if (rc < 0)
			smblib_err(chg, "Couldn't disable DPDM rc=%d\n", rc);

		if (chg->typec_port)
			typec_partner_unregister(chg);
	}
out:
	vote(chg->awake_votable, DETACH_DETECT_VOTER, false, 0);
}

static void smblib_uusb_otg_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						uusb_otg_work.work);
	int rc;
	u8 stat;
	bool otg;

	rc = smblib_read(chg, TYPEC_U_USB_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_3 rc=%d\n", rc);
		goto out;
	}
	otg = !!(stat & U_USB_GROUND_NOVBUS_BIT);
	if (chg->otg_present != otg)
		smblib_notify_usb_host(chg, otg);
	else
		goto out;

	chg->otg_present = otg;
	if (!otg)
		chg->boost_current_ua = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher,
				otg ? chg->chg_freq.freq_below_otg_threshold
					: chg->chg_freq.freq_removal);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_boost rc=%d\n", rc);

	smblib_dbg(chg, PR_REGISTER, "TYPE_C_U_USB_STATUS = 0x%02x OTG=%d\n",
			stat, otg);
	power_supply_changed(chg->usb_psy);

out:
	vote(chg->awake_votable, OTG_DELAY_VOTER, false, 0);
}

static void bms_update_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						bms_update_work);

	smblib_suspend_on_debug_battery(chg);

	if (chg->batt_psy)
		power_supply_changed(chg->batt_psy);
}

static void pl_update_work(struct work_struct *work)
{
	union power_supply_propval prop_val;
	struct smb_charger *chg = container_of(work, struct smb_charger,
						pl_update_work);
	int rc;

	if (chg->smb_temp_max == -EINVAL) {
		rc = smblib_get_thermal_threshold(chg,
					SMB_REG_H_THRESHOLD_MSB_REG,
					&chg->smb_temp_max);
		if (rc < 0) {
			dev_err(chg->dev, "Couldn't get charger_temp_max rc=%d\n",
					rc);
			return;
		}
	}

	prop_val.intval = chg->smb_temp_max;
	rc = power_supply_set_property(chg->pl.psy,
				POWER_SUPPLY_PROP_CHARGER_TEMP_MAX,
				&prop_val);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't set POWER_SUPPLY_PROP_CHARGER_TEMP_MAX rc=%d\n",
				rc);
		return;
	}

	if (chg->sec_chg_selected == POWER_SUPPLY_CHARGER_SEC_CP)
		return;

	smblib_select_sec_charger(chg, POWER_SUPPLY_CHARGER_SEC_PL,
				POWER_SUPPLY_CP_NONE, false);
}

static void smblib_panel_register_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							panel_register_work.work);
	int rc;

	pr_err("Register panel notifer for PD charging.\n");
#if defined(CONFIG_FB)
	chg->fb_notif.notifier_call = fb_notifier_callback;

	rc = fb_register_client(&chg->fb_notif);
	if (rc)
		pr_err("Unable to register fb_notifier: %d\n", rc);
#elif defined(CONFIG_MSM_RDM_NOTIFY)
	chg->msm_drm_notifier.notifier_call = msm_drm_notifier_callback;
	if (lcd_active_panel) {
		rc = drm_panel_notifier_register(lcd_active_panel, &chg->msm_drm_notifier);
		if (rc) {
			pr_err("Smb unable to register notifier: %d\n", rc);
		} else {
			pr_err("register notifier drm panel success!");
		}
	}
#endif /*CONFIG_FB*/
}

static void clear_hdc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						clear_hdc_work.work);

	chg->is_hdc = false;
	vote(chg->hdc_irq_disable_votable, HDC_IRQ_VOTER, false, 0);
}

static void smblib_icl_change_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							icl_change_work.work);
	int rc, settled_ua;

	rc = smblib_get_charge_param(chg, &chg->param.icl_stat, &settled_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
		return;
	}

	power_supply_changed(chg->usb_main_psy);

	smblib_dbg(chg, PR_INTERRUPT, "icl_settled=%d\n", settled_ua);
}

static void smblib_pl_enable_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							pl_enable_work.work);

	smblib_dbg(chg, PR_PARALLEL, "timer expired, enabling parallel\n");
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, false, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);
}

static void smblib_thermal_regulation_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						thermal_regulation_work.work);
	int rc;

	rc = smblib_update_thermal_readings(chg);
	if (rc < 0)
		smblib_err(chg, "Couldn't read current thermal values %d\n",
					rc);

	rc = smblib_process_thermal_readings(chg);
	if (rc < 0)
		smblib_err(chg, "Couldn't run sw thermal regulation %d\n",
					rc);
}

#define MOISTURE_PROTECTION_CHECK_DELAY_MS 300000		/* 5 mins */
static void smblib_moisture_protection_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						moisture_protection_work);
	int rc;
	bool usb_plugged_in;
	u8 stat;

	/*
	 * Hold awake votable to prevent pm_relax being called prior to
	 * completion of this work.
	 */
	vote(chg->awake_votable, MOISTURE_VOTER, true, 0);

	/*
	 * Disable 1% duty cycle on CC_ID pin and enable uUSB factory mode
	 * detection to track any change on RID, as interrupts are disable.
	 */
	rc = smblib_write(chg, ((chg->chg_param.smb_version == PMI632_SUBTYPE) ?
			PMI632_TYPEC_U_USB_WATER_PROTECTION_CFG_REG :
			TYPEC_U_USB_WATER_PROTECTION_CFG_REG), 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable periodic monitoring of CC_ID rc=%d\n",
			rc);
		goto out;
	}

	rc = smblib_masked_write(chg, TYPEC_U_USB_CFG_REG,
					EN_MICRO_USB_FACTORY_MODE_BIT,
					EN_MICRO_USB_FACTORY_MODE_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable uUSB factory mode detection rc=%d\n",
			rc);
		goto out;
	}

	/*
	 * Add a delay of 100ms to allow change in rid to reflect on
	 * status registers.
	 */
	msleep(100);

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		goto out;
	}
	usb_plugged_in = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);

	/* Check uUSB status for moisture presence */
	rc = smblib_read(chg, TYPEC_U_USB_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_U_USB_STATUS_REG rc=%d\n",
				rc);
		goto out;
	}

	/*
	 * Factory mode detection happens in case of USB plugged-in by using
	 * a different current source of 2uA which can hamper moisture
	 * detection. Since factory mode is not supported in kernel, factory
	 * mode detection can be considered as equivalent to presence of
	 * moisture.
	 */
	if (stat == U_USB_STATUS_WATER_PRESENT || stat == U_USB_FMB1_BIT ||
			stat == U_USB_FMB2_BIT || (usb_plugged_in &&
			stat == U_USB_FLOAT1_BIT)) {
		smblib_set_moisture_protection(chg, true);
		alarm_start_relative(&chg->moisture_protection_alarm,
			ms_to_ktime(MOISTURE_PROTECTION_CHECK_DELAY_MS));
	} else {
		smblib_set_moisture_protection(chg, false);
		rc = alarm_cancel(&chg->moisture_protection_alarm);
		if (rc < 0)
			smblib_err(chg, "Couldn't cancel moisture protection alarm\n");
	}

out:
	vote(chg->awake_votable, MOISTURE_VOTER, false, 0);
}

static enum alarmtimer_restart moisture_protection_alarm_cb(struct alarm *alarm,
							ktime_t now)
{
	struct smb_charger *chg = container_of(alarm, struct smb_charger,
					moisture_protection_alarm);

	smblib_dbg(chg, PR_MISC, "moisture Protection Alarm Triggered %lld\n",
			ktime_to_ms(now));

	/* Atomic context, cannot use voter */
	pm_stay_awake(chg->dev);
	schedule_work(&chg->moisture_protection_work);

	return ALARMTIMER_NORESTART;
}

static void smblib_chg_termination_work(struct work_struct *work)
{
	union power_supply_propval pval;
	struct smb_charger *chg = container_of(work, struct smb_charger,
						chg_termination_work);
	int rc, input_present, delay = CHG_TERM_WA_ENTRY_DELAY_MS;
	int vbat_now_uv, max_fv_uv;

	/*
	 * Hold awake votable to prevent pm_relax being called prior to
	 * completion of this work.
	 */
	vote(chg->awake_votable, CHG_TERMINATION_VOTER, true, 0);

	rc = smblib_is_input_present(chg, &input_present);
	if ((rc < 0) || !input_present)
		goto out;

	rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_REAL_CAPACITY, &pval);
	if ((rc < 0) || (pval.intval < 100)) {
		vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);
		vote(chg->dc_suspend_votable, CHG_TERMINATION_VOTER, false, 0);
		goto out;
	}

	/* Get the battery float voltage */
	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_VOLTAGE_MAX,
				&pval);
	if (rc < 0)
		goto out;

	max_fv_uv = pval.intval;

	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CHARGE_FULL,
					&pval);
	if (rc < 0)
		goto out;

	/*
	 * On change in the value of learned capacity, re-initialize the
	 * reference cc_soc value due to change in cc_soc characteristic value
	 * at full capacity. Also, in case cc_soc_ref value is reset,
	 * re-initialize it.
	 */
	if (pval.intval != chg->charge_full_cc || !chg->cc_soc_ref) {
		chg->charge_full_cc = pval.intval;

		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
		if (rc < 0)
			goto out;

		/*
		 * Store the Vbat at the charge termination to compare with
		 * the current voltage to see if the Vbat is increasing after
		 * charge termination in BSM.
		 */
		chg->term_vbat_uv = pval.intval;
		vbat_now_uv = pval.intval;

		rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CC_SOC,
					&pval);
		if (rc < 0)
			goto out;

		chg->cc_soc_ref = pval.intval;
	} else {
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
		if (rc < 0)
			goto out;

		vbat_now_uv = pval.intval;

		rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CC_SOC,
					&pval);
		if (rc < 0)
			goto out;
	}

	/*
	 * In BSM a sudden jump in CC_SOC is not expected. If seen, its a
	 * good_ocv or updated capacity, reject it.
	 */
	if (chg->last_cc_soc && pval.intval > (chg->last_cc_soc + 100)) {
		/* CC_SOC has increased by 1% from last time */
		chg->cc_soc_ref = pval.intval;
		smblib_dbg(chg, PR_MISC, "cc_soc jumped(%d->%d), reset cc_soc_ref\n",
				chg->last_cc_soc, pval.intval);
	}
	chg->last_cc_soc = pval.intval;

	/*
	 * Suspend/Unsuspend USB input to keep cc_soc within the 0.5% to 0.75%
	 * overshoot range of the cc_soc value at termination and make sure that
	 * vbat is indeed rising above vfloat.
	 */
	if (pval.intval < DIV_ROUND_CLOSEST(chg->cc_soc_ref * 10050, 10000)) {
		vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);
		vote(chg->dc_suspend_votable, CHG_TERMINATION_VOTER, false, 0);
		delay = CHG_TERM_WA_ENTRY_DELAY_MS;
	} else if ((pval.intval > DIV_ROUND_CLOSEST(chg->cc_soc_ref * 10075,
								10000))
		  && ((vbat_now_uv > chg->term_vbat_uv) &&
		     (vbat_now_uv > max_fv_uv))) {

		if (input_present & INPUT_PRESENT_USB)
			vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER,
					true, 0);
		if (input_present & INPUT_PRESENT_DC)
			vote(chg->dc_suspend_votable, CHG_TERMINATION_VOTER,
					true, 0);
		delay = CHG_TERM_WA_EXIT_DELAY_MS;
	}

	smblib_dbg(chg, PR_MISC, "Chg Term WA readings: cc_soc: %d, cc_soc_ref: %d, delay: %d vbat_now %d term_vbat %d\n",
			pval.intval, chg->cc_soc_ref, delay, vbat_now_uv,
			chg->term_vbat_uv);
	alarm_start_relative(&chg->chg_termination_alarm, ms_to_ktime(delay));
out:
	vote(chg->awake_votable, CHG_TERMINATION_VOTER, false, 0);
}

static enum alarmtimer_restart chg_termination_alarm_cb(struct alarm *alarm,
								ktime_t now)
{
	struct smb_charger *chg = container_of(alarm, struct smb_charger,
							chg_termination_alarm);

	smblib_dbg(chg, PR_MISC, "Charge termination WA alarm triggered %lld\n",
			ktime_to_ms(now));

	/* Atomic context, cannot use voter */
	pm_stay_awake(chg->dev);
	schedule_work(&chg->chg_termination_work);

	return ALARMTIMER_NORESTART;
}

static void apsd_timer_cb(struct timer_list *tm)
{
	struct smb_charger *chg = container_of(tm, struct smb_charger,
							apsd_timer);

	smblib_dbg(chg, PR_MISC, "APSD Extented timer timeout at %u\n",
			jiffies_to_msecs(jiffies));

	chg->apsd_ext_timeout = true;
}

#define SOFT_JEITA_HYSTERESIS_OFFSET	0x200
static void jeita_update_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						jeita_update_work);
	struct device_node *node = chg->dev->of_node;
	struct device_node *batt_node, *pnode;
	union power_supply_propval val;
	int rc, tmp[2], max_fcc_ma, max_fv_uv;
	u32 jeita_hard_thresholds[2];
	u16 addr;
	u8 buff[2];

	batt_node = of_find_node_by_name(node, "qcom,battery-data");
	if (!batt_node) {
		smblib_err(chg, "Batterydata not available\n");
		goto out;
	}

	/* if BMS is not ready, defer the work */
	if (!chg->bms_psy)
		return;

	rc = smblib_get_prop_from_bms(chg,
			POWER_SUPPLY_PROP_RESISTANCE_ID, &val);
	if (rc < 0) {
		smblib_err(chg, "Failed to get batt-id rc=%d\n", rc);
		goto out;
	}

	/* if BMS hasn't read out the batt_id yet, defer the work */
	if (val.intval <= 0)
		return;

	pnode = of_batterydata_get_best_profile(batt_node,
					val.intval / 1000, NULL);
	if (IS_ERR(pnode)) {
		rc = PTR_ERR(pnode);
		smblib_err(chg, "Failed to detect valid battery profile %d\n",
				rc);
		goto out;
	}

	rc = of_property_read_u32_array(pnode, "qcom,jeita-hard-thresholds",
				jeita_hard_thresholds, 2);
	if (!rc) {
		rc = smblib_update_jeita(chg, jeita_hard_thresholds,
					JEITA_HARD);
		if (rc < 0) {
			smblib_err(chg, "Couldn't configure Hard Jeita rc=%d\n",
					rc);
			goto out;
		}
	}

	rc = of_property_read_u32_array(pnode, "qcom,jeita-soft-thresholds",
				chg->jeita_soft_thlds, 2);
	if (!rc) {
		rc = smblib_update_jeita(chg, chg->jeita_soft_thlds,
					JEITA_SOFT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't configure Soft Jeita rc=%d\n",
					rc);
			goto out;
		}

		rc = of_property_read_u32_array(pnode,
					"qcom,jeita-soft-hys-thresholds",
					chg->jeita_soft_hys_thlds, 2);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get Soft Jeita hysteresis thresholds rc=%d\n",
					rc);
			goto out;
		}
	} else {
		/* Populate the jeita-soft-thresholds */
		addr = CHGR_JEITA_THRESHOLD_BASE_REG(JEITA_SOFT);
		rc = smblib_batch_read(chg, addr, buff, 2);
		if (rc < 0) {
			pr_err("failed to read 0x%4X, rc=%d\n", addr, rc);
			goto out;
		}

		chg->jeita_soft_thlds[1] = buff[1] | buff[0] << 8;

		rc = smblib_batch_read(chg, addr + 2, buff, 2);
		if (rc < 0) {
			pr_err("failed to read 0x%4X, rc=%d\n", addr + 2, rc);
			goto out;
		}

		chg->jeita_soft_thlds[0] = buff[1] | buff[0] << 8;

		/*
		 * Update the soft jeita hysteresis 2 DegC less for warm and
		 * 2 DegC more for cool than the soft jeita thresholds to avoid
		 * overwriting the registers with invalid values.
		 */
		chg->jeita_soft_hys_thlds[0] =
			chg->jeita_soft_thlds[0] - SOFT_JEITA_HYSTERESIS_OFFSET;
		chg->jeita_soft_hys_thlds[1] =
			chg->jeita_soft_thlds[1] + SOFT_JEITA_HYSTERESIS_OFFSET;
	}

	chg->jeita_soft_fcc[0] = chg->jeita_soft_fcc[1] = -EINVAL;
	chg->jeita_soft_fv[0] = chg->jeita_soft_fv[1] = -EINVAL;
	max_fcc_ma = max_fv_uv = -EINVAL;

	of_property_read_u32(pnode, "qcom,fastchg-current-ma", &max_fcc_ma);
	of_property_read_u32(pnode, "qcom,max-voltage-uv", &max_fv_uv);

	if (max_fcc_ma <= 0 || max_fv_uv <= 0) {
		smblib_err(chg, "Incorrect fastchg-current-ma or max-voltage-uv\n");
		goto out;
	}

	rc = of_property_read_u32_array(pnode, "qcom,jeita-soft-fcc-ua",
					tmp, 2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get fcc values for soft JEITA rc=%d\n",
				rc);
		goto out;
	}

	max_fcc_ma *= 1000;
	if (tmp[0] > max_fcc_ma || tmp[1] > max_fcc_ma) {
		smblib_err(chg, "Incorrect FCC value [%d %d] max: %d\n", tmp[0],
			tmp[1], max_fcc_ma);
		goto out;
	}
	chg->jeita_soft_fcc[0] = tmp[0];
	chg->jeita_soft_fcc[1] = tmp[1];

	rc = of_property_read_u32_array(pnode, "qcom,jeita-soft-fv-uv", tmp,
					2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get fv values for soft JEITA rc=%d\n",
				rc);
		goto out;
	}

	if (tmp[0] > max_fv_uv || tmp[1] > max_fv_uv) {
		smblib_err(chg, "Incorrect FV value [%d %d] max: %d\n", tmp[0],
			tmp[1], max_fv_uv);
		goto out;
	}
	chg->jeita_soft_fv[0] = tmp[0];
	chg->jeita_soft_fv[1] = tmp[1];

	rc = smblib_soft_jeita_arb_wa(chg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't fix soft jeita arb rc=%d\n",
				rc);
		goto out;
	}

	chg->jeita_configured = JEITA_CFG_COMPLETE;
	return;

out:
	chg->jeita_configured = JEITA_CFG_FAILURE;
}

static void smblib_lpd_ra_open_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							lpd_ra_open_work.work);
	union power_supply_propval pval;
	u8 stat;
	int rc;

	if (chg->pr_swap_in_progress || chg->pd_hard_reset) {
		chg->lpd_stage = LPD_STAGE_NONE;
		goto out;
	}

	if (chg->lpd_stage != LPD_STAGE_FLOAT)
		goto out;

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n",
			rc);
		goto out;
	}

	/* quit if moisture status is gone or in attached state */
	if (!(stat & TYPEC_WATER_DETECTION_STATUS_BIT)
			|| (stat & TYPEC_TCCDEBOUNCE_DONE_STATUS_BIT)) {
		chg->lpd_stage = LPD_STAGE_NONE;
		goto out;
	}

	chg->lpd_stage = LPD_STAGE_COMMIT;

	/* Enable source only mode */
	pval.intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
	rc = smblib_set_prop_typec_power_role(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set typec source only mode rc=%d\n",
					rc);
		goto out;
	}

	/* Wait 1.5ms to get SBUx ready */
	usleep_range(1500, 1510);

	if (smblib_rsbux_low(chg, RSBU_K_300K_UV)) {
		/* Moisture detected, enable sink only mode */
		pval.intval = POWER_SUPPLY_TYPEC_PR_SINK;
		rc = smblib_set_prop_typec_power_role(chg, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set typec sink only rc=%d\n",
				rc);
			goto out;
		}

		chg->lpd_reason = LPD_MOISTURE_DETECTED;
		chg->moisture_present =  true;
		vote(chg->usb_icl_votable, LPD_VOTER, true, 0);

	} else {
		/* Floating cable, disable water detection irq temporarily */
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					TYPEC_WATER_DETECTION_INT_EN_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set TYPE_C_INTERRUPT_EN_CFG_2_REG rc=%d\n",
					rc);
			goto out;
		}

		/* restore DRP mode */
		pval.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		rc = smblib_set_prop_typec_power_role(chg, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
				pval.intval, rc);
			goto out;
		}

		chg->lpd_reason = LPD_FLOATING_CABLE;
	}

	/* recheck in 60 seconds */
	alarm_start_relative(&chg->lpd_recheck_timer, ms_to_ktime(60000));
out:
	vote(chg->awake_votable, LPD_VOTER, false, 0);
}

static void smblib_lpd_detach_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							lpd_detach_work.work);

	if (chg->lpd_stage == LPD_STAGE_FLOAT_CANCEL)
		chg->lpd_stage = LPD_STAGE_NONE;
}

static void smblib_cp_status_change_work(struct work_struct *work)
{
	int rc;
	union power_supply_propval pval;
	struct smb_charger *chg = container_of(work, struct smb_charger,
			cp_status_change_work);

	if (!chg->cp_psy)
		chg->cp_psy = power_supply_get_by_name("charge_pump_master");

	if (!chg->cp_psy)
		goto relax;

	if (chg->cp_topo == -EINVAL) {
		rc = power_supply_get_property(chg->cp_psy,
				POWER_SUPPLY_PROP_PARALLEL_OUTPUT_MODE, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read cp topo rc=%d\n", rc);
			goto relax;
		}

		chg->cp_topo = pval.intval;

		if (chg->cp_topo == POWER_SUPPLY_PL_OUTPUT_VBAT &&
				chg->cp_reason == POWER_SUPPLY_CP_WIRELESS)
			vote(chg->fcc_main_votable, WLS_PL_CHARGING_VOTER, true,
					800000);
	}
relax:
	pm_relax(chg->dev);
}

static int smblib_create_votables(struct smb_charger *chg)
{
	int rc = 0;

	chg->fcc_votable = find_votable("FCC");
	if (chg->fcc_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FCC votable rc=%d\n", rc);
		return rc;
	}

	chg->fcc_main_votable = find_votable("FCC_MAIN");
	if (chg->fcc_main_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FCC Main votable rc=%d\n", rc);
		return rc;
	}

	chg->fv_votable = find_votable("FV");
	if (chg->fv_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FV votable rc=%d\n", rc);
		return rc;
	}

	chg->usb_icl_votable = find_votable("USB_ICL");
	if (chg->usb_icl_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find USB_ICL votable rc=%d\n", rc);
		return rc;
	}

	chg->pl_disable_votable = find_votable("PL_DISABLE");
	if (chg->pl_disable_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find votable PL_DISABLE rc=%d\n", rc);
		return rc;
	}

	chg->pl_enable_votable_indirect = find_votable("PL_ENABLE_INDIRECT");
	if (chg->pl_enable_votable_indirect == NULL) {
		rc = -EINVAL;
		smblib_err(chg,
			"Couldn't find votable PL_ENABLE_INDIRECT rc=%d\n",
			rc);
		return rc;
	}

	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);

	chg->smb_override_votable = create_votable("SMB_EN_OVERRIDE",
				VOTE_SET_ANY,
				smblib_smb_disable_override_vote_callback, chg);
	if (IS_ERR(chg->smb_override_votable)) {
		rc = PTR_ERR(chg->smb_override_votable);
		chg->smb_override_votable = NULL;
		return rc;
	}

	chg->dc_suspend_votable = create_votable("DC_SUSPEND", VOTE_SET_ANY,
					smblib_dc_suspend_vote_callback,
					chg);
	if (IS_ERR(chg->dc_suspend_votable)) {
		rc = PTR_ERR(chg->dc_suspend_votable);
		chg->dc_suspend_votable = NULL;
		return rc;
	}

	chg->otg_toggle_votable = create_votable("OTG_TOGGLE", VOTE_SET_ANY,
					smblib_otg_toggle_vote_callback,
					chg);
	if (IS_ERR(chg->otg_toggle_votable)) {
		rc = PTR_ERR(chg->otg_toggle_votable);
		return rc;
	}

	chg->awake_votable = create_votable("AWAKE", VOTE_SET_ANY,
					smblib_awake_vote_callback,
					chg);
	if (IS_ERR(chg->awake_votable)) {
		rc = PTR_ERR(chg->awake_votable);
		chg->awake_votable = NULL;
		return rc;
	}

	chg->chg_disable_votable = create_votable("CHG_DISABLE", VOTE_SET_ANY,
					smblib_chg_disable_vote_callback,
					chg);
	if (IS_ERR(chg->chg_disable_votable)) {
		rc = PTR_ERR(chg->chg_disable_votable);
		chg->chg_disable_votable = NULL;
		return rc;
	}

	chg->limited_irq_disable_votable = create_votable(
				"USB_LIMITED_IRQ_DISABLE",
				VOTE_SET_ANY,
				smblib_limited_irq_disable_vote_callback,
				chg);
	if (IS_ERR(chg->limited_irq_disable_votable)) {
		rc = PTR_ERR(chg->limited_irq_disable_votable);
		chg->limited_irq_disable_votable = NULL;
		return rc;
	}

	chg->hdc_irq_disable_votable = create_votable("USB_HDC_IRQ_DISABLE",
					VOTE_SET_ANY,
					smblib_hdc_irq_disable_vote_callback,
					chg);
	if (IS_ERR(chg->hdc_irq_disable_votable)) {
		rc = PTR_ERR(chg->hdc_irq_disable_votable);
		chg->hdc_irq_disable_votable = NULL;
		return rc;
	}

	chg->icl_irq_disable_votable = create_votable("USB_ICL_IRQ_DISABLE",
					VOTE_SET_ANY,
					smblib_icl_irq_disable_vote_callback,
					chg);
	if (IS_ERR(chg->icl_irq_disable_votable)) {
		rc = PTR_ERR(chg->icl_irq_disable_votable);
		chg->icl_irq_disable_votable = NULL;
		return rc;
	}

	chg->temp_change_irq_disable_votable = create_votable(
			"TEMP_CHANGE_IRQ_DISABLE", VOTE_SET_ANY,
			smblib_temp_change_irq_disable_vote_callback, chg);
	if (IS_ERR(chg->temp_change_irq_disable_votable)) {
		rc = PTR_ERR(chg->temp_change_irq_disable_votable);
		chg->temp_change_irq_disable_votable = NULL;
		return rc;
	}

	return rc;
}

static void smblib_destroy_votables(struct smb_charger *chg)
{
	if (chg->dc_suspend_votable)
		destroy_votable(chg->dc_suspend_votable);
	if (chg->otg_toggle_votable)
		destroy_votable(chg->otg_toggle_votable);
	if (chg->usb_icl_votable)
		destroy_votable(chg->usb_icl_votable);
	if (chg->awake_votable)
		destroy_votable(chg->awake_votable);
	if (chg->chg_disable_votable)
		destroy_votable(chg->chg_disable_votable);
}

static void smblib_iio_deinit(struct smb_charger *chg)
{
	if (!IS_ERR_OR_NULL(chg->iio.usbin_v_chan))
		iio_channel_release(chg->iio.usbin_v_chan);
	if (!IS_ERR_OR_NULL(chg->iio.usbin_i_chan))
		iio_channel_release(chg->iio.usbin_i_chan);
	if (!IS_ERR_OR_NULL(chg->iio.temp_chan))
		iio_channel_release(chg->iio.temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.sbux_chan))
		iio_channel_release(chg->iio.sbux_chan);
	if (!IS_ERR_OR_NULL(chg->iio.vph_v_chan))
		iio_channel_release(chg->iio.vph_v_chan);
	if (!IS_ERR_OR_NULL(chg->iio.die_temp_chan))
		iio_channel_release(chg->iio.die_temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.connector_temp_chan))
		iio_channel_release(chg->iio.connector_temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.skin_temp_chan))
		iio_channel_release(chg->iio.skin_temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.smb_temp_chan))
		iio_channel_release(chg->iio.smb_temp_chan);
}

int smblib_init(struct smb_charger *chg)
{
	union power_supply_propval prop_val;
	int rc = 0;

	mutex_init(&chg->smb_lock);
	mutex_init(&chg->irq_status_lock);
	mutex_init(&chg->dcin_aicl_lock);

	mutex_init(&chg->write_lock);
	mutex_init(&chg->sw_dash_lock);
	mutex_init(&chg->dpdm_lock);
	spin_lock_init(&chg->typec_pr_lock);
	INIT_WORK(&chg->bms_update_work, bms_update_work);
	INIT_WORK(&chg->pl_update_work, pl_update_work);
	INIT_WORK(&chg->jeita_update_work, jeita_update_work);
	INIT_WORK(&chg->dcin_aicl_work, dcin_aicl_work);
	INIT_WORK(&chg->dump_reg_work, dump_register_work);

	INIT_DELAYED_WORK(&chg->panel_register_work, smblib_panel_register_work);
	INIT_DELAYED_WORK(&chg->rechk_sw_dsh_work, retrigger_dash_work);
	INIT_DELAYED_WORK(&chg->re_kick_work, op_re_kick_work);
	INIT_DELAYED_WORK(&chg->unplug_check_work, op_usb_remove_check_work);
	INIT_DELAYED_WORK(&chg->op_check_apsd_work, op_chek_apsd_done_work);
	INIT_DELAYED_WORK(&chg->slow_chg_check_work, op_check_slow_charge_work);
	INIT_DELAYED_WORK(&chg->recovery_suspend_work,
		op_recovery_usb_suspend_work);
	INIT_DELAYED_WORK(&chg->check_switch_dash_work,
			op_check_allow_switch_dash_work);
	INIT_DELAYED_WORK(&chg->heartbeat_work,
			op_heartbeat_work);
	INIT_DELAYED_WORK(&chg->non_standard_charger_check_work,
		check_non_standard_charger_work);
	INIT_DELAYED_WORK(&chg->re_det_work, smbchg_re_det_work);
	INIT_DELAYED_WORK(&chg->op_re_set_work, op_recovery_set_work);
	INIT_WORK(&chg->get_aicl_work, op_get_aicl_work);
	/*usb connector hw auto detection*/
	INIT_WORK(&chg->otg_switch_work, op_otg_switch);
	INIT_DELAYED_WORK(&chg->connecter_check_work,
			op_connecter_temp_check_work);
	INIT_DELAYED_WORK(&chg->connecter_recovery_work,
			op_connecter_recovery_charge_work);
	INIT_DELAYED_WORK(&chg->pd_status_check_work,
			pd_charge_status_check_work);
	INIT_WORK(&chg->op_pd_active_work, op_set_pd_active_work);
	INIT_DELAYED_WORK(&chg->reset_rd_work,
			op_reset_rd_work);
	INIT_DELAYED_WORK(&chg->run_sw_aicl_work,
			op_run_sw_aicl_work);
	INIT_DELAYED_WORK(&chg->disable_chg_flag_work,
				op_reset_chg_disable_flag_work);
	INIT_DELAYED_WORK(&chg->icon_debounce_unplug_check,
				op_icon_debounce_unplug_check_work);
	schedule_delayed_work(&chg->heartbeat_work,
			msecs_to_jiffies(HEARTBEAT_INTERVAL_MS));
	schedule_delayed_work(&chg->panel_register_work, msecs_to_jiffies(2000));
	if (gpio_is_valid(chg->vbus_ctrl))
		schedule_delayed_work(&chg->connecter_check_work,
				msecs_to_jiffies(200));
	notify_dash_unplug_register(&notify_unplug_event);
	chg->chg_wake_lock = wakeup_source_register(chg->dev, "chg_wake_lock");
	chg->chg_aging_wake_lock = wakeup_source_register(chg->dev, "chg_aging_wake_lock");
	g_chg = chg;
	regsister_notify_usb_enumeration_status(&usb_enumeration);

	INIT_WORK(&chg->cp_status_change_work, smblib_cp_status_change_work);
	INIT_DELAYED_WORK(&chg->clear_hdc_work, clear_hdc_work);
	INIT_DELAYED_WORK(&chg->icl_change_work, smblib_icl_change_work);
	INIT_DELAYED_WORK(&chg->pl_enable_work, smblib_pl_enable_work);
	op_set_collapse_fet(chg, false);

	INIT_DELAYED_WORK(&chg->uusb_otg_work, smblib_uusb_otg_work);
	INIT_DELAYED_WORK(&chg->bb_removal_work, smblib_bb_removal_work);
	INIT_DELAYED_WORK(&chg->lpd_ra_open_work, smblib_lpd_ra_open_work);
	INIT_DELAYED_WORK(&chg->lpd_detach_work, smblib_lpd_detach_work);
	INIT_DELAYED_WORK(&chg->thermal_regulation_work,
					smblib_thermal_regulation_work);
	INIT_DELAYED_WORK(&chg->usbov_dbc_work, smblib_usbov_dbc_work);
	INIT_DELAYED_WORK(&chg->pr_swap_detach_work,
					smblib_pr_swap_detach_work);
	INIT_DELAYED_WORK(&chg->pr_lock_clear_work,
					smblib_pr_lock_clear_work);
	timer_setup(&chg->apsd_timer, apsd_timer_cb, 0);

	INIT_DELAYED_WORK(&chg->role_reversal_check,
					smblib_typec_role_check_work);

	INIT_DELAYED_WORK(&chg->register_pps_work, op_register_pps_work);
	schedule_delayed_work(&chg->register_pps_work, msecs_to_jiffies(2000));
	if (chg->wa_flags & CHG_TERMINATION_WA) {
		INIT_WORK(&chg->chg_termination_work,
					smblib_chg_termination_work);

		if (alarmtimer_get_rtcdev()) {
			pr_info("chg_termination_alarm\n");
			alarm_init(&chg->chg_termination_alarm, ALARM_BOOTTIME,
						chg_termination_alarm_cb);
		} else {
			smblib_err(chg, "Couldn't get rtc device\n");
			return -ENODEV;
		}
	}

	if (chg->uusb_moisture_protection_enabled) {
		INIT_WORK(&chg->moisture_protection_work,
					smblib_moisture_protection_work);

		if (alarmtimer_get_rtcdev()) {
			pr_info("moisture_protection_alarm\n");
			alarm_init(&chg->moisture_protection_alarm,
				ALARM_BOOTTIME, moisture_protection_alarm_cb);
		} else {
			smblib_err(chg, "Failed to initialize moisture protection alarm\n");
			return -ENODEV;
		}
	}

	if (alarmtimer_get_rtcdev()) {
		alarm_init(&chg->dcin_aicl_alarm, ALARM_REALTIME,
				dcin_aicl_alarm_cb);
	} else {
		smblib_err(chg, "Failed to initialize dcin aicl alarm\n");
		return -ENODEV;
	}

	chg->fake_capacity = -EINVAL;
	chg->fake_capacity_level = -EINVAL;
	chg->fake_input_current_limited = -EINVAL;
	chg->fake_batt_status = -EINVAL;
	chg->sink_src_mode = UNATTACHED_MODE;
	chg->jeita_configured = false;
	chg->sec_chg_selected = POWER_SUPPLY_CHARGER_SEC_NONE;
	chg->cp_reason = POWER_SUPPLY_CP_NONE;
	chg->thermal_status = TEMP_BELOW_RANGE;
	chg->typec_irq_en = true;
	chg->chg_wake_lock_on = false;
	chg->cp_topo = -EINVAL;
	chg->dr_mode = TYPEC_PORT_DRP;

	switch (chg->mode) {
	case PARALLEL_MASTER:
		rc = qcom_batt_init(&chg->chg_param);
		if (rc < 0) {
			smblib_err(chg, "Couldn't init qcom_batt_init rc=%d\n",
				rc);
			return rc;
		}

		rc = qcom_step_chg_init(chg->dev, chg->step_chg_enabled,
						chg->sw_jeita_enabled, false);
		if (rc < 0) {
			smblib_err(chg, "Couldn't init qcom_step_chg_init rc=%d\n",
				rc);
			return rc;
		}

		rc = smblib_create_votables(chg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't create votables rc=%d\n",
				rc);
			return rc;
		}

		chg->bms_psy = power_supply_get_by_name("bms");

		if (chg->sec_pl_present) {
			chg->pl.psy = power_supply_get_by_name("parallel");
			if (chg->pl.psy) {
				if (chg->sec_chg_selected
					!= POWER_SUPPLY_CHARGER_SEC_CP) {
					rc = smblib_select_sec_charger(chg,
						POWER_SUPPLY_CHARGER_SEC_PL,
						POWER_SUPPLY_CP_NONE, false);
					if (rc < 0)
						smblib_err(chg, "Couldn't config pl charger rc=%d\n",
							rc);
				}

				if (chg->smb_temp_max == -EINVAL) {
					rc = smblib_get_thermal_threshold(chg,
						SMB_REG_H_THRESHOLD_MSB_REG,
						&chg->smb_temp_max);
					if (rc < 0) {
						dev_err(chg->dev, "Couldn't get charger_temp_max rc=%d\n",
								rc);
						return rc;
					}
				}

				prop_val.intval = chg->smb_temp_max;
				rc = power_supply_set_property(chg->pl.psy,
					POWER_SUPPLY_PROP_CHARGER_TEMP_MAX,
					&prop_val);
				if (rc < 0) {
					dev_err(chg->dev, "Couldn't set POWER_SUPPLY_PROP_CHARGER_TEMP_MAX rc=%d\n",
							rc);
					return rc;
				}
			}
		}

		rc = smblib_register_notifier(chg);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't register notifier rc=%d\n", rc);
			return rc;
		}
		break;
	case PARALLEL_SLAVE:
		break;
	default:
		smblib_err(chg, "Unsupported mode %d\n", chg->mode);
		return -EINVAL;
	}

	return rc;
}

int smblib_deinit(struct smb_charger *chg)
{
	switch (chg->mode) {
	case PARALLEL_MASTER:
		if (chg->uusb_moisture_protection_enabled) {
			alarm_cancel(&chg->moisture_protection_alarm);
			cancel_work_sync(&chg->moisture_protection_work);
		}
		if (chg->wa_flags & CHG_TERMINATION_WA) {
			alarm_cancel(&chg->chg_termination_alarm);
			cancel_work_sync(&chg->chg_termination_work);
		}
		del_timer_sync(&chg->apsd_timer);
		cancel_work_sync(&chg->bms_update_work);
		cancel_work_sync(&chg->jeita_update_work);
		cancel_work_sync(&chg->pl_update_work);
		cancel_work_sync(&chg->dcin_aicl_work);
		cancel_work_sync(&chg->cp_status_change_work);
		cancel_delayed_work_sync(&chg->clear_hdc_work);
		cancel_delayed_work_sync(&chg->icl_change_work);
		cancel_delayed_work_sync(&chg->pl_enable_work);
		cancel_delayed_work_sync(&chg->uusb_otg_work);
		cancel_delayed_work_sync(&chg->bb_removal_work);
		cancel_delayed_work_sync(&chg->lpd_ra_open_work);
		cancel_delayed_work_sync(&chg->lpd_detach_work);

		if (chg->nb.notifier_call)
			power_supply_unreg_notifier(&chg->nb);
		cancel_delayed_work_sync(&chg->thermal_regulation_work);
		cancel_delayed_work_sync(&chg->usbov_dbc_work);
		cancel_delayed_work_sync(&chg->role_reversal_check);
		cancel_delayed_work_sync(&chg->pr_swap_detach_work);
		power_supply_unreg_notifier(&chg->nb);
		smblib_destroy_votables(chg);
		qcom_step_chg_deinit();
		qcom_batt_deinit();
		break;
	case PARALLEL_SLAVE:
		break;
	default:
		smblib_err(chg, "Unsupported mode %d\n", chg->mode);
		return -EINVAL;
	}
	notify_dash_unplug_unregister(&notify_unplug_event);
	smblib_iio_deinit(chg);

	return 0;
}
