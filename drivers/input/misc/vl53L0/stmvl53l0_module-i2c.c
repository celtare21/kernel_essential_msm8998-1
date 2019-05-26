/*
 *  stmvl53l0_module-i2c.c - Linux kernel modules for STM VL53L0 FlightSense TOF
 *							sensor
 *
 *  Copyright (C) 2016 STMicroelectronics Imaging Division.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/miscdevice.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/platform_device.h>
/*
 * power specific includes
 */
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/clk.h>
#include <linux/of_gpio.h>
/*
 * API includes
 */
#include "vl53l0_api.h"
#include "vl53l0_def.h"
#include "vl53l0_platform.h"
#include "stmvl53l0-i2c.h"
#include "stmvl53l0-cci.h"
#include "stmvl53l0.h"
#ifndef CAMERA_CCI
#define PINCTRL_STATE_ACTIVE "vl53l0_en_active"

/*
 * Global data
 */
static int stmvl53l0_parse_vdd(struct device *dev, struct i2c_data *data);

/*
 * QCOM specific functions
 */
static int stmvl53l0_parse_vdd(struct device *dev, struct i2c_data *data)
{
	int ret = 0;

	vl53l0_dbgmsg("Enter\n");

	if (dev->of_node) {
		data->vana = regulator_get(dev, "vdd");
		if (IS_ERR(data->vana)) {
			vl53l0_errmsg("vdd supply is not provided\n");
			ret = -1;
		}
	}
	vl53l0_dbgmsg("End\n");

	return ret;
}

static int stmvl53l0_pinctrl_init(struct i2c_client *client) {
	int rc = 0;
	struct stmvl53l0_data *data = i2c_get_clientdata(client);

	vl53l0_dbgmsg("Enter\n");

	vl53l0_dbgmsg("[STM VL5310] %s start \n", __func__);
    /* Get pinctrl if target uses pinctrl */
    data->ts_pinctrl = devm_pinctrl_get((&(client->dev)));
    if (IS_ERR_OR_NULL(data->ts_pinctrl)) {
        rc = PTR_ERR(data->ts_pinctrl);
        pr_err("Target does not use pinctrl %d\n", rc);
        goto err_pinctrl_get;
    }

    data->pinctrl_state_active
        = pinctrl_lookup_state(data->ts_pinctrl, "vl53l0_en_active");
    if (IS_ERR_OR_NULL(data->pinctrl_state_active)) {
        rc = PTR_ERR(data->pinctrl_state_active);
        dev_err(&client->dev, "Can not lookup %s pinstate %d\n", PINCTRL_STATE_ACTIVE, rc);
        goto err_pinctrl_lookup;
    }

    return 0;

err_pinctrl_lookup:
    devm_pinctrl_put(data->ts_pinctrl);
err_pinctrl_get:
    data->ts_pinctrl = NULL;
    return rc;

}

static int stmvl53l0_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	int rc = 0;
	struct stmvl53l0_data *vl53l0_data = NULL;
	struct i2c_data *i2c_object = NULL;

	vl53l0_dbgmsg("Enter\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE)) {
		vl53l0_dbgmsg("fail at %s, %d \n", __func__, __LINE__);
		rc = -EIO;
		goto END;
	}

	vl53l0_data = kzalloc(sizeof(struct stmvl53l0_data), GFP_KERNEL);
	if (!vl53l0_data) {
		vl53l0_dbgmsg("fail at %s, %d \n", __func__, __LINE__);
		rc = -ENOMEM;
		goto END;
	}
	if (vl53l0_data) {
		vl53l0_data->client_object =
		    kzalloc(sizeof(struct i2c_data), GFP_KERNEL);
		i2c_object = (struct i2c_data *)vl53l0_data->client_object;
	}
	i2c_object->client = client;

	/* setup bus type */
	vl53l0_data->bus_type = I2C_BUS;

	/* setup regulator */
	rc = stmvl53l0_parse_vdd(&i2c_object->client->dev, i2c_object);
	if(rc < 0)
		goto END;

	/* setup device name */
	vl53l0_data->dev_name = dev_name(&client->dev);

	/* setup device data */
	dev_set_drvdata(&client->dev, vl53l0_data);

	/* setup client data */
	i2c_set_clientdata(client, vl53l0_data);

	/* setup other stuff */
	rc = stmvl53l0_setup(vl53l0_data);
	if(rc < 0)
		goto END;

	rc = stmvl53l0_pinctrl_init(client);
	if(rc < 0)
		goto END;

	if(&i2c_object->client->dev.of_node){
		vl53l0_data->en_gpio = of_get_named_gpio(i2c_object->client->dev.of_node, "qcom,en_gpio", 0);
		if(!gpio_is_valid(vl53l0_data->en_gpio)){
			vl53l0_errmsg("%s:%d, en gpio not specified\n",
				__func__, __LINE__);
			rc = -1;
			goto END;
		}else{
			rc = gpio_request(vl53l0_data->en_gpio, "en_stmvl53l0");
			if(rc)
				vl53l0_errmsg("request en_stmvl53l0 gpio failed, ret=%d\n", rc);
		}
	}

	/* init default value */
	i2c_object->power_up = 0;
	gpio_direction_output(vl53l0_data->en_gpio, 0);

	vl53l0_dbgmsg("End\n");
	return rc;

END:
	return rc;
}

static int stmvl53l0_remove(struct i2c_client *client)
{
	struct stmvl53l0_data *data = i2c_get_clientdata(client);

	vl53l0_dbgmsg("Enter\n");

	/* Power down the device */
	stmvl53l0_power_down_i2c(data->client_object);
	stmvl53l0_cleanup(data);
	kfree(data->client_object);
	kfree(data);
	vl53l0_dbgmsg("End\n");
	return 0;
}

static const struct i2c_device_id stmvl53l0_id[] = {
	{STMVL53L0_DRV_NAME, 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, stmvl53l0_id);

static const struct of_device_id st_stmvl53l0_dt_match[] = {
	{.compatible = "st,stmvl53l0",},
	{},
};

static struct i2c_driver stmvl53l0_driver = {
	.driver = {
		   .name = STMVL53L0_DRV_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = st_stmvl53l0_dt_match,
		   },
	.probe = stmvl53l0_probe,
	.remove = stmvl53l0_remove,
	.id_table = stmvl53l0_id,

};

int stmvl53l0_power_up_i2c(void *i2c_object, unsigned int *preset_flag)
{
	int ret = 0;
#ifndef STM_TEST
	struct i2c_data *data = (struct i2c_data *)i2c_object;
	struct i2c_client *client = data->client;
	struct stmvl53l0_data *vl53l0_data = i2c_get_clientdata(client);
#endif

	vl53l0_dbgmsg("Enter\n");

	/* actual power on */
#ifndef STM_TEST
	ret = regulator_enable(data->vana);
	msleep(3);
	if (ret < 0) {
		vl53l0_errmsg("reg enable(%p) failed.rc=%d\n", data->vana, ret);
		return ret;
	}
	data->power_up = 1;
	*preset_flag = 1;
	gpio_direction_output(vl53l0_data->en_gpio, 1);
#endif

	vl53l0_dbgmsg("End\n");
	return ret;
}

int stmvl53l0_power_down_i2c(void *i2c_object)
{
	int ret = 0;
#ifndef STM_TEST
	struct i2c_data *data = (struct i2c_data *)i2c_object;
	struct i2c_client *client = data->client;
	struct stmvl53l0_data *vl53l0_data = i2c_get_clientdata(client);
#endif

	vl53l0_dbgmsg("Enter\n");
#ifndef STM_TEST
	msleep(3);
	ret = regulator_disable(data->vana);
	if (ret < 0)
		vl53l0_errmsg("reg disable(%p) failed.rc=%d\n",
			      data->vana, ret);

	data->power_up = 0;
	gpio_direction_output(vl53l0_data->en_gpio, 0);
#endif

	vl53l0_dbgmsg("End\n");
	return ret;
}

int stmvl53l0_init_i2c(void)
{
	int ret = 0;

#ifdef STM_TEST
	struct i2c_client *client = NULL;
	struct i2c_adapter *adapter;
	struct i2c_board_info info = {
		.type = "stmvl53l0",
		.addr = STMVL53L0_SLAVE_ADDR,
	};
#endif

	vl53l0_dbgmsg("Enter\n");

	/* register as a i2c client device */
	ret = i2c_add_driver(&stmvl53l0_driver);
	if (ret)
		vl53l0_errmsg("%d erro ret:%d\n", __LINE__, ret);

#ifdef STM_TEST
	if (!ret) {
		adapter = i2c_get_adapter(4);
		if (!adapter)
			ret = -EINVAL;
		else
			client = i2c_new_device(adapter, &info);
		if (!client)
			ret = -EINVAL;
	}
#endif

	vl53l0_dbgmsg("End with rc:%d\n", ret);

	return ret;
}

void stmvl53l0_exit_i2c(void *i2c_object)
{
	vl53l0_dbgmsg("Enter\n");
	i2c_del_driver(&stmvl53l0_driver);

	vl53l0_dbgmsg("End\n");
}

#endif				/* end of NOT CAMERA_CCI */
