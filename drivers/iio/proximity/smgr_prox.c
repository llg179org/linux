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
 * For this one the first is proximity and the second is ambient light, so both
 * are exposed as channels over the same scan layout the core pushes.
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/iio/buffer.h>
#include <linux/iio/common/qcom_smgr.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>

static int smgr_prox_read_raw(struct iio_dev *iio_dev,
			      struct iio_chan_spec const *chan, int *val,
			      int *val2, long mask)
{
	struct smgr_iio_priv *priv = iio_priv(iio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = priv->sensor->data_types[0].cur_sample_rate;
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
		priv->sensor->data_types[0].cur_sample_rate = val;

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
		samp_freq_vals[2] = priv->sensor->data_types[0].max_sample_rate;
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
		.type = IIO_PROXIMITY,
		.scan_index = 0,
		.scan_type = {
			.sign = 'u',
			.realbits = 32,
			.storagebits = 32,
			.endianness = IIO_LE,
		},
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	},
	{
		.type = IIO_LIGHT,
		.scan_index = 1,
		.scan_type = {
			.sign = 'u',
			.realbits = 32,
			.storagebits = 32,
			.endianness = IIO_LE,
		},
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
