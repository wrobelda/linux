// SPDX-License-Identifier: GPL-2.0-only
/*
 * HID driver for the Xiaomi Pad pogo keyboard bridge
 *
 * Copyright (C) 2026 Dawid Wrobel
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "hid-ids.h"

#define XIAOMI_POGO_REPORT_STATUS	0x26
#define XIAOMI_POGO_REPORT_STATUS_REPLY	0x24
#define XIAOMI_POGO_REPORT_SIZE		64
#define XIAOMI_POGO_REPORT_QUERY		0x4e
#define XIAOMI_POGO_QUERY_SIZE		32
#define XIAOMI_POGO_QUERY_CHECKSUM_OFFSET 7
#define XIAOMI_POGO_CHECKSUM_OFFSET	19
#define XIAOMI_POGO_PRESENCE_OFFSET	9
#define XIAOMI_POGO_FIELD_KEYBOARD	0x38
#define XIAOMI_POGO_FIELD_HOST		0x80
#define XIAOMI_POGO_COMMAND_STATUS	0xa2
#define XIAOMI_POGO_COMMAND_QUERY	0xa1
#define XIAOMI_POGO_STATUS_CONNECTED	BIT(0)
#define XIAOMI_POGO_CONTROL_INTERFACE	2
#define XIAOMI_POGO_EVENT_INTERFACE	3

struct xiaomi_pogo {
	struct input_dev *input;
};

static u8 xiaomi_pogo_interface(struct hid_device *hdev)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

	return intf->cur_altsetting->desc.bInterfaceNumber;
}

static struct hid_device *
xiaomi_pogo_sibling(struct hid_device *hdev, unsigned int interface)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	struct usb_device *udev = interface_to_usbdev(intf);

	intf = usb_ifnum_to_if(udev, interface);
	if (!intf)
		return NULL;

	return usb_get_intfdata(intf);
}

static struct xiaomi_pogo *xiaomi_pogo_event_data(struct hid_device *hdev)
{
	struct hid_device *event_hdev;

	event_hdev = xiaomi_pogo_sibling(hdev, XIAOMI_POGO_EVENT_INTERFACE);
	if (!event_hdev)
		return NULL;

	return hid_get_drvdata(event_hdev);
}

static bool xiaomi_pogo_valid_report(const u8 *data, int size)
{
	u8 checksum = 0;
	int i;

	if (size != XIAOMI_POGO_REPORT_SIZE ||
	    (data[0] != XIAOMI_POGO_REPORT_STATUS &&
	     data[0] != XIAOMI_POGO_REPORT_STATUS_REPLY) ||
	    data[2] != XIAOMI_POGO_FIELD_KEYBOARD ||
	    data[3] != XIAOMI_POGO_FIELD_HOST ||
	    data[4] != XIAOMI_POGO_COMMAND_STATUS)
		return false;

	for (i = 0; i < XIAOMI_POGO_CHECKSUM_OFFSET; i++)
		checksum += data[i];

	return checksum == data[XIAOMI_POGO_CHECKSUM_OFFSET];
}

static int xiaomi_pogo_raw_event(struct hid_device *hdev,
				 struct hid_report *report, u8 *data, int size)
{
	struct xiaomi_pogo *pogo = xiaomi_pogo_event_data(hdev);

	if (!pogo || !xiaomi_pogo_valid_report(data, size))
		return 0;

	input_report_switch(pogo->input, SW_TABLET_MODE,
			    !(data[XIAOMI_POGO_PRESENCE_OFFSET] &
			      XIAOMI_POGO_STATUS_CONNECTED));
	input_sync(pogo->input);

	return 0;
}

static void xiaomi_pogo_query_status(struct hid_device *hdev)
{
	static const u8 query_template[XIAOMI_POGO_QUERY_SIZE] = {
		XIAOMI_POGO_REPORT_QUERY, 0x31,
		XIAOMI_POGO_FIELD_HOST, XIAOMI_POGO_FIELD_KEYBOARD,
		XIAOMI_POGO_COMMAND_QUERY, 0x01, 0x01,
	};
	struct hid_device *control_hdev;
	u8 *query;
	unsigned int i;
	int ret;

	control_hdev = xiaomi_pogo_sibling(hdev,
					   XIAOMI_POGO_CONTROL_INTERFACE);
	if (!control_hdev || !hid_get_drvdata(control_hdev))
		return;

	query = kmemdup(query_template, sizeof(query_template), GFP_KERNEL);
	if (!query)
		return;

	for (i = 0; i < XIAOMI_POGO_QUERY_CHECKSUM_OFFSET; i++)
		query[XIAOMI_POGO_QUERY_CHECKSUM_OFFSET] += query[i];

	ret = hid_hw_output_report(control_hdev, query, sizeof(query_template));
	kfree(query);
	if (ret < 0)
		hid_warn(control_hdev, "failed to query keyboard status: %d\n",
			 ret);
}

static int xiaomi_pogo_probe(struct hid_device *hdev,
			     const struct hid_device_id *id)
{
	struct xiaomi_pogo *pogo;
	u8 interface;
	int ret;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	interface = xiaomi_pogo_interface(hdev);

	pogo = devm_kzalloc(&hdev->dev, sizeof(*pogo), GFP_KERNEL);
	if (!pogo)
		return -ENOMEM;

	hid_set_drvdata(hdev, pogo);

	if (interface == XIAOMI_POGO_EVENT_INTERFACE) {
		pogo->input = devm_input_allocate_device(&hdev->dev);
		if (!pogo->input)
			return -ENOMEM;

		pogo->input->name = "Xiaomi Pogo Keyboard Tablet Mode Switch";
		pogo->input->phys = hdev->phys;
		pogo->input->id.bustype = hdev->bus;
		pogo->input->id.vendor = hdev->vendor;
		pogo->input->id.product = hdev->product;
		pogo->input->id.version = hdev->version;
		input_set_capability(pogo->input, EV_SW, SW_TABLET_MODE);

		ret = input_register_device(pogo->input);
		if (ret)
			return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}

	if (interface == XIAOMI_POGO_EVENT_INTERFACE)
		xiaomi_pogo_query_status(hdev);

	return 0;
}

static void xiaomi_pogo_remove(struct hid_device *hdev)
{
	if (hid_get_drvdata(hdev))
		hid_hw_close(hdev);
	hid_hw_stop(hdev);
	hid_set_drvdata(hdev, NULL);
}

static const struct hid_device_id xiaomi_pogo_devices[] = {
	{ HID_DEVICE(BUS_USB, HID_GROUP_XIAOMI_POGO, USB_VENDOR_ID_XIAOMI_POGO,
		     USB_DEVICE_ID_XIAOMI_POGO_BRIDGE) },
	{ }
};
MODULE_DEVICE_TABLE(hid, xiaomi_pogo_devices);

static struct hid_driver xiaomi_pogo_driver = {
	.name = "xiaomi-pogo",
	.id_table = xiaomi_pogo_devices,
	.probe = xiaomi_pogo_probe,
	.remove = xiaomi_pogo_remove,
	.raw_event = xiaomi_pogo_raw_event,
};
module_hid_driver(xiaomi_pogo_driver);

MODULE_AUTHOR("Dawid Wrobel");
MODULE_DESCRIPTION("Xiaomi Pad pogo keyboard bridge driver");
MODULE_LICENSE("GPL");
