#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/miscdevice.h>

#include <linux/platform_data/dmtimer-omap.h>

#include <clocksource/timer-ti-dm.h>
#include <linux/interrupt.h>
#include <linux/clk.h>

#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>

#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/delay.h>

struct plng_dmtimer_device {
	struct miscdevice mdev;
	struct platform_device * pdev;
	struct omap_dm_timer * dm_timer;
	const struct omap_dm_timer_ops *tops;
	struct gpio_desc *gpiop;
};

static inline
struct plng_dmtimer_device *file_to_dma_dev(struct file *file)
{
	struct miscdevice *misc = file->private_data;
	return container_of(misc, struct plng_dmtimer_device, mdev);
}

/**********************/
/******** INIT ********/
/**********************/
#define DMTIMER_FLIP(val) do {val ^= 1;} while(0)
#define NSEC_PER_MSEC   1000000L

static unsigned val = 0;

/**********************/
/******* WRITE ********/
/**********************/
static ssize_t
dmtimer_drv_write(struct file *fp, const char __user *src, size_t len,
		loff_t *off)
{
	struct plng_dmtimer_device * adev =
		file_to_dma_dev(fp);
	
	DMTIMER_FLIP(val);
	gpiod_set_value(adev->gpiop, val);
	return len;
}

static struct file_operations dmtimer_drv_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.write = dmtimer_drv_write,
	.open = nonseekable_open,
};

static irqreturn_t timer_irq_handler( int irq, void * dev_id)
{
	int status = 0;
	struct plng_dmtimer_device * adev =
		(struct plng_dmtimer_device*)dev_id;
	
	struct omap_dm_timer * dm_timer = adev->dm_timer;
	const struct omap_dm_timer_ops *tops = adev->tops;;

	/* Read the current Status */
	status = tops->read_status(dm_timer);
	/* Clear the timer interrupt */
	if (status == OMAP_TIMER_INT_OVERFLOW)
	{
		DMTIMER_FLIP(val);
		gpiod_set_value(adev->gpiop, val);
		tops->write_status(dm_timer,  OMAP_TIMER_INT_OVERFLOW);
	}   
	/* Stop the Timer */
	// tops->stop(adev->dm_timer);
	/* Indicate the Interrupt was handled */
	return IRQ_HANDLED;
}

static int dmtimer_init(struct plng_dmtimer_device * adev)
{
	int ret = 0, irq;
	unsigned long timer_rate;
	struct clk *timer_clk;

	struct device_node *timer;
	struct platform_device *timer_pdev;
	struct dmtimer_platform_data *timer_pdata;

	struct platform_device * pdev = adev->pdev;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;

	struct omap_dm_timer * dm_timer;
	const struct omap_dm_timer_ops *tops;

	/* get parent device and its pdata */
	timer = of_parse_phandle(np, "ti,timers", 0);
	if (!timer) {
		dev_err(&pdev->dev, "Unable to find timer of node!!\n");
		return -ENODEV;
	}

	timer_pdev = of_find_device_by_node(timer);
	if (!timer_pdev) {
		dev_err(&pdev->dev, "Unable to find Timer pdev!!\n");
		ret = -ENODEV;
		goto put_timer_node;
	}

	timer_pdata = dev_get_platdata(&timer_pdev->dev);
	if (!timer_pdata) {
		dev_err(&pdev->dev,
			"dmtimer pdata structure NULL!!\n");
		ret = -ENODEV;
		goto put_timer_node;
	}

	tops = timer_pdata->timer_ops;

	if (!tops || !tops->request_by_node ||
	    !tops->free ||
	    !tops->enable ||
	    !tops->disable ||
	    !tops->get_fclk ||
	    !tops->start ||
	    !tops->stop ||
	    !tops->set_load ||
	    !tops->set_match ||
	    !tops->set_pwm ||
	    !tops->set_prescaler ||
	    !tops->write_counter) {
		dev_err(&pdev->dev, "Incomplete dmtimer pdata structure!!\n");
		ret = -EINVAL;
		goto put_timer_node;
	}

	adev->tops = tops;

	dm_timer = tops->request_by_node(timer);
	if(dm_timer == NULL) {
		dev_err(&pdev->dev,
			"DM timer not available!!\n");
		ret = -ENODEV;
		goto put_timer_node;
	}

	adev->dm_timer = dm_timer;

	/* Enable the Timer */
	/* Needs to be done before we can write to the counter */
	/* returns void */
	tops->enable(dm_timer);

	/* Set the Clock source to the System Clock */
	ret = tops->set_source(dm_timer,
			       OMAP_TIMER_SRC_SYS_CLK);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Can't set clock source!!\n");
		goto free_dmtimer;
	}

	/* Determine what IRQ the timer triggers */
	ret = tops->get_irq(dm_timer);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Can't get irq number!!\n");
		goto free_dmtimer;
	}
	irq = ret;

	/* Setup the IRQ handler */
	ret = devm_request_irq(dev, irq, timer_irq_handler,
			       IRQF_TIMER, "DmtimerTimerIRQ", adev);
	if (ret != 0) {
		dev_err(dev, "Failed to request irq of '%s'\n", "DmtimerTimerIRQ");
		goto free_dmtimer;
	}

	/* Setup the timer to trigger the IRQ on the match event */
	ret = tops->set_int_enable(dm_timer,
				   OMAP_TIMER_INT_OVERFLOW);
	if (ret != 0) {
		dev_err(dev, "Failed to enable int\n");
		goto free_dmtimer;
	}

	/* Get the Clock rate in Hz */
	timer_clk = tops->get_fclk(dm_timer);
	if (!timer_clk) {
		dev_err(dev, "Failed to get fclk\n");
		ret = -ENODEV;
		goto disable_irq;
	}

	timer_rate = clk_get_rate(timer_clk);

	dev_info(&pdev->dev, "Timer rate = %lu Hz\n", timer_rate);

	/* Set the initial Count */
	ret = tops->write_counter(dm_timer,
				  /* 24Mhz -> 24 ticks in 1 msec, */
				  /* 20 * 24 gives irq shot every 20 usec */
				  0xffffffff - 24 * 20);
	if (ret != 0) {
		dev_err(dev, "Failed to write initial count\n");
		goto disable_irq;
	}

	/* Setup the Load Register */
	/* Setup as autoload to set the the counter
	 * back to our valueon an overflow */
	ret = tops->set_load(dm_timer,
			     1,
				 /* 24Mhz -> 24 ticks in 1 msec, */
				 /* 20 * 24 gives irq shot every 20 usec */
			     0xffffffff - 24 * 20);
	if (ret != 0) {
		dev_err(dev, "Failed to write setup the Load Register\n");
		goto disable_irq;
	}

	/* Start the timer */
	ret = tops->start(dm_timer);
	if (ret != 0) {
		dev_err(dev, "Failed to start timer\n");
		goto disable_irq;
	}

	goto put_timer_node;

disable_irq:
	tops->set_int_disable(dm_timer,
			      0);
free_dmtimer:
	tops->free(dm_timer);
put_timer_node:
	of_node_put(timer);
	return ret;
}

static void dmtimer_shutdown(struct plng_dmtimer_device * adev)
{
	struct omap_dm_timer * dm_timer = adev->dm_timer;
	const struct omap_dm_timer_ops *tops = adev->tops;;

	/* stop the timer */
	tops->stop(dm_timer);
	/* disable timer irq */
	tops->set_int_disable(dm_timer,
			      0);
	/* Release the timer */
	tops->free(dm_timer);
}

static int dmtimer_drv_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct plng_dmtimer_device * adev;
	struct gpio_desc * gpiop;
	
	struct pinctrl *pinctrl;

	/* we only support OF */
	if (np == NULL) {
		dev_err(&pdev->dev, "No platform of_node!\n");
		return -ENODEV;
	}

	adev = devm_kzalloc(&pdev->dev, sizeof(*adev), GFP_KERNEL);
	if (!adev) {
		dev_err(&pdev->dev, "Unable to allocate mem\n");
		return -ENOMEM;
	}

	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl)) {
		/* special handling for probe defer */
		if (PTR_ERR(pinctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_warn(&pdev->dev,
			 "pins are not configured from the driver\n");
	}

	gpiop = devm_gpiod_get_index(dev,
				     "pin2toggle",
				     0, GPIOD_OUT_LOW);

	if ( !gpiop || IS_ERR(gpiop)) {
		dev_err(&pdev->dev, "Unable to get gpio\n");
		return -ENODEV;
	}


	if (gpiod_direction_output(gpiop, 0) != 0)
		dev_warn(&pdev->dev,
			 "direction_output() nonzero ret\n");

	adev->pdev = pdev;
	adev->gpiop = gpiop;
	
	ret = dmtimer_init(adev);
	if (ret) {
		dev_err(dev, "Failed to init dm_timer\n");
		return ret;
	}

	adev->mdev.minor  = MISC_DYNAMIC_MINOR;
	adev->mdev.name   = "dmtimer_toggler";
	adev->mdev.fops   = &dmtimer_drv_fops;
	adev->mdev.parent = dev;
	
	ret = misc_register(&adev->mdev);
	if (ret) {
		dev_err(dev, "Failed to register miscdev\n");
		return ret;
	}

	dev_info(dev, "misc_register 10:%d done\n", adev->mdev.minor);
	dev_info(&pdev->dev, "Dmtimer example driver probed.\n");

	platform_set_drvdata(pdev, adev);
	return 0;
}

/**********************/
/******** EXIT ********/
/**********************/
static int dmtimer_drv_remove(struct platform_device *pdev)
{
	struct plng_dmtimer_device * adev =
		platform_get_drvdata(pdev);
	
	dmtimer_shutdown(adev);
	gpiod_set_value(adev->gpiop, 0);
	misc_deregister(&adev->mdev);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id dmtimer_drv_of_match[] = {
	{ .compatible = "plng,dmtimer_toggler", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, dmtimer_drv_of_match);
#endif

static struct platform_driver dmtimer_driver = {
	.probe		= dmtimer_drv_probe,
	.remove		= dmtimer_drv_remove,
	.driver		= {
		.name	= "dmtimer_toggler_driver",
		.of_match_table = of_match_ptr(dmtimer_drv_of_match),
	},
};

module_platform_driver(dmtimer_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Doroshenko K");
