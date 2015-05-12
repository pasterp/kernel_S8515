/*
 * MAX77660 haptic controller driver
 *
 * Author: Syed Rafiuddin <srafiuddin@nvidia.com>
 * Author: Sumit Sharma <sumsharma@nvidia.com>
 * Renamed max77665 haptic driver and reusing as max77660 haptic driver
 * Copyright (c) 2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * Based on driver max8997_haptic.c
 * Copyright (c) 2012 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/pwm.h>
#include <linux/input.h>
#include <linux/mfd/max77660/max77660-core.h>
#include <linux/regulator/consumer.h>
#include <linux/edp.h>

/* Below name is matched with the haptic driver name in Max77660 core driver */
#define MAX77660_HAPTIC_DRIVER_MATCHED_NAME "max77660-vibrator"

//Ivan add static flag
//static int haptic_enable_processing = 0;

struct max77660_haptic {
	struct device *dev;
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct regulator *regulator;
	struct work_struct work;

	bool enabled;

	unsigned int level;

	struct pwm_device *pwm;
	int pwm_period;
	enum max77660_haptic_pwm_divisor pwm_divisor;

	enum max77660_haptic_motor_type type;
	enum max77660_haptic_pulse_mode mode;
	enum max77660_haptic_invert invert;
	enum max77660_haptic_continous_mode cont_mode;

	int internal_mode_pattern;
	int pattern_cycle;
	int pattern_signal_period;
	int feedback_duty_cycle;
	int motor_startup_val;
	int scf_val;
	struct mutex		enable_lock;

	struct edp_client *haptic_edp_client;
};

static int max77660_haptic_set_duty_cycle(struct max77660_haptic *chip)
{
	int duty, i;
	u8 duty_index = 0;
	int ret = 0;
	int reg1 = 0xFF, reg2 = 0xFF;
	bool internal_mode_valid = true;

	if (chip->mode == MAX77660_EXTERNAL_MODE) {
		duty = chip->pwm_period * chip->level / 100;
		ret = pwm_config(chip->pwm, duty, chip->pwm_period);
	} else {
		for (i = 0; i < 64; i++) {
			if (chip->level <= (i + 1) * 100 / 64) {
				duty_index = i;
				break;
			}
		}
		switch (chip->internal_mode_pattern) {
		case 0:
			reg1 = MAX77660_HAPTIC_REG_SIGDC1;
			reg2 = MAX77660_HAPTIC_REG_SIGPWMDC1;
			break;
		case 1:
			reg1 = MAX77660_HAPTIC_REG_SIGDC1;
			reg2 = MAX77660_HAPTIC_REG_SIGPWMDC2;
			break;
		case 2:
			reg1 = MAX77660_HAPTIC_REG_SIGDC2;
			reg2 = MAX77660_HAPTIC_REG_SIGPWMDC3;
			break;
		case 3:
			reg1 = MAX77660_HAPTIC_REG_SIGDC1;
			reg2 = MAX77660_HAPTIC_REG_SIGPWMDC4;
			break;
		default:
			internal_mode_valid = false;
			break;
		}

		if (internal_mode_valid) {
			if (chip->internal_mode_pattern % 2 == 1)
				max77660_reg_write(chip->dev->parent,
					MAX77660_HAPTIC_SLAVE, reg1,
					chip->feedback_duty_cycle);
			else
				max77660_reg_write(chip->dev->parent,
					MAX77660_HAPTIC_SLAVE, reg1,
					chip->feedback_duty_cycle << 4);

			max77660_reg_write(chip->dev->parent,
				MAX77660_HAPTIC_SLAVE, reg2, duty_index);
		}
	}
	return ret;
}

static void max77660_haptic_configure(struct max77660_haptic *chip)
{
	u8 value1, value2, reg1, reg2;
	bool internal_mode_valid = true;

	value1 = chip->type << MAX77660_MOTOR_TYPE_SHIFT |
		chip->enabled << MAX77660_ENABLE_SHIFT |
		chip->mode << MAX77660_MODE_SHIFT | chip->pwm_divisor;
	max77660_reg_write(chip->dev->parent, MAX77660_HAPTIC_SLAVE,
		MAX77660_HAPTIC_REG_CONF2, value1);

	if ((chip->mode == MAX77660_INTERNAL_MODE) &&
		(chip->enabled)) {
		value1 = chip->invert << MAX77660_INVERT_SHIFT |
			chip->cont_mode << MAX77660_CONT_MODE_SHIFT |
			chip->motor_startup_val << MAX77660_MOTOR_STRT_SHIFT |
			chip->scf_val;

		max77660_reg_write(chip->dev->parent, MAX77660_HAPTIC_SLAVE,
			MAX77660_HAPTIC_REG_CONF1, value1);

		switch (chip->internal_mode_pattern) {
		case 0:
			value1 = chip->pattern_cycle << 4;
			reg1 = MAX77660_HAPTIC_REG_CYCLECONF1;
			value2 = chip->pattern_signal_period;
			reg2 = MAX77660_HAPTIC_REG_SIGCONF1;
			break;
		case 1:
			value1 = chip->pattern_cycle;
			reg1 = MAX77660_HAPTIC_REG_CYCLECONF1;
			value2 = chip->pattern_signal_period;
			reg2 = MAX77660_HAPTIC_REG_SIGCONF2;
			break;
		case 2:
			value1 = chip->pattern_cycle << 4;
			reg1 = MAX77660_HAPTIC_REG_CYCLECONF2;
			value2 = chip->pattern_signal_period;
			reg2 = MAX77660_HAPTIC_REG_SIGCONF3;
			break;
		case 3:
			value1 = chip->pattern_cycle;
			reg1 = MAX77660_HAPTIC_REG_CYCLECONF2;
			value2 = chip->pattern_signal_period;
			reg2 = MAX77660_HAPTIC_REG_SIGCONF4;
			break;
		default:
			internal_mode_valid = false;
			break;
		}

		if (internal_mode_valid) {
			max77660_reg_write(chip->dev->parent,
				MAX77660_HAPTIC_SLAVE, reg1, value1);
			max77660_reg_write(chip->dev->parent,
				MAX77660_HAPTIC_SLAVE, reg2, value2);
			value1 = chip->internal_mode_pattern
					<< MAX77660_CYCLE_SHIFT |
				chip->internal_mode_pattern
					<< MAX77660_SIG_PERIOD_SHIFT |
				chip->internal_mode_pattern
					<< MAX77660_SIG_DUTY_SHIFT |
				chip->internal_mode_pattern
					<< MAX77660_PWM_DUTY_SHIFT;
			max77660_reg_write(chip->dev->parent,
				MAX77660_HAPTIC_SLAVE,
				MAX77660_HAPTIC_REG_DRVCONF, value1);
		}
	}
}

static void max77660_haptic_enable(struct max77660_haptic *chip, bool enable)
{
//  printk("Ivan max77660_haptic_enable = %d \n", enable);
	if (chip->enabled == enable)
		return;

//	mutex_lock(&chip->enable_lock);
	chip->enabled = enable;

	if (enable) {
//  printk("Ivan max77660_haptic_enable ENABLE! \n");	  
		regulator_enable(chip->regulator);
		max77660_haptic_configure(chip);
		if (chip->mode == MAX77660_EXTERNAL_MODE)
			pwm_enable(chip->pwm);
//		haptic_enable_processing = 0;		//Ivan
	} else {
//  printk("Ivan max77660_haptic_enable DISABLE! \n");	  
		max77660_haptic_configure(chip);
		
	max77660_reg_write(chip->dev->parent, MAX77660_HAPTIC_SLAVE,
		MAX77660_HAPTIC_REG_CONF2, 2);
	
		if (chip->mode == MAX77660_EXTERNAL_MODE)
			pwm_disable(chip->pwm);
		regulator_disable(chip->regulator);
	}
//	mutex_unlock(&chip->enable_lock);
}

static void max77660_haptic_throttle(unsigned int new_state, void *priv_data)
{
	struct max77660_haptic *chip = priv_data;

	if (!chip)
		return;

	max77660_haptic_enable(chip, false);
}

static void max77660_haptic_play_effect_work(struct work_struct *work)
{
	struct max77660_haptic *chip =
		container_of(work, struct max77660_haptic, work);
	unsigned int approved;
	int ret;

	if (chip->level) {
		/* Request E-state before operating */
		if (chip->haptic_edp_client) {
			ret = edp_update_client_request(chip->haptic_edp_client,
					MAX77660_HAPTIC_EDP_HIGH, &approved);
			if (ret || approved != MAX77660_HAPTIC_EDP_HIGH) {
				dev_err(chip->dev,
					"E state high transition failed, error=%d, approved=%d\n",
					ret, approved);
//				haptic_enable_processing = 0;		//Ivan
				mutex_unlock(&chip->enable_lock);
				return;
			}
		}
		ret = max77660_haptic_set_duty_cycle(chip);
		if (ret) {
			dev_err(chip->dev, "set_pwm_cycle failed\n");
//			haptic_enable_processing = 0;			//Ivan
			mutex_unlock(&chip->enable_lock);
			return;
		}

		max77660_haptic_enable(chip, true);
		mutex_unlock(&chip->enable_lock);		
	} else {
		/* Disable device before releasing E-state request */
		max77660_haptic_enable(chip, false);
		if (chip->haptic_edp_client) {
			ret = edp_update_client_request(chip->haptic_edp_client,
					MAX77660_HAPTIC_EDP_LOW, NULL);
			if (ret) {
				dev_err(chip->dev,
					"E state low transition failed, error=%d\n",
					ret);
				mutex_unlock(&chip->enable_lock);				
				return;
			}
		}
		mutex_unlock(&chip->enable_lock);
	}
}


static int max77660_haptic_play_effect(struct input_dev *dev, void *data,
				  struct ff_effect *effect)
{
	struct max77660_haptic *chip = input_get_drvdata(dev);

	chip->level = effect->u.rumble.strong_magnitude;
	if (!chip->level)
		chip->level = effect->u.rumble.weak_magnitude;

	schedule_work(&chip->work);

	return 0;
}

static ssize_t max77660_haptic_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct max77660_haptic *chip = dev_get_drvdata(dev);
	int var = 0;

	if (strcmp(attr->attr.name, "pattern_cycle") == 0)
		var = chip->pattern_cycle;
	else if (strcmp(attr->attr.name, "pattern_signal_period") == 0)
		var = chip->pattern_signal_period;
	else if (strcmp(attr->attr.name, "feedback_duty_cycle") == 0)
		var = chip->feedback_duty_cycle;
	else if (strcmp(attr->attr.name, "scf_val") == 0)
		var = chip->scf_val;
	else if (strcmp(attr->attr.name, "pwm_divisor") == 0)
		var = chip->pwm_divisor;
	else if (strcmp(attr->attr.name, "vibrator_enable") == 0)
		var = chip->enabled;

	return sprintf(buf, "%d\n", var);
}

static ssize_t max77660_haptic_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct max77660_haptic *chip = dev_get_drvdata(dev);
	int var;

	sscanf(buf, "%d", &var);
	if (strcmp(attr->attr.name, "pattern_cycle") == 0) {
		if (var >= 0 && var <= 15)
			chip->pattern_cycle = var;
	} else if (strcmp(attr->attr.name, "pattern_signal_period") == 0) {
		if (var >= 0 && var <= 0xFF)
			chip->pattern_signal_period = var;
	} else if (strcmp(attr->attr.name, "feedback_duty_cycle") == 0) {
		if (var >= 0 && var <= 15)
			chip->feedback_duty_cycle = var;
	} else if (strcmp(attr->attr.name, "scf_val") == 0) {
		if (var >= 0 && var <= 7)
			chip->scf_val = var;
	} else if (strcmp(attr->attr.name, "pwm_divisor") == 0) {
		if (var >= 0 && var <= 3)
			chip->pwm_divisor = var;
	}

	return count;
}

static ssize_t max77660_haptic_vibrator_ctrl(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct max77660_haptic *chip = dev_get_drvdata(dev);
	int var;

	mutex_lock(&chip->enable_lock);
	sscanf(buf, "%d", &var);
	if (var == 0) {			/* stop vibrator */		  
//		if (haptic_enable_processing == 1)
//		  flush_scheduled_work();
//		  schedule_delayed_work(&chip->work,msecs_to_jiffies(5));
//		else
		  chip->level = 0;
		  schedule_work(&chip->work);
//		haptic_enable_processing = 0;
	} else if (var == 1) {
		chip->level = 100;
//		haptic_enable_processing = 1;			//Ivan setup flag to indicate enable processing...
		schedule_work(&chip->work);
	}

	return count;
}

static DEVICE_ATTR(pattern_cycle, 0640, max77660_haptic_show,
					max77660_haptic_store);
static DEVICE_ATTR(pattern_signal_period, 0640, max77660_haptic_show,
					max77660_haptic_store);
static DEVICE_ATTR(feedback_duty_cycle, 0640, max77660_haptic_show,
					max77660_haptic_store);
static DEVICE_ATTR(scf_val, 0640, max77660_haptic_show,
					max77660_haptic_store);
static DEVICE_ATTR(pwm_divisor, 0640, max77660_haptic_show,
					max77660_haptic_store);
static DEVICE_ATTR(vibrator_enable, 0640, max77660_haptic_show,
					max77660_haptic_vibrator_ctrl);

static struct attribute *max77660_haptics_attr[] = {
	&dev_attr_pattern_cycle.attr,
	&dev_attr_pattern_signal_period.attr,
	&dev_attr_feedback_duty_cycle.attr,
	&dev_attr_scf_val.attr,
	&dev_attr_pwm_divisor.attr,
	&dev_attr_vibrator_enable.attr,
	NULL,
};

static const struct attribute_group max77660_haptics_attr_group = {
	.attrs = max77660_haptics_attr,
};

static int __devinit max77660_haptic_probe(struct platform_device *pdev)
{
	/* we register the parent platform data */
	struct max77660_platform_data *parent_pdata;
	struct max77660_haptic_platform_data *haptic_pdata;
	struct max77660_haptic *chip;
	struct edp_manager *battery_manager = NULL;
	struct input_dev *input_dev;
	int ret;

	parent_pdata = dev_get_platdata(pdev->dev.parent);
	if (!parent_pdata) {
		dev_err(&pdev->dev, "no haptic parent platform data\n");
		return -EINVAL;
	}
	if ((!parent_pdata->haptic_pdata) ||
		!parent_pdata->haptic_pdata->pdata) {
		dev_err(&pdev->dev, "no haptic platform data\n");
		return -EINVAL;
	}
	haptic_pdata = parent_pdata->haptic_pdata->pdata;

	chip = devm_kzalloc(&pdev->dev, sizeof(struct max77660_haptic),
							GFP_KERNEL);
	if (!chip) {
		dev_err(&pdev->dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(&pdev->dev,
			"unable to allocate memory for input dev\n");
		ret = -ENOMEM;
		goto err_input_alloc;
	}

	mutex_init(&chip->enable_lock);
	chip->dev = &pdev->dev;
	chip->input_dev = input_dev;
	chip->type = haptic_pdata->type;
	chip->mode = haptic_pdata->mode;

	if (chip->mode == MAX77660_INTERNAL_MODE) {
		chip->internal_mode_pattern =
				haptic_pdata->internal_mode_pattern;
		chip->pattern_cycle = haptic_pdata->pattern_cycle;
		chip->pattern_signal_period =
				haptic_pdata->pattern_signal_period;
		chip->feedback_duty_cycle =
				haptic_pdata->feedback_duty_cycle;
		chip->invert = haptic_pdata->invert;
		chip->cont_mode = haptic_pdata->cont_mode;
		chip->motor_startup_val = haptic_pdata->motor_startup_val;
		chip->scf_val = haptic_pdata->scf_val;
	}

	if (chip->mode == MAX77660_EXTERNAL_MODE) {
		chip->pwm = pwm_request(haptic_pdata->pwm_channel_id,
					"max-vbrtr");
		if (IS_ERR(chip->pwm)) {
			dev_err(&pdev->dev,
				"unable to request PWM for haptic\n");
			ret = PTR_ERR(chip->pwm);
			goto err_pwm;
		}
	}

	chip->regulator = regulator_get(&pdev->dev, "vdd_vbrtr");
	if (IS_ERR(chip->regulator)) {
		dev_err(&pdev->dev, "unable to get regulator\n");
		ret = PTR_ERR(chip->regulator);
		goto err_regulator;
	}

	if (haptic_pdata->edp_states == NULL)
		goto register_input;

	chip->haptic_edp_client = devm_kzalloc(&pdev->dev,
				sizeof(struct edp_client), GFP_KERNEL);
	if (IS_ERR_OR_NULL(chip->haptic_edp_client)) {
		dev_err(&pdev->dev, "could not allocate edp client\n");
		goto register_input;
	}

	chip->haptic_edp_client->name[EDP_NAME_LEN - 1] = '\0';
	strncpy(chip->haptic_edp_client->name, "vibrator", EDP_NAME_LEN - 1);
	chip->haptic_edp_client->states = haptic_pdata->edp_states;
	chip->haptic_edp_client->num_states = MAX77660_HAPTIC_EDP_NUM_STATES;
	chip->haptic_edp_client->e0_index = MAX77660_HAPTIC_EDP_LOW;
	chip->haptic_edp_client->priority = EDP_MAX_PRIO + 2;
	chip->haptic_edp_client->throttle = max77660_haptic_throttle;
	chip->haptic_edp_client->private_data = chip;

	battery_manager = edp_get_manager("battery");
	if (!battery_manager) {
		dev_err(&pdev->dev, "unable to get edp manager\n");
	} else {
		ret = edp_register_client(battery_manager,
					chip->haptic_edp_client);
		if (ret) {
			dev_err(&pdev->dev, "unable to register edp client\n");
		} else {
			ret = edp_update_client_request(chip->haptic_edp_client,
				MAX77660_HAPTIC_EDP_LOW, NULL);
			if (ret) {
				dev_err(&pdev->dev,
					"unable to set E0 EDP state\n");
				edp_unregister_client(chip->haptic_edp_client);
			} else {
				goto register_input;
			}
		}
	}

	devm_kfree(&pdev->dev, chip->haptic_edp_client);
	chip->haptic_edp_client = NULL;

register_input:
	dev_set_drvdata(&pdev->dev, chip);
	input_dev->name = MAX77660_HAPTIC_DRIVER_MATCHED_NAME;
	input_dev->id.version = 1;
	input_dev->dev.parent = &pdev->dev;
	input_set_drvdata(input_dev, chip);
	input_set_capability(input_dev, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(input_dev, NULL,
				max77660_haptic_play_effect);
	if (ret) {
		dev_err(&pdev->dev,
			"unable to create FF device(ret : %d)\n", ret);
		goto err_ff_memless;
	}
	INIT_WORK(&chip->work,
			max77660_haptic_play_effect_work);

	ret = input_register_device(input_dev);
	if (ret) {
		dev_err(&pdev->dev,
			"unable to register input device(ret : %d)\n", ret);
		goto err_input_register;
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &max77660_haptics_attr_group);
	if (ret < 0)
		dev_err(&pdev->dev,
			"unable to create sysfs %d\n", ret);

	return 0;

err_input_register:
	destroy_work_on_stack(&chip->work);
	input_ff_destroy(input_dev);
err_ff_memless:
	regulator_put(chip->regulator);
err_regulator:
	if (chip->mode == MAX77660_EXTERNAL_MODE)
		pwm_free(chip->pwm);
err_pwm:
	input_free_device(input_dev);
err_input_alloc:

	dev_err(&pdev->dev, "Error: %s return ret=%d\n", __func__, ret);
	return ret;
}

static int __devexit max77660_haptic_remove(struct platform_device *pdev)
{
	struct max77660_haptic *chip = platform_get_drvdata(pdev);

	destroy_work_on_stack(&chip->work);
	input_unregister_device(chip->input_dev);
	sysfs_remove_group(&pdev->dev.kobj, &max77660_haptics_attr_group);
	regulator_put(chip->regulator);

	if (chip->mode == MAX77660_EXTERNAL_MODE)
		pwm_free(chip->pwm);

	return 0;
}

static int max77660_haptic_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct max77660_haptic *chip = platform_get_drvdata(pdev);
	int ret;

	/* Disable device before releasing E-state request */
//Ivan added
//	cancel_delayed_work(&chip->work);
	max77660_haptic_enable(chip, false);
	if (chip->haptic_edp_client) {
		ret = edp_update_client_request(chip->haptic_edp_client,
				MAX77660_HAPTIC_EDP_LOW, NULL);
		if (ret) {
			dev_err(chip->dev,
				"E state low transition failed in suspend\n");
			return ret;
		}
	}

	return 0;
}

static SIMPLE_DEV_PM_OPS(max77660_haptic_pm_ops, max77660_haptic_suspend, NULL);

static const struct platform_device_id max77660_haptic_id[] = {
	{ MAX77660_HAPTIC_DRIVER_MATCHED_NAME, 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, max77660_haptic_id);

static struct platform_driver max77660_haptic_driver = {
	.driver	= {
		.name	= "max77660-haptic",
		.owner	= THIS_MODULE,
		.pm	= &max77660_haptic_pm_ops,
	},
	.probe		= max77660_haptic_probe,
	.remove		= __devexit_p(max77660_haptic_remove),
	.id_table	= max77660_haptic_id,
};

module_platform_driver(max77660_haptic_driver);

MODULE_ALIAS("platform:max77660-haptic");
MODULE_DESCRIPTION("max77660_haptic driver");
MODULE_AUTHOR("Bitan Biswas <bbiswas@nvidia.com>");
MODULE_LICENSE("GPL v2");
