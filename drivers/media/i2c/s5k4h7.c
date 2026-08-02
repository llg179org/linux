// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K4H7 image sensor - bring-up driver.
 *
 * This identifies the sensor and nothing more: it brings the supplies, the
 * master clock and the reset line up, reads the model ID and registers no
 * subdevice, because the mode register sequences this part needs are not
 * available under a licence this file could take them from.
 *
 * Copyright (c) 2026 Lajosházi, László Gergely
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>

#define S5K4H7_REG_MODEL_ID	CCI_REG16(0x0000)
#define S5K4H7_MODEL_ID		0x487b

#define S5K4H7_XCLK_FREQ	24000000

static const char * const s5k4h7_supply_names[] = {
	"vdda",		/* analog, 2.8 V */
	"vddio",	/* interface, 1.8 V */
	"vddd",		/* digital core, 1.2 V */
};

struct s5k4h7 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(s5k4h7_supply_names)];
};

static int s5k4h7_power_on(struct s5k4h7 *sensor)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
	if (ret) {
		dev_err(sensor->dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(sensor->xclk);
	if (ret) {
		dev_err(sensor->dev, "failed to enable the master clock: %d\n", ret);
		goto disable_supplies;
	}

	/*
	 * The datasheet is not public and the vendor stack does not describe
	 * the timing either, so these delays are conventional rather than
	 * measured: let the rails settle, then give the sensor a few master
	 * clock cycles before releasing reset.
	 */
	usleep_range(1000, 1200);
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(10000, 11000);

	return 0;

disable_supplies:
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
	return ret;
}

static void s5k4h7_power_off(struct s5k4h7 *sensor)
{
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	clk_disable_unprepare(sensor->xclk);
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
}

static int s5k4h7_identify(struct s5k4h7 *sensor)
{
	u64 model_id;
	int ret = 0;

	cci_read(sensor->regmap, S5K4H7_REG_MODEL_ID, &model_id, &ret);
	if (ret) {
		dev_err(sensor->dev, "failed to read the model ID: %d\n", ret);
		return ret;
	}

	if (model_id != S5K4H7_MODEL_ID) {
		dev_err(sensor->dev, "read model ID %#llx, expected %#x\n",
			model_id, S5K4H7_MODEL_ID);
		return -ENODEV;
	}

	dev_info(sensor->dev, "S5K4H7 detected, model ID %#llx\n", model_id);
	return 0;
}

static int s5k4h7_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5k4h7 *sensor;
	unsigned long rate;
	unsigned int i;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = dev;

	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "failed to initialise the CCI regmap\n");

	for (i = 0; i < ARRAY_SIZE(s5k4h7_supply_names); i++)
		sensor->supplies[i].supply = s5k4h7_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies),
				      sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get the supplies\n");

	sensor->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "failed to get the master clock\n");

	rate = clk_get_rate(sensor->xclk);
	if (rate != S5K4H7_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "master clock is %lu Hz, expected %u Hz\n",
				     rate, S5K4H7_XCLK_FREQ);

	sensor->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get the reset GPIO\n");

	ret = s5k4h7_power_on(sensor);
	if (ret)
		return ret;

	ret = s5k4h7_identify(sensor);

	s5k4h7_power_off(sensor);

	return ret;
}

static const struct of_device_id s5k4h7_of_match[] = {
	{ .compatible = "samsung,s5k4h7" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5k4h7_of_match);

static struct i2c_driver s5k4h7_i2c_driver = {
	.driver = {
		.name = "s5k4h7",
		.of_match_table = s5k4h7_of_match,
	},
	.probe = s5k4h7_probe,
};
module_i2c_driver(s5k4h7_i2c_driver);

MODULE_DESCRIPTION("Samsung S5K4H7 sensor bring-up driver");
MODULE_AUTHOR("Lajosházi, László Gergely <lajoshazilg@gmail.com>");
MODULE_LICENSE("GPL");
