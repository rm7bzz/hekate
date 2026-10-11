/*
 * USB Gadget HID driver for Tegra X1
 *
 * Copyright (c) 2019-2026 CTCaer
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

#include <string.h>

#include <usb/usbd.h>
#include <gfx_utils.h>
#include <input/joycon.h>
#include <input/touch.h>
#include <soc/hw_init.h>
#include <soc/timer.h>
#include <soc/t210.h>

#include <memory_map.h>

//#define DPRINTF(...) gfx_printf(__VA_ARGS__)
#define DPRINTF(...)

typedef struct _gamepad_report_t
{
	u8 x;
	u8 y;
	u8 z;
	u8 rz;

	u8 hat:4;
	u8 btn1:1;
	u8 btn2:1;
	u8 btn3:1;
	u8 btn4:1;

	u8 btn5:1;
	u8 btn6:1;
	u8 btn7:1;
	u8 btn8:1;
	u8 btn9:1;
	u8 btn10:1;
	u8 btn11:1;
	u8 btn12:1;
} __attribute__((packed)) gamepad_report_t;

typedef struct _jc_cal_t
{
// 15ms * JC_CAL_MAX_STEPS = 240 ms.
#define JC_CAL_MAX_STEPS 16
	u32 cl_step;
	u32 cr_step;

	u16 clx_max;
	u16 clx_min;
	u16 cly_max;
	u16 cly_min;
	u16 crx_max;
	u16 crx_min;
	u16 cry_max;
	u16 cry_min;
} jc_cal_t;

enum {
	INPUT_POLL_HAS_PACKET,
	INPUT_POLL_NO_PACKET,
	INPUT_POLL_EXIT,
};

static jc_cal_t jc_cal_ctx;
static usb_ops_t usb_ops;
static u32 hid_polling_time;
static bool hid_started;

static u8 *rpt_buffer = (u8 *)USB_EP_BULK_IN_BUF_ADDR;
static bool hid_tx_pending;
static bool hid_tx_hold;
static u8 hid_connection_state;
static u32 hid_report_generation;
static u32 hid_tx_generation;
static u32 hid_next_key_time;
static u32 hid_mouse_idle;
static u32 hid_mouse_report_time;
static usb_hid_mouse_report_t hid_last_mouse_report;

#define USB_HID_KEYBOARD_QUEUE_LEN 64

#define USB_HID_REPORT_ID_KEYBOARD 1
#define USB_HID_REPORT_ID_MOUSE    2

typedef struct _usb_hid_keyboard_ctxt_t
{
	usb_hid_keyboard_report_t reports[USB_HID_KEYBOARD_QUEUE_LEN];
	usb_hid_keyboard_report_t last_report;
	u32 read_idx;
	u32 write_idx;
	u32 last_report_time;
	bool exit_requested;
} usb_hid_keyboard_ctxt_t;

static usb_hid_keyboard_ctxt_t usb_hid_keyboard_ctxt;

typedef struct _usb_hid_keyboard_mouse_entry_t
{
	u8 report_id;
	u8 size;
	u8 data[8];
} usb_hid_keyboard_mouse_entry_t;

typedef struct _usb_hid_keyboard_mouse_ctxt_t
{
	usb_hid_keyboard_mouse_entry_t reports[USB_HID_KEYBOARD_QUEUE_LEN];
	u32 read_idx;
	u32 write_idx;
} usb_hid_keyboard_mouse_ctxt_t;

static usb_hid_keyboard_mouse_ctxt_t usb_hid_keyboard_mouse_ctxt;

void usb_hid_keyboard_queue_clear(void)
{
	memset(&usb_hid_keyboard_ctxt, 0, sizeof(usb_hid_keyboard_ctxt));
}

bool usb_hid_keyboard_queue_report(const usb_hid_keyboard_report_t *report)
{
	u32 next_idx = (usb_hid_keyboard_ctxt.write_idx + 1) % USB_HID_KEYBOARD_QUEUE_LEN;
	if (next_idx == usb_hid_keyboard_ctxt.read_idx)
		return false;

	memcpy(&usb_hid_keyboard_ctxt.reports[usb_hid_keyboard_ctxt.write_idx], report, sizeof(usb_hid_keyboard_report_t));
	usb_hid_keyboard_ctxt.write_idx = next_idx;

	return true;
}

void usb_hid_keyboard_mouse_queue_clear(void)
{
	memset(&usb_hid_keyboard_mouse_ctxt, 0, sizeof(usb_hid_keyboard_mouse_ctxt));
}

static bool _usb_hid_keyboard_mouse_queue(u8 report_id, const void *data, u8 size)
{
	u32 next_idx = (usb_hid_keyboard_mouse_ctxt.write_idx + 1) % USB_HID_KEYBOARD_QUEUE_LEN;
	usb_hid_keyboard_mouse_entry_t *entry;

	if (size > sizeof(usb_hid_keyboard_mouse_ctxt.reports[0].data)
		|| next_idx == usb_hid_keyboard_mouse_ctxt.read_idx)
		return false;

	entry = &usb_hid_keyboard_mouse_ctxt.reports[usb_hid_keyboard_mouse_ctxt.write_idx];
	entry->report_id = report_id;
	entry->size = size;
	memcpy(entry->data, data, size);
	usb_hid_keyboard_mouse_ctxt.write_idx = next_idx;

	return true;
}

bool usb_hid_keyboard_mouse_queue_keyboard(const usb_hid_keyboard_report_t *report)
{
	return _usb_hid_keyboard_mouse_queue(USB_HID_REPORT_ID_KEYBOARD, report, sizeof(*report));
}

bool usb_hid_keyboard_mouse_queue_mouse(const usb_hid_mouse_report_t *report)
{
	// Leave room for a complete press/release pair even during pointer motion.
	u32 used = (usb_hid_keyboard_mouse_ctxt.write_idx + USB_HID_KEYBOARD_QUEUE_LEN
		- usb_hid_keyboard_mouse_ctxt.read_idx) % USB_HID_KEYBOARD_QUEUE_LEN;
	if (used >= USB_HID_KEYBOARD_QUEUE_LEN - 4)
		return false;
	return _usb_hid_keyboard_mouse_queue(USB_HID_REPORT_ID_MOUSE, report, sizeof(*report));
}

bool usb_hid_keyboard_queue_tap(const usb_hid_keyboard_report_t *report, bool combined)
{
	u32 read_idx = combined ? usb_hid_keyboard_mouse_ctxt.read_idx : usb_hid_keyboard_ctxt.read_idx;
	u32 write_idx = combined ? usb_hid_keyboard_mouse_ctxt.write_idx : usb_hid_keyboard_ctxt.write_idx;
	u32 used = (write_idx + USB_HID_KEYBOARD_QUEUE_LEN - read_idx) % USB_HID_KEYBOARD_QUEUE_LEN;
	usb_hid_keyboard_report_t release = {0};
	if (used >= USB_HID_KEYBOARD_QUEUE_LEN - 2)
		return false;
	// Reserve both slots before enqueueing, so a full queue cannot leave a key held.
	if (combined)
	{
		usb_hid_keyboard_mouse_queue_keyboard(report);
		usb_hid_keyboard_mouse_queue_keyboard(&release);
	}
	else
	{
		usb_hid_keyboard_queue_report(report);
		usb_hid_keyboard_queue_report(&release);
	}
	return true;
}

bool usb_hid_mouse_queue_click(u8 buttons)
{
	u32 used = (usb_hid_keyboard_mouse_ctxt.write_idx + USB_HID_KEYBOARD_QUEUE_LEN
		- usb_hid_keyboard_mouse_ctxt.read_idx) % USB_HID_KEYBOARD_QUEUE_LEN;
	usb_hid_mouse_report_t press = { .buttons = buttons };
	usb_hid_mouse_report_t release = {0};
	if (used >= USB_HID_KEYBOARD_QUEUE_LEN - 2)
		return false;
	_usb_hid_keyboard_mouse_queue(USB_HID_REPORT_ID_MOUSE, &press, sizeof(press));
	_usb_hid_keyboard_mouse_queue(USB_HID_REPORT_ID_MOUSE, &release, sizeof(release));
	return true;
}

void usb_hid_keyboard_reset_reports(void)
{
	bool exit_requested = usb_hid_keyboard_ctxt.exit_requested;
	usb_hid_keyboard_queue_clear();
	usb_hid_keyboard_mouse_queue_clear();
	usb_hid_keyboard_ctxt.exit_requested = exit_requested;
	memset(&hid_last_mouse_report, 0, sizeof(hid_last_mouse_report));
	hid_mouse_idle = 0;
	hid_polling_time = 0;
	hid_report_generation++;
	if (hid_started)
	{
		usb_hid_keyboard_report_t release = {0};
		usb_hid_keyboard_queue_report(&release);
		usb_hid_keyboard_mouse_queue_keyboard(&release);
	}
}

int usb_hid_keyboard_get_idle(usb_gadget_type gadget, u8 report_id)
{
	if (report_id == 0 || (gadget == USB_GADGET_HID_KEYBOARD_MOUSE && report_id == USB_HID_REPORT_ID_KEYBOARD))
		return hid_polling_time / 4000;
	if (gadget == USB_GADGET_HID_KEYBOARD_MOUSE && report_id == USB_HID_REPORT_ID_MOUSE)
		return hid_mouse_idle / 4000;
	return -1;
}

bool usb_hid_keyboard_set_idle(usb_gadget_type gadget, u16 value)
{
	u8 id = value & 0xFF;
	u32 duration = (value >> 8) * 4000;
	if (usb_hid_keyboard_get_idle(gadget, id) < 0)
		return false;
	if (id == 0 || id == USB_HID_REPORT_ID_KEYBOARD)
		hid_polling_time = duration;
	if (gadget == USB_GADGET_HID_KEYBOARD_MOUSE && (id == 0 || id == USB_HID_REPORT_ID_MOUSE))
		hid_mouse_idle = duration;
	return true;
}

u32 usb_hid_keyboard_get_report(usb_gadget_type gadget, u8 protocol, u16 value, u8 *buffer)
{
	u8 type = value >> 8;
	u8 id = value & 0xFF;
	bool combined = gadget == USB_GADGET_HID_KEYBOARD_MOUSE;
	bool prefix = combined && protocol == 1;
	if ((!combined && id != 0) || (prefix && id != USB_HID_REPORT_ID_KEYBOARD && id != USB_HID_REPORT_ID_MOUSE)
		|| (combined && !prefix && id > USB_HID_REPORT_ID_KEYBOARD))
		return 0;
	if (type == 2 && (!prefix || id == USB_HID_REPORT_ID_KEYBOARD))
	{
		buffer[0] = prefix ? USB_HID_REPORT_ID_KEYBOARD : 0;
		if (prefix)
			buffer[1] = 0; // LEDs are accepted but not displayed.
		return prefix ? 2 : 1;
	}
	if (type != 1)
		return 0;
	if (prefix)
		buffer[0] = id;
	if (prefix && id == USB_HID_REPORT_ID_MOUSE)
	{
		memcpy(buffer + 1, &hid_last_mouse_report, sizeof(hid_last_mouse_report));
		return sizeof(hid_last_mouse_report) + 1;
	}
	memcpy(buffer + prefix, &usb_hid_keyboard_ctxt.last_report, sizeof(usb_hid_keyboard_report_t));
	return sizeof(usb_hid_keyboard_report_t) + prefix;
}

void usb_hid_keyboard_set_exit_requested(bool exit_requested)
{
	usb_hid_keyboard_ctxt.exit_requested = exit_requested;
}

bool usb_hid_keyboard_get_exit_requested(void)
{
	return usb_hid_keyboard_ctxt.exit_requested;
}

static bool _usb_hid_keyboard_dequeue_report(usb_hid_keyboard_report_t *report)
{
	if (usb_hid_keyboard_ctxt.read_idx == usb_hid_keyboard_ctxt.write_idx)
		return false;

	memcpy(report, &usb_hid_keyboard_ctxt.reports[usb_hid_keyboard_ctxt.read_idx], sizeof(usb_hid_keyboard_report_t));
	usb_hid_keyboard_ctxt.read_idx = (usb_hid_keyboard_ctxt.read_idx + 1) % USB_HID_KEYBOARD_QUEUE_LEN;

	return true;
}

static bool _usb_hid_keyboard_mouse_dequeue(usb_hid_keyboard_mouse_entry_t *entry)
{
	if (usb_hid_keyboard_mouse_ctxt.read_idx == usb_hid_keyboard_mouse_ctxt.write_idx)
		return false;

	memcpy(entry, &usb_hid_keyboard_mouse_ctxt.reports[usb_hid_keyboard_mouse_ctxt.read_idx], sizeof(*entry));
	usb_hid_keyboard_mouse_ctxt.read_idx = (usb_hid_keyboard_mouse_ctxt.read_idx + 1) % USB_HID_KEYBOARD_QUEUE_LEN;

	return true;
}

static bool _jc_calibration(const jc_gamepad_rpt_t *jc_pad)
{
	// Calibrate left stick.
	if (jc_cal_ctx.cl_step != JC_CAL_MAX_STEPS)
	{
		if (jc_pad->conn_l
			&& jc_pad->lstick_x > 0x400 && jc_pad->lstick_y > 0x400
			&& jc_pad->lstick_x < 0xC00 && jc_pad->lstick_y < 0xC00)
		{
			jc_cal_ctx.cl_step++;
			jc_cal_ctx.clx_max = jc_pad->lstick_x + 0x72;
			jc_cal_ctx.clx_min = jc_pad->lstick_x - 0x72;
			jc_cal_ctx.cly_max = jc_pad->lstick_y + 0x72;
			jc_cal_ctx.cly_min = jc_pad->lstick_y - 0x72;

			if (jc_cal_ctx.cl_step != JC_CAL_MAX_STEPS)
				return false;
		}
		else
			return false;
	}

	// Calibrate right stick.
	if (jc_cal_ctx.cr_step != JC_CAL_MAX_STEPS)
	{
		if (jc_pad->conn_r
			&& jc_pad->rstick_x > 0x400 && jc_pad->rstick_y > 0x400
			&& jc_pad->rstick_x < 0xC00 && jc_pad->rstick_y < 0xC00)
		{
			jc_cal_ctx.cr_step++;
			jc_cal_ctx.crx_max = jc_pad->rstick_x + 0x72;
			jc_cal_ctx.crx_min = jc_pad->rstick_x - 0x72;
			jc_cal_ctx.cry_max = jc_pad->rstick_y + 0x72;
			jc_cal_ctx.cry_min = jc_pad->rstick_y - 0x72;

			if (jc_cal_ctx.cr_step != JC_CAL_MAX_STEPS)
				return false;
		}
		else
			return false;
	}

	return true;
}

static int _jc_poll(gamepad_report_t *rpt)
{
	static gamepad_report_t prev_rpt = {0};

	// Poll Joy-Con.
	jc_gamepad_rpt_t *jc_pad = joycon_poll();

	if (!jc_pad)
		return INPUT_POLL_NO_PACKET;

	// Exit emulation if Left stick and Home are pressed.
	if (jc_pad->l3 && jc_pad->home)
		return INPUT_POLL_EXIT;

	if (jc_cal_ctx.cl_step != JC_CAL_MAX_STEPS || jc_cal_ctx.cr_step != JC_CAL_MAX_STEPS)
	{
		if (!_jc_calibration(jc_pad))
			return INPUT_POLL_NO_PACKET;
	}

	// Re-calibrate on disconnection.
	if (!jc_pad->conn_l)
		jc_cal_ctx.cl_step = 0;
	if (!jc_pad->conn_r)
		jc_cal_ctx.cr_step = 0;

	// Calculate left analog stick.
	if (jc_pad->lstick_x <= jc_cal_ctx.clx_max && jc_pad->lstick_x >= jc_cal_ctx.clx_min)
		rpt->x = 0x7F;
	else if (jc_pad->lstick_x > jc_cal_ctx.clx_max)
	{
		u16 x_raw = (jc_pad->lstick_x - jc_cal_ctx.clx_max) / 7;
		if (x_raw > 0x7F)
			x_raw = 0x7F;
		rpt->x = 0x7F + x_raw;
	}
	else
	{
		u16 x_raw = (jc_cal_ctx.clx_min - jc_pad->lstick_x) / 7;
		if (x_raw > 0x7F)
			x_raw = 0x7F;
		rpt->x = 0x7F - x_raw;
	}

	if (jc_pad->lstick_y <= jc_cal_ctx.cly_max && jc_pad->lstick_y >= jc_cal_ctx.cly_min)
		rpt->y = 0x7F;
	else if (jc_pad->lstick_y > jc_cal_ctx.cly_max)
	{
		u16 y_raw = (jc_pad->lstick_y - jc_cal_ctx.cly_max) / 7;
		if (y_raw > 0x7F)
			y_raw = 0x7F;
		// Hoag has inverted Y axis.
		if (!jc_pad->sio_mode)
			rpt->y = 0x7F - y_raw;
		else
			rpt->y = 0x7F + y_raw;
	}
	else
	{
		u16 y_raw = (jc_cal_ctx.cly_min - jc_pad->lstick_y) / 7;
		if (y_raw > 0x7F)
			y_raw = 0x7F;
		// Hoag has inverted Y axis.
		if (!jc_pad->sio_mode)
			rpt->y = 0x7F + y_raw;
		else
			rpt->y = 0x7F - y_raw;
	}

	// Calculate right analog stick.
	if (jc_pad->rstick_x <= jc_cal_ctx.crx_max && jc_pad->rstick_x >= jc_cal_ctx.crx_min)
		rpt->z = 0x7F;
	else if (jc_pad->rstick_x > jc_cal_ctx.crx_max)
	{
		u16 x_raw = (jc_pad->rstick_x - jc_cal_ctx.crx_max) / 7;
		if (x_raw > 0x7F)
			x_raw = 0x7F;
		rpt->z = 0x7F + x_raw;
	}
	else
	{
		u16 x_raw = (jc_cal_ctx.crx_min - jc_pad->rstick_x) / 7;
		if (x_raw > 0x7F)
			x_raw = 0x7F;
		rpt->z = 0x7F - x_raw;
	}

	if (jc_pad->rstick_y <= jc_cal_ctx.cry_max && jc_pad->rstick_y >= jc_cal_ctx.cry_min)
		rpt->rz = 0x7F;
	else if (jc_pad->rstick_y > jc_cal_ctx.cry_max)
	{
		u16 y_raw = (jc_pad->rstick_y - jc_cal_ctx.cry_max) / 7;
		if (y_raw > 0x7F)
			y_raw = 0x7F;
		// Hoag has inverted Y axis.
		if (!jc_pad->sio_mode)
			rpt->rz = 0x7F - y_raw;
		else
			rpt->rz = 0x7F + y_raw;
	}
	else
	{
		u16 y_raw = (jc_cal_ctx.cry_min - jc_pad->rstick_y) / 7;
		if (y_raw > 0x7F)
			y_raw = 0x7F;
		// Hoag has inverted Y axis.
		if (!jc_pad->sio_mode)
			rpt->rz = 0x7F + y_raw;
		else
			rpt->rz = 0x7F - y_raw;
	}

	// Set D-pad.
	switch ((jc_pad->buttons >> 16) & 0xF)
	{
	case 0: // none
		rpt->hat = 0xF;
		break;
	case 1: // down
		rpt->hat = 4;
		break;
	case 2: // up
		rpt->hat = 0;
		break;
	case 4: // right
		rpt->hat = 2;
		break;
	case 5: // down + right
		rpt->hat = 3;
		break;
	case 6: // up + right
		rpt->hat = 1;
		break;
	case 8: // left
		rpt->hat = 6;
		break;
	case 9: // down + left
		rpt->hat = 5;
		break;
	case 10: // up + left
		rpt->hat = 7;
		break;
	default:
		rpt->hat = 0xF;
		break;
	}

	// Set buttons.
	rpt->btn1 = jc_pad->b; // x.
	rpt->btn2 = jc_pad->a; // a.
	rpt->btn3 = jc_pad->y; // b.
	rpt->btn4 = jc_pad->x; // y.

	rpt->btn5  = jc_pad->l;
	rpt->btn6  = jc_pad->r;
	rpt->btn7  = jc_pad->zl;
	rpt->btn8  = jc_pad->zr;
	rpt->btn9  = jc_pad->minus;
	rpt->btn10 = jc_pad->plus;
	rpt->btn11 = jc_pad->l3;
	rpt->btn12 = jc_pad->r3;

	//rpt->btn13 = jc_pad->cap;
	//rpt->btn14 = jc_pad->home;

	if (!memcmp(rpt, &prev_rpt, sizeof(gamepad_report_t)))
		return INPUT_POLL_NO_PACKET;

	memcpy(&prev_rpt, rpt, sizeof(gamepad_report_t));

	return INPUT_POLL_HAS_PACKET;
}

typedef struct _touchpad_report_t
{
	u8  rpt_id;
	u8  tip_switch:1;
	u8  count:7;

	u8  id;

	//u16 z;
	u16 x;
	u16 y;
} __attribute__((packed)) touchpad_report_t;

static bool _fts_touch_read(touchpad_report_t *rpt)
{
	static touch_event_t touchpad;

	if (touch_poll(&touchpad))
		return false;

	rpt->rpt_id = 5;
	rpt->count = 1;

	// Decide touch enable.
	if (touchpad.touch)
	{
		rpt->x = touchpad.x;
		rpt->y = touchpad.y;
		//rpt->z = touchpad.z;
		rpt->id = touchpad.finger;
		rpt->tip_switch = 1;
	}
	else
	{
		rpt->x = touchpad.x;
		rpt->y = touchpad.y;
		//rpt->z = touchpad.z;
		rpt->id = touchpad.finger;
		rpt->tip_switch = 0;
	}

	return true;
}

static u8 _hid_transfer_start(usb_ctxt_t *usbs, u32 len)
{
	u8 status = usb_ops.usb_device_ep1_in_write(rpt_buffer, len, NULL, USB_XFER_SYNCED_CMD);
	if (status == USB_ERROR_XFER_ERROR)
	{
		usbs->set_text(usbs->label, "#FFDD00 Error:# EP IN transfer!");
		if (usb_ops.usbd_flush_endpoint)
			usb_ops.usbd_flush_endpoint(USB_EP_BULK_IN);
	}

	// Linux mitigation: If timed out, clear status.
	if (status == USB_ERROR_TIMEOUT)
		return 0;

	return status;
}

static bool _hid_poll_jc(usb_ctxt_t *usbs)
{
	int res = _jc_poll((gamepad_report_t *)rpt_buffer);
	if (res == INPUT_POLL_EXIT)
		return true;

	// Send HID report.
	if (res == INPUT_POLL_HAS_PACKET || usbs->idle)
		if (_hid_transfer_start(usbs, sizeof(gamepad_report_t)))
			return true; // EP Error.

	return false;
}

static bool _hid_poll_touch(usb_ctxt_t *usbs)
{
	_fts_touch_read((touchpad_report_t *)rpt_buffer);

	// Send HID report.
	if (_hid_transfer_start(usbs, sizeof(touchpad_report_t)))
		return true; // EP Error.

	return false;
}

static bool _hid_poll_keyboard(usb_ctxt_t *usbs)
{
	bool combined = usbs->type == USB_HID_KEYBOARD_MOUSE;
	u8 protocol = usb_ops.usb_device_get_hid_protocol();
	u32 now = get_tmr_us();
	usb_hid_keyboard_mouse_entry_t entry = { .report_id = USB_HID_REPORT_ID_KEYBOARD, .size = 8 };
	if (hid_tx_generation != hid_report_generation)
	{
		// A protocol switch must not overwrite a packet still owned by DMA.
		if (hid_tx_pending && usb_ops.usb_device_hid_poll() == USB_ERROR_TIMEOUT)
			return false;
		hid_tx_pending = false;
		hid_tx_generation = hid_report_generation;
		hid_next_key_time = now;
	}
	if (hid_tx_pending)
	{
		int status = usb_ops.usb_device_hid_poll();
		if (status == USB_ERROR_TIMEOUT)
			return false;
		if (status)
			return true;
		hid_tx_pending = false;
		// Hold each tap across multiple firmware polling intervals.
		hid_next_key_time = now + (hid_tx_hold ? 40000 : 0);
	}
	if (!usb_ops.usb_device_hid_ready() || (s32)(now - hid_next_key_time) < 0)
		return false;
	bool has_report = combined ? _usb_hid_keyboard_mouse_dequeue(&entry)
		: _usb_hid_keyboard_dequeue_report((usb_hid_keyboard_report_t *)entry.data);
	if (!has_report)
	{
		if (hid_polling_time && (u32)(now - usb_hid_keyboard_ctxt.last_report_time) >= hid_polling_time)
			memcpy(entry.data, &usb_hid_keyboard_ctxt.last_report, 8);
		else if (combined && protocol == 1 && hid_mouse_idle && (u32)(now - hid_mouse_report_time) >= hid_mouse_idle)
		{
			entry.report_id = USB_HID_REPORT_ID_MOUSE;
			entry.size = sizeof(hid_last_mouse_report);
			memcpy(entry.data, &hid_last_mouse_report, entry.size);
		}
		else
			return false;
	}
	// A boot keyboard never emits report IDs or mouse packets.
	if (entry.report_id == USB_HID_REPORT_ID_MOUSE && protocol == 0)
		return false;
	u32 prefix = combined && protocol == 1;
	if (prefix)
		rpt_buffer[0] = entry.report_id;
	memcpy(rpt_buffer + prefix, entry.data, entry.size);
	int status = usb_ops.usb_device_ep1_in_write(rpt_buffer, entry.size + prefix, NULL, USB_XFER_START);
	if (status)
		return true;
	hid_tx_pending = true;
	hid_tx_hold = entry.report_id == USB_HID_REPORT_ID_KEYBOARD || entry.data[0] != 0;
	if (entry.report_id == USB_HID_REPORT_ID_KEYBOARD)
	{
		memcpy(&usb_hid_keyboard_ctxt.last_report, entry.data, 8);
		usb_hid_keyboard_ctxt.last_report_time = now;
	}
	else
	{
		memcpy(&hid_last_mouse_report, entry.data, sizeof(hid_last_mouse_report));
		// Idle retransmissions must not replay relative movement or wheel events.
		hid_last_mouse_report.x = hid_last_mouse_report.y = hid_last_mouse_report.wheel = 0;
		hid_mouse_report_time = now;
	}
	return false;
}

int usb_device_gadget_hid_start(usb_ctxt_t *usbs)
{
	u32 gadget_type;
	hid_tx_pending = false;
	hid_connection_state = 0xFF;
	hid_next_key_time = get_tmr_us();

	// Get USB Controller ops.
	if (hw_get_chip_id() == GP_HIDREV_MAJOR_T210)
		usb_device_get_ops(&usb_ops);
	else
		xusb_device_get_ops(&usb_ops);

	// Default to "always send" for the existing HID modes and event-driven behavior for keyboard.
	usbs->idle = (usbs->type == USB_HID_KEYBOARD || usbs->type == USB_HID_KEYBOARD_MOUSE) ? 0 : 1;

	if (usbs->type == USB_HID_GAMEPAD)
	{
		hid_polling_time = 15000;
		gadget_type = USB_GADGET_HID_GAMEPAD;
	}
	else if (usbs->type == USB_HID_TOUCHPAD)
	{
		hid_polling_time = 4000;
		gadget_type = USB_GADGET_HID_TOUCHPAD;
	}
	else if (usbs->type == USB_HID_KEYBOARD)
	{
		hid_polling_time = 0;
		gadget_type = USB_GADGET_HID_KEYBOARD;
		usb_hid_keyboard_reset_reports();
	}
	else
	{
		hid_polling_time = 0;
		gadget_type = USB_GADGET_HID_KEYBOARD_MOUSE;
		usb_hid_keyboard_reset_reports();
	}

	usbs->set_text(usbs->label, "#C7EA46 Status:# Started USB");

	if (usb_ops.usb_device_init())
	{
		usb_ops.usbd_end(false, true);
		return 1;
	}

	usbs->set_text(usbs->label, "#C7EA46 Status:# Waiting for connection");

	// Initialize Control Endpoint.
	if (usb_ops.usb_device_enumerate(gadget_type))
		goto error;

	if (usbs->type == USB_HID_KEYBOARD || usbs->type == USB_HID_KEYBOARD_MOUSE)
	{
		/*
		 * Keyboard mode is driven by the Nyx UI. Avoid blocking here waiting for
		 * the host's first interrupt-IN poll, otherwise the GUI appears frozen
		 * until the cable is unplugged.
		 */
		usbs->set_text(usbs->label, "#C7EA46 Status:# Waiting for keypress");
	}
	else
	{
		usbs->set_text(usbs->label, "#C7EA46 Status:# Waiting for HID report request");

		u32 rpt_size;
		if (usbs->type == USB_HID_GAMEPAD)
			rpt_size = sizeof(gamepad_report_t);
		else
			rpt_size = sizeof(touchpad_report_t);

		if (usb_ops.usb_device_class_send_hid_report(rpt_buffer, rpt_size))
			goto error;
	}

	usbs->set_text(usbs->label, "#C7EA46 Status:# Started HID emulation");
	hid_started = true;

	return 0;

error:
	usbs->set_text(usbs->label, "#FFDD00 Error:# Timed out or canceled");
	usb_ops.usbd_end(false, true);
	return 1;
}

int usb_device_gadget_hid_pump(usb_ctxt_t *usbs)
{
	if (!hid_started)
		return 1;

	bool keyboard = usbs->type == USB_HID_KEYBOARD || usbs->type == USB_HID_KEYBOARD_MOUSE;
	if (keyboard && usb_hid_keyboard_get_exit_requested())
		return 1;
	// Firmware can suspend the bus while rebooting. Keep the keyboard UI open
	// and EP0 serviced so it can enumerate again; End HID always stays usable.
	if (!keyboard && usb_ops.usb_device_get_suspended())
	{
		usbs->set_text(usbs->label, "#C7EA46 Status:# HID ended");
		return 1;
	}

	// Handle control endpoint.
	usb_ops.usbd_handle_ep0_ctrl_setup(&usbs->idle);
	if (keyboard)
	{
		u8 state = usb_ops.usb_device_hid_ready() ? (usb_ops.usb_device_get_hid_protocol() + 1) : 0;
		if (state != hid_connection_state)
		{
			hid_connection_state = state;
			if (!state)
				usbs->set_text(usbs->label, "#FFDD00 Status:# USB paused/disconnected - reconnect or Close");
			else if (state == 1)
				usbs->set_text(usbs->label, "#C7EA46 Status:# Boot keyboard (BIOS/UEFI; mouse unavailable)");
			else
				usbs->set_text(usbs->label, usbs->type == USB_HID_KEYBOARD_MOUSE
					? "#C7EA46 Status:# OS keyboard + mouse" : "#C7EA46 Status:# OS keyboard");
		}
	}

	// Parse input device.
	if (usbs->type == USB_HID_GAMEPAD)
	{
		if (_hid_poll_jc(usbs))
			return 1;
	}
	else if (usbs->type == USB_HID_TOUCHPAD)
	{
		if (_hid_poll_touch(usbs))
			return 1;
	}
	else if (usbs->type == USB_HID_KEYBOARD)
	{
		if (_hid_poll_keyboard(usbs))
			return 1;
	}
	else
	{
		if (_hid_poll_keyboard(usbs))
			return 1;
	}

	return 0;
}

void usb_device_gadget_hid_stop(usb_ctxt_t *usbs)
{
	if (!hid_started)
		return;
	// Bound shutdown even if the host stopped polling during reboot/disconnect.
	u32 deadline = get_tmr_ms() + 100;
	while (hid_tx_pending && (s32)(get_tmr_ms() - deadline) < 0)
	{
		usb_ops.usbd_handle_ep0_ctrl_setup(NULL);
		if (usb_ops.usb_device_hid_poll() != USB_ERROR_TIMEOUT)
			hid_tx_pending = false;
	}

	if (!hid_tx_pending && usb_ops.usb_device_hid_ready() && usbs->type == USB_HID_KEYBOARD)
	{
		usb_hid_keyboard_report_t release_report = {0};
		memcpy(rpt_buffer, &release_report, sizeof(release_report));
		usb_ops.usb_device_ep1_in_write(rpt_buffer, sizeof(release_report), NULL, 25000);
		usb_hid_keyboard_queue_clear();
	}
	else if (!hid_tx_pending && usb_ops.usb_device_hid_ready() && usbs->type == USB_HID_KEYBOARD_MOUSE)
	{
		usb_hid_keyboard_report_t keyboard_release = {0};
		usb_hid_mouse_report_t mouse_release = {0};
		u32 prefix = usb_ops.usb_device_get_hid_protocol() == 1;
		rpt_buffer[0] = USB_HID_REPORT_ID_KEYBOARD;
		memcpy(rpt_buffer + prefix, &keyboard_release, sizeof(keyboard_release));
		int status = usb_ops.usb_device_ep1_in_write(rpt_buffer, sizeof(keyboard_release) + prefix, NULL, 25000);
		if (prefix && status == USB_RES_OK)
		{
			rpt_buffer[0] = USB_HID_REPORT_ID_MOUSE;
			memcpy(rpt_buffer + 1, &mouse_release, sizeof(mouse_release));
			usb_ops.usb_device_ep1_in_write(rpt_buffer, sizeof(mouse_release) + 1, NULL, 25000);
		}
		usb_hid_keyboard_mouse_queue_clear();
	}

	usb_ops.usbd_end(true, false);
	hid_started = false;
}

int usb_device_gadget_hid(usb_ctxt_t *usbs)
{
	int res = usb_device_gadget_hid_start(usbs);
	if (res)
		return res;

	u32 timer_sys = get_tmr_ms() + 5000;
	while (true)
	{
		u32 timer = get_tmr_us();

		if (usb_device_gadget_hid_pump(usbs))
			break;

		timer = get_tmr_us() - timer;
		if (timer < hid_polling_time)
			usleep(hid_polling_time - timer);

		if (timer_sys < get_tmr_ms())
		{
			usbs->system_maintenance(true);
			timer_sys = get_tmr_ms() + 5000;
		}
	}

	usb_device_gadget_hid_stop(usbs);
	return 0;
}
