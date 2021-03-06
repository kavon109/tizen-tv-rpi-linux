/*
 * Driver for memory based ft5406 touchscreen
 *
 * Copyright (C) 2015 Raspberry Pi
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */


#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/bitops.h>
#include <linux/input/mt.h>
#include <linux/kthread.h>
#include <linux/platform_device.h>
#include <asm/io.h>
#include <linux/platform_data/mailbox-bcm2708.h>

#define MAXIMUM_SUPPORTED_POINTS 10
struct ft5406_regs {
	uint8_t device_mode;
	uint8_t gesture_id;
	uint8_t num_points;
	struct ft5406_touch {
		uint8_t xh;
		uint8_t xl;
		uint8_t yh;
		uint8_t yl;
		uint8_t res1;
		uint8_t res2;
	} point[MAXIMUM_SUPPORTED_POINTS];
};

#define SCREEN_WIDTH  800
#define SCREEN_HEIGHT 480

struct ft5406 {
	struct platform_device * pdev;
	struct input_dev       * input_dev;
	void __iomem           * ts_base;
	struct ft5406_regs     * regs;
	struct task_struct     * thread;
};


/* tag part of the message */
struct vc_msg_tag {
	uint32_t tag_id;		/* the message id */
	uint32_t buffer_size;	/* size of the buffer (which in this case is always 8 bytes) */
	uint32_t data_size;		/* amount of data being sent or received */
	uint32_t val;           /* data buffer */
};

/* message structure to be sent to videocore */
struct vc_msg {
	uint32_t msg_size;		/* simply, sizeof(struct vc_msg) */
	uint32_t request_code;	/* holds various information like the success and number of bytes returned (refer to mailboxes wiki) */
	struct vc_msg_tag tag;	/* the tag structure above to make */
	uint32_t end_tag;		/* an end identifier, should be set to NULL */
};

/* Thread to poll for touchscreen events
 * 
 * This thread polls the memory based register copy of the ft5406 registers
 * using the number of points register to know whether the copy has been
 * updated (we write 99 to the memory copy, the GPU will write between 
 * 0 - 10 points)
 */
static int ft5406_thread(void *arg)
{
	struct ft5406 *ts = (struct ft5406 *) arg;
	struct ft5406_regs regs;
	int known_ids = 0;
	
	while(!kthread_should_stop())
	{
		// 60fps polling
		msleep(17);
		memcpy_fromio(&regs, ts->regs, sizeof(*ts->regs));
		writel(99, &ts->regs->num_points);
		// Do not output if theres no new information (num_points is 99)
		// or we have no touch points and don't need to release any
		if(!(regs.num_points == 99 || (regs.num_points == 0 && known_ids == 0)))
		{
			int i;
			int modified_ids = 0, released_ids;
			for(i = 0; i < regs.num_points; i++)
			{
				int x = (((int) regs.point[i].xh & 0xf) << 8) + regs.point[i].xl;
				int y = (((int) regs.point[i].yh & 0xf) << 8) + regs.point[i].yl;
				int touchid = (regs.point[i].yh >> 4) & 0xf;
				
				modified_ids |= 1 << touchid;

				if(!((1 << touchid) & known_ids))
					dev_dbg(&ts->pdev->dev, "x = %d, y = %d, touchid = %d\n", x, y, touchid);
				
				input_mt_slot(ts->input_dev, touchid);
				input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 1);

				input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);

			}

			released_ids = known_ids & ~modified_ids;
			for(i = 0; released_ids && i < MAXIMUM_SUPPORTED_POINTS; i++)
			{
				if(released_ids & (1<<i))
				{
					dev_dbg(&ts->pdev->dev, "Released %d, known = %x modified = %x\n", i, known_ids, modified_ids);
					input_mt_slot(ts->input_dev, i);
					input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
					modified_ids &= ~(1 << i);
				}
			}
			known_ids = modified_ids;
			
			input_mt_report_pointer_emulation(ts->input_dev, true);
			input_sync(ts->input_dev);
		}
			
	}
	
	return 0;
}

static int ft5406_probe(struct platform_device *pdev)
{
	int ret;
	struct input_dev * input_dev = input_allocate_device();
	struct vc_msg request;
	struct ft5406 * ts;
	
	dev_info(&pdev->dev, "Probing device\n");
	
	ts = kzalloc(sizeof(struct ft5406), GFP_KERNEL);

	if (!ts || !input_dev) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "Failed to allocate memory\n");
		return ret;
	}
	ts->input_dev = input_dev;
	platform_set_drvdata(pdev, ts);
	ts->pdev = pdev;
	
	input_dev->name = "FT5406 memory based driver";
	
	__set_bit(EV_KEY, input_dev->evbit);
	__set_bit(EV_SYN, input_dev->evbit);
	__set_bit(EV_ABS, input_dev->evbit);

	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0,
			     SCREEN_WIDTH, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0,
			     SCREEN_HEIGHT, 0, 0);

	input_mt_init_slots(input_dev, MAXIMUM_SUPPORTED_POINTS, INPUT_MT_DIRECT);

	input_set_drvdata(input_dev, ts);
	
	ret = input_register_device(input_dev);
	if (ret) {
		dev_err(&pdev->dev, "could not register input device, %d\n",
			ret);
		return ret;
	}
	
	memset(&request, 0, sizeof request);

	request.msg_size = sizeof request;
	request.request_code = VCMSG_PROCESS_REQUEST;
	request.tag.tag_id = VCMSG_GET_TOUCHBUF;
	request.tag.buffer_size = 4;
	request.tag.data_size = 4;
	
	bcm_mailbox_property(&request, sizeof(request));
	
	if(request.request_code == VCMSG_REQUEST_SUCCESSFUL)
	{
		dev_dbg(&pdev->dev, "Got TS buffer 0x%x\n", request.tag.val);
	}
	else
	{
		input_unregister_device(input_dev);
		kzfree(ts);
		return -1;
	}
	
	// mmap the physical memory
	request.tag.val &= ~0xc0000000;
	ts->ts_base = ioremap(request.tag.val, sizeof(*ts->regs));
	if(ts->ts_base == NULL)
	{
		dev_err(&pdev->dev, "Failed to map physical address\n");
		input_unregister_device(input_dev);
		kzfree(ts);
		return -1;	
	}
	
	ts->regs = (struct ft5406_regs *) ts->ts_base;

	// create thread to poll the touch events
	ts->thread = kthread_run(ft5406_thread, ts, "ft5406");
	if(ts->thread == NULL)
	{
		dev_err(&pdev->dev, "Failed to create kernel thread");
		iounmap(ts->ts_base);
		input_unregister_device(input_dev);
		kzfree(ts);
	}

	return 0;
}

static int ft5406_remove(struct platform_device *pdev)
{
	struct ft5406 *ts = (struct ft5406 *) platform_get_drvdata(pdev);
	
	dev_info(&pdev->dev, "Removing rpi-ft5406\n");
	
	kthread_stop(ts->thread);
	iounmap(ts->ts_base);
	input_unregister_device(ts->input_dev);
	kzfree(ts);
	
	return 0;
}

static const struct of_device_id ft5406_match[] = {
	{ .compatible = "rpi,rpi-ft5406", },
	{},
};
MODULE_DEVICE_TABLE(of, ft5406_match);

static struct platform_driver ft5406_driver = {
	.driver = {
		.name   = "rpi-ft5406",
		.owner  = THIS_MODULE,
		.of_match_table = ft5406_match,
	},
	.probe          = ft5406_probe,
	.remove         = ft5406_remove,
};

module_platform_driver(ft5406_driver);

MODULE_AUTHOR("Gordon Hollingworth");
MODULE_DESCRIPTION("Touchscreen driver for memory based FT5406");
MODULE_LICENSE("GPL");
