// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm Sensor Manager magnetometer driver
 *
 * Copyright (c) 2026 Lajosházi, László Gergely <lajoshazilg@gmail.com>
 *
 * Modelled on Yassine Oudjana's smgr_accel.c. The Sensor Manager registers a
 * platform device per sensor it enumerates, and the magnetometer was left
 * unbound for want of a driver. Which part it is on the Fairphone 3 is not
 * established: the SSC's own log names an ICM20602 and an EPL259x, neither of
 * which is a magnetometer, so the part behind this sensor ID is still unknown.
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/iio/buffer.h>
#include <linux/iio/common/qcom_smgr.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>

static int smgr_mag_read_raw(struct iio_dev *iio_dev,
			     struct iio_chan_spec const *chan, int *val,
			     int *val2, long mask)
{
	struct smgr_iio_priv *priv = iio_priv(iio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = priv->sensor->data_types[0].cur_sample_rate;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/*
		 * TODO: verify against a known rotation.
		 *
		 * The accelerometer reports about 640000 for one g, i.e. very
		 * nearly 2^16 per m/s^2, so the Sensor Manager appears to hand
		 * out Q16 fixed point in the sensor's SI unit. IIO wants Gauss
		 * here and the sensor's own unit is more likely microtesla, so
		 * this is two assumptions deep and needs a compass reading
		 * against a known heading before it can be trusted.
		 *
		 * Measured: the channels follow rotation clearly, but at rest
		 * they read (0.35, -1.22, 0.36) with this scale, and the
		 * magnitude swings between 0.6 and 2.8 while turning instead
		 * of staying at the Earth's field. That is a large hard-iron
		 * offset -- the phone's own magnets -- on top of an unverified
		 * scale, so neither number can be calibrated out of the other
		 * without a full-sphere fit.
		 */
		*val = 0;
		*val2 = 15259; /* 10^9 / 2^16 */
		return IIO_VAL_INT_PLUS_NANO;
	}

	return -EINVAL;
}

static int smgr_mag_write_raw(struct iio_dev *iio_dev,
			      struct iio_chan_spec const *chan, int val,
			      int val2, long mask)
{
	struct smgr_iio_priv *priv = iio_priv(iio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		priv->sensor->data_types[0].cur_sample_rate = val;

		if (iio_buffer_enabled(iio_dev))
			return iio_dev->setup_ops->postenable(iio_dev);

		return 0;
	}

	return -EINVAL;
}

static int smgr_mag_read_avail(struct iio_dev *iio_dev,
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

static const struct iio_info smgr_mag_iio_info = {
	.read_raw = smgr_mag_read_raw,
	.write_raw = smgr_mag_write_raw,
	.read_avail = smgr_mag_read_avail,
};

#define SMGR_MAG_CHANNEL(axis, index)					\
	{								\
		.type = IIO_MAGN,					\
		.modified = true,					\
		.channel2 = IIO_MOD_##axis,				\
		.scan_index = index,					\
		.scan_type = {						\
			.sign = 's',					\
			.realbits = 24,					\
			.storagebits = 32,				\
			.endianness = IIO_LE,				\
		},							\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |	\
					    BIT(IIO_CHAN_INFO_SAMP_FREQ),\
	}

static const struct iio_chan_spec smgr_mag_iio_channels[] = {
	SMGR_MAG_CHANNEL(X, 0),
	SMGR_MAG_CHANNEL(Y, 1),
	SMGR_MAG_CHANNEL(Z, 2),
	{
		.type = IIO_TIMESTAMP,
		.channel = -1,
		.scan_index = 3,
		.scan_type = {
			.sign = 'u',
			.realbits = 32,
			.storagebits = 64,
			.endianness = IIO_LE,
		},
	},
};

static int smgr_mag_probe(struct platform_device *pdev)
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

	platform_set_drvdata(pdev, iio_dev);

	iio_dev->name = "qcom-smgr-mag";
	iio_dev->info = &smgr_mag_iio_info;
	iio_dev->channels = smgr_mag_iio_channels;
	iio_dev->num_channels = ARRAY_SIZE(smgr_mag_iio_channels);

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

static void smgr_mag_remove(struct platform_device *pdev)
{
	struct iio_dev *iio_dev = platform_get_drvdata(pdev);
	struct smgr_iio_priv *priv = iio_priv(iio_dev);

	priv->sensor->iio_dev = NULL;
}

static const struct platform_device_id smgr_mag_ids[] = {
	{ .name = "qcom-smgr-mag" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, smgr_mag_ids);

static struct platform_driver smgr_mag_driver = {
	.probe = smgr_mag_probe,
	.remove = smgr_mag_remove,
	.driver	= {
		.name = "smgr_mag",
	},
	.id_table = smgr_mag_ids,
};
module_platform_driver(smgr_mag_driver);

MODULE_AUTHOR("Lajosházi, László Gergely <lajoshazilg@gmail.com>");
MODULE_DESCRIPTION("Qualcomm Sensor Manager magnetometer driver");
MODULE_LICENSE("GPL");
