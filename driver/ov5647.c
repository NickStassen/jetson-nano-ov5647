/*
 * ov5647.c - OmniVision OV5647 sensor driver for the NVIDIA tegracam
 * framework (Jetson Nano, L4T 32.5.1 / kernel 4.9.201-tegra).
 *
 * Structure follows NVIDIA's imx219.c tegracam reference driver; the
 * register level behaviour (init sequence, MIPI bring-up, exposure/gain
 * encoding) follows the Raspberry Pi kernel ov5647 driver.
 *
 * Copyright (c) 2026 Nick Stassen
 * Copyright (c) 2015-2020, NVIDIA CORPORATION (imx219.c template)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <media/tegra_v4l2_camera.h>
#include <media/tegracam_core.h>

#include "ov5647_mode_tbls.h"

/* Registers */
#define OV5647_REG_SW_STANDBY		0x0100
#define OV5647_REG_SW_RESET		0x0103
#define OV5647_REG_PAD_OE0		0x3000
#define OV5647_REG_PAD_OE1		0x3001
#define OV5647_REG_PAD_OE2		0x3002
#define OV5647_REG_CHIP_ID_H		0x300a
#define OV5647_REG_CHIP_ID_L		0x300b
#define OV5647_REG_PAD_OUT		0x300d
#define OV5647_REG_EXP_H		0x3500	/* [3:0] = exposure[19:16] */
#define OV5647_REG_EXP_M		0x3501	/* exposure[15:8] */
#define OV5647_REG_EXP_L		0x3502	/* exposure[7:0]; [3:0] are 1/16 line */
#define OV5647_REG_AEC_AGC		0x3503
#define OV5647_REG_GAIN_H		0x350a	/* [1:0] = gain[9:8] */
#define OV5647_REG_GAIN_L		0x350b	/* gain[7:0], 16 = 1.0x */
#define OV5647_REG_VTS_H		0x380e
#define OV5647_REG_VTS_L		0x380f
#define OV5647_REG_FRAME_OFF_NUM	0x4202
#define OV5647_REG_MIPI_CTRL00		0x4800
#define OV5647_REG_MIPI_CTRL14		0x4814

#define MIPI_CTRL00_CLOCK_LANE_GATE	BIT(5)
#define MIPI_CTRL00_LINE_SYNC_ENABLE	BIT(4)
#define MIPI_CTRL00_BUS_IDLE		BIT(2)
#define MIPI_CTRL00_CLOCK_LANE_DISABLE	BIT(0)

#define OV5647_CHIP_ID			0x5647

/* Sensor parameter limits */
#define OV5647_MIN_GAIN			16	/* 1.0x  (gain_factor = 16) */
#define OV5647_MAX_GAIN			1023	/* ~64x, 10-bit register */
#define OV5647_MAX_FRAME_LENGTH		0x7fff
#define OV5647_MIN_COARSE_EXPOSURE	4
#define OV5647_MAX_COARSE_DIFF		4

/*
 * The Raspberry Pi style module gates its on-board LDOs with the PWDN line
 * (Tegra CAMx_PWDN, "reset-gpios" in DT). The sensor needs a few ms after
 * power before it answers on I2C.
 */
#define OV5647_POWER_ON_DELAY_MS	30

static bool continuous_clock;
module_param(continuous_clock, bool, 0444);
MODULE_PARM_DESC(continuous_clock,
	"Drive the MIPI clock lane continuously (default 0 = non-continuous, "
	"matches discontinuous_clk = \"yes\" in the device tree)");

static const struct of_device_id ov5647_of_match[] = {
	{ .compatible = "nvidia,ov5647", },
	{ },
};
MODULE_DEVICE_TABLE(of, ov5647_of_match);

static const u32 ctrl_cid_list[] = {
	TEGRA_CAMERA_CID_GAIN,
	TEGRA_CAMERA_CID_EXPOSURE,
	TEGRA_CAMERA_CID_FRAME_RATE,
	TEGRA_CAMERA_CID_SENSOR_MODE_ID,
};

struct ov5647 {
	struct i2c_client		*i2c_client;
	struct v4l2_subdev		*subdev;
	u32				frame_length;
	struct camera_common_data	*s_data;
	struct tegracam_device		*tc_dev;
};

static const struct regmap_config sensor_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
	.use_single_rw = true,
};

static inline int ov5647_read_reg(struct camera_common_data *s_data,
	u16 addr, u8 *val)
{
	int err = 0;
	u32 reg_val = 0;

	err = regmap_read(s_data->regmap, addr, &reg_val);
	*val = reg_val & 0xff;

	return err;
}

static inline int ov5647_write_reg(struct camera_common_data *s_data,
	u16 addr, u8 val)
{
	int err = 0;

	err = regmap_write(s_data->regmap, addr, val);
	if (err)
		dev_err(s_data->dev, "%s: i2c write failed, 0x%x = %x\n",
			__func__, addr, val);

	return err;
}

static int ov5647_write_table(struct ov5647 *priv, const ov5647_reg table[])
{
	return regmap_util_write_table_8(priv->s_data->regmap, table, NULL, 0,
		OV5647_TABLE_WAIT_MS, OV5647_TABLE_END);
}

static int ov5647_set_group_hold(struct tegracam_device *tc_dev, bool val)
{
	/* Controls are applied directly; group hold intentionally unused. */
	return 0;
}

static int ov5647_set_gain(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = s_data->dev;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	u32 gain;
	int err;

	if (val < mode->control_properties.min_gain_val)
		val = mode->control_properties.min_gain_val;
	else if (val > mode->control_properties.max_gain_val)
		val = mode->control_properties.max_gain_val;

	/*
	 * DT gain_factor is 16 and the sensor register is in 1/16 steps,
	 * so the normalized control value maps 1:1 onto the register.
	 */
	gain = (u32)(val * 16 / mode->control_properties.gain_factor);

	if (gain < OV5647_MIN_GAIN)
		gain = OV5647_MIN_GAIN;
	else if (gain > OV5647_MAX_GAIN)
		gain = OV5647_MAX_GAIN;

	dev_dbg(dev, "%s: val: %lld (/%d) [times], gain reg: %u\n",
		__func__, val, mode->control_properties.gain_factor, gain);

	err = ov5647_write_reg(s_data, OV5647_REG_GAIN_H, (gain >> 8) & 0x03);
	err |= ov5647_write_reg(s_data, OV5647_REG_GAIN_L, gain & 0xff);
	if (err)
		dev_dbg(dev, "%s: gain control error\n", __func__);

	return 0;
}

static int ov5647_set_frame_rate(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct ov5647 *priv = (struct ov5647 *)tc_dev->priv;
	struct device *dev = tc_dev->dev;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	u32 frame_length;
	u32 min_frame_length = ov5647_mode_min_vts[s_data->mode];
	int err;

	if (val <= 0)
		return -EINVAL;

	frame_length = (u32)(mode->signal_properties.pixel_clock.val *
		(u64)mode->control_properties.framerate_factor /
		mode->image_properties.line_length / val);

	if (frame_length < min_frame_length)
		frame_length = min_frame_length;
	else if (frame_length > OV5647_MAX_FRAME_LENGTH)
		frame_length = OV5647_MAX_FRAME_LENGTH;

	dev_dbg(dev, "%s: val: %llde-6 [fps], frame_length (VTS): %u [lines]\n",
		__func__, val, frame_length);

	err = ov5647_write_reg(s_data, OV5647_REG_VTS_H,
		(frame_length >> 8) & 0xff);
	err |= ov5647_write_reg(s_data, OV5647_REG_VTS_L, frame_length & 0xff);
	if (err) {
		dev_dbg(dev, "%s: frame_length control error\n", __func__);
		return err;
	}

	priv->frame_length = frame_length;

	return 0;
}

static int ov5647_set_exposure(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct ov5647 *priv = (struct ov5647 *)tc_dev->priv;
	struct device *dev = tc_dev->dev;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	const s32 max_coarse_time = priv->frame_length - OV5647_MAX_COARSE_DIFF;
	u32 coarse_time;
	u32 exp;
	int err;

	/* val is in microseconds (exposure_factor = 1000000) */
	coarse_time = (u32)((u64)val * mode->signal_properties.pixel_clock.val
		/ mode->control_properties.exposure_factor
		/ mode->image_properties.line_length);

	if (coarse_time < OV5647_MIN_COARSE_EXPOSURE)
		coarse_time = OV5647_MIN_COARSE_EXPOSURE;
	else if ((s32)coarse_time > max_coarse_time) {
		coarse_time = max_coarse_time;
		dev_dbg(dev,
			"%s: exposure limited by frame_length: %d [lines]\n",
			__func__, max_coarse_time);
	}

	dev_dbg(dev, "%s: val: %lld [us], coarse_time: %u [lines]\n",
		__func__, val, coarse_time);

	/* 20-bit register, low 4 bits are 1/16 line fractions (left 0). */
	exp = coarse_time << 4;
	err = ov5647_write_reg(s_data, OV5647_REG_EXP_H, (exp >> 16) & 0x0f);
	err |= ov5647_write_reg(s_data, OV5647_REG_EXP_M, (exp >> 8) & 0xff);
	err |= ov5647_write_reg(s_data, OV5647_REG_EXP_L, exp & 0xff);
	if (err) {
		dev_dbg(dev, "%s: coarse_time control error\n", __func__);
		return err;
	}

	return 0;
}

static struct tegracam_ctrl_ops ov5647_ctrl_ops = {
	.numctrls = ARRAY_SIZE(ctrl_cid_list),
	.ctrl_cid_list = ctrl_cid_list,
	.set_gain = ov5647_set_gain,
	.set_exposure = ov5647_set_exposure,
	.set_frame_rate = ov5647_set_frame_rate,
	.set_group_hold = ov5647_set_group_hold,
};

/* Park the MIPI lanes (LP-11) and stop frame output. */
static int ov5647_mipi_stream_off(struct camera_common_data *s_data)
{
	int err;

	err = ov5647_write_reg(s_data, OV5647_REG_MIPI_CTRL00,
		MIPI_CTRL00_CLOCK_LANE_GATE | MIPI_CTRL00_BUS_IDLE |
		MIPI_CTRL00_CLOCK_LANE_DISABLE);
	err |= ov5647_write_reg(s_data, OV5647_REG_FRAME_OFF_NUM, 0x0f);
	err |= ov5647_write_reg(s_data, OV5647_REG_PAD_OUT, 0x01);

	return err;
}

static int ov5647_mipi_stream_on(struct camera_common_data *s_data)
{
	u8 val = MIPI_CTRL00_BUS_IDLE;
	int err;

	if (!continuous_clock)
		val |= MIPI_CTRL00_CLOCK_LANE_GATE |
		       MIPI_CTRL00_LINE_SYNC_ENABLE;

	err = ov5647_write_reg(s_data, OV5647_REG_MIPI_CTRL00, val);
	err |= ov5647_write_reg(s_data, OV5647_REG_FRAME_OFF_NUM, 0x00);
	err |= ov5647_write_reg(s_data, OV5647_REG_PAD_OUT, 0x00);

	return err;
}

static int ov5647_power_on(struct camera_common_data *s_data)
{
	int err = 0;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;

	dev_dbg(dev, "%s: power on\n", __func__);
	if (pdata && pdata->power_on) {
		err = pdata->power_on(pw);
		if (err)
			dev_err(dev, "%s failed.\n", __func__);
		else
			pw->state = SWITCH_ON;
		return err;
	}

	if (pw->reset_gpio) {
		if (gpio_cansleep(pw->reset_gpio))
			gpio_set_value_cansleep(pw->reset_gpio, 0);
		else
			gpio_set_value(pw->reset_gpio, 0);
	}

	if (unlikely(!(pw->avdd || pw->iovdd || pw->dvdd)))
		goto skip_power_seqn;

	usleep_range(10, 20);

	if (pw->avdd) {
		err = regulator_enable(pw->avdd);
		if (err)
			goto ov5647_avdd_fail;
	}

	if (pw->iovdd) {
		err = regulator_enable(pw->iovdd);
		if (err)
			goto ov5647_iovdd_fail;
	}

	if (pw->dvdd) {
		err = regulator_enable(pw->dvdd);
		if (err)
			goto ov5647_dvdd_fail;
	}

	usleep_range(10, 20);

skip_power_seqn:
	/* PWDN high = module LDOs on (Raspberry Pi style module wiring). */
	if (pw->reset_gpio) {
		if (gpio_cansleep(pw->reset_gpio))
			gpio_set_value_cansleep(pw->reset_gpio, 1);
		else
			gpio_set_value(pw->reset_gpio, 1);
	}

	msleep(OV5647_POWER_ON_DELAY_MS);

	/* Enable pad output drivers, then hold the MIPI lanes in LP-11. */
	err = ov5647_write_reg(s_data, OV5647_REG_PAD_OE0, 0x0f);
	err |= ov5647_write_reg(s_data, OV5647_REG_PAD_OE1, 0xff);
	err |= ov5647_write_reg(s_data, OV5647_REG_PAD_OE2, 0xe4);
	err |= ov5647_mipi_stream_off(s_data);
	if (err) {
		dev_err(dev, "%s: sensor not responding after power on\n",
			__func__);
		goto ov5647_i2c_fail;
	}

	pw->state = SWITCH_ON;

	return 0;

ov5647_i2c_fail:
	if (pw->reset_gpio) {
		if (gpio_cansleep(pw->reset_gpio))
			gpio_set_value_cansleep(pw->reset_gpio, 0);
		else
			gpio_set_value(pw->reset_gpio, 0);
	}
	if (pw->dvdd)
		regulator_disable(pw->dvdd);
ov5647_dvdd_fail:
	if (pw->iovdd)
		regulator_disable(pw->iovdd);
ov5647_iovdd_fail:
	if (pw->avdd)
		regulator_disable(pw->avdd);
ov5647_avdd_fail:
	dev_err(dev, "%s failed.\n", __func__);

	return -ENODEV;
}

static int ov5647_power_off(struct camera_common_data *s_data)
{
	int err = 0;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;

	dev_dbg(dev, "%s: power off\n", __func__);

	if (pdata && pdata->power_off) {
		err = pdata->power_off(pw);
		if (err) {
			dev_err(dev, "%s failed.\n", __func__);
			return err;
		}
	} else {
		/* Disable pad outputs and enter software standby (best effort). */
		ov5647_write_reg(s_data, OV5647_REG_PAD_OE0, 0x00);
		ov5647_write_reg(s_data, OV5647_REG_PAD_OE1, 0x00);
		ov5647_write_reg(s_data, OV5647_REG_PAD_OE2, 0x00);
		ov5647_write_reg(s_data, OV5647_REG_SW_STANDBY, 0x00);

		if (pw->reset_gpio) {
			if (gpio_cansleep(pw->reset_gpio))
				gpio_set_value_cansleep(pw->reset_gpio, 0);
			else
				gpio_set_value(pw->reset_gpio, 0);
		}

		usleep_range(10, 10);

		if (pw->dvdd)
			regulator_disable(pw->dvdd);
		if (pw->iovdd)
			regulator_disable(pw->iovdd);
		if (pw->avdd)
			regulator_disable(pw->avdd);
	}

	pw->state = SWITCH_OFF;

	return 0;
}

static int ov5647_power_put(struct tegracam_device *tc_dev)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;

	if (unlikely(!pw))
		return -EFAULT;

	if (likely(pw->dvdd))
		devm_regulator_put(pw->dvdd);

	if (likely(pw->avdd))
		devm_regulator_put(pw->avdd);

	if (likely(pw->iovdd))
		devm_regulator_put(pw->iovdd);

	pw->dvdd = NULL;
	pw->avdd = NULL;
	pw->iovdd = NULL;

	if (likely(pw->reset_gpio))
		gpio_free(pw->reset_gpio);

	return 0;
}

static int ov5647_power_get(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct clk *parent;
	int err = 0;

	if (!pdata) {
		dev_err(dev, "pdata missing\n");
		return -EFAULT;
	}

	/* Sensor MCLK (the Pi style module has its own 25 MHz crystal, so
	 * this is normally absent from the device tree). */
	if (pdata->mclk_name) {
		pw->mclk = devm_clk_get(dev, pdata->mclk_name);
		if (IS_ERR(pw->mclk)) {
			dev_err(dev, "unable to get clock %s\n",
				pdata->mclk_name);
			return PTR_ERR(pw->mclk);
		}

		if (pdata->parentclk_name) {
			parent = devm_clk_get(dev, pdata->parentclk_name);
			if (IS_ERR(parent)) {
				dev_err(dev, "unable to get parent clock %s",
					pdata->parentclk_name);
			} else
				clk_set_parent(pw->mclk, parent);
		}
	}

	/* analog 2.8v */
	if (pdata->regulators.avdd)
		err |= camera_common_regulator_get(dev,
				&pw->avdd, pdata->regulators.avdd);
	/* IO 1.8v */
	if (pdata->regulators.iovdd)
		err |= camera_common_regulator_get(dev,
				&pw->iovdd, pdata->regulators.iovdd);
	/* dig 1.2v */
	if (pdata->regulators.dvdd)
		err |= camera_common_regulator_get(dev,
				&pw->dvdd, pdata->regulators.dvdd);
	if (err) {
		dev_err(dev, "%s: unable to get regulator(s)\n", __func__);
		goto done;
	}

	/* Reset or ENABLE GPIO */
	pw->reset_gpio = pdata->reset_gpio;
	err = gpio_request(pw->reset_gpio, "cam_reset_gpio");
	if (err < 0) {
		dev_err(dev, "%s: unable to request reset_gpio (%d)\n",
			__func__, err);
		goto done;
	}

done:
	pw->state = SWITCH_OFF;

	return err;
}

static struct camera_common_pdata *ov5647_parse_dt(
	struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct device_node *np = dev->of_node;
	struct camera_common_pdata *board_priv_pdata;
	const struct of_device_id *match;
	struct camera_common_pdata *ret = NULL;
	int err = 0;
	int gpio;

	if (!np)
		return NULL;

	match = of_match_device(ov5647_of_match, dev);
	if (!match) {
		dev_err(dev, "Failed to find matching dt id\n");
		return NULL;
	}

	board_priv_pdata = devm_kzalloc(dev,
		sizeof(*board_priv_pdata), GFP_KERNEL);
	if (!board_priv_pdata)
		return NULL;

	gpio = of_get_named_gpio(np, "reset-gpios", 0);
	if (gpio < 0) {
		if (gpio == -EPROBE_DEFER)
			ret = ERR_PTR(-EPROBE_DEFER);
		dev_err(dev, "reset-gpios not found\n");
		goto error;
	}
	board_priv_pdata->reset_gpio = (unsigned int)gpio;

	err = of_property_read_string(np, "mclk", &board_priv_pdata->mclk_name);
	if (err)
		dev_dbg(dev, "mclk name not present, "
			"assume sensor driven externally\n");

	err = of_property_read_string(np, "avdd-reg",
		&board_priv_pdata->regulators.avdd);
	err |= of_property_read_string(np, "iovdd-reg",
		&board_priv_pdata->regulators.iovdd);
	err |= of_property_read_string(np, "dvdd-reg",
		&board_priv_pdata->regulators.dvdd);
	if (err)
		dev_dbg(dev, "avdd, iovdd and/or dvdd reglrs. not present, "
			"assume sensor powered independently\n");

	board_priv_pdata->has_eeprom =
		of_property_read_bool(np, "has-eeprom");

	return board_priv_pdata;

error:
	devm_kfree(dev, board_priv_pdata);

	return ret;
}

static int ov5647_set_mode(struct tegracam_device *tc_dev)
{
	struct ov5647 *priv = (struct ov5647 *)tegracam_get_privdata(tc_dev);
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = tc_dev->dev;
	u8 val;
	int err = 0;

	if (s_data->mode >= ARRAY_SIZE(ov5647_mode_min_vts)) {
		dev_err(dev, "%s: invalid mode %d\n", __func__, s_data->mode);
		return -EINVAL;
	}

	err = ov5647_write_table(priv, mode_table[OV5647_MODE_COMMON]);
	if (err)
		return err;

	err = ov5647_write_table(priv, mode_table[s_data->mode]);
	if (err)
		return err;

	priv->frame_length = ov5647_mode_min_vts[s_data->mode];

	/* Virtual channel 0 */
	err = ov5647_read_reg(s_data, OV5647_REG_MIPI_CTRL14, &val);
	if (err)
		return err;
	val &= ~(3 << 6);
	err = ov5647_write_reg(s_data, OV5647_REG_MIPI_CTRL14, val);
	if (err)
		return err;

	/* The mode tables leave the sensor running (0x0100 = 1); make sure. */
	err = ov5647_read_reg(s_data, OV5647_REG_SW_STANDBY, &val);
	if (err)
		return err;
	if (!(val & 0x01)) {
		dev_dbg(dev, "%s: sensor still in SW standby, waking\n",
			__func__);
		err = ov5647_write_reg(s_data, OV5647_REG_SW_STANDBY, 0x01);
		if (err)
			return err;
	}

	return 0;
}

static int ov5647_start_streaming(struct tegracam_device *tc_dev)
{
	return ov5647_mipi_stream_on(tc_dev->s_data);
}

static int ov5647_stop_streaming(struct tegracam_device *tc_dev)
{
	int err;

	err = ov5647_mipi_stream_off(tc_dev->s_data);

	usleep_range(50000, 51000);

	return err;
}

static struct camera_common_sensor_ops ov5647_common_ops = {
	.numfrmfmts = ARRAY_SIZE(ov5647_frmfmt),
	.frmfmt_table = ov5647_frmfmt,
	.power_on = ov5647_power_on,
	.power_off = ov5647_power_off,
	.write_reg = ov5647_write_reg,
	.read_reg = ov5647_read_reg,
	.parse_dt = ov5647_parse_dt,
	.power_get = ov5647_power_get,
	.power_put = ov5647_power_put,
	.set_mode = ov5647_set_mode,
	.start_streaming = ov5647_start_streaming,
	.stop_streaming = ov5647_stop_streaming,
};

static int ov5647_board_setup(struct ov5647 *priv)
{
	struct camera_common_data *s_data = priv->s_data;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;
	u8 reg_val[2];
	u16 chip_id;
	int err = 0;

	if (pdata->mclk_name) {
		err = camera_common_mclk_enable(s_data);
		if (err) {
			dev_err(dev, "error turning on mclk (%d)\n", err);
			goto done;
		}
	}

	err = ov5647_power_on(s_data);
	if (err) {
		dev_err(dev, "error during power on sensor (%d)\n", err);
		goto err_power_on;
	}

	/* Probe sensor model id registers */
	err = ov5647_read_reg(s_data, OV5647_REG_CHIP_ID_H, &reg_val[0]);
	if (err) {
		dev_err(dev, "%s: error during i2c read probe (%d)\n",
			__func__, err);
		goto err_reg_probe;
	}
	err = ov5647_read_reg(s_data, OV5647_REG_CHIP_ID_L, &reg_val[1]);
	if (err) {
		dev_err(dev, "%s: error during i2c read probe (%d)\n",
			__func__, err);
		goto err_reg_probe;
	}

	chip_id = (reg_val[0] << 8) | reg_val[1];
	if (chip_id != OV5647_CHIP_ID) {
		dev_err(dev, "%s: invalid sensor model id: 0x%04x\n",
			__func__, chip_id);
		err = -ENODEV;
		goto err_reg_probe;
	}
	dev_info(dev, "OV5647 chip id 0x%04x detected\n", chip_id);

err_reg_probe:
	ov5647_power_off(s_data);

err_power_on:
	if (pdata->mclk_name)
		camera_common_mclk_disable(s_data);

done:
	return err;
}

static int ov5647_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_dbg(&client->dev, "%s:\n", __func__);

	return 0;
}

static const struct v4l2_subdev_internal_ops ov5647_subdev_internal_ops = {
	.open = ov5647_open,
};

static int ov5647_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct tegracam_device *tc_dev;
	struct ov5647 *priv;
	int err;

	dev_dbg(dev, "probing v4l2 sensor at addr 0x%0x\n", client->addr);

	if (!IS_ENABLED(CONFIG_OF) || !client->dev.of_node)
		return -EINVAL;

	priv = devm_kzalloc(dev,
			sizeof(struct ov5647), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	tc_dev = devm_kzalloc(dev,
			sizeof(struct tegracam_device), GFP_KERNEL);
	if (!tc_dev)
		return -ENOMEM;

	priv->i2c_client = tc_dev->client = client;
	tc_dev->dev = dev;
	strncpy(tc_dev->name, "ov5647", sizeof(tc_dev->name));
	tc_dev->dev_regmap_config = &sensor_regmap_config;
	tc_dev->sensor_ops = &ov5647_common_ops;
	tc_dev->v4l2sd_internal_ops = &ov5647_subdev_internal_ops;
	tc_dev->tcctrl_ops = &ov5647_ctrl_ops;

	err = tegracam_device_register(tc_dev);
	if (err) {
		dev_err(dev, "tegra camera driver registration failed\n");
		return err;
	}
	priv->tc_dev = tc_dev;
	priv->s_data = tc_dev->s_data;
	priv->subdev = &tc_dev->s_data->subdev;
	tegracam_set_privdata(tc_dev, (void *)priv);

	err = ov5647_board_setup(priv);
	if (err) {
		tegracam_device_unregister(tc_dev);
		dev_err(dev, "board setup failed\n");
		return err;
	}

	err = tegracam_v4l2subdev_register(tc_dev, true);
	if (err) {
		dev_err(dev, "tegra camera subdev registration failed\n");
		return err;
	}

	dev_info(dev, "detected ov5647 sensor\n");

	return 0;
}

static int ov5647_remove(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct ov5647 *priv = (struct ov5647 *)s_data->priv;

	tegracam_v4l2subdev_unregister(priv->tc_dev);
	tegracam_device_unregister(priv->tc_dev);

	return 0;
}

static const struct i2c_device_id ov5647_id[] = {
	{ "ov5647", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ov5647_id);

static struct i2c_driver ov5647_i2c_driver = {
	.driver = {
		.name = "ov5647",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ov5647_of_match),
	},
	.probe = ov5647_probe,
	.remove = ov5647_remove,
	.id_table = ov5647_id,
};
module_i2c_driver(ov5647_i2c_driver);

MODULE_DESCRIPTION("Media Controller driver for OmniVision OV5647 (tegracam)");
MODULE_AUTHOR("Nick Stassen");
MODULE_LICENSE("GPL v2");
