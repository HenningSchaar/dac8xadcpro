// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASoC Driver for HiFiBerry DAC 8x ADC Pro
 *
 * Author:	Daniel Matuschek, Stuart MacLean <stuart@hifiberry.com>
 *		Copyright 2014-2015
 *		based on code by Florian Meier <florian.meier@koalo.de>
 *		ADC, HP added by Joerg Schambacher <joerg@hifiberry.com>
 *		Adapted to 8x Pro by Henning Schaar <kontakt@henningschaar.de>
 *		Copyright 2018-21
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include "codecs/pcm186x.h"
#include "codecs/pcm512x.h"
#include "hifiberry_adc_controls.h"

#define HIFIBERRY_DACPRO_NOCLOCK 0
#define HIFIBERRY_DACPRO_CLK44EN 1
#define HIFIBERRY_DACPRO_CLK48EN 2

struct pcm512x_priv {
	struct regmap *regmap;
	struct clk *sclk;
};

/* Clock rate of CLK44EN attached to GPIO6 pin */
#define CLK_44EN_RATE 22579200UL
/* Clock rate of CLK48EN attached to GPIO3 pin */
#define CLK_48EN_RATE 24576000UL

static bool snd_rpi_hifiberry_is_dacpro;
static bool digital_gain_0db_limit = true;
static bool leds_off;

static int pcm1863_add_controls(struct snd_soc_component *component)
{
	snd_soc_add_component_controls(component, pcm1863_snd_controls_card,
				       ARRAY_SIZE(pcm1863_snd_controls_card));
	return 0;
}

static void
snd_rpi_hifiberry_dac8xadcpro_select_clk(struct snd_soc_component *component,
					 int clk_id)
{
	switch (clk_id) {
	case HIFIBERRY_DACPRO_NOCLOCK:
		snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1,
					      0x24, 0x00);
		break;
	case HIFIBERRY_DACPRO_CLK44EN:
		snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1,
					      0x24, 0x20);
		break;
	case HIFIBERRY_DACPRO_CLK48EN:
		snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1,
					      0x24, 0x04);
		break;
	}
	usleep_range(3000, 4000);
}

static void
snd_rpi_hifiberry_dac8xadcpro_clk_gpio(struct snd_soc_component *component)
{
	snd_soc_component_update_bits(component, PCM512x_GPIO_EN, 0x24, 0x24);
	snd_soc_component_update_bits(component, PCM512x_GPIO_OUTPUT_3, 0x0f,
				      0x02);
	snd_soc_component_update_bits(component, PCM512x_GPIO_OUTPUT_6, 0x0f,
				      0x02);
}

static bool
snd_rpi_hifiberry_dac8xadcpro_is_sclk(struct snd_soc_component *component)
{
	unsigned int sck;

	sck = snd_soc_component_read(component, PCM512x_RATE_DET_4);
	return (!(sck & 0x40));
}

static bool
snd_rpi_hifiberry_dac8xadcpro_is_pro_card(struct snd_soc_component *component)
{
	bool is_clk_44en, is_clk_48en, is_no_clk;

	snd_rpi_hifiberry_dac8xadcpro_clk_gpio(component);

	snd_rpi_hifiberry_dac8xadcpro_select_clk(component,
						 HIFIBERRY_DACPRO_CLK44EN);
	is_clk_44en = snd_rpi_hifiberry_dac8xadcpro_is_sclk(component);

	snd_rpi_hifiberry_dac8xadcpro_select_clk(component,
						 HIFIBERRY_DACPRO_NOCLOCK);
	is_no_clk = snd_rpi_hifiberry_dac8xadcpro_is_sclk(component);

	snd_rpi_hifiberry_dac8xadcpro_select_clk(component,
						 HIFIBERRY_DACPRO_CLK48EN);
	is_clk_48en = snd_rpi_hifiberry_dac8xadcpro_is_sclk(component);

	return (is_clk_44en && is_clk_48en && !is_no_clk);
}

static int snd_rpi_hifiberry_dac8xadcpro_clk_for_rate(int sample_rate)
{
	int type;

	switch (sample_rate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
	case 176400:
	case 352800:
		type = HIFIBERRY_DACPRO_CLK44EN;
		break;
	default:
		type = HIFIBERRY_DACPRO_CLK48EN;
		break;
	}
	return type;
}

static void
snd_rpi_hifiberry_dac8xadcpro_set_sclk(struct snd_soc_component *component,
				       int sample_rate)
{
	struct pcm512x_priv *pcm512x = snd_soc_component_get_drvdata(component);

	if (!IS_ERR(pcm512x->sclk)) {
		int ctype;

		ctype = snd_rpi_hifiberry_dac8xadcpro_clk_for_rate(sample_rate);
		clk_set_rate(pcm512x->sclk,
			     (ctype == HIFIBERRY_DACPRO_CLK44EN) ?
				     CLK_44EN_RATE :
				     CLK_48EN_RATE);
		snd_rpi_hifiberry_dac8xadcpro_select_clk(component, ctype);
	}
}

static int snd_rpi_hifiberry_dac8xadcpro_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *dac0 =
		snd_soc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_component *dac1 =
		snd_soc_rtd_to_codec(rtd, 1)->component;
	struct snd_soc_component *dac2 =
		snd_soc_rtd_to_codec(rtd, 2)->component;
	struct snd_soc_component *dac3 =
		snd_soc_rtd_to_codec(rtd, 3)->component;
	struct snd_soc_component *adc = snd_soc_rtd_to_codec(rtd, 4)->component;
	struct snd_soc_component *dacs[] = { dac0, dac1, dac2, dac3 };
	struct pcm512x_priv *priv;
	int i;
	int ret;

	snd_rpi_hifiberry_is_dacpro =
		snd_rpi_hifiberry_dac8xadcpro_is_pro_card(dacs[1]);
	if (!snd_rpi_hifiberry_is_dacpro)
		return -ENODEV;

	struct snd_soc_dai_link *dai = rtd->dai_link;

	dai->name = "HiFiBerry DAC+ADC Pro";
	dai->stream_name = "HiFiBerry DAC+ADC Pro HiFi";
	/* Set CPU as provider, codecs as consumers */
	ret = snd_soc_dai_set_fmt(snd_soc_rtd_to_cpu(rtd, 0),
				  SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
					  SND_SOC_DAIFMT_CBC_CFC);
	if (ret < 0)
		return ret;

	/* set DAC DAI configuration */
	ret = snd_soc_dai_set_fmt(snd_soc_rtd_to_codec(rtd, 1),
				  SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
					  SND_SOC_DAIFMT_CBP_CFP);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(dacs); i++) {
		if (i == 1)
			continue;
		ret = snd_soc_dai_set_fmt(snd_soc_rtd_to_codec(rtd, i),
					SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
						SND_SOC_DAIFMT_CBC_CFC);
		if (ret < 0)
			return ret;
	}

	/* set ADC DAI configuration */
	ret = snd_soc_dai_set_fmt(snd_soc_rtd_to_codec(rtd, 4),
				  SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
					  SND_SOC_DAIFMT_CBC_CFC);
	if (ret < 0)
		return ret;

	/* Configure second DAC as provider */
	snd_soc_component_update_bits(dacs[1], PCM512x_BCLK_LRCLK_CFG, 0x31,
				      0x11);
	snd_soc_component_update_bits(dacs[1], PCM512x_MASTER_MODE, 0x03, 0x03);
	snd_soc_component_update_bits(dacs[1], PCM512x_MASTER_CLKDIV_2, 0x7f,
				      63);
	/* Configure first, third and fourth DACs as consumers */
	for (i = 0; i < ARRAY_SIZE(dacs); i++) {
		if (i == 1)
			continue;
		snd_soc_component_update_bits(dacs[i], PCM512x_PLL_REF, 0x70, 0x10);
		priv = snd_soc_component_get_drvdata(dacs[i]);
		priv->sclk = ERR_PTR(-ENOENT);
	}

	/* Configure GPIOs for DAC LEDs */
	for (i = 0; i < ARRAY_SIZE(dacs); i++) {
		snd_soc_component_update_bits(dacs[i], PCM512x_GPIO_EN, 0x08,
					      0x08);
		snd_soc_component_update_bits(dacs[i], PCM512x_GPIO_OUTPUT_4,
					      0x0f, 0x02);
	}
	if (leds_off) {
		for (i = 0; i < ARRAY_SIZE(dacs); i++)
			snd_soc_component_update_bits(dacs[i],
						      PCM512x_GPIO_CONTROL_1,
						      0x08, 0x00);
	} else {
		for (i = 0; i < ARRAY_SIZE(dacs); i++)
			snd_soc_component_update_bits(dacs[i],
						      PCM512x_GPIO_CONTROL_1,
						      0x08, 0x08);
	}

	ret = pcm1863_add_controls(adc);
	if (ret < 0)
		dev_warn(rtd->dev, "Failed to add pcm1863 controls: %d\n", ret);

	/* set GPIO2 to output, GPIO3 input */
	snd_soc_component_write(adc, PCM186X_GPIO3_2_CTRL, 0x00);
	snd_soc_component_write(adc, PCM186X_GPIO3_2_DIR_CTRL, 0x04);
	if (leds_off)
		snd_soc_component_update_bits(adc, PCM186X_GPIO_IN_OUT, 0x40,
					      0x00);
	else
		snd_soc_component_update_bits(adc, PCM186X_GPIO_IN_OUT, 0x40,
					      0x40);

	if (digital_gain_0db_limit) {
		int ret;
		struct snd_soc_card *card = rtd->card;

		ret = snd_soc_limit_volume(card, "Digital Playback Volume",
					   207);
		if (ret < 0)
			dev_warn(card->dev, "Failed to set volume limit: %d\n",
				 ret);
	}

	return 0;
}

static int
snd_rpi_hifiberry_dac8xadcpro_update_rate_den(struct snd_pcm_substream *substream,
					       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component =
		snd_soc_rtd_to_codec(rtd, 1)->component; /* only use DAC */
	struct pcm512x_priv *pcm512x = snd_soc_component_get_drvdata(component);
	struct snd_ratnum *rats_no_pll;
	unsigned int num = 0, den = 0;
	int err;

	rats_no_pll = devm_kzalloc(rtd->dev, sizeof(*rats_no_pll), GFP_KERNEL);
	if (!rats_no_pll)
		return -ENOMEM;

	rats_no_pll->num = clk_get_rate(pcm512x->sclk) / 64;
	rats_no_pll->den_min = 1;
	rats_no_pll->den_max = 128;
	rats_no_pll->den_step = 1;

	err = snd_interval_ratnum(hw_param_interval(params,
						    SNDRV_PCM_HW_PARAM_RATE),
				  1, rats_no_pll, &num, &den);
	if (err >= 0 && den) {
		params->rate_num = num;
		params->rate_den = den;
	}

	devm_kfree(rtd->dev, rats_no_pll);
	return 0;
}

static int
snd_rpi_hifiberry_dac8xadcpro_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int width = snd_pcm_format_width(params_format(params));
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *dac0_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *dac1_dai = snd_soc_rtd_to_codec(rtd, 1);
	struct snd_soc_dai *dac2_dai = snd_soc_rtd_to_codec(rtd, 2);
	struct snd_soc_dai *dac3_dai = snd_soc_rtd_to_codec(rtd, 3);
	struct snd_soc_dai *dais[] = { dac0_dai, dac1_dai, dac2_dai,
					     dac3_dai };
	struct snd_soc_dai_driver *drv = dac1_dai->driver;
	const struct snd_soc_dai_ops *ops = drv->ops;

	/* Using powers of 2 allows for an integer clock divisor */
	width = width <= 16 ? 16 : 32;

	snd_rpi_hifiberry_dac8xadcpro_set_sclk(dais[1]->component, params_rate(params));

	ret = snd_rpi_hifiberry_dac8xadcpro_update_rate_den(substream, params);
	if (ret)
		return ret;

	ret = snd_soc_dai_set_bclk_ratio(cpu_dai, 2 * width);
	if (ret)
		return ret;
	for (int i = 0; i < ARRAY_SIZE(dais); i++) {
		ret = snd_soc_dai_set_bclk_ratio(dais[i], 2 * width);
		if (ret)
			return ret;
	}

	if (snd_rpi_hifiberry_is_dacpro && ops->hw_params)
		ret = ops->hw_params(substream, params, dais[1]);
	return ret;
}

static int
snd_rpi_hifiberry_dac8xadcpro_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *dac0 =
		snd_soc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_component *dac1 =
		snd_soc_rtd_to_codec(rtd, 1)->component;
	struct snd_soc_component *dac2 =
		snd_soc_rtd_to_codec(rtd, 2)->component;
	struct snd_soc_component *dac3 =
		snd_soc_rtd_to_codec(rtd, 3)->component;
	struct snd_soc_component *dacs[] = {
		dac0,
		dac1,
		dac2,
		dac3,
	};

	struct snd_soc_component *adc = snd_soc_rtd_to_codec(rtd, 4)->component;
	/* synchronize dacs */
	for (int i = 0; i < ARRAY_SIZE(dacs); i++)
		snd_soc_component_update_bits(dacs[i], PCM512x_SYNCHRONIZE,
					      0x01, 0x01);
	for (int i = 0; i < ARRAY_SIZE(dacs); i++)
		snd_soc_component_update_bits(dacs[i], PCM512x_SYNCHRONIZE,
					      0x01, 0x00);
	if (leds_off)
		return 0;
	/* switch on respective LEDs */
	if (!substream->stream) {
		for (int i = 0; i < ARRAY_SIZE(dacs); i++)
			snd_soc_component_update_bits(dacs[i],
						      PCM512x_GPIO_CONTROL_1,
						      0x08, 0x08);
	} else {
		snd_soc_component_update_bits(adc, PCM186X_GPIO_IN_OUT, 0x40,
					      0x40);
	}
	return 0;
}

static void
snd_rpi_hifiberry_dac8xadcpro_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *dac0 =
		snd_soc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_component *dac1 =
		snd_soc_rtd_to_codec(rtd, 1)->component;
	struct snd_soc_component *dac2 =
		snd_soc_rtd_to_codec(rtd, 2)->component;
	struct snd_soc_component *dac3 =
		snd_soc_rtd_to_codec(rtd, 3)->component;
	struct snd_soc_component *dacs[] = {
		dac0,
		dac1,
		dac2,
		dac3,
	};
	struct snd_soc_component *adc = snd_soc_rtd_to_codec(rtd, 4)->component;

	/* switch off respective LEDs */
	if (!substream->stream) {
		for (int i = 0; i < ARRAY_SIZE(dacs); i++)
			snd_soc_component_update_bits(dacs[i],
						      PCM512x_GPIO_CONTROL_1,
						      0x08, 0x00);
	} else {
		snd_soc_component_update_bits(adc, PCM186X_GPIO_IN_OUT, 0x40,
					      0x00);
	}
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_hifiberry_dac8xadcpro_ops = {
	.hw_params = snd_rpi_hifiberry_dac8xadcpro_hw_params,
	.startup = snd_rpi_hifiberry_dac8xadcpro_startup,
	.shutdown = snd_rpi_hifiberry_dac8xadcpro_shutdown,
};

SND_SOC_DAILINK_DEFS(hifi,
	DAILINK_COMP_ARRAY(COMP_CPU("dw-i2s.1")),
	DAILINK_COMP_ARRAY(COMP_CODEC("pcm512x.1-004c", "pcm512x-hifi"),
			   COMP_CODEC("pcm512x.1-004d", "pcm512x-hifi"),
			   COMP_CODEC("pcm512x.1-004e", "pcm512x-hifi"),
			   COMP_CODEC("pcm512x.1-004f", "pcm512x-hifi"),
			   COMP_CODEC("pcm186x.1-004a", "pcm1863-aif")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("dw-i2s.1")));

static struct snd_soc_dai_link snd_rpi_hifiberry_dac8xadcpro_dai[] = {
	{
		.name = "HiFiBerry DAC+ADC PRO",
		.stream_name = "HiFiBerry DAC+ADC PRO HiFi",
		.ops = &snd_rpi_hifiberry_dac8xadcpro_ops,
		.init = snd_rpi_hifiberry_dac8xadcpro_init,
		SND_SOC_DAILINK_REG(hifi),
	},
};

static struct snd_soc_card snd_rpi_hifiberry_dac8xadcpro = {
	.name = "snd_rpi_hifiberry_dac8xadcpro",
	.driver_name = "HifiberryDac8xAdcPro",
	.owner = THIS_MODULE,
	.dai_link = snd_rpi_hifiberry_dac8xadcpro_dai,
	.num_links = ARRAY_SIZE(snd_rpi_hifiberry_dac8xadcpro_dai),
};

static int snd_rpi_hifiberry_dac8xadcpro_probe(struct platform_device *pdev)
{
	int ret = 0, i = 0;
	struct snd_soc_card *card = &snd_rpi_hifiberry_dac8xadcpro;

	snd_rpi_hifiberry_dac8xadcpro.dev = &pdev->dev;
	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_rpi_hifiberry_dac8xadcpro_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node, "i2s-controller",
					    0);
		if (i2s_node) {
			for (i = 0; i < card->num_links; i++) {
				dai->cpus->dai_name = NULL;
				dai->cpus->of_node = i2s_node;
				dai->platforms->name = NULL;
				dai->platforms->of_node = i2s_node;
			}
		}
	}
	digital_gain_0db_limit =
		!of_property_read_bool(pdev->dev.of_node,
				       "hifiberry-dac8xadcpro,24db_digital_gain");
	leds_off = of_property_read_bool(pdev->dev.of_node,
					 "hifiberry-dac8xadcpro,leds_off");
	ret = snd_soc_register_card(&snd_rpi_hifiberry_dac8xadcpro);
	if (ret && ret != -EPROBE_DEFER)
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n",
			ret);

	return ret;
}

static const struct of_device_id snd_rpi_hifiberry_dac8xadcpro_of_match[] = {
	{
		.compatible = "hifiberry,hifiberry-dac8xadcpro",
	},
	{},
};

MODULE_DEVICE_TABLE(of, snd_rpi_hifiberry_dac8xadcpro_of_match);

static struct platform_driver snd_rpi_hifiberry_dac8xadcpro_driver = {
	.driver = {
		.name = "snd-rpi-hifiberry-dac8xadcpro",
		.owner = THIS_MODULE,
		.of_match_table = snd_rpi_hifiberry_dac8xadcpro_of_match,
	},
	.probe = snd_rpi_hifiberry_dac8xadcpro_probe,
};

module_platform_driver(snd_rpi_hifiberry_dac8xadcpro_driver);

MODULE_AUTHOR("Joerg Schambacher <joerg@hifiberry.com>");
MODULE_AUTHOR("Daniel Matuschek <daniel@hifiberry.com>");
MODULE_AUTHOR("Henning Schaar <kontakt@henningschaar.de>");
MODULE_DESCRIPTION("ASoC Driver for HiFiBerry DAC 8x ADC Pro");
MODULE_LICENSE("GPL v2");
