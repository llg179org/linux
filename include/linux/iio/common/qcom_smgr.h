/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __QCOM_SMGR_H__
#define __QCOM_SMGR_H__

#include <linux/completion.h>
#include <linux/iio/types.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct smgr;

enum smgr_sensor_type {
	SNS_SMGR_SENSOR_TYPE_UNKNOWN,
	SNS_SMGR_SENSOR_TYPE_ACCEL,
	SNS_SMGR_SENSOR_TYPE_GYRO,
	SNS_SMGR_SENSOR_TYPE_MAG,
	SNS_SMGR_SENSOR_TYPE_PROX_LIGHT,
	SNS_SMGR_SENSOR_TYPE_PRESSURE,
	SNS_SMGR_SENSOR_TYPE_HALL_EFFECT,

	SNS_SMGR_SENSOR_TYPE_COUNT
};

enum smgr_data_type {
	SNS_SMGR_DATA_TYPE_PRIMARY,
	SNS_SMGR_DATA_TYPE_SECONDARY,

	SNS_SMGR_DATA_TYPE_COUNT
};

struct smgr_data_type_item
{
	const char *name;
	const char *vendor;
	u16 max_sample_rate;
	u16 cur_sample_rate;
};

#define SMGR_SAMPLE_VALUES		3

struct smgr_sensor
{
	struct smgr *smgr;

	u8 id;
	enum smgr_sensor_type type;
	u8 data_type_count;
	struct smgr_data_type_item *data_types;

	struct iio_dev *iio_dev;

	/*
	 * Last sample the Sensor Manager sent for this sensor, kept so it can
	 * be read without a buffer. On-change sensors such as the proximity
	 * one send a report when the reading changes and then stay quiet, so
	 * the buffer alone leaves nothing to read between changes.
	 */
	struct mutex lock;
	struct completion sample_avail;
	u32 last_values[SMGR_SAMPLE_VALUES];
};

struct smgr_iio_priv
{
	struct smgr_sensor *sensor;
};

extern struct iio_buffer_setup_ops smgr_buffer_ops;

int smgr_sensor_read_sample(struct smgr_sensor *sensor, u32 *values);

#endif /* __QCOM_SMGR_H__ */
