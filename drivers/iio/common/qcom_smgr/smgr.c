// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm Sensor Manager driver
 *
 * Copyright (c) 2021, Yassine Oudjana <y.oudjana@protonmail.com>
 */

#include <linux/iio/buffer.h>
#include <linux/iio/common/qcom_smgr.h>
#include <linux/iio/iio.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/soc/qcom/qrtr.h>
#include <linux/types.h>
#include <net/sock.h>

#include "qmi/sns_smgr.h"

#define SMGR_REPORT_RATE_IN_HZ		0xf000

/*
 * Every sample carries the data type and sensor it came from packed into the
 * report metadata, which is what tells the two halves of a combined sensor
 * apart when one report covers both.
 */
#define SMGR_METADATA_DATA_TYPE(val1)	(((val1) >> 16) & 0xff)
#define SMGR_METADATA_SENSOR_ID(val1)	(((val1) >> 8) & 0xff)

struct smgr {
	struct device *dev;

	struct qmi_handle sns_smgr_hdl;
	struct sockaddr_qrtr sns_smgr_info;
	struct work_struct sns_smgr_work;

	u8 sensor_count;
	struct smgr_sensor *sensors;
};

static const char *smgr_sensor_type_platform_names[] = {
	[SNS_SMGR_SENSOR_TYPE_ACCEL] = "qcom-smgr-accel",
	[SNS_SMGR_SENSOR_TYPE_GYRO] = "qcom-smgr-gyro",
	[SNS_SMGR_SENSOR_TYPE_MAG] = "qcom-smgr-mag",
	[SNS_SMGR_SENSOR_TYPE_PROX_LIGHT] = "qcom-smgr-prox-light",
	[SNS_SMGR_SENSOR_TYPE_PRESSURE] = "qcom-smgr-pressure",
	[SNS_SMGR_SENSOR_TYPE_HALL_EFFECT] = "qcom-smgr-hall-effect"
};

static void smgr_unregister_sensor(void *data)
{
	struct platform_device *pdev = data;

	platform_device_unregister(pdev);
}

static int smgr_register_sensor(struct smgr *smgr, struct smgr_sensor *sensor)
{
	struct platform_device *pdev;
	const char *name = smgr_sensor_type_platform_names[sensor->type];

	pdev = platform_device_register_data(smgr->dev, name, sensor->id,
					     &sensor, sizeof(sensor));
	if (IS_ERR(pdev)) {
		dev_err(smgr->dev, "Failed to register %s: %pe\n", name, pdev);
		return PTR_ERR(pdev);
	}

	return devm_add_action_or_reset(smgr->dev, smgr_unregister_sensor,
					pdev);
}

static int smgr_request_all_sensor_info(struct smgr *smgr,
					struct smgr_sensor **sensors)
{
	struct sns_smgr_all_sensor_info_resp resp = {};
	struct qmi_txn txn;
	u8 i;
	int ret;

	dev_dbg(smgr->dev, "Getting available sensors\n");

	ret = qmi_txn_init(&smgr->sns_smgr_hdl, &txn,
			   sns_smgr_all_sensor_info_resp_ei, &resp);
	if (ret < 0) {
		dev_err(smgr->dev, "Failed to initialize QMI TXN: %d\n", ret);
		return ret;
	}

	ret = qmi_send_request(&smgr->sns_smgr_hdl, &smgr->sns_smgr_info, &txn,
			       SNS_SMGR_ALL_SENSOR_INFO_MSG_ID,
			       SNS_SMGR_ALL_SENSOR_INFO_REQ_MAX_LEN, NULL,
			       NULL);
	if (ret) {
		dev_err(smgr->dev,
			"Failed to send available sensors request: %d\n", ret);
		qmi_txn_cancel(&txn);
		return ret;
	}

	ret = qmi_txn_wait(&txn, 5 * HZ);
	if (ret < 0)
		return ret;

	/* Check the response */
	if (resp.result) {
		dev_err(smgr->dev, "Available sensors request failed: 0x%x\n",
			resp.result);
		return -EREMOTEIO;
	}

	*sensors = devm_kzalloc(smgr->dev,
				sizeof(struct smgr_sensor) * resp.item_len,
				GFP_KERNEL);

	for (i = 0; i < resp.item_len; ++i) {
		u8 j;

		(*sensors)[i].smgr = smgr;
		(*sensors)[i].id = resp.items[i].id;
		(*sensors)[i].type =
			sns_smgr_sensor_type_from_str(resp.items[i].type);
		mutex_init(&(*sensors)[i].lock);

		for (j = 0; j < SNS_SMGR_DATA_TYPE_COUNT; ++j)
			init_completion(&(*sensors)[i].samples[j].avail);
	}

	return resp.item_len;
}

static int smgr_request_single_sensor_info(struct smgr *smgr,
					   struct smgr_sensor *sensor)
{
	struct sns_smgr_single_sensor_info_req req = {
		.sensor_id = sensor->id,
	};
	struct sns_smgr_single_sensor_info_resp resp = {};
	struct qmi_txn txn;
	u8 i;
	int ret;

	dev_vdbg(smgr->dev, "Getting single sensor info for ID 0x%02x\n",
		 sensor->id);

	ret = qmi_txn_init(&smgr->sns_smgr_hdl, &txn,
			   sns_smgr_single_sensor_info_resp_ei, &resp);
	if (ret < 0) {
		dev_err(smgr->dev, "Failed to initialize QMI transaction: %d\n",
			ret);
		return ret;
	}

	ret = qmi_send_request(&smgr->sns_smgr_hdl, &smgr->sns_smgr_info, &txn,
			       SNS_SMGR_SINGLE_SENSOR_INFO_MSG_ID,
			       SNS_SMGR_SINGLE_SENSOR_INFO_REQ_MAX_LEN,
			       sns_smgr_single_sensor_info_req_ei, &req);
	if (ret < 0) {
		dev_err(smgr->dev, "Failed to send sensor data request: %d\n",
			ret);
		qmi_txn_cancel(&txn);
		return ret;
	}

	ret = qmi_txn_wait(&txn, 5 * HZ);
	if (ret < 0)
		return ret;

	/* Check the response */
	if (resp.result) {
		dev_err(smgr->dev, "Single sensor info request failed: 0x%x\n",
			resp.result);
		return -EREMOTEIO;
	}

	sensor->data_type_count = resp.data_type_len;
	sensor->data_types = devm_kzalloc(smgr->dev,
					  sizeof(struct smgr_data_type_item) *
						  sensor->data_type_count,
					  GFP_KERNEL);
	if (!sensor->data_types)
		return -ENOMEM;

	for (i = 0; i < sensor->data_type_count; ++i) {
		sensor->data_types[i].name = devm_kstrdup_const(
			smgr->dev, resp.data_types[i].name, GFP_KERNEL);
		sensor->data_types[i].vendor = devm_kstrdup_const(
			smgr->dev, resp.data_types[i].vendor, GFP_KERNEL);

		sensor->data_types[i].max_sample_rate =
			resp.data_types[i].max_sample_rate;
	}

	return 0;
}

static int smgr_request_buffering(struct smgr *smgr, struct smgr_sensor *sensor,
				  bool enable)
{
	struct sns_smgr_buffering_req req = {
		/*
		 * Reuse sensor ID as a report ID to avoid having to keep track
		 * of a separate set of IDs
		 */
		.report_id = sensor->id,
		.notify_suspend_valid = false
	};
	struct sns_smgr_buffering_resp resp = {};
	struct qmi_txn txn;
	int ret;

	if (enable) {
		u16 rate = 0;
		u8 i;

		req.action = SNS_SMGR_BUFFERING_ACTION_ADD;
		/*
		 * Ask for every data type the sensor advertises, not just the
		 * primary one: a combined part such as the Fairphone 3's
		 * EPL259x reports proximity as data type 0 and ambient light
		 * as data type 1, and the light half is silent unless it is
		 * asked for.
		 */
		req.item_len = min_t(u8, sensor->data_type_count,
				     SNS_SMGR_DATA_TYPE_COUNT);

		for (i = 0; i < req.item_len; ++i) {
			req.items[i].sensor_id = sensor->id;
			req.items[i].data_type = i;
			/* TODO: Replace hardcoded values */
			req.items[i].decimation = 0x3;
			req.items[i].calibration = 0xf;
			req.items[i].sampling_rate =
				sensor->data_types[i].cur_sample_rate;

			rate = max(rate, sensor->data_types[i].cur_sample_rate);
		}

		/* One report rate covers the whole request */
		req.report_rate = rate * SMGR_REPORT_RATE_IN_HZ;

		dev_dbg(smgr->dev,
			"Requesting buffering for sensor 0x%02x, %d data type(s), report rate: %d",
			sensor->id, req.item_len, req.report_rate);
	} else
		req.action = SNS_SMGR_BUFFERING_ACTION_DELETE;

	ret = qmi_txn_init(&smgr->sns_smgr_hdl, &txn,
			   sns_smgr_buffering_resp_ei, &resp);
	if (ret < 0) {
		dev_err(smgr->dev, "Failed to initialize QMI TXN: %d\n", ret);
		return ret;
	}

	ret = qmi_send_request(&smgr->sns_smgr_hdl, &smgr->sns_smgr_info, &txn,
			       SNS_SMGR_BUFFERING_MSG_ID,
			       SNS_SMGR_BUFFERING_REQ_MAX_LEN,
			       sns_smgr_buffering_req_ei, &req);
	if (ret < 0) {
		dev_err(smgr->dev, "Failed to send buffering request: %d\n",
			ret);
		qmi_txn_cancel(&txn);
		return ret;
	}

	ret = qmi_txn_wait(&txn, 5 * HZ);
	if (ret < 0)
		return ret;

	/* Check the response */
	if (resp.result) {
		dev_err(smgr->dev, "Buffering request failed: 0x%x\n",
			resp.result);
		return -EREMOTEIO;
	}

	dev_dbg(smgr->dev, "Buffering response ack_nak %d\n", resp.ack_nak);

	sensor->report_running = enable;

	return 0;
}

static void smgr_buffering_report_handler(struct qmi_handle *hdl,
					  struct sockaddr_qrtr *sq,
					  struct qmi_txn *txn, const void *data)
{
	struct smgr *smgr = container_of(hdl, struct smgr, sns_smgr_hdl);
	struct sns_smgr_buffering_report_ind *ind =
		(struct sns_smgr_buffering_report_ind *)data;
	struct smgr_sensor *sensor;
	struct smgr_sample *sample;
	u8 data_type;
	u8 i;

	data_type = SMGR_METADATA_DATA_TYPE(ind->metadata.val1);
	if (data_type >= SNS_SMGR_DATA_TYPE_COUNT) {
		dev_warn_ratelimited(smgr->dev,
				     "Report for unknown data type %d\n",
				     data_type);
		return;
	}

	for (i = 0; i < smgr->sensor_count; ++i) {
		sensor = &smgr->sensors[i];

		if (sensor->id != ind->report_id)
			continue;

		sample = &sensor->samples[data_type];

		// TODO: handle multiple samples
		memcpy(sample->values, ind->samples[0].values,
		       sizeof(sample->values));
		sample->valid = true;
		complete(&sample->avail);

		/*
		 * Only the primary data type has a place in the buffer: its
		 * scan layout is fixed per device, so a secondary reading
		 * would be pushed as if it were a primary one.
		 */
		if (data_type == SNS_SMGR_DATA_TYPE_PRIMARY)
			iio_push_to_buffers_with_timestamp(
				sensor->iio_dev, ind->samples[0].values,
				ind->metadata.timestamp);

		break;
	}
}

/**
 * smgr_sensor_read_sample() - read one sample outside of the buffer
 * @sensor: sensor to read
 * @data_type: which of the sensor's data types to read
 * @values: where to store the three values a report carries
 *
 * Starts a report if none is running and returns the values of the most
 * recent one. An on-change sensor reports when its reading changes and is
 * quiet otherwise, so the stored values stay current between reports.
 *
 * Waits on the requested data type rather than on the report. One report
 * covers every data type of a sensor, so a report started for one of them is
 * already running when another is first read -- and that data type has no
 * sample yet. Treating a running report as proof that this data type has
 * reported fails the first read of the ambient light half of the proximity
 * sensor whenever the light has not changed since the report began.
 *
 * The report is deliberately left running. Stopping it after each read and
 * starting it again for the next one looks tidier, but on the Fairphone 3's
 * SSC that pattern dies: the first such read returns a sample and every
 * subsequent one times out until the sensor core is restarted.
 */
int smgr_sensor_read_sample(struct smgr_sensor *sensor,
			    enum smgr_data_type data_type, u32 *values)
{
	struct smgr_sample *sample;
	int ret;

	if (data_type >= SNS_SMGR_DATA_TYPE_COUNT ||
	    data_type >= sensor->data_type_count)
		return -EINVAL;

	sample = &sensor->samples[data_type];

	mutex_lock(&sensor->lock);

	if (!sensor->report_running) {
		reinit_completion(&sample->avail);

		ret = smgr_request_buffering(sensor->smgr, sensor, true);
		if (ret)
			goto out;
	}

	/*
	 * A sample that already arrived has left the completion signalled, so
	 * this returns at once; it only blocks when this data type has yet to
	 * report.
	 */
	if (!sample->valid &&
	    !wait_for_completion_timeout(&sample->avail, HZ)) {
		ret = -ETIMEDOUT;
		goto out;
	}

	memcpy(values, sample->values, sizeof(sample->values));
	ret = 0;

out:
	mutex_unlock(&sensor->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(smgr_sensor_read_sample);

static const struct qmi_msg_handler smgr_msg_handlers[] = {
	{
		.type = QMI_INDICATION,
		.msg_id = SNS_SMGR_BUFFERING_REPORT_MSG_ID,
		.ei = sns_smgr_buffering_report_ind_ei,
		.decoded_size = sizeof(struct sns_smgr_buffering_report_ind),
		.fn = smgr_buffering_report_handler,
	},
	{}
};

static int smgr_sensor_postenable(struct iio_dev *iio_dev)
{
	struct smgr *smgr = dev_get_drvdata(iio_dev->dev.parent->parent);
	struct smgr_iio_priv *priv = iio_priv(iio_dev);
	struct smgr_sensor *sensor = priv->sensor;

	return smgr_request_buffering(smgr, sensor, true);
}

static int smgr_sensor_postdisable(struct iio_dev *iio_dev)
{
	struct smgr *smgr = dev_get_drvdata(iio_dev->dev.parent->parent);
	struct smgr_iio_priv *priv = iio_priv(iio_dev);
	struct smgr_sensor *sensor = priv->sensor;

	return smgr_request_buffering(smgr, sensor, false);
}

struct iio_buffer_setup_ops smgr_buffer_ops = {
	.postenable = &smgr_sensor_postenable,
	.postdisable = &smgr_sensor_postdisable
};
EXPORT_SYMBOL_GPL(smgr_buffer_ops);

static int smgr_probe(struct qrtr_device *qdev)
{
	struct smgr *smgr;
	int i, j;
	int ret;

	smgr = devm_kzalloc(&qdev->dev, sizeof(*smgr), GFP_KERNEL);
	if (!smgr)
		return -ENOMEM;

	smgr->dev = &qdev->dev;

	smgr->sns_smgr_info.sq_family = AF_QIPCRTR;
	smgr->sns_smgr_info.sq_node = qdev->node;
	smgr->sns_smgr_info.sq_port = qdev->port;

	dev_set_drvdata(&qdev->dev, smgr);

	ret = qmi_handle_init(&smgr->sns_smgr_hdl,
			      SNS_SMGR_SINGLE_SENSOR_INFO_RESP_MAX_LEN, NULL,
			      smgr_msg_handlers);
	if (ret < 0) {
		dev_err(smgr->dev,
			"Failed to initialize sensor manager handle: %d\n",
			ret);
		return ret;
	}

	ret = smgr_request_all_sensor_info(smgr, &smgr->sensors);
	if (ret < 0) {
		dev_err(smgr->dev, "Failed to get available sensors: %pe\n",
			ERR_PTR(ret));
		return ret;
	}
	smgr->sensor_count = ret;

	/* Get primary and secondary sensors from each sensor ID */
	for (i = 0; i < smgr->sensor_count; i++) {
		ret = smgr_request_single_sensor_info(smgr, &smgr->sensors[i]);
		if (ret < 0) {
			dev_err(smgr->dev,
				"Failed to get sensors from ID 0x%02x: %pe\n",
				smgr->sensors[i].id, ERR_PTR(ret));
			return ret;
		}

		for (j = 0; j < smgr->sensors[i].data_type_count; j++) {
			/* Default to maximum sample rate */
			smgr->sensors[i].data_types[j].cur_sample_rate =
				smgr->sensors[i].data_types[j].max_sample_rate;

			dev_dbg(smgr->dev, "0x%02x,%d: %s %s\n",
				smgr->sensors[i].id, j,
				smgr->sensors[i].data_types[j].vendor,
				smgr->sensors[i].data_types[j].name);
		}

		smgr_register_sensor(smgr, &smgr->sensors[i]);
	}

	return 0;
}

static void smgr_remove(struct qrtr_device *qdev)
{
	struct smgr *smgr = dev_get_drvdata(&qdev->dev);

	qmi_handle_release(&smgr->sns_smgr_hdl);
}

static const struct qrtr_device_id smgr_qrtr_match[] = {
	{
		.service = SNS_SMGR_QMI_SVC_ID,
		.instance = QRTR_INSTANCE(SNS_SMGR_QMI_SVC_V1,
					  SNS_SMGR_QMI_INS_ID)
	},
	{},
};
MODULE_DEVICE_TABLE(qrtr, smgr_qrtr_match);

static struct qrtr_driver smgr_driver = {
	.probe = smgr_probe,
	.remove = smgr_remove,
	.id_table = smgr_qrtr_match,
	.driver	= {
		.name = "smgr",
	},
};
module_qrtr_driver(smgr_driver);

MODULE_AUTHOR("Yassine Oudjana <y.oudjana@protonmail.com>");
MODULE_DESCRIPTION("Qualcomm Sensor Manager driver");
MODULE_LICENSE("GPL");
