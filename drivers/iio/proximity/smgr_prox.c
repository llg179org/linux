// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm Sensor Manager proximity and ambient light driver
 *
 * Copyright (c) 2026 Lajosházi, László Gergely <lajoshazilg@gmail.com>
 *
 * Modelled directly on Yassine Oudjana's smgr_accel.c; the Sensor Manager core
 * it binds to, and the QRTR bus underneath, are his work too. The SSC registers
 * one platform device per sensor it finds, and upstream only had a driver for
 * the accelerometer, so the "qcom-smgr-prox-light" device sat unbound. On the
 * Fairphone 3 that device is an EPL259x, which the SSC's own debug log shows it
 * initialising (dd_epl259x.c: set_psensor_intr_threshold / enable_pflag).
 *
 * A buffering report carries three u32 values per sample regardless of sensor.
 * Measured against a hand over the earpiece, the primary data type puts the
 * SSC's near/far decision in the first and the reflected-infrared count in the
 * second; the third stays zero.
 *
 * The same part is also the ambient light sensor, as its name from
 * SINGLE_SENSOR_INFO says: "EPL259x ALS/PS". PS is the primary data type, ALS
 * the secondary one, and the secondary is silent unless the core asks for it.
 * Measured with a hand over the sensor and then a torch shone into it: the
 * light half puts illuminance in lux in the first value as a Q16 fixed-point
 * number -- always a whole number of lux, so the low 16 bits are zero -- and
 * the raw ADC count behind it in the second, at a steady 2.598 counts per lux.
 * A covered sensor reads exactly 0, a dim room 7..24 lux, and a torch drives it
 * to 25230 lux, where the count reaches 65535 and stops: the reading saturates
 * there rather than rolling over.
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/iio/buffer.h>
#include <linux/iio/common/qcom_smgr.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>

/* Illuminance arrives as Q16 fixed point */
#define SMGR_PROX_LIGHT_SCALE		65536

/* Proximity is the primary data type of this part, ambient light the secondary */
static enum smgr_data_type smgr_prox_data_type(struct iio_chan_spec const *chan)
{
	return chan->type == IIO_LIGHT ? SNS_SMGR_DATA_TYPE_SECONDARY :
					 SNS_SMGR_DATA_TYPE_PRIMARY;
}

static int smgr_prox_read_raw(struct iio_dev *iio_dev,
			      struct iio_chan_spec const *chan, int *val,
			      int *val2, long mask)
{
	struct smgr_iio_priv *priv = iio_priv(iio_dev);
	enum smgr_data_type data_type = smgr_prox_data_type(chan);
	u32 values[SMGR_SAMPLE_VALUES];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = smgr_sensor_read_sample(priv->sensor, data_type, values);
		if (ret)
			return ret;

		*val = values[chan->scan_index];
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_PROCESSED:
		ret = smgr_sensor_read_sample(priv->sensor, data_type, values);
		if (ret)
			return ret;

		*val = values[0];
		*val2 = SMGR_PROX_LIGHT_SCALE;
		return IIO_VAL_FRACTIONAL;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = priv->sensor->data_types[data_type].cur_sample_rate;
		return IIO_VAL_INT;
	}

	return -EINVAL;
}

static int smgr_prox_write_raw(struct iio_dev *iio_dev,
			       struct iio_chan_spec const *chan, int val,
			       int val2, long mask)
{
	struct smgr_iio_priv *priv = iio_priv(iio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		priv->sensor->data_types[smgr_prox_data_type(chan)]
			.cur_sample_rate = val;

		/*
		 * Send a new SMGR buffering request with the updated rate if
		 * the buffer is already running.
		 */
		if (iio_buffer_enabled(iio_dev))
			return iio_dev->setup_ops->postenable(iio_dev);

		return 0;
	}

	return -EINVAL;
}

static int smgr_prox_read_avail(struct iio_dev *iio_dev,
				struct iio_chan_spec const *chan,
				const int **vals, int *type, int *length,
				long mask)
{
	struct smgr_iio_priv *priv = iio_priv(iio_dev);
	static int samp_freq_vals[3];

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		samp_freq_vals[0] = 1;
		samp_freq_vals[1] = 1;
		samp_freq_vals[2] =
			priv->sensor->data_types[smgr_prox_data_type(chan)]
				.max_sample_rate;
		*type = IIO_VAL_INT;
		*vals = samp_freq_vals;
		*length = ARRAY_SIZE(samp_freq_vals);
		return IIO_AVAIL_RANGE;
	}

	return -EINVAL;
}

static const struct iio_info smgr_prox_iio_info = {
	.read_raw = smgr_prox_read_raw,
	.write_raw = smgr_prox_write_raw,
	.read_avail = smgr_prox_read_avail,
};

static const struct iio_chan_spec smgr_prox_iio_channels[] = {
	{
		/*
		 * The SSC's own near/far decision, 1.0 in Q16 for near. It is
		 * not filled in on the first sample of a report -- measured
		 * with a hand on the sensor: this read 0 while the count below
		 * read 13815 -- so it cannot back a raw read, and a poll would
		 * answer "nothing near" with a hand on the phone. The buffer
		 * still carries it.
		 */
		.type = IIO_PROXIMITY,
		.indexed = true,
		.channel = 0,
		.scan_index = 0,
		.scan_type = {
			.sign = 'u',
			.realbits = 32,
			.storagebits = 32,
			.endianness = IIO_LE,
		},
	},
	{
		/*
		 * Reflected infrared, live from the first sample on. Measured
		 * in a dark room: 0..485 with nothing near, 1579..2714 with a
		 * hand over the earpiece, and the phone's factory calibration
		 * in /persist puts the near threshold at 1570, between the two.
		 *
		 * iio-sensor-proxy, and so phosh's in-call blanking, has no
		 * buffered proximity driver at all -- it polls in_proximity_raw
		 * -- so this channel has to be readable without a buffer.
		 */
		.type = IIO_PROXIMITY,
		.scan_index = 1,
		.scan_type = {
			.sign = 'u',
			.realbits = 32,
			.storagebits = 32,
			.endianness = IIO_LE,
		},
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	},
	{
		/*
		 * Ambient light, from the secondary data type of the same part.
		 * Reported straight in lux, so it is a processed channel and
		 * needs no scale; userspace reads in_illuminance_input.
		 *
		 * It is not a scan element: the buffer's layout is fixed by the
		 * primary data type, and a light sample pushed into it would
		 * arrive as a proximity one.
		 */
		.type = IIO_LIGHT,
		.scan_index = -1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	},
	{
		.type = IIO_TIMESTAMP,
		.channel = -1,
		.scan_index = 2,
		.scan_type = {
			.sign = 'u',
			.realbits = 32,
			.storagebits = 64,
			.endianness = IIO_LE,
		},
	},
};

static int smgr_prox_probe(struct platform_device *pdev)
{
	struct iio_dev *iio_dev;
	struct smgr_iio_priv *priv;
	int ret;

	iio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*priv));
	if (!iio_dev)
		return -ENOMEM;

	priv = iio_priv(iio_dev);
	priv->sensor = *(struct smgr_sensor **)pdev->dev.platform_data;
	priv->sensor->iio_dev = iio_dev;

	/*
	 * devm_iio_device_alloc does not set the platform drvdata, and the
	 * remove callback below reads it -- copying smgr_accel.c verbatim gave
	 * a NULL dereference the first time the device was unbound by hand.
	 */
	platform_set_drvdata(pdev, iio_dev);

	iio_dev->name = "qcom-smgr-prox-light";
	iio_dev->info = &smgr_prox_iio_info;
	iio_dev->channels = smgr_prox_iio_channels;
	iio_dev->num_channels = ARRAY_SIZE(smgr_prox_iio_channels);

	ret = devm_iio_kfifo_buffer_setup(&pdev->dev, iio_dev,
					  &smgr_buffer_ops);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to setup buffer\n");

	ret = devm_iio_device_register(&pdev->dev, iio_dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register IIO device\n");

	return 0;
}

static void smgr_prox_remove(struct platform_device *pdev)
{
	struct iio_dev *iio_dev = platform_get_drvdata(pdev);
	struct smgr_iio_priv *priv = iio_priv(iio_dev);

	priv->sensor->iio_dev = NULL;
}

static const struct platform_device_id smgr_prox_ids[] = {
	{ .name = "qcom-smgr-prox-light" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, smgr_prox_ids);

static struct platform_driver smgr_prox_driver = {
	.probe = smgr_prox_probe,
	.remove = smgr_prox_remove,
	.driver	= {
		.name = "smgr_prox",
	},
	.id_table = smgr_prox_ids,
};
module_platform_driver(smgr_prox_driver);

MODULE_AUTHOR("Lajosházi, László Gergely <lajoshazilg@gmail.com>");
MODULE_DESCRIPTION("Qualcomm Sensor Manager proximity and light driver");
MODULE_LICENSE("GPL");
