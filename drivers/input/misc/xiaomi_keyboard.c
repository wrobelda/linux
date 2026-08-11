// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi pogo keyboard power-control driver
 *
 * Copyright (C) 2021 Xiaomi Inc.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

struct xiaomi_keyboard {
	struct device *dev;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	struct gpio_desc *vdd_gpio;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_active;
	struct pinctrl_state *pins_suspend;
	int irq;
};

static irqreturn_t xiaomi_keyboard_irq(int irq, void *data)
{
	struct xiaomi_keyboard *keyboard = data;

	/* GPIO 83 is a doorbell, not a keyboard-presence signal. */
	pm_wakeup_event(keyboard->dev, 500);

	return IRQ_HANDLED;
}

static int xiaomi_keyboard_power_on(struct xiaomi_keyboard *keyboard)
{
	int ret;

	gpiod_set_value_cansleep(keyboard->vdd_gpio, 1);
	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(keyboard->reset_gpio, 1);
	usleep_range(2000, 3000);

	ret = pinctrl_select_state(keyboard->pinctrl, keyboard->pins_active);
	if (ret) {
		gpiod_set_value_cansleep(keyboard->reset_gpio, 0);
		gpiod_set_value_cansleep(keyboard->vdd_gpio, 0);
	}

	return ret;
}

static void xiaomi_keyboard_power_off(struct xiaomi_keyboard *keyboard)
{
	pinctrl_select_state(keyboard->pinctrl, keyboard->pins_suspend);
	gpiod_set_value_cansleep(keyboard->reset_gpio, 0);
	gpiod_set_value_cansleep(keyboard->vdd_gpio, 0);
}

static int xiaomi_keyboard_suspend(struct device *dev)
{
	struct xiaomi_keyboard *keyboard = dev_get_drvdata(dev);
	int ret;

	ret = pinctrl_select_state(keyboard->pinctrl, keyboard->pins_suspend);
	if (ret)
		return ret;

	if (device_may_wakeup(dev))
		return enable_irq_wake(keyboard->irq);

	return 0;
}

static int xiaomi_keyboard_resume(struct device *dev)
{
	struct xiaomi_keyboard *keyboard = dev_get_drvdata(dev);
	int ret = 0;

	if (device_may_wakeup(dev))
		ret = disable_irq_wake(keyboard->irq);

	if (ret)
		return ret;

	return pinctrl_select_state(keyboard->pinctrl, keyboard->pins_active);
}

static DEFINE_SIMPLE_DEV_PM_OPS(xiaomi_keyboard_pm_ops,
				xiaomi_keyboard_suspend,
				xiaomi_keyboard_resume);

static int xiaomi_keyboard_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xiaomi_keyboard *keyboard;
	int ret;

	keyboard = devm_kzalloc(dev, sizeof(*keyboard), GFP_KERNEL);
	if (!keyboard)
		return -ENOMEM;

	keyboard->dev = dev;

	keyboard->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(keyboard->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(keyboard->reset_gpio),
				     "failed to get reset GPIO\n");

	keyboard->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(keyboard->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(keyboard->irq_gpio),
				     "failed to get wake GPIO\n");

	keyboard->vdd_gpio = devm_gpiod_get(dev, "vdd", GPIOD_OUT_LOW);
	if (IS_ERR(keyboard->vdd_gpio))
		return dev_err_probe(dev, PTR_ERR(keyboard->vdd_gpio),
				     "failed to get VDD GPIO\n");

	keyboard->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(keyboard->pinctrl))
		return dev_err_probe(dev, PTR_ERR(keyboard->pinctrl),
				     "failed to get pinctrl\n");

	keyboard->pins_active =
		pinctrl_lookup_state(keyboard->pinctrl, PINCTRL_STATE_DEFAULT);
	if (IS_ERR(keyboard->pins_active))
		return dev_err_probe(dev, PTR_ERR(keyboard->pins_active),
				     "failed to get active pin state\n");

	keyboard->pins_suspend =
		pinctrl_lookup_state(keyboard->pinctrl, PINCTRL_STATE_SLEEP);
	if (IS_ERR(keyboard->pins_suspend))
		return dev_err_probe(dev, PTR_ERR(keyboard->pins_suspend),
				     "failed to get suspend pin state\n");

	keyboard->irq = gpiod_to_irq(keyboard->irq_gpio);
	if (keyboard->irq < 0)
		return dev_err_probe(dev, keyboard->irq,
				     "failed to map wake IRQ\n");

	platform_set_drvdata(pdev, keyboard);
	device_init_wakeup(dev, true);

	ret = xiaomi_keyboard_power_on(keyboard);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power on keyboard MCU\n");

	ret = devm_request_threaded_irq(dev, keyboard->irq, NULL,
					xiaomi_keyboard_irq,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"xiaomi-keyboard", keyboard);
	if (ret) {
		xiaomi_keyboard_power_off(keyboard);
		return dev_err_probe(dev, ret, "failed to request wake IRQ\n");
	}

	return 0;
}

static void xiaomi_keyboard_remove(struct platform_device *pdev)
{
	struct xiaomi_keyboard *keyboard = platform_get_drvdata(pdev);

	device_init_wakeup(&pdev->dev, false);
	xiaomi_keyboard_power_off(keyboard);
}

static const struct of_device_id xiaomi_keyboard_of_match[] = {
	{ .compatible = "xiaomi,pad-5-keyboard" },
	{ }
};
MODULE_DEVICE_TABLE(of, xiaomi_keyboard_of_match);

static struct platform_driver xiaomi_keyboard_driver = {
	.probe = xiaomi_keyboard_probe,
	.remove = xiaomi_keyboard_remove,
	.driver = {
		.name = "xiaomi-keyboard",
		.of_match_table = xiaomi_keyboard_of_match,
		.pm = pm_sleep_ptr(&xiaomi_keyboard_pm_ops),
	},
};
module_platform_driver(xiaomi_keyboard_driver);

MODULE_AUTHOR("Tonghui Wang <wangtonghui@xiaomi.com>");
MODULE_DESCRIPTION("Xiaomi pogo keyboard power-control driver");
MODULE_LICENSE("GPL");
