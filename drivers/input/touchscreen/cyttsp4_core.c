// SPDX-License-Identifier: GPL-2.0-only
/*
 * cyttsp4_core.c
 * Cypress TrueTouch(TM) Standard Product V4 Core driver module.
 * For use with Cypress Txx4xx parts.
 * Supported parts include:
 * TMA4XX
 * TMA1036
 *
 * Copyright (C) 2012 Cypress Semiconductor
 * Copyright (C) 2026 Erikas Bitovtas
 *
 * Contact Cypress Semiconductor at www.cypress.com <ttdrivers@cypress.com>
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "cyttsp4_core.h"

#define GET_HSTMODE(reg) ((reg & 0x70) >> 4)
#define CY_REG_BASE 0x00
#define IS_BOOTLOADER(hst_mode, reset_detect) \
	((hst_mode) & 0x01 || (reset_detect) != 0)
#define CY_CORE_BL_HOST_SYNC_BYTE 0xff

#define CY_NUM_RETRY 3
#define CY_DELAY_DFLT 20
#define CY_DELAY_MAX 500

static u8 ldr_exit[] = { 0xFF, 0x01, 0x3B, 0x00, 0x00, 0x4F, 0x6D, 0x17 };

static int ttsp_read_block_data(struct cyttsp4 *ts, u16 command, u8 length,
				void *buf)
{
	int error;
	int tries;

	for (tries = 0; tries < CY_NUM_RETRY; tries++) {
		error = ts->bus_ops->read(ts->dev, ts->xfer_buf, command,
					  length, buf);
		if (!error)
			return 0;

		msleep(CY_DELAY_DFLT);
	}

	return -EIO;
}

static int ttsp_write_block_data(struct cyttsp4 *ts, u16 command, u8 length,
				 void *buf)
{
	int error;
	int tries;

	for (tries = 0; tries < CY_NUM_RETRY; tries++) {
		error = ts->bus_ops->write(ts->dev, ts->xfer_buf, command,
					   length, buf);
		if (!error)
			return 0;

		msleep(CY_DELAY_DFLT);
	}

	return -EIO;
}

static int cyttsp4_enter_mode(struct cyttsp4 *ts, u8 mode)
{
	u8 new_mode;
	int ret = ttsp_read_block_data(ts, CY_REG_BASE, sizeof(new_mode),
				       &new_mode);
	if (ret)
		return ret;

	new_mode &= ~CY_HST_MODE;
	new_mode |= mode | CY_HST_MODE_CHANGE;

	return ttsp_write_block_data(ts, CY_REG_BASE, sizeof(new_mode),
				     &new_mode);
}

static int cyttsp4_soft_reset(struct cyttsp4 *ts)
{
	u8 cmd = CY_HST_RESET;

	return ttsp_write_block_data(ts, CY_REG_BASE, sizeof(cmd), &cmd);
}

static void cyttsp4_hard_reset(struct cyttsp4 *ts)
{
	gpiod_set_value_cansleep(ts->rst_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ts->rst_gpio, 0);
	usleep_range(5000, 6000);
}

static int cyttsp4_reset(struct cyttsp4 *ts)
{
	int ret = 0;
	if (ts->rst_gpio)
		cyttsp4_hard_reset(ts);
	else
		ret = cyttsp4_soft_reset(ts);

	return ret;
}

static int cyttsp4_enable(struct cyttsp4 *ts)
{
	u8 mode;

	/*
	 * The device firmware can wake on an I2C or SPI memory slave
	 * address match. So just reading a register is sufficient to
	 * wake up the device. The first read attempt will fail but it
	 * will wake it up making the second read attempt successful.
	 */
	return ttsp_read_block_data(ts, CY_REG_BASE, sizeof(mode), &mode);
}

static int cyttsp4_disable(struct cyttsp4 *ts)
{
	int ret;

	ret = cyttsp4_enter_mode(ts, CY_HST_LOWPOW);
	if (ret)
		return ret;

	return 0;
}

static int cyttsp4_open(struct input_dev *dev)
{
	struct cyttsp4 *ts = input_get_drvdata(dev);
	int retval = 0;

	if (!ts->suspended)
		retval = cyttsp4_enable(ts);

	return retval;
}

static void cyttsp4_close(struct input_dev *dev)
{
	struct cyttsp4 *ts = input_get_drvdata(dev);

	if (!ts->suspended)
		cyttsp4_disable(ts);
}

static int cyttsp4_handshake(struct cyttsp4 *ts, u8 mode)
{
	u8 cmd = mode ^ CY_HST_TOGGLE;
	if (mode & CY_HST_MODE_CHANGE)
		return 0;

	return ttsp_write_block_data(ts, CY_REG_BASE, sizeof(cmd), &cmd);
}

static int cyttsp4_enter_operational_mode(struct cyttsp4 *ts)
{
	guard(mutex)(&ts->input->mutex);
	int ret = cyttsp4_enter_mode(ts, CY_HST_OPERATE);
	if (ret)
		return ret;

	ts->state = CY_ACTIVE_STATE;
	return 0;
}

static int cyttsp4_get_sysinfo_offsets(struct cyttsp4 *ts)
{
	struct cyttsp4_sysinfo_offsets *si = &ts->sysinfo_offsets;
	int ret;

	guard(mutex)(&ts->input->mutex);
	ret = ttsp_read_block_data(ts, CY_REG_BASE, sizeof(*si), si);
	if (ret)
		return ret;

	si->hst_mode = be16_to_cpu(si->hst_mode);
	si->map_sz = be16_to_cpu(si->map_sz);
	si->cydata_ofs = be16_to_cpu(si->cydata_ofs);
	si->test_ofs = be16_to_cpu(si->test_ofs);
	si->pcfg_ofs = be16_to_cpu(si->pcfg_ofs);
	si->opcfg_ofs = be16_to_cpu(si->opcfg_ofs);
	si->ddata_ofs = be16_to_cpu(si->ddata_ofs);
	si->mdata_ofs = be16_to_cpu(si->mdata_ofs);

	return cyttsp4_handshake(ts, si->hst_mode);
}

static int cyttsp4_read_pcfg(struct cyttsp4 *ts)
{
	struct cyttsp4_sysinfo_offsets *so = &ts->sysinfo_offsets;
	u8 pcfg_size = so->opcfg_ofs - so->pcfg_ofs;

	int ret;

	guard(mutex)(&ts->input->mutex);
	ret = ttsp_read_block_data(ts, so->pcfg_ofs, pcfg_size, &ts->pcfg);
	if (ret)
		return ret;

	ts->max_x = FIELD_GET(CY_RESOLUTION_MASK, be16_to_cpu(ts->pcfg.res_x));
	ts->max_y = FIELD_GET(CY_RESOLUTION_MASK, be16_to_cpu(ts->pcfg.res_y));
	ts->max_p = be16_to_cpu(ts->pcfg.max_z);

	return 0;
}

static int cyttsp4_read_opcfg(struct cyttsp4 *ts)
{
	struct cyttsp4_sysinfo_offsets *so = &ts->sysinfo_offsets;
	struct cyttsp4_tch_abs_params *tch_abs;
	struct cyttsp4_tch_rec_params *tch_rec;
	u8 opcfg_size = so->ddata_ofs - so->opcfg_ofs;
	int ret, i;

	guard(mutex)(&ts->input->mutex);
	ret = ttsp_read_block_data(ts, so->opcfg_ofs, opcfg_size, &ts->opcfg);
	if (ret)
		return ret;

	ts->max_tchs = FIELD_GET(CY_OFS_MASK, ts->opcfg.max_tchs);
	ts->tch_rec_size = FIELD_GET(CY_OFS_MASK, ts->opcfg.tch_rec_size);

	for (i = 0; i < CY_NUM_TOTAL_TCH_FIELDS; i++) {
		tch_abs = &ts->tch_abs[i];
		tch_rec = i < 7 ? &ts->opcfg.tch_rec_old[i] : &ts->opcfg.tch_rec_new[i - 7];
		tch_abs->ofs = FIELD_GET(CY_OFS_MASK, tch_rec->loc);
		tch_abs->bofs = FIELD_GET(CY_BOFS_MASK, tch_rec->loc);
		tch_abs->size = (tch_rec->size + 7) / 8;
		tch_abs->max = 1 << tch_rec->size;
	}

	return 0;
}

static int cyttsp4_enter_sysinfo_mode(struct cyttsp4 *ts)
{
	int ret;

	reinit_completion(&ts->bl_ready);

	guard(mutex)(&ts->input->mutex);
	ret = ttsp_write_block_data(ts, CY_REG_BASE, sizeof(ldr_exit),
				    (u8 *)ldr_exit);
	if (ret)
		return ret;

	if (!wait_for_completion_timeout(
		    &ts->bl_ready,
		    msecs_to_jiffies(CY_DELAY_DFLT * CY_DELAY_MAX))) {
		dev_err(ts->dev, "timeout waiting for soft reset");
		return -EIO;
	}

	return 0;
}

static void cyttsp4_lift_all(struct cyttsp4 *ts)
{
	struct input_dev *input = ts->input;
	int i;

	for (i = 0; i < ts->max_tchs; i++) {
		input_mt_slot(input, i);
		input_mt_report_slot_inactive(input);
	}

	input_mt_sync_frame(input);
	input_sync(input);

	ts->num_prv_rec = 0;
}

static void cyttsp4_get_touch_axis(struct cyttsp4 *ts,
				   struct cyttsp4_tch_abs_params *tch_abs,
				   u8 *xy_data, int *axis)
{
	size_t bofs = tch_abs->bofs;
	size_t size = tch_abs->size;
	size_t max = tch_abs->max;
	int nbytes;

	for (nbytes = 0, *axis = 0; nbytes < size; nbytes++)
		*axis = (*axis << 8) + (xy_data[nbytes] >> bofs);

	*axis &= max - 1;
}

static void cyttsp4_get_touch(struct cyttsp4 *ts, struct cyttsp4_touch *tch,
			      u8 *xy_data)
{
	struct cyttsp4_tch_abs_params *tch_abs;
	u8 *xy_touch_data;
	int abs;
	for (abs = CY_TCH_X; abs < CY_TCH_NUM_ABS; abs++) {
		tch_abs = &ts->tch_abs[abs];
		xy_touch_data = xy_data + ts->tch_abs[abs].ofs;
		cyttsp4_get_touch_axis(ts, tch_abs, xy_touch_data,
				       &tch->abs[abs]);
	}
}

static int cyttsp4_process_tchdata(struct cyttsp4 *ts, int num_of_touches)
{
	struct input_dev *input = ts->input;
	struct cyttsp4_touch tch;
	int ret, tch_id, i;

	ret = ttsp_read_block_data(ts, ts->opcfg.tt_stat_ofs + 1,
				   num_of_touches * ts->tch_rec_size,
				   ts->xy_data);
	if (ret)
		return ret;

	for (i = 0; i < num_of_touches; i++) {
		cyttsp4_get_touch(ts, &tch, ts->xy_data + i * ts->tch_rec_size);
		tch_id = tch.abs[CY_TCH_T];

		input_mt_slot(input, tch_id);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);

		touchscreen_report_pos(input, &ts->props, tch.abs[CY_TCH_X],
				       tch.abs[CY_TCH_Y], true);

		input_report_abs(input, ABS_MT_TOUCH_MAJOR,
				 tch.abs[CY_TCH_MAJ]);
		input_report_abs(input, ABS_MT_TOUCH_MINOR,
				 tch.abs[CY_TCH_MIN]);
		input_report_abs(input, ABS_MT_PRESSURE, tch.abs[CY_TCH_P]);
	}

	input_mt_sync_frame(input);
	input_sync(input);

	ts->num_prv_rec = num_of_touches;

	return 0;
}

static int cyttsp4_xy_worker(struct cyttsp4 *ts)
{
	struct cyttsp4_opcfg *opcfg = &ts->opcfg;
	int ret, num_of_touches;
	u8 rep_stat, tt_stat;

	ret = ttsp_read_block_data(ts, CY_REG_BASE, opcfg->tt_stat_ofs + 1,
				   ts->xy_mode);
	if (ret)
		return ret;

	rep_stat = ts->xy_mode[opcfg->rep_ofs + 1];
	tt_stat = ts->xy_mode[opcfg->tt_stat_ofs];

	if (IS_BAD_PKT(rep_stat)) {
		dev_err(ts->dev, "received a bad packet from the sensor!");
		return -EIO;
	}

	num_of_touches = GET_NUM_TOUCH_RECORDS(tt_stat);

	if (num_of_touches == 0 && ts->num_prv_rec == 0)
		return 0;

	if (num_of_touches)
		return cyttsp4_process_tchdata(ts, num_of_touches);
	else
		cyttsp4_lift_all(ts);

	return 0;
}

static irqreturn_t cyttsp4_irq(int irq, void *handle)
{
	struct cyttsp4 *ts = handle;
	int ret;
	u8 mode[2];
	ret = ttsp_read_block_data(ts, CY_REG_BASE, sizeof(mode), &mode);
	if (ret) {
		dev_err(ts->dev, "Failed to read HST: %pe", ERR_PTR(ret));
		goto out;
	}
	if (unlikely(ts->state == CY_BL_STATE &&
		     mode[0] != CY_CORE_BL_HOST_SYNC_BYTE)) {
		complete(&ts->bl_ready);
		goto out;
	}

	if (IS_BOOTLOADER(mode[0], mode[1]))
		ts->state = CY_BL_STATE;

	if (ts->state != CY_ACTIVE_STATE)
		goto handshake;

	cyttsp4_xy_worker(ts);

handshake:
	cyttsp4_handshake(ts, mode[0]);

out:
	return IRQ_HANDLED;
}

struct cyttsp4 *cyttsp4_probe(const struct cyttsp4_bus_ops *ops,
			      struct device *dev, u16 irq, size_t xfer_buf_size)
{
	int ret;
	struct cyttsp4 *ts;
	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (IS_ERR(ts))
		return ERR_PTR(-ENOMEM);

	ts->input = devm_input_allocate_device(dev);
	if (IS_ERR(ts->input))
		return ERR_PTR(-ENOMEM);

	const char *const supplies[] = { "vccd", "vddd" };

	ts->dev = dev;
	ts->irq = irq;
	ts->bus_ops = ops;

	ts->xfer_buf = devm_kzalloc(dev, xfer_buf_size, GFP_KERNEL);
	if (IS_ERR(ts->xfer_buf))
		return ERR_PTR(-ENOMEM);

	ts->rst_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->rst_gpio))
		return ERR_PTR(PTR_ERR(ts->rst_gpio));

	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(supplies),
					     supplies);
	if (ret)
		return ERR_PTR(ret);

	init_completion(&ts->bl_ready);

	ts->input->name = "Cypress TTSP4 TouchScreen";
	ts->input->id.bustype = ops->bustype;
	ts->input->dev.parent = ts->dev;

	ts->input->open = cyttsp4_open;
	ts->input->close = cyttsp4_close;

	ret = devm_request_threaded_irq(dev, ts->irq, NULL, cyttsp4_irq,
					IRQF_ONESHOT, "cyttsp4", ts);
	if (ret)
		return ERR_PTR(ret);

	ret = cyttsp4_reset(ts);
	if (ret)
		return ERR_PTR(ret);

	ret = cyttsp4_enter_sysinfo_mode(ts);
	if (ret)
		return ERR_PTR(ret);

	ret = cyttsp4_get_sysinfo_offsets(ts);
	if (ret)
		return ERR_PTR(ret);

	ret = cyttsp4_read_pcfg(ts);
	if (ret)
		return ERR_PTR(ret);

	ret = cyttsp4_read_opcfg(ts);
	if (ret)
		return ERR_PTR(ret);

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, ts->max_x - 1, 0,
			     0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, ts->max_y - 1, 0,
			     0);
	/* One byte for width 0..255 so this is the limit */
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, ts->max_p, 0, 0);

	touchscreen_parse_properties(ts->input, true, &ts->props);

	ret = input_mt_init_slots(ts->input, ts->max_tchs,
				  INPUT_MT_DROP_UNUSED | INPUT_MT_DIRECT);
	if (ret)
		return ERR_PTR(ret);

	input_set_drvdata(ts->input, ts);

	ts->xy_mode =
		devm_kzalloc(ts->dev, ts->opcfg.tt_stat_ofs + 1, GFP_KERNEL);
	if (IS_ERR(ts->xy_mode))
		return ERR_PTR(-ENOMEM);

	ts->xy_data = devm_kzalloc(ts->dev, ts->max_tchs * ts->tch_rec_size,
				   GFP_KERNEL);
	if (IS_ERR(ts->xy_data))
		return ERR_PTR(-ENOMEM);

	ret = cyttsp4_enter_operational_mode(ts);
	if (ret)
		return ERR_PTR(ret);

	ret = input_register_device(ts->input);
	if (ret)
		return ERR_PTR(ret);

	return ts;
}
EXPORT_SYMBOL_GPL(cyttsp4_probe);

static int cyttsp4_suspend(struct device *dev)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	int retval = 0;

	guard(mutex)(&ts->input->mutex);

	if (input_device_enabled(ts->input)) {
		retval = cyttsp4_disable(ts);
		if (retval == 0)
			ts->suspended = true;
	}

	return retval;
}

static int cyttsp4_resume(struct device *dev)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	guard(mutex)(&ts->input->mutex);

	if (input_device_enabled(ts->input))
		cyttsp4_enable(ts);

	ts->suspended = false;

	return 0;
}

EXPORT_GPL_SIMPLE_DEV_PM_OPS(cyttsp4_pm_ops, cyttsp4_suspend, cyttsp4_resume);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cypress TrueTouch4(R) Standard touchscreen driver core");
