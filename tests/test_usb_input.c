#include <stdio.h>
#include <assert.h>
#include "../bdk/usb/usb_gadget_hid.c"
#include "../bdk/usb/usb_descriptors.c"

static u32 clock_us;
static u8 protocol = 1;
static bool ready = true;
static bool busy;
static u8 packet[16];
static u32 packet_size;
static u32 writes;

u32 get_tmr_us(void) { return clock_us; }
u32 get_tmr_ms(void) { return clock_us / 1000; }
static u8 mock_protocol(void) { return protocol; }
static bool mock_ready(void) { return ready; }
static int mock_poll(void) { return busy ? USB_ERROR_TIMEOUT : USB_RES_OK; }
static int mock_write(u8 *data, u32 size, u32 *written, u32 timeout)
{
    (void)written;
    assert(timeout == USB_XFER_START);
    assert(!busy);
    memcpy(packet, data, size);
    packet_size = size;
    busy = true;
    writes++;
    return 0;
}

static void reset(void)
{
    usb_hid_keyboard_set_exit_requested(false);
    usb_hid_keyboard_reset_reports();
    hid_tx_pending = false;
    clock_us = 100000;
    writes = 0;
    busy = false;
    ready = true;
    protocol = 1;
}

static void finish_and_advance(usb_ctxt_t *ctx)
{
    busy = false;
    assert(!_hid_poll_keyboard(ctx));
    clock_us += 40001;
}

static void check_descriptor(const u8 *desc, u32 size, bool combined)
{
    u32 input_bits[3] = {0}, output_bits[3] = {0};
    u32 count = 0, bits = 0, id = 0;
    for (u32 offset = 0; offset < size;)
    {
        u8 prefix = desc[offset++];
        u32 bytes = prefix & 3;
        if (bytes == 3) bytes = 4;
        assert(prefix != 0xfe && offset + bytes <= size);
        u32 value = 0;
        for (u32 b = 0; b < bytes; b++) value |= desc[offset++] << (b * 8);
        u8 type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1 && tag == 7) bits = value;
        if (type == 1 && tag == 8) id = value;
        if (type == 1 && tag == 9) count = value;
        assert(id < 3);
        if (type == 0 && tag == 8) input_bits[id] += bits * count;
        if (type == 0 && tag == 9) output_bits[id] += bits * count;
    }
    assert(input_bits[combined ? 1 : 0] == 64);
    assert(output_bits[combined ? 1 : 0] == 8);
    if (combined) assert(input_bits[2] == 32 && input_bits[0] == 0);
}

int main(void)
{
    usb_ops.usb_device_hid_ready = mock_ready;
    usb_ops.usb_device_get_hid_protocol = mock_protocol;
    usb_ops.usb_device_hid_poll = mock_poll;
    usb_ops.usb_device_ep1_in_write = mock_write;
    rpt_buffer = packet;
    usb_ctxt_t ctx = { .type = USB_HID_KEYBOARD_MOUSE };
    usb_hid_keyboard_report_t esc = { .keycode = {0x29} };
    u8 ctrl[16];

    usb_cfg_hid_descr_t *cfg = (void *)usb_gadget_hid_keyboard_mouse_descriptors.cfg;
    assert(cfg->interface.bInterfaceSubClass == 1);
    assert(cfg->interface.bInterfaceProtocol == 1);
    assert(cfg->hid.bDescriptorLength == sizeof(hid_report_descriptor_keyboard_mouse));
    check_descriptor(hid_report_descriptor_keyboard, sizeof(hid_report_descriptor_keyboard), false);
    check_descriptor(hid_report_descriptor_keyboard_mouse, sizeof(hid_report_descriptor_keyboard_mouse), true);

    reset();
    assert(usb_hid_keyboard_queue_tap(&esc, true));
    assert(!_hid_poll_keyboard(&ctx));
    assert(writes == 1 && packet_size == 9 && packet[0] == 1 && packet[3] == 0x29);
    assert(!_hid_poll_keyboard(&ctx) && writes == 1); // Busy: do not overwrite DMA.
    assert(usb_hid_keyboard_get_report(USB_GADGET_HID_KEYBOARD_MOUSE, 1, 0x0101, ctrl) == 9);
    assert(ctrl[3] == 0x29);
    finish_and_advance(&ctx);
    assert(!_hid_poll_keyboard(&ctx));
    assert(writes == 2 && packet_size == 9 && packet[3] == 0);

    reset();
    protocol = 0;
    assert(usb_hid_keyboard_queue_tap(&esc, true));
    assert(!_hid_poll_keyboard(&ctx));
    assert(packet_size == 8 && packet[0] == 0 && packet[2] == 0x29);
    assert(usb_hid_keyboard_get_report(USB_GADGET_HID_KEYBOARD_MOUSE, 0, 0x0100, ctrl) == 8);
    assert(ctrl[2] == 0x29);
    finish_and_advance(&ctx);
    assert(!_hid_poll_keyboard(&ctx) && packet[2] == 0);
    finish_and_advance(&ctx);
    usb_hid_mouse_report_t move = { .x = 12 };
    assert(usb_hid_keyboard_mouse_queue_mouse(&move));
    assert(!_hid_poll_keyboard(&ctx) && writes == 2); // No mouse in boot protocol.

    reset();
    ctx.type = USB_HID_KEYBOARD;
    assert(usb_hid_keyboard_queue_tap(&esc, false));
    assert(!_hid_poll_keyboard(&ctx) && packet_size == 8 && packet[2] == 0x29);
    assert(usb_hid_keyboard_get_report(USB_GADGET_HID_KEYBOARD, 1, 0x0101, ctrl) == 0);

    reset();
    ctx.type = USB_HID_KEYBOARD_MOUSE;
    assert(usb_hid_keyboard_set_idle(USB_GADGET_HID_KEYBOARD_MOUSE, 0x0701));
    assert(usb_hid_keyboard_get_idle(USB_GADGET_HID_KEYBOARD_MOUSE, 1) == 7);
    assert(usb_hid_keyboard_get_idle(USB_GADGET_HID_KEYBOARD_MOUSE, 2) == 0);
    assert(usb_hid_keyboard_set_idle(USB_GADGET_HID_KEYBOARD_MOUSE, 0x0300));
    assert(usb_hid_keyboard_get_idle(USB_GADGET_HID_KEYBOARD_MOUSE, 2) == 3);
    assert(!usb_hid_keyboard_set_idle(USB_GADGET_HID_KEYBOARD_MOUSE, 0x0103));
    assert(usb_hid_keyboard_get_report(USB_GADGET_HID_KEYBOARD_MOUSE, 1, 0x0201, ctrl) == 2);
    assert(usb_hid_keyboard_get_report(USB_GADGET_HID_KEYBOARD_MOUSE, 1, 0x0202, ctrl) == 0);
    assert(usb_hid_keyboard_get_report(USB_GADGET_HID_KEYBOARD, 0, 0x0200, ctrl) == 1);

    reset();
    assert(usb_hid_keyboard_set_idle(USB_GADGET_HID_KEYBOARD_MOUSE, 0x0102));
    assert(usb_hid_keyboard_mouse_queue_mouse(&move));
    assert(!_hid_poll_keyboard(&ctx) && packet_size == 5 && packet[2] == 12);
    finish_and_advance(&ctx);
    assert(!_hid_poll_keyboard(&ctx) && packet[2] == 0); // Never repeat relative motion.

    for (int combined = 0; combined <= 1; combined++)
    {
        reset();
        for (int i = 0; i < 31; i++)
            assert(usb_hid_keyboard_queue_tap(&esc, combined));
        assert(!usb_hid_keyboard_queue_tap(&esc, combined));
        for (int i = 0; i < 62; i++)
        {
            usb_hid_keyboard_report_t report;
            if (combined)
            {
                usb_hid_keyboard_mouse_entry_t entry;
                assert(_usb_hid_keyboard_mouse_dequeue(&entry));
                memcpy(&report, entry.data, sizeof(report));
            }
            else
                assert(_usb_hid_keyboard_dequeue_report(&report));
            assert(report.keycode[0] == (i % 2 ? 0 : 0x29));
        }
    }
    reset();
    ready = false;
    assert(usb_hid_keyboard_queue_tap(&esc, true));
    assert(!_hid_poll_keyboard(&ctx) && writes == 0);
    ready = true;
    assert(!_hid_poll_keyboard(&ctx) && writes == 1);
    usb_hid_keyboard_set_exit_requested(true);
    usb_hid_keyboard_reset_reports();
    assert(usb_hid_keyboard_get_exit_requested());
    reset();
    assert(usb_hid_keyboard_queue_tap(&esc, true));
    assert(!_hid_poll_keyboard(&ctx) && writes == 1);
    hid_started = true;
    protocol = 0;
    usb_hid_keyboard_reset_reports();
    assert(!_hid_poll_keyboard(&ctx) && writes == 1); // Still owned by DMA.
    busy = false;
    assert(!_hid_poll_keyboard(&ctx) && writes == 2);
    assert(packet_size == 8 && packet[2] == 0); // New protocol starts with release.
    busy = false;
    usb_hid_keyboard_reset_reports();
    ready = false;
    assert(!_hid_poll_keyboard(&ctx) && writes == 2);
    ready = true;
    assert(!_hid_poll_keyboard(&ctx) && writes == 3 && packet[2] == 0);
    hid_started = false;
    puts("USB input tests passed: boot/report packets, controls, idle, atomic queues, suspend and reset");
    return 0;
}
