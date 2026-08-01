// SPDX-License-Identifier: GPL-2.0
/*
 * ON Semiconductor LC898217 voice coil motor driver
 *
 * Copyright (c) 2026 Laszlo Gergely Lajoshazi <lajoshazilg@gmail.com>
 *
 * The LC898217 is a lens actuator driver with an 8-bit register address and
 * 16-bit register data. The focus position is a 10-bit DAC code written to
 * LC898217_REG_POSITION; the code is right-aligned in the 16-bit word.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define LC898217_REG_POSITION		0x84
#define LC898217_REG_ENABLE		0xe0
#define LC898217_ENABLE			0x01

/* The DAC is 10 bits wide. */
#define LC898217_FOCUS_MAX		1023
#define LC898217_FOCUS_STEPS		1

/* Time the actuator needs after power-on before it answers on the bus. */
#define LC898217_POWER_DELAY_US		10000

/* Time the actuator needs after being enabled before it accepts a position. */
#define LC898217_ENABLE_DELAY_US	10000

/*
 * The first transfer after the actuator is powered can time out on the bus,
 * so the enable write is retried rather than taken as final on its first
 * failure.
 */
#define LC898217_ENABLE_TRIES		5
#define LC898217_RETRY_DELAY_US		5000

/*
 * V4L2 defines a larger V4L2_CID_FOCUS_ABSOLUTE as a closer focus, while a
 * larger DAC code on this actuator drives the lens the other way, so the
 * control value is mirrored on its way to the hardware.
 */
static inline u16 lc898217_position_to_code(s32 position)
{
	return LC898217_FOCUS_MAX - position;
}

struct lc898217 {
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_subdev sd;
	struct v4l2_ctrl *focus;
	struct regulator *vcc;
};

static inline struct lc898217 *ctrl_to_lc898217(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct lc898217, ctrls);
}

static inline struct lc898217 *sd_to_lc898217(struct v4l2_subdev *sd)
{
	return container_of(sd, struct lc898217, sd);
}

static int lc898217_write(struct lc898217 *lc898217, u8 reg, u16 val, u8 size)
{
	struct i2c_client *client = v4l2_get_subdevdata(&lc898217->sd);
	u8 buf[3];
	int ret;

	buf[0] = reg;
	buf[size] = val & 0xff;
	if (size == 2)
		buf[1] = val >> 8;

	ret = i2c_master_send(client, buf, size + 1);
	if (ret < 0)
		return ret;
	if (ret != size + 1)
		return -EIO;

	return 0;
}

static int lc898217_set_position(struct lc898217 *lc898217, s32 position)
{
	return lc898217_write(lc898217, LC898217_REG_POSITION,
			      lc898217_position_to_code(position), 2);
}

static int lc898217_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct lc898217 *lc898217 = ctrl_to_lc898217(ctrl);

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;

	return lc898217_set_position(lc898217, ctrl->val);
}

static const struct v4l2_ctrl_ops lc898217_ctrl_ops = {
	.s_ctrl = lc898217_set_ctrl,
};

static int lc898217_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int lc898217_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops lc898217_internal_ops = {
	.open = lc898217_open,
	.close = lc898217_close,
};

static const struct v4l2_subdev_ops lc898217_ops = { };

static int lc898217_init_controls(struct lc898217 *lc898217)
{
	struct v4l2_ctrl_handler *hdl = &lc898217->ctrls;

	v4l2_ctrl_handler_init(hdl, 1);

	lc898217->focus = v4l2_ctrl_new_std(hdl, &lc898217_ctrl_ops,
					    V4L2_CID_FOCUS_ABSOLUTE, 0,
					    LC898217_FOCUS_MAX,
					    LC898217_FOCUS_STEPS, 0);
	if (hdl->error)
		return hdl->error;

	lc898217->sd.ctrl_handler = hdl;

	return 0;
}

static int lc898217_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct lc898217 *lc898217;
	int ret;

	lc898217 = devm_kzalloc(dev, sizeof(*lc898217), GFP_KERNEL);
	if (!lc898217)
		return -ENOMEM;

	lc898217->vcc = devm_regulator_get(dev, "vcc");
	if (IS_ERR(lc898217->vcc))
		return dev_err_probe(dev, PTR_ERR(lc898217->vcc),
				     "failed to get the vcc regulator\n");

	v4l2_i2c_subdev_init(&lc898217->sd, client, &lc898217_ops);
	lc898217->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	lc898217->sd.internal_ops = &lc898217_internal_ops;
	lc898217->sd.entity.function = MEDIA_ENT_F_LENS;

	ret = lc898217_init_controls(lc898217);
	if (ret)
		goto err_free_ctrls;

	ret = media_entity_pads_init(&lc898217->sd.entity, 0, NULL);
	if (ret < 0)
		goto err_free_ctrls;

	ret = v4l2_async_register_subdev(&lc898217->sd);
	if (ret < 0)
		goto err_cleanup_entity;

	pm_runtime_enable(dev);

	return 0;

err_cleanup_entity:
	media_entity_cleanup(&lc898217->sd.entity);
err_free_ctrls:
	v4l2_ctrl_handler_free(&lc898217->ctrls);

	return ret;
}

static int lc898217_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lc898217 *lc898217 = sd_to_lc898217(sd);

	return regulator_disable(lc898217->vcc);
}

static int lc898217_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lc898217 *lc898217 = sd_to_lc898217(sd);
	unsigned int tries;
	int ret;

	ret = regulator_enable(lc898217->vcc);
	if (ret)
		return ret;

	usleep_range(LC898217_POWER_DELAY_US, LC898217_POWER_DELAY_US + 500);

	for (tries = 0; tries < LC898217_ENABLE_TRIES; tries++) {
		ret = lc898217_write(lc898217, LC898217_REG_ENABLE,
				     LC898217_ENABLE, 1);
		if (!ret)
			break;
		usleep_range(LC898217_RETRY_DELAY_US,
			     LC898217_RETRY_DELAY_US + 500);
	}
	if (ret) {
		dev_err(dev, "failed to enable the actuator: %d\n", ret);
		goto err_disable_vcc;
	}

	usleep_range(LC898217_ENABLE_DELAY_US, LC898217_ENABLE_DELAY_US + 500);

	ret = lc898217_set_position(lc898217, lc898217->focus->val);
	if (ret) {
		dev_err(dev, "failed to set the lens position: %d\n", ret);
		goto err_disable_vcc;
	}

	return 0;

err_disable_vcc:
	regulator_disable(lc898217->vcc);

	return ret;
}

static void lc898217_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct lc898217 *lc898217 = sd_to_lc898217(sd);
	struct device *dev = &client->dev;

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&lc898217->ctrls);

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		lc898217_runtime_suspend(dev);
	pm_runtime_set_suspended(dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(lc898217_pm_ops, lc898217_runtime_suspend,
				 lc898217_runtime_resume, NULL);

static const struct of_device_id lc898217_of_match[] = {
	{ .compatible = "onnn,lc898217xc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lc898217_of_match);

static struct i2c_driver lc898217_i2c_driver = {
	.driver = {
		.name = "lc898217",
		.pm = pm_ptr(&lc898217_pm_ops),
		.of_match_table = lc898217_of_match,
	},
	.probe = lc898217_probe,
	.remove = lc898217_remove,
};
module_i2c_driver(lc898217_i2c_driver);

MODULE_AUTHOR("Laszlo Gergely Lajoshazi <lajoshazilg@gmail.com>");
MODULE_DESCRIPTION("ON Semiconductor LC898217 VCM driver");
MODULE_LICENSE("GPL");
