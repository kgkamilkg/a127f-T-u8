/*
 * f_ncm.c -- USB CDC Network (NCM) link function driver
 *
 * Copyright (C) 2010 Nokia Corporation
 * Contact: Yauheni Kaliuta <yauheni.kaliuta@nokia.com>
 *
 * The driver borrows from f_ecm.c which is:
 *
 * Copyright (C) 2003-2005,2008 David Brownell
 * Copyright (C) 2008 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/crc32.h>

#include <linux/usb/cdc.h>
#include <linux/miscdevice.h>
#include "u_ether.h"
#include "u_ether_configfs.h"
#include "u_ncm.h"

/*
 * This function is a "CDC Network Control Model" (CDC NCM) Ethernet link.
 * NCM is intended to be used with high-speed network attachments.
 *
 * Note that NCM requires the use of "alternate settings" for its data
 * interface.  This means that the set_alt() method has real work to do,
 * and also means that a get_alt() method is required.
 */

/* to trigger crc/non-crc ndp signature */

#define NCM_NDP_HDR_CRC_MASK	0x01000000
#define NCM_NDP_HDR_CRC		0x01000000
#define NCM_NDP_HDR_NOCRC	0x00000000

enum ncm_notify_state {
	NCM_NOTIFY_NONE,		/* don't notify */
	NCM_NOTIFY_CONNECT,		/* issue CONNECT next */
	NCM_NOTIFY_SPEED,		/* issue SPEED_CHANGE next */
};

struct f_ncm {
	struct gether			port;
	u8				ctrl_id, data_id;

	char				ethaddr[14];

	struct usb_ep			*notify;
	struct usb_request		*notify_req;
	u8				notify_state;
	atomic_t			notify_count;
	bool				is_open;

	const struct ndp_parser_opts	*parser_opts;
	bool				is_crc;
	u32				ndp_sign;
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
	uint16_t	dgramsize;
	struct net_device *net;
#endif
	/*
	 * for notification, it is accessed from both
	 * callback and ethernet open/close
	 */
	spinlock_t			lock;
};

static inline struct f_ncm *func_to_ncm(struct usb_function *f)
{
	return container_of(f, struct f_ncm, port.func);
}

/* peak (theoretical) bulk transfer rate in bits-per-second */
static inline unsigned ncm_bitrate(struct usb_gadget *g)
{
	if (gadget_is_superspeed(g) && g->speed >= USB_SPEED_SUPER)
		return 13 * 1024 * 8 * 1000 * 8;
	else if (gadget_is_dualspeed(g) && g->speed == USB_SPEED_HIGH)
		return 13 * 512 * 8 * 1000 * 8;
	else
		return 19 *  64 * 1 * 1000 * 8;
}

/*-------------------------------------------------------------------------*/

/*
 * We cannot group frames so use just the minimal size which ok to put
 * one max-size ethernet frame.
 * If the host can group frames, allow it to do that, 16K is selected,
 * because it's used by default by the current linux host driver
 */
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
#define NTB_DEFAULT_IN_SIZE	(16384 * 4)
#define NCM_MAX_DGRAM_SIZE	(16384 * 4 - 1 - NCM_HEADER_SIZE - 1)
#define MAX_NDP_DATAGRAMS	1
#define NTH_NDP_OUT_TOTAL_SIZE	\
		(ALIGN(sizeof(struct usb_cdc_ncm_nth16),	\
		ntb_parameters.wNdpOutAlignment) +	\
		sizeof(struct usb_cdc_ncm_ndp16) +	\
		((MAX_NDP_DATAGRAMS)*sizeof(struct usb_cdc_ncm_dpe16)))
#else
#define NTB_DEFAULT_IN_SIZE	USB_CDC_NCM_NTB_MIN_IN_SIZE
#endif
#define NTB_OUT_SIZE		(16384 * 4)

/*
 * skbs of size less than that will not be aligned
 * to NCM's dwNtbInMaxSize to save bus bandwidth
 */

#define	MAX_TX_NONFIXED		(512 * 3)

#define FORMATS_SUPPORTED	(USB_CDC_NCM_NTB16_SUPPORTED |	\
				 USB_CDC_NCM_NTB32_SUPPORTED)

static struct usb_cdc_ncm_ntb_parameters ntb_parameters = {
	.wLength = cpu_to_le16(sizeof(ntb_parameters)),
	.bmNtbFormatsSupported = cpu_to_le16(FORMATS_SUPPORTED),
	.dwNtbInMaxSize = cpu_to_le32(NTB_DEFAULT_IN_SIZE),
	.wNdpInDivisor = cpu_to_le16(4),
	.wNdpInPayloadRemainder = cpu_to_le16(0),
	.wNdpInAlignment = cpu_to_le16(4),

	.dwNtbOutMaxSize = cpu_to_le32(NTB_OUT_SIZE),
	.wNdpOutDivisor = cpu_to_le16(4),
	.wNdpOutPayloadRemainder = cpu_to_le16(0),
	.wNdpOutAlignment = cpu_to_le16(4),
};

/*
 * Use wMaxPacketSize big enough to fit CDC_NOTIFY_SPEED_CHANGE in one
 * packet, to simplify cancellation; and a big transfer interval, to
 * waste less bandwidth.
 */

#define NCM_STATUS_INTERVAL_MS		32
#define NCM_STATUS_BYTECOUNT		16	/* 8 byte header + data */

static struct usb_interface_assoc_descriptor ncm_iad_desc  = {
	.bLength =		sizeof ncm_iad_desc,
	.bDescriptorType =	USB_DT_INTERFACE_ASSOCIATION,

	/* .bFirstInterface =	DYNAMIC, */
	.bInterfaceCount =	2,	/* control + data */
	.bFunctionClass =	USB_CLASS_COMM,
	.bFunctionSubClass =	USB_CDC_SUBCLASS_NCM,
	.bFunctionProtocol =	USB_CDC_PROTO_NONE,
	/* .iFunction =		DYNAMIC */
};

/* interface descriptor: */

static struct usb_interface_descriptor ncm_control_intf  = {
	.bLength =		sizeof ncm_control_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	/* .bInterfaceNumber = DYNAMIC */
	.bNumEndpoints =	1,
	.bInterfaceClass =	USB_CLASS_COMM,
	.bInterfaceSubClass =	USB_CDC_SUBCLASS_NCM,
	.bInterfaceProtocol =	USB_CDC_PROTO_NONE,
	/* .iInterface = DYNAMIC */
};

static struct usb_cdc_header_desc ncm_header_desc  = {
	.bLength =		sizeof ncm_header_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_HEADER_TYPE,

	.bcdCDC =		cpu_to_le16(0x0110),
};

static struct usb_cdc_union_desc ncm_union_desc  = {
	.bLength =		sizeof(ncm_union_desc),
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_UNION_TYPE,
	/* .bMasterInterface0 =	DYNAMIC */
	/* .bSlaveInterface0 =	DYNAMIC */
};

static struct usb_cdc_ether_desc ecm_desc  = {
	.bLength =		sizeof ecm_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_ETHERNET_TYPE,

	/* this descriptor actually adds value, surprise! */
	/* .iMACAddress = DYNAMIC */
	.bmEthernetStatistics =	cpu_to_le32(0), /* no statistics */
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
	.wMaxSegmentSize =	cpu_to_le16(NCM_MAX_DGRAM_SIZE),
#else
	.wMaxSegmentSize =	cpu_to_le16(ETH_FRAME_LEN),
#endif
	.wNumberMCFilters =	cpu_to_le16(0),
	.bNumberPowerFilters =	0,
};
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
#define _NCAPS	(USB_CDC_NCM_NCAP_ETH_FILTER | USB_CDC_NCM_NCAP_MAX_DATAGRAM_SIZE)
#else
#define _NCAPS	(USB_CDC_NCM_NCAP_ETH_FILTER | USB_CDC_NCM_NCAP_CRC_MODE)
#endif
static struct usb_cdc_ncm_desc ncm_desc  = {
	.bLength =		sizeof ncm_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_NCM_TYPE,

	.bcdNcmVersion =	cpu_to_le16(0x0100),
	/* can process SetEthernetPacketFilter */
	.bmNetworkCapabilities = _NCAPS,
};

/* the default data interface has no endpoints ... */

static struct usb_interface_descriptor ncm_data_nop_intf  = {
	.bLength =		sizeof ncm_data_nop_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	.bInterfaceNumber =	1,
	.bAlternateSetting =	0,
	.bNumEndpoints =	0,
	.bInterfaceClass =	USB_CLASS_CDC_DATA,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	USB_CDC_NCM_PROTO_NTB,
	/* .iInterface = DYNAMIC */
};

/* ... but the "real" data interface has two bulk endpoints */

static struct usb_interface_descriptor ncm_data_intf  = {
	.bLength =		sizeof ncm_data_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	.bInterfaceNumber =	1,
	.bAlternateSetting =	1,
	.bNumEndpoints =	2,
	.bInterfaceClass =	USB_CLASS_CDC_DATA,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	USB_CDC_NCM_PROTO_NTB,
	/* .iInterface = DYNAMIC */
};

/* full speed support: */

static struct usb_endpoint_descriptor fs_ncm_notify_desc  = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	cpu_to_le16(NCM_STATUS_BYTECOUNT),
	.bInterval =		NCM_STATUS_INTERVAL_MS,
};

static struct usb_endpoint_descriptor fs_ncm_in_desc  = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor fs_ncm_out_desc  = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

static struct usb_descriptor_header *ncm_fs_function[]  = {
	(struct usb_descriptor_header *) &ncm_iad_desc,
	/* CDC NCM control descriptors */
	(struct usb_descriptor_header *) &ncm_control_intf,
	(struct usb_descriptor_header *) &ncm_header_desc,
	(struct usb_descriptor_header *) &ncm_union_desc,
	(struct usb_descriptor_header *) &ecm_desc,
	(struct usb_descriptor_header *) &ncm_desc,
	(struct usb_descriptor_header *) &fs_ncm_notify_desc,
	/* data interface, altsettings 0 and 1 */
	(struct usb_descriptor_header *) &ncm_data_nop_intf,
	(struct usb_descriptor_header *) &ncm_data_intf,
	(struct usb_descriptor_header *) &fs_ncm_in_desc,
	(struct usb_descriptor_header *) &fs_ncm_out_desc,
	NULL,
};

/* high speed support: */

static struct usb_endpoint_descriptor hs_ncm_notify_desc  = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	cpu_to_le16(NCM_STATUS_BYTECOUNT),
	.bInterval =		USB_MS_TO_HS_INTERVAL(NCM_STATUS_INTERVAL_MS),
};
static struct usb_endpoint_descriptor hs_ncm_in_desc  = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor hs_ncm_out_desc  = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(512),
};

static struct usb_descriptor_header *ncm_hs_function[]  = {
	(struct usb_descriptor_header *) &ncm_iad_desc,
	/* CDC NCM control descriptors */
	(struct usb_descriptor_header *) &ncm_control_intf,
	(struct usb_descriptor_header *) &ncm_header_desc,
	(struct usb_descriptor_header *) &ncm_union_desc,
	(struct usb_descriptor_header *) &ecm_desc,
	(struct usb_descriptor_header *) &ncm_desc,
	(struct usb_descriptor_header *) &hs_ncm_notify_desc,
	/* data interface, altsettings 0 and 1 */
	(struct usb_descriptor_header *) &ncm_data_nop_intf,
	(struct usb_descriptor_header *) &ncm_data_intf,
	(struct usb_descriptor_header *) &hs_ncm_in_desc,
	(struct usb_descriptor_header *) &hs_ncm_out_desc,
	NULL,
};

/* super speed support: */

static struct usb_endpoint_descriptor ss_ncm_notify_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	cpu_to_le16(NCM_STATUS_BYTECOUNT),
	.bInterval =		USB_MS_TO_HS_INTERVAL(NCM_STATUS_INTERVAL_MS)
};

static struct usb_ss_ep_comp_descriptor ss_ncm_notify_comp_desc = {
	.bLength =		sizeof(ss_ncm_notify_comp_desc),
	.bDescriptorType =	USB_DT_SS_ENDPOINT_COMP,

	/* the following 3 values can be tweaked if necessary */
	/* .bMaxBurst =		0, */
	/* .bmAttributes =	0, */
	.wBytesPerInterval =	cpu_to_le16(NCM_STATUS_BYTECOUNT),
};

static struct usb_endpoint_descriptor ss_ncm_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor ss_ncm_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor ss_ncm_bulk_comp_desc = {
	.bLength =		sizeof(ss_ncm_bulk_comp_desc),
	.bDescriptorType =	USB_DT_SS_ENDPOINT_COMP,

	/* the following 2 values can be tweaked if necessary */
	.bMaxBurst =		4,
	/* .bmAttributes =	0, */
};

static struct usb_descriptor_header *ncm_ss_function[] = {
	(struct usb_descriptor_header *) &ncm_iad_desc,
	/* CDC NCM control descriptors */
	(struct usb_descriptor_header *) &ncm_control_intf,
	(struct usb_descriptor_header *) &ncm_header_desc,
	(struct usb_descriptor_header *) &ncm_union_desc,
	(struct usb_descriptor_header *) &ecm_desc,
	(struct usb_descriptor_header *) &ncm_desc,
	(struct usb_descriptor_header *) &ss_ncm_notify_desc,
	(struct usb_descriptor_header *) &ss_ncm_notify_comp_desc,
	/* data interface, altsettings 0 and 1 */
	(struct usb_descriptor_header *) &ncm_data_nop_intf,
	(struct usb_descriptor_header *) &ncm_data_intf,
	(struct usb_descriptor_header *) &ss_ncm_in_desc,
	(struct usb_descriptor_header *) &ss_ncm_bulk_comp_desc,
	(struct usb_descriptor_header *) &ss_ncm_out_desc,
	(struct usb_descriptor_header *) &ss_ncm_bulk_comp_desc,
	NULL,
};
/* string descriptors: */

#define STRING_CTRL_IDX	0
#define STRING_MAC_IDX	1
#define STRING_DATA_IDX	2
#define STRING_IAD_IDX	3

static struct usb_string ncm_string_defs[] = {
	[STRING_CTRL_IDX].s = "CDC Network Control Model (NCM)",
	[STRING_MAC_IDX].s = "",
	[STRING_DATA_IDX].s = "CDC Network Data",
	[STRING_IAD_IDX].s = "CDC NCM",
	{  } /* end of list */
};

static struct usb_gadget_strings ncm_string_table = {
	.language =		0x0409,	/* en-us */
	.strings =		ncm_string_defs,
};

static struct usb_gadget_strings *ncm_strings[] = {
	&ncm_string_table,
	NULL,
};

/*
 * Here are options for NCM Datagram Pointer table (NDP) parser.
 * There are 2 different formats: NDP16 and NDP32 in the spec (ch. 3),
 * in NDP16 offsets and sizes fields are 1 16bit word wide,
 * in NDP32 -- 2 16bit words wide. Also signatures are different.
 * To make the parser code the same, put the differences in the structure,
 * and switch pointers to the structures when the format is changed.
 */

struct ndp_parser_opts {
	u32		nth_sign;
	u32		ndp_sign;
	unsigned	nth_size;
	unsigned	ndp_size;
	unsigned	ndplen_align;
	/* sizes in u16 units */
	unsigned	dgram_item_len; /* index or length */
	unsigned	block_length;
	unsigned	fp_index;
	unsigned	reserved1;
	unsigned	reserved2;
	unsigned	next_fp_index;
};

#define INIT_NDP16_OPTS {					\
		.nth_sign = USB_CDC_NCM_NTH16_SIGN,		\
		.ndp_sign = USB_CDC_NCM_NDP16_NOCRC_SIGN,	\
		.nth_size = sizeof(struct usb_cdc_ncm_nth16),	\
		.ndp_size = sizeof(struct usb_cdc_ncm_ndp16),	\
		.ndplen_align = 4,				\
		.dgram_item_len = 1,				\
		.block_length = 1,				\
		.fp_index = 1,					\
		.reserved1 = 0,					\
		.reserved2 = 0,					\
		.next_fp_index = 1,				\
	}


#define INIT_NDP32_OPTS {					\
		.nth_sign = USB_CDC_NCM_NTH32_SIGN,		\
		.ndp_sign = USB_CDC_NCM_NDP32_NOCRC_SIGN,	\
		.nth_size = sizeof(struct usb_cdc_ncm_nth32),	\
		.ndp_size = sizeof(struct usb_cdc_ncm_ndp32),	\
		.ndplen_align = 8,				\
		.dgram_item_len = 2,				\
		.block_length = 2,				\
		.fp_index = 2,					\
		.reserved1 = 1,					\
		.reserved2 = 2,					\
		.next_fp_index = 2,				\
	}

static const struct ndp_parser_opts ndp16_opts = INIT_NDP16_OPTS;
static const struct ndp_parser_opts ndp32_opts = INIT_NDP32_OPTS;

static inline void put_ncm(__le16 **p, unsigned size, unsigned val)
{
	switch (size) {
	case 1:
		put_unaligned_le16((u16)val, *p);
		break;
	case 2:
		put_unaligned_le32((u32)val, *p);

		break;
	default:
		BUG();
	}

	*p += size;
}

static inline unsigned get_ncm(__le16 **p, unsigned size)
{
	unsigned tmp;

	switch (size) {
	case 1:
		tmp = get_unaligned_le16(*p);
		break;
	case 2:
		tmp = get_unaligned_le32(*p);
		break;
	default:
		BUG();
	}

	*p += size;
	return tmp;
}

#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
struct ncm_dev {
	struct work_struct work;
};


static const char mirrorlink_shortname[] = "usb_ncm";
/* Create misc driver for Mirror Link cmd */
static struct miscdevice mirrorlink_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = mirrorlink_shortname,
};

static bool ncm_connect;

/* terminal version using vendor specific request */
u16 terminal_mode_version;
u16 terminal_mode_vendor_id;

static struct ncm_dev *_ncm_dev;
#endif
/*-------------------------------------------------------------------------*/

static inline void ncm_reset_values(struct f_ncm *ncm)
{
	ncm->parser_opts = &ndp16_opts;
	ncm->is_crc = false;
	ncm->port.cdc_filter = DEFAULT_FILTER;
	/* doesn't make sense for ncm, fixed size used */
	ncm->port.header_len = 0;
	ncm->port.fixed_out_len = le32_to_cpu(ntb_parameters.dwNtbOutMaxSize);
	ncm->port.fixed_in_len = NTB_DEFAULT_IN_SIZE;

	/* ncm->ndp_sign must be initialized  */
	ncm->ndp_sign = ncm->parser_opts->ndp_sign;
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
	/* Revisit issue for the case of Toyota Head Unit */
	ncm->dgramsize = NCM_MAX_DGRAM_SIZE;	//ETH_FRAME_LEN
	ncm->net = NULL;
#endif
}

/*
 * Context: ncm->lock held
 */
static void ncm_do_notify(struct f_ncm *ncm)
{
	struct usb_request		*req = ncm->notify_req;
	struct usb_cdc_notification	*event;
	struct usb_composite_dev	*cdev = ncm->port.func.config->cdev;
	__le32				*data;
	int				status;

	/* notification already in flight? */
	if (atomic_read(&ncm->notify_count))
		return;

	event = req->buf;
	switch (ncm->notify_state) {
	case NCM_NOTIFY_NONE:
		return;

	case NCM_NOTIFY_CONNECT:
		event->bNotificationType = USB_CDC_NOTIFY_NETWORK_CONNECTION;
		if (ncm->is_open)
			event->wValue = cpu_to_le16(1);
		else
			event->wValue = cpu_to_le16(0);
		event->wLength = 0;
		req->length = sizeof *event;

		DBG(cdev, "notify connect %s\n",
				ncm->is_open ? "true" : "false");
		ncm->notify_state = NCM_NOTIFY_NONE;
		break;

	case NCM_NOTIFY_SPEED:
		event->bNotificationType = USB_CDC_NOTIFY_SPEED_CHANGE;
		event->wValue = cpu_to_le16(0);
		event->wLength = cpu_to_le16(8);
		req->length = NCM_STATUS_BYTECOUNT;

		/* SPEED_CHANGE data is up/down speeds in bits/sec */
		data = req->buf + sizeof *event;
		data[0] = cpu_to_le32(ncm_bitrate(cdev->gadget));
		data[1] = data[0];

		DBG(cdev, "notify speed %d\n", ncm_bitrate(cdev->gadget));
		ncm->notify_state = NCM_NOTIFY_CONNECT;
		break;
	}
	event->bmRequestType = 0xA1;
	event->wIndex = cpu_to_le16(ncm->ctrl_id);

	atomic_inc(&ncm->notify_count);

	/*
	 * In double buffering if there is a space in FIFO,
	 * completion callback can be called right after the call,
	 * so unlocking
	 */
	spin_unlock(&ncm->lock);
	status = usb_ep_queue(ncm->notify, req, GFP_ATOMIC);
	spin_lock(&ncm->lock);
	if (status < 0) {
		atomic_dec(&ncm->notify_count);
		DBG(cdev, "notify --> %d\n", status);
	}
}

/*
 * Context: ncm->lock held
 */
static void ncm_notify(struct f_ncm *ncm)
{
	/*
	 * NOTE on most versions of Linux, host side cdc-ethernet
	 * won't listen for notifications until its netdevice opens.
	 * The first notification then sits in the FIFO for a long
	 * time, and the second one is queued.
	 *
	 * If ncm_notify() is called before the second (CONNECT)
	 * notification is sent, then it will reset to send the SPEED
	 * notificaion again (and again, and again), but it's not a problem
	 */
	ncm->notify_state = NCM_NOTIFY_SPEED;
	ncm_do_notify(ncm);
}

static void ncm_notify_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_ncm			*ncm = req->context;
	struct usb_composite_dev	*cdev = ncm->port.func.config->cdev;
	struct usb_cdc_notification	*event = req->buf;

	spin_lock(&ncm->lock);
	switch (req->status) {
	case 0:
		VDBG(cdev, "Notification %02x sent\n",
		     event->bNotificationType);
		atomic_dec(&ncm->notify_count);
		break;
	case -ECONNRESET:
	case -ESHUTDOWN:
		atomic_set(&ncm->notify_count, 0);
		ncm->notify_state = NCM_NOTIFY_NONE;
		break;
	default:
		DBG(cdev, "event %02x --> %d\n",
			event->bNotificationType, req->status);
		atomic_dec(&ncm->notify_count);
		break;
	}
	ncm_do_notify(ncm);
	spin_unlock(&ncm->lock);
}

static void ncm_ep0out_complete(struct usb_ep *ep, struct usb_request *req)
{
	/* now for SET_NTB_INPUT_SIZE only */
	unsigned		in_size;
	struct usb_function	*f = req->context;
	struct f_ncm		*ncm = func_to_ncm(f);
	struct usb_composite_dev *cdev = ep->driver_data;

	req->context = NULL;
	if (req->status || req->actual != req->length) {
		DBG(cdev, "Bad control-OUT transfer\n");
		goto invalid;
	}

	in_size = get_unaligned_le32(req->buf);
	if (in_size < USB_CDC_NCM_NTB_MIN_IN_SIZE ||
	    in_size > le32_to_cpu(ntb_parameters.dwNtbInMaxSize)) {
		DBG(cdev, "Got wrong INPUT SIZE (%d) from host\n", in_size);
		goto invalid;
	}

	ncm->port.fixed_in_len = in_size;
	VDBG(cdev, "Set NTB INPUT SIZE %d\n", in_size);
	return;

invalid:
	usb_ep_set_halt(ep);
	return;
}
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
static void ncm_setdgram_complete(struct usb_ep *ep, struct usb_request *req)
{
	/* now for SET_MAX_DATAGRAM_SIZE only */
	unsigned		dgram_size;
	struct usb_function	*f = req->context;
	struct f_ncm		*ncm = func_to_ncm(f);
	int ntb_min_size = min(ntb_parameters.dwNtbOutMaxSize,
				ntb_parameters.dwNtbInMaxSize);
		req->context = NULL;
	if (req->status || req->actual != req->length) {
		printk(KERN_ERR"usb:%s * Bad control-OUT transfer *\n", __func__);
		goto invalid;
	}

	dgram_size = get_unaligned_le16(req->buf);
	if (dgram_size < ETH_FRAME_LEN ||
	    dgram_size > NCM_MAX_DGRAM_SIZE) {
		printk(KERN_ERR"usb:%s * Got wrong MTU SIZE (%d) from host *\n", __func__, dgram_size);
		goto invalid;
	}

	if (dgram_size + NTH_NDP_OUT_TOTAL_SIZE > ntb_min_size) {
		printk(KERN_ERR"usb:%s * MTU(%d) SIZE is larger than NTB SIZE (%d + %d) from host * \n",
			__func__, ntb_min_size, dgram_size, NTH_NDP_OUT_TOTAL_SIZE);
		printk(KERN_ERR"*************************************************\n");
		goto invalid;
	}

	ncm->dgramsize = dgram_size;

	if (ncm->net)
		ncm->net->mtu = NCM_MTU_SIZE; //ncm->dgramsize - ETH_HLEN;

	printk(KERN_ERR"usb:%s * Set MTU SIZE %d *\n", __func__, dgram_size);

	return;

invalid:
	usb_ep_set_halt(ep);
	return;
}
#endif

static int ncm_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct f_ncm		*ncm = func_to_ncm(f);
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_request	*req = cdev->req;
	int			value = -EOPNOTSUPP;
	u16			w_index = le16_to_cpu(ctrl->wIndex);
	u16			w_value = le16_to_cpu(ctrl->wValue);
	u16			w_length = le16_to_cpu(ctrl->wLength);

	/*
	 * composite driver infrastructure handles everything except
	 * CDC class messages; interface activation uses set_alt().
	 */
	switch ((ctrl->bRequestType << 8) | ctrl->bRequest) {
	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
			| USB_CDC_SET_ETHERNET_PACKET_FILTER:
		/*
		 * see 6.2.30: no data, wIndex = interface,
		 * wValue = packet filter bitmap
		 */
		if (w_length != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		DBG(cdev, "packet filter %02x\n", w_value);
		/*
		 * REVISIT locking of cdc_filter.  This assumes the UDC
		 * driver won't have a concurrent packet TX irq running on
		 * another CPU; or that if it does, this write is atomic...
		 */
		ncm->port.cdc_filter = w_value;
		value = 0;
		break;
	/*
	 * and optionally:
	 * case USB_CDC_SEND_ENCAPSULATED_COMMAND:
	 * case USB_CDC_GET_ENCAPSULATED_RESPONSE:
	 * case USB_CDC_SET_ETHERNET_MULTICAST_FILTERS:
	 * case USB_CDC_SET_ETHERNET_PM_PATTERN_FILTER:
	 * case USB_CDC_GET_ETHERNET_PM_PATTERN_FILTER:
	 * case USB_CDC_GET_ETHERNET_STATISTIC:
	 */

	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
		| USB_CDC_GET_NTB_PARAMETERS:

		if (w_length == 0 || w_value != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		value = w_length > sizeof ntb_parameters ?
			sizeof ntb_parameters : w_length;
		memcpy(req->buf, &ntb_parameters, value);
		VDBG(cdev, "Host asked NTB parameters\n");
		break;

	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
		| USB_CDC_GET_NTB_INPUT_SIZE:

		if (w_length < 4 || w_value != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		put_unaligned_le32(ncm->port.fixed_in_len, req->buf);
		value = 4;
		VDBG(cdev, "Host asked INPUT SIZE, sending %d\n",
		     ncm->port.fixed_in_len);
		break;

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
		| USB_CDC_SET_NTB_INPUT_SIZE:
	{
		if (w_length != 4 || w_value != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		req->complete = ncm_ep0out_complete;
		req->length = w_length;
		req->context = f;

		value = req->length;
		break;
	}

	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
		| USB_CDC_GET_NTB_FORMAT:
	{
		uint16_t format;

		if (w_length < 2 || w_value != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		format = (ncm->parser_opts == &ndp16_opts) ? 0x0000 : 0x0001;
		put_unaligned_le16(format, req->buf);
		value = 2;
		VDBG(cdev, "Host asked NTB FORMAT, sending %d\n", format);
		break;
	}

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
		| USB_CDC_SET_NTB_FORMAT:
	{
		if (w_length != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		switch (w_value) {
		case 0x0000:
			ncm->parser_opts = &ndp16_opts;
			DBG(cdev, "NCM16 selected\n");
			break;
		case 0x0001:
			ncm->parser_opts = &ndp32_opts;
			DBG(cdev, "NCM32 selected\n");
			break;
		default:
			goto invalid;
		}
		value = 0;
		break;
	}
	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
		| USB_CDC_GET_CRC_MODE:
	{
		uint16_t is_crc;

		if (w_length < 2 || w_value != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		is_crc = ncm->is_crc ? 0x0001 : 0x0000;
		put_unaligned_le16(is_crc, req->buf);
		value = 2;
		VDBG(cdev, "Host asked CRC MODE, sending %d\n", is_crc);
		break;
	}

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
		| USB_CDC_SET_CRC_MODE:
	{
		int ndp_hdr_crc = 0;

		if (w_length != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		switch (w_value) {
		case 0x0000:
			ncm->is_crc = false;
			ndp_hdr_crc = NCM_NDP_HDR_NOCRC;
			DBG(cdev, "non-CRC mode selected\n");
			break;
		case 0x0001:
			ncm->is_crc = true;
			ndp_hdr_crc = NCM_NDP_HDR_CRC;
			DBG(cdev, "CRC mode selected\n");
			break;
		default:
			goto invalid;
		}
		ncm->ndp_sign = ncm->parser_opts->ndp_sign | ndp_hdr_crc;
		value = 0;
		break;
	}
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
		| USB_CDC_GET_MAX_DATAGRAM_SIZE:
	{
		if (w_length < 2 || w_value != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		value = 2;
		put_unaligned_le16(ncm->dgramsize, req->buf);
		printk(KERN_ERR"usb:%s * Host asked current MaxDatagramSize, sending %d *\n",
		     __func__, ncm->dgramsize);
		break;
	}

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
		| USB_CDC_SET_MAX_DATAGRAM_SIZE:
	{
		if (w_length != 2 || w_value != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		req->complete = ncm_setdgram_complete;
		req->length = w_length;
		req->context = f;

		value = req->length;
		break;
	}
#endif
	/* and disabled in ncm descriptor: */
	/* case USB_CDC_GET_NET_ADDRESS: */
	/* case USB_CDC_SET_NET_ADDRESS: */
	/* case USB_CDC_GET_MAX_DATAGRAM_SIZE: */
	/* case USB_CDC_SET_MAX_DATAGRAM_SIZE: */

	default:
invalid:
		DBG(cdev, "invalid control req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);
	}

	/* respond with data transfer or status phase? */
	if (value >= 0) {
		DBG(cdev, "ncm req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);
		req->zero = 0;
		req->length = value;
		value = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (value < 0)
			ERROR(cdev, "ncm req %02x.%02x response err %d\n",
					ctrl->bRequestType, ctrl->bRequest,
					value);
	}

	/* device either stalls (value < 0) or reports success */
	return value;
}


static int ncm_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_ncm		*ncm = func_to_ncm(f);
	struct usb_composite_dev *cdev = f->config->cdev;

	/* Control interface has only altsetting 0 */
	if (intf == ncm->ctrl_id) {
		if (alt != 0)
			goto fail;

		if (ncm->notify->driver_data) {
			DBG(cdev, "reset ncm control %d\n", intf);
			usb_ep_disable(ncm->notify);
		}

		if (!(ncm->notify->desc)) {
			DBG(cdev, "init ncm ctrl %d\n", intf);
			if (config_ep_by_speed(cdev->gadget, f, ncm->notify))
				goto fail;
		}
		usb_ep_enable(ncm->notify);
		ncm->notify->driver_data = ncm;

	/* Data interface has two altsettings, 0 and 1 */
	} else if (intf == ncm->data_id) {
		if (alt > 1)
			goto fail;
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
		if (alt == 0) {
#endif
		if (ncm->port.in_ep->driver_data) {
			DBG(cdev, "reset ncm\n");
			gether_disconnect(&ncm->port);
			ncm_reset_values(ncm);
		}
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
		}
#endif
		/*
		 * CDC Network only sends data in non-default altsettings.
		 * Changing altsettings resets filters, statistics, etc.
		 */
		if (alt == 1) {
			struct net_device	*net;

			if (!ncm->port.in_ep->desc ||
			    !ncm->port.out_ep->desc) {
				DBG(cdev, "init ncm\n");
				if (config_ep_by_speed(cdev->gadget, f,
						       ncm->port.in_ep) ||
				    config_ep_by_speed(cdev->gadget, f,
						       ncm->port.out_ep)) {
					ncm->port.in_ep->desc = NULL;
					ncm->port.out_ep->desc = NULL;
					goto fail;
				}
			}

			/* TODO */
			/* Enable zlps by default for NCM conformance;
			 * override for musb_hdrc (avoids txdma ovhead)
			 */
			ncm->port.is_zlp_ok =
				gadget_is_zlp_supported(cdev->gadget);
			ncm->port.cdc_filter = DEFAULT_FILTER;
			DBG(cdev, "activate ncm\n");
			net = gether_connect(&ncm->port);
			if (IS_ERR(net))
				return PTR_ERR(net);
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
			ncm->net = net;
			ncm->net->mtu = NCM_MTU_SIZE; //ncm->dgramsize - ETH_HLEN;
			printk(KERN_DEBUG "activate ncm setting MTU size (%d)\n", ncm->net->mtu);
#endif
		}
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
		/*
		 * we don't need below code, because devguru host driver can't
		 * accpet SpeedChange/Connect Notify while Alterate Setting
		 * which call ncm_set_alt()
		 */
#else
		spin_lock(&ncm->lock);
		ncm_notify(ncm);
		spin_unlock(&ncm->lock);
#endif
	} else
		goto fail;

	return 0;
fail:
	return -EINVAL;
}

/*
 * Because the data interface supports multiple altsettings,
 * this NCM function *MUST* implement a get_alt() method.
 */
static int ncm_get_alt(struct usb_function *f, unsigned intf)
{
	struct f_ncm		*ncm = func_to_ncm(f);

	if (intf == ncm->ctrl_id)
		return 0;
	return ncm->port.in_ep->driver_data ? 1 : 0;
}



	

static struct sk_buff *ncm_wrap_ntb(struct gether *port,
				    struct sk_buff *skb)
{
	struct f_ncm	*ncm = func_to_ncm(&port->func);
	struct sk_buff	*skb2;
	int		ncb_len = 0;
	__le16		*tmp;
	int		div;
	int		rem;
	int		pad;
	int		ndp_align;
	int		ndp_pad;
	unsigned	max_size = ncm->port.fixed_in_len;
	const struct ndp_parser_opts *opts = ncm->parser_opts;
	unsigned	crc_len = ncm->is_crc ? sizeof(uint32_t) : 0;
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
	int		force_shortpkt = 0;
#endif
	/* If multi-packet is enabled, ncm header will be added directly
	 * in USB request buffer.
	 */
	div = le16_to_cpu(ntb_parameters.wNdpInDivisor);
	rem = le16_to_cpu(ntb_parameters.wNdpInPayloadRemainder);
	ndp_align = le16_to_cpu(ntb_parameters.wNdpInAlignment);

	ncb_len += opts->nth_size;
	ndp_pad = ALIGN(ncb_len, ndp_align) - ncb_len;
	ncb_len += ndp_pad;
	ncb_len += opts->ndp_size;
	ncb_len += 2 * 2 * opts->dgram_item_len * NCM_MAX_DATAGRAM; /* Datagram entry */
	ncb_len += 2 * 2 * opts->dgram_item_len; /* Zero datagram entry */
	pad = ALIGN(ncb_len, div) + rem - ncb_len;
	ncb_len += pad;

	if (ncb_len + skb->len + crc_len > max_size) {
		printk(KERN_ERR"usb: %s Dropped skb skblen (%d) \n", __func__, skb->len);
		dev_kfree_skb_any(skb);
		return NULL;
	}
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
	if ((ncb_len + skb->len + crc_len < max_size) && (((ncb_len + skb->len + crc_len) %
		le16_to_cpu(ncm->port.in_ep->desc->wMaxPacketSize)) == 0)) {
		/* force short packet */
		printk(KERN_ERR "usb: force short packet %d  \n", ncm->port.in_ep->desc->wMaxPacketSize);
		force_shortpkt = 1;
	}
#endif
	skb2 = skb_copy_expand(skb, ncb_len,
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
			       force_shortpkt,
#else
			       max_size - skb->len - ncb_len - crc_len,
#endif
			       GFP_ATOMIC);
	dev_kfree_skb_any(skb);
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
	if (!skb2) {
		printk(KERN_ERR"usb: %s Dropped skb skblen realloc failed (%d) \n", __func__, skb->len);
		return NULL;
	}
#else
	if (!skb2)
		return NULL;
#endif
	skb = skb2;

	tmp = (void *) skb_push(skb, ncb_len);
	memset(tmp, 0, ncb_len);

	put_unaligned_le32(opts->nth_sign, tmp); /* dwSignature */
	tmp += 2;
	/* wHeaderLength */
	put_unaligned_le16(opts->nth_size, tmp++);
	tmp++; /* skip wSequence */
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
	put_ncm(&tmp, opts->block_length, skb->len + force_shortpkt); /* (d)wBlockLength */
#else
	put_ncm(&tmp, opts->block_length, skb->len); /* (d)wBlockLength */
#endif
	/* (d)wFpIndex */
	/* the first pointer is right after the NTH + align */
	put_ncm(&tmp, opts->fp_index, opts->nth_size + ndp_pad);

	tmp = (void *)tmp + ndp_pad;

	/* NDP */
	put_unaligned_le32(ncm->ndp_sign, tmp); /* dwSignature */
	tmp += 2;
	/* wLength */
	put_unaligned_le16(ncb_len - opts->nth_size - pad, tmp++);

	tmp += opts->reserved1;
	tmp += opts->next_fp_index; /* skip reserved (d)wNextFpIndex */
	tmp += opts->reserved2;

	if (ncm->is_crc) {
		uint32_t crc;

		crc = ~crc32_le(~0,
				skb->data + ncb_len,
				skb->len - ncb_len);
		put_unaligned_le32(crc, skb->data + skb->len);
		skb_put(skb, crc_len);
	}

	/* (d)wDatagramIndex[0] */
	put_ncm(&tmp, opts->dgram_item_len, ncb_len);
	/* (d)wDatagramLength[0] */
	put_ncm(&tmp, opts->dgram_item_len, skb->len - ncb_len);
	/* (d)wDatagramIndex[1] and  (d)wDatagramLength[1] already zeroed */

	/* Incase of Higher MTU size we do not need to expand and zero off the remaining
	   In packet size to max_size . This saves bandwidth . e.g for 16K In size max mtu is 9k
	*/
#ifndef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
	if (skb->len > MAX_TX_NONFIXED) {
		memset(skb_put(skb, max_size - skb->len),
		       0, max_size - skb->len);
		printk(KERN_DEBUG"usb:%s Expanding the buffer %d \n", __func__, skb->len);
	}
#else
	if (force_shortpkt) {
		memset(skb_put(skb, force_shortpkt),
		       0, force_shortpkt);
		printk(KERN_ERR"usb:%s final Expanding the buffer %d \n", __func__, skb->len);
	}
#endif
	return skb;
}

static int ncm_unwrap_ntb(struct gether *port,
			  struct sk_buff *skb,
			  struct sk_buff_head *list)
{
	struct f_ncm	*ncm = func_to_ncm(&port->func);
	__le16		*tmp = (void *) skb->data;
	unsigned	index, index2;
	unsigned	dg_len, dg_len2;
	unsigned	ndp_len;
	struct sk_buff	*skb2;
	int		ret = -EINVAL;
	unsigned	max_size = le32_to_cpu(ntb_parameters.dwNtbOutMaxSize);
	const struct ndp_parser_opts *opts = ncm->parser_opts;
	unsigned	crc_len = ncm->is_crc ? sizeof(uint32_t) : 0;
	int		dgram_counter;

	/* dwSignature */
	if (get_unaligned_le32(tmp) != opts->nth_sign) {
		INFO(port->func.config->cdev, "Wrong NTH SIGN, skblen %d\n",
			skb->len);
		print_hex_dump(KERN_INFO, "HEAD:", DUMP_PREFIX_ADDRESS, 32, 1,
			       skb->data, 32, false);

		goto err;
	}
	tmp += 2;
	/* wHeaderLength */
	if (get_unaligned_le16(tmp++) != opts->nth_size) {
		INFO(port->func.config->cdev, "Wrong NTB headersize\n");
		goto err;
	}
	tmp++; /* skip wSequence */

	/* (d)wBlockLength */
	if (get_ncm(&tmp, opts->block_length) > max_size) {
		INFO(port->func.config->cdev, "OUT size exceeded\n");
		goto err;
	}

	index = get_ncm(&tmp, opts->fp_index);
	/* NCM 3.2 */
	if (((index % 4) != 0) && (index < opts->nth_size)) {
		INFO(port->func.config->cdev, "Bad index: %x\n",
			index);
		goto err;
	}

	/* walk through NDP */
	tmp = ((void *)skb->data) + index;
	if (get_unaligned_le32(tmp) != ncm->ndp_sign) {
		INFO(port->func.config->cdev, "Wrong NDP SIGN\n");
		goto err;
	}
	tmp += 2;

	ndp_len = get_unaligned_le16(tmp++);
	/*
	 * NCM 3.3.1
	 * entry is 2 items
	 * item size is 16/32 bits, opts->dgram_item_len * 2 bytes
	 * minimal: struct usb_cdc_ncm_ndpX + normal entry + zero entry
	 */
	if ((ndp_len < opts->ndp_size + 2 * 2 * (opts->dgram_item_len * 2))
	    || (ndp_len % opts->ndplen_align != 0)) {
		INFO(port->func.config->cdev, "Bad NDP length: %x\n", ndp_len);
		goto err;
	}
	tmp += opts->reserved1;
	tmp += opts->next_fp_index; /* skip reserved (d)wNextFpIndex */
	tmp += opts->reserved2;

	ndp_len -= opts->ndp_size;
	index2 = get_ncm(&tmp, opts->dgram_item_len);
	dg_len2 = get_ncm(&tmp, opts->dgram_item_len);
	dgram_counter = 0;

	do {
		index = index2;
		dg_len = dg_len2;
		if (dg_len < 14 + crc_len) { /* ethernet header + crc */
			INFO(port->func.config->cdev, "Bad dgram length: %x\n",
			     dg_len);
			goto err;
		}
		if (ncm->is_crc) {
			uint32_t crc, crc2;

			crc = get_unaligned_le32(skb->data +
						 index + dg_len - crc_len);
			crc2 = ~crc32_le(~0,
					 skb->data + index,
					 dg_len - crc_len);
			if (crc != crc2) {
				INFO(port->func.config->cdev, "Bad CRC\n");
				goto err;
			}
		}

		index2 = get_ncm(&tmp, opts->dgram_item_len);
		dg_len2 = get_ncm(&tmp, opts->dgram_item_len);

		if (index2 == 0 || dg_len2 == 0) {
			skb2 = skb;
		} else {
			skb2 = skb_clone(skb, GFP_ATOMIC);
			if (skb2 == NULL)
				goto err;
		}

		if (!skb_pull(skb2, index)) {
			ret = -EOVERFLOW;
			goto err;
		}

		skb_trim(skb2, dg_len - crc_len);
		skb_queue_tail(list, skb2);

		ndp_len -= 2 * (opts->dgram_item_len * 2);

		dgram_counter++;

		if (index2 == 0 || dg_len2 == 0)
			break;
	} while (ndp_len > 2 * (opts->dgram_item_len * 2)); /* zero entry */

	VDBG(port->func.config->cdev,
	     "Parsed NTB with %d frames\n", dgram_counter);
	return 0;
err:
	printk(KERN_DEBUG"usb:%s Dropped %d \n", __func__, skb->len);
	skb_queue_purge(list);
	dev_kfree_skb_any(skb);
	return ret;
}

static void ncm_disable(struct usb_function *f)
{
	struct f_ncm		*ncm = func_to_ncm(f);
	struct usb_composite_dev *cdev = f->config->cdev;

	DBG(cdev, "ncm deactivated\n");

	if (ncm->port.in_ep->driver_data)
		gether_disconnect(&ncm->port);

	if (ncm->notify->driver_data) {
		usb_ep_disable(ncm->notify);
		ncm->notify->driver_data = NULL;
		ncm->notify->desc = NULL;
	}
}

/*-------------------------------------------------------------------------*/

/*
 * Callbacks let us notify the host about connect/disconnect when the
 * net device is opened or closed.
 *
 * For testing, note that link states on this side include both opened
 * and closed variants of:
 *
 *   - disconnected/unconfigured
 *   - configured but inactive (data alt 0)
 *   - configured and active (data alt 1)
 *
 * Each needs to be tested with unplug, rmmod, SET_CONFIGURATION, and
 * SET_INTERFACE (altsetting).  Remember also that "configured" doesn't
 * imply the host is actually polling the notification endpoint, and
 * likewise that "active" doesn't imply it's actually using the data
 * endpoints for traffic.
 */

static void ncm_open(struct gether *geth)
{
	struct f_ncm		*ncm = func_to_ncm(&geth->func);

	DBG(ncm->port.func.config->cdev, "%s\n", __func__);

	spin_lock(&ncm->lock);
	ncm->is_open = true;
	ncm_notify(ncm);
	spin_unlock(&ncm->lock);
}

static void ncm_close(struct gether *geth)
{
	struct f_ncm		*ncm = func_to_ncm(&geth->func);

	DBG(ncm->port.func.config->cdev, "%s\n", __func__);

	spin_lock(&ncm->lock);
	ncm->is_open = false;
	ncm_notify(ncm);
	spin_unlock(&ncm->lock);
}

/*-------------------------------------------------------------------------*/

/* ethernet function driver setup/binding */

static int
ncm_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_composite_dev *cdev = c->cdev;
	struct f_ncm		*ncm = func_to_ncm(f);
	struct usb_string	*us;
	int			status;
	struct usb_ep		*ep;
	struct f_ncm_opts	*ncm_opts;

	if (!can_support_ecm(cdev->gadget))
		return -EINVAL;

	ncm_opts = container_of(f->fi, struct f_ncm_opts, func_inst);

	/*
	 * in drivers/usb/gadget/configfs.c:configfs_composite_bind()
	 * configurations are bound in sequence with list_for_each_entry,
	 * in each configuration its functions are bound in sequence
	 * with list_for_each_entry, so we assume no race condition
	 * with regard to ncm_opts->bound access
	 */
	if (!ncm_opts->bound) {
		mutex_lock(&ncm_opts->lock);

		ncm_opts->net = gether_setup_name_default("ncm");
		if (IS_ERR(ncm_opts->net)) {
			status = PTR_ERR(ncm_opts->net);
			mutex_unlock(&ncm_opts->lock);
			ERROR(cdev, "%s: failed to setup ethernet\n", f->name);
			return status;
		}

		ncm->port.ioport = netdev_priv(ncm_opts->net);
		gether_set_gadget(ncm_opts->net, cdev->gadget);
		status = gether_register_netdev(ncm_opts->net);
		mutex_unlock(&ncm_opts->lock);
		if (status) {
			free_netdev(ncm_opts->net);
			return status;
		}
		ncm_opts->bound = true;
	}
	/* need to clear local value */
	ncm_reset_values(ncm);

	/* export host's Ethernet address in CDC format */
	status = gether_get_host_addr_cdc(ncm_opts->net, ncm->ethaddr,
				      sizeof(ncm->ethaddr));

	if (status < 12) { /* strlen("01234567890a") */
		status = -EINVAL;
		goto netdev_cleanup;
	}

	us = usb_gstrings_attach(cdev, ncm_strings,
				 ARRAY_SIZE(ncm_string_defs));
	if (IS_ERR(us))
		return PTR_ERR(us);
	ncm_control_intf.iInterface = us[STRING_CTRL_IDX].id;
	ncm_data_nop_intf.iInterface = us[STRING_DATA_IDX].id;
	ncm_data_intf.iInterface = us[STRING_DATA_IDX].id;
	ecm_desc.iMACAddress = us[STRING_MAC_IDX].id;
	ncm_iad_desc.iFunction = us[STRING_IAD_IDX].id;

	/* allocate instance-specific interface IDs */
	status = usb_interface_id(c, f);
	if (status < 0)
		goto fail;
	ncm->ctrl_id = status;
	ncm_iad_desc.bFirstInterface = status;

	ncm_control_intf.bInterfaceNumber = status;
	ncm_union_desc.bMasterInterface0 = status;

	status = usb_interface_id(c, f);
	if (status < 0)
		goto fail;
	ncm->data_id = status;

	ncm_data_nop_intf.bInterfaceNumber = status;
	ncm_data_intf.bInterfaceNumber = status;
	ncm_union_desc.bSlaveInterface0 = status;

	status = -ENODEV;

	/* allocate instance-specific endpoints */
	ep = usb_ep_autoconfig(cdev->gadget, &fs_ncm_in_desc);
	if (!ep)
		goto fail;
	ncm->port.in_ep = ep;
	ep->driver_data = cdev;	/* claim */

	ep = usb_ep_autoconfig(cdev->gadget, &fs_ncm_out_desc);
	if (!ep)
		goto fail;
	ncm->port.out_ep = ep;
	ep->driver_data = cdev;	/* claim */

	/* request rndis queue */
	status = gether_alloc_request(&ncm->port);
	printk("usb: %s : ncm queue reqsest ret = %d \n", __func__, status);
	if (status < 0)
		goto fail;

	ep = usb_ep_autoconfig(cdev->gadget, &fs_ncm_notify_desc);
	if (!ep)
		goto fail;
	ncm->notify = ep;
	ep->driver_data = cdev;	/* claim */

	status = -ENOMEM;

	/* allocate notification request and buffer */
	ncm->notify_req = usb_ep_alloc_request(ep, GFP_KERNEL);
	if (!ncm->notify_req)
		goto fail;
	ncm->notify_req->buf = kmalloc(NCM_STATUS_BYTECOUNT, GFP_KERNEL);
	if (!ncm->notify_req->buf)
		goto fail;
	ncm->notify_req->context = ncm;
	ncm->notify_req->complete = ncm_notify_complete;

	/*
	 * support all relevant hardware speeds... we expect that when
	 * hardware is dual speed, all bulk-capable endpoints work at
	 * both speeds
	 */
	hs_ncm_in_desc.bEndpointAddress = fs_ncm_in_desc.bEndpointAddress;
	hs_ncm_out_desc.bEndpointAddress = fs_ncm_out_desc.bEndpointAddress;
	hs_ncm_notify_desc.bEndpointAddress =
		fs_ncm_notify_desc.bEndpointAddress;

	ss_ncm_in_desc.bEndpointAddress = fs_ncm_in_desc.bEndpointAddress;
	ss_ncm_out_desc.bEndpointAddress = fs_ncm_out_desc.bEndpointAddress;
	ss_ncm_notify_desc.bEndpointAddress =
		fs_ncm_notify_desc.bEndpointAddress;

	status = usb_assign_descriptors(f, ncm_fs_function, ncm_hs_function,
			ncm_ss_function, ncm_ss_function);
	if (status)
		goto fail;

	/*
	 * NOTE:  all that is done without knowing or caring about
	 * the network link ... which is unavailable to this code
	 * until we're activated via set_alt().
	 */

	ncm->port.open = ncm_open;
	ncm->port.close = ncm_close;

	DBG(cdev, "CDC Network: %s speed IN/%s OUT/%s NOTIFY/%s\n",
			gadget_is_dualspeed(c->cdev->gadget) ? "dual" : "full",
			ncm->port.in_ep->name, ncm->port.out_ep->name,
			ncm->notify->name);
	return 0;

fail:
	usb_free_all_descriptors(f);
	if (ncm->notify_req) {
		kfree(ncm->notify_req->buf);
		usb_ep_free_request(ncm->notify, ncm->notify_req);
	}

	/* we might as well release our claims on endpoints */
	if (ncm->notify)
		ncm->notify->driver_data = NULL;
	if (ncm->port.out_ep)
		ncm->port.out_ep->driver_data = NULL;
	if (ncm->port.in_ep)
		ncm->port.in_ep->driver_data = NULL;

netdev_cleanup:
	ncm_opts->bound = false;
	gether_cleanup(netdev_priv(ncm_opts->net));

	ERROR(cdev, "%s: can't bind, err %d\n", f->name, status);

	return status;
}


#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
extern struct device *create_function_device(char *name);
void set_ncm_ready(bool ready)
{
	if (ready != ncm_connect) {
		printk(KERN_DEBUG "usb: %s old status=%d, new status=%d\n",
				__func__, ncm_connect, ready);
		ncm_connect = ready;
		schedule_work(&_ncm_dev->work);
	} else
		ncm_connect = ready;

	if (ready == false) {
		terminal_mode_version = 0;
		terminal_mode_vendor_id = 0;
	}
}
EXPORT_SYMBOL(set_ncm_ready);

#ifdef CHECK_ETHER_TX_LEN
extern ktime_t uether_complete_time[4096];
extern int uether_queue_size[4096];
extern int uether_queue_index;
#endif

static ssize_t terminal_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret;
	ret = sprintf(buf, "major %x minor %x vendor %x\n",
			terminal_mode_version & 0xff,
			(terminal_mode_version >> 8 & 0xff),
			terminal_mode_vendor_id);
	if (terminal_mode_version)
		printk(KERN_DEBUG "usb: %s terminal_mode %s\n", __func__, buf);

#ifdef CHECK_ETHER_TX_LEN
	for (i = 0; i < 4096; i++) {
		printk(KERN_INFO "%d : %u - %d\n", i,
				uether_complete_time[i], uether_queue_size[i]);
	}
	printk("Current Index : %d\n", uether_queue_index);
#endif


	return ret;
}

static ssize_t terminal_version_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int value;
	sscanf(buf, "%x", &value);
	terminal_mode_version = (u16)value;
	printk(KERN_DEBUG "usb: %s buf=%s\n", __func__, buf);
	/* only set ncm ready when terminal verision value is not zero */
	if (value)
		set_ncm_ready(true);
	else
		set_ncm_ready(false);
	return size;
}

static DEVICE_ATTR(terminal_version,  S_IRUGO | S_IWUSR,
		terminal_version_show, terminal_version_store);

static int create_terminal_attribute(void)
{
	struct device *android_dev;
	int err = 0;

	android_dev = create_function_device("terminal_version");
	if (IS_ERR(android_dev))
		return PTR_ERR(android_dev);

	err = device_create_file(android_dev, &dev_attr_terminal_version);
	if (err) {
		printk(KERN_DEBUG "usb: %s failed to create attr\n",
				__func__);
		device_destroy(android_dev->class, android_dev->devt);
		return err;
	}
	return 0;
}

int terminal_ctrl_request(struct usb_composite_dev *cdev,
				const struct usb_ctrlrequest *ctrl)
{
	int	value = -EOPNOTSUPP;
	u16	w_index = le16_to_cpu(ctrl->wIndex);
	u16	w_value = le16_to_cpu(ctrl->wValue);

	if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_VENDOR) {
		/* Handle Terminal mode request */
		if (ctrl->bRequest == 0xf0) {
			terminal_mode_version = w_value;
			terminal_mode_vendor_id = w_index;
			set_ncm_ready(true);
			printk(KERN_DEBUG "usb: %s ver=0x%x vendor_id=0x%x\n",
				__func__, terminal_mode_version,
				terminal_mode_vendor_id);
			value = 0;
		}
	}

	/* respond ZLP */
	if (value >= 0) {
		int rc;
		cdev->req->zero = 0;
		cdev->req->length = value;
		rc = usb_ep_queue(cdev->gadget->ep0, cdev->req, GFP_ATOMIC);
		if (rc < 0)
			printk(KERN_DEBUG "usb: %s failed usb_ep_queue\n",
					__func__);
	}
	return value;
}
EXPORT_SYMBOL_GPL(terminal_ctrl_request);

static void ncm_work(struct work_struct *data)
{
	char *ncm_start[2] = { "NCM_DEVICE=START", NULL };
	char *ncm_release[2] = { "NCM_DEVICE=RELEASE", NULL };
	char **uevent_envp = NULL;

	printk(KERN_DEBUG "usb: %s ncm_connect=%d\n", __func__, ncm_connect);

	if (ncm_connect == true)
		uevent_envp = ncm_start;
	else
		uevent_envp = ncm_release;

	kobject_uevent_env(&mirrorlink_device.this_device->kobj, KOBJ_CHANGE, uevent_envp);
}

static int ncm_function_init(void)
{
	struct ncm_dev *dev;
	int ret = 0;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	INIT_WORK(&dev->work, ncm_work);

	_ncm_dev = dev;

	ret = misc_register(&mirrorlink_device);
	if (ret)
		printk("usb: %s - usb_ncm misc driver fail \n", __func__);
	return 0;
}

static void ncm_function_cleanup(void)
{
	misc_deregister(&mirrorlink_device);
	kfree(_ncm_dev);
	_ncm_dev = NULL;
}
#endif
static inline struct f_ncm_opts *to_f_ncm_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_ncm_opts,
			    func_inst.group);
}

/* f_ncm_item_ops */
USB_ETHERNET_CONFIGFS_ITEM(ncm);

/* f_ncm_opts_dev_addr */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_DEV_ADDR(ncm);

/* f_ncm_opts_host_addr */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_HOST_ADDR(ncm);

/* f_ncm_opts_qmult */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_QMULT(ncm);

/* f_ncm_opts_ifname */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_IFNAME(ncm);

static struct configfs_attribute *ncm_attrs[] = {
	&ncm_opts_attr_dev_addr,
	&ncm_opts_attr_host_addr,
	&ncm_opts_attr_qmult,
	&ncm_opts_attr_ifname,
	NULL,
};

static struct config_item_type ncm_func_type = {
	.ct_item_ops	= &ncm_item_ops,
	.ct_attrs	= ncm_attrs,
	.ct_owner	= THIS_MODULE,
};

static void ncm_free_inst(struct usb_function_instance *f)
{
	struct f_ncm_opts *opts;

	opts = container_of(f, struct f_ncm_opts, func_inst);
	if (opts->bound)
		gether_cleanup(netdev_priv(opts->net));
/*
To prevent crash in case we are not bound.
	else
		free_netdev(opts->net);
*/
	kfree(opts);
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
	ncm_function_cleanup();
#endif
}

static struct usb_function_instance *ncm_alloc_inst(void)
{
	struct f_ncm_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);
	mutex_init(&opts->lock);
	opts->func_inst.free_func_inst = ncm_free_inst;
	config_group_init_type_name(&opts->func_inst.group, "", &ncm_func_type);
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
	create_terminal_attribute();
#endif
	return &opts->func_inst;
}

static void ncm_free(struct usb_function *f)
{
	struct f_ncm *ncm;
	struct f_ncm_opts *opts;

	ncm = func_to_ncm(f);
	opts = container_of(f->fi, struct f_ncm_opts, func_inst);
	kfree(ncm);
	mutex_lock(&opts->lock);
	opts->refcnt--;
	mutex_unlock(&opts->lock);
}

static void ncm_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_ncm *ncm = func_to_ncm(f);
#ifdef CONFIG_USB_CONFIGFS_UEVENT
	struct f_ncm_opts	*opts;
#endif
#ifdef CONFIG_USB_CONFIGFS_UEVENT
	opts = container_of(f->fi, struct f_ncm_opts, func_inst);
	opts->bound = false;
#endif
	DBG(c->cdev, "ncm unbind\n");

	ncm_string_defs[0].id = 0;
	usb_free_all_descriptors(f);

	if (atomic_read(&ncm->notify_count)) {
		usb_ep_dequeue(ncm->notify, ncm->notify_req);
		atomic_set(&ncm->notify_count, 0);
	}

	kfree(ncm->notify_req->buf);
	usb_ep_free_request(ncm->notify, ncm->notify_req);

	gether_free_request(&ncm->port);

#ifdef CONFIG_USB_CONFIGFS_UEVENT
	gether_cleanup(netdev_priv(opts->net));
#endif
}

static struct usb_function *ncm_alloc(struct usb_function_instance *fi)
{
	struct f_ncm		*ncm;
	struct f_ncm_opts	*opts;

	/* allocate and initialize one new instance */
	ncm = kzalloc(sizeof(*ncm), GFP_KERNEL);
	if (!ncm)
		return ERR_PTR(-ENOMEM);

	opts = container_of(fi, struct f_ncm_opts, func_inst);
	mutex_lock(&opts->lock);
	opts->refcnt++;

	ncm_string_defs[STRING_MAC_IDX].s = ncm->ethaddr;

	spin_lock_init(&ncm->lock);
	ncm_reset_values(ncm);
	mutex_unlock(&opts->lock);
	ncm->port.is_fixed = true;
	ncm->port.supports_multi_frame = true;
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
	ncm->port.func.name = "ncm";
#else
	ncm->port.func.name = "ncm";
#endif
	/* descriptors are per-instance copies */
	ncm->port.func.bind = ncm_bind;
	ncm->port.func.unbind = ncm_unbind;
	ncm->port.func.set_alt = ncm_set_alt;
	ncm->port.func.get_alt = ncm_get_alt;
	ncm->port.func.setup = ncm_setup;
	ncm->port.func.disable = ncm_disable;
	ncm->port.func.free_func = ncm_free;

	ncm->port.wrap = ncm_wrap_ntb;
	ncm->port.unwrap = ncm_unwrap_ntb;

	ncm->port.is_fixed = false;
	ncm->port.multi_pkt_xfer = 1;
	ncm->port.ul_max_pkts_per_xfer = 1;
	ncm->port.dl_max_pkts_per_xfer = NCM_MAX_DATAGRAM;

	if (ncm->port.multi_pkt_xfer == 1)
		ncm->port.wrap = NULL;

#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
	ncm_function_init();
#endif
	return &ncm->port.func;
}

DECLARE_USB_FUNCTION_INIT(ncm, ncm_alloc_inst, ncm_alloc);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yauheni Kaliuta");

void ncm_add_header(u32 packet_start_offset, void *buf, u32 data_len)
{
    struct ncm_header *tmp_header;

    memset(buf, 0, packet_start_offset);

    tmp_header = (struct ncm_header *)buf;
    tmp_header->signature = NCM_NTH_SIGNATURE;
    tmp_header->header_len = NCM_NTH_LEN16;
    tmp_header->sequence = NCM_NTH_SEQUENCE;
    tmp_header->blk_len = data_len + packet_start_offset;
    tmp_header->index = NCM_NTH_INDEX16;
    tmp_header->dgram_sig = NCM_NDP_SIGNATURE;
    tmp_header->dgram_header_len = NCM_NDP_LEN16;
    tmp_header->dgram_rev = NCM_NDP_REV;
    tmp_header->dgram_index0 = packet_start_offset;
    tmp_header->dgram_len0 = data_len;
}
EXPORT_SYMBOL(ncm_add_header);

int ncm_add_datagram(struct gether *port, __le16 *tmp, int length, int holdcnt)
{
    int tmp_val;
    __le16 *tmp_addr;
    u32 prev_index, prev_len;
    int i;

    tmp += 4;
    tmp_val = get_unaligned_le16(tmp);
    put_unaligned_le16(tmp_val + length, tmp);

    tmp_val = get_unaligned_le16(tmp);
    tmp_addr = tmp + 6;
    tmp_addr += ((holdcnt - 1) * 2);

    prev_index = get_unaligned_le16(tmp_addr);
    tmp_addr += 1;
    prev_len = get_unaligned_le16(tmp_addr);
    tmp_addr += 1;

    put_unaligned_le16(prev_index + prev_len, tmp_addr);
    tmp_addr += 1;
    put_unaligned_le16(length, tmp_addr);

    return i;
}
EXPORT_SYMBOL(ncm_add_datagram);
