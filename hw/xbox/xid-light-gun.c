/*
 * QEMU USB XID Devices
 *
 * Copyright (c) 2013 espes
 * Copyright (c) 2017 Jannik Vogel
 * Copyright (c) 2018-2021 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "xid.h"

/*
 * Enumerate as an EMS TopGun II lightgun (XID subtype 0x50). Some titles
 * (e.g. Silent Scope Complete) only enable lightgun behaviour, such as
 * the scope zoom, for a gun peripheral they recognise by USB VID/PID, so
 * we present the TopGun II identity rather than a generic Microsoft one.
 */
#define USB_VENDOR_EMS 0x0b9a
#define USB_PRODUCT_TOPGUN_II 0x016b

#define LIGHT_GUN_IN_ENDPOINT_ID 0x02
#define LIGHT_GUN_OUT_ENDPOINT_ID 0x02

/* Dedicated string table: the shared desc_strings advertises a Microsoft
 * Xbox Controller, which would contradict the TopGun II VID/PID. */
static const USBDescStrings desc_strings_light_gun = {
    [STR_MANUFACTURER] = "EMS Production",
    [STR_PRODUCT]      = "TopGun II",
    [STR_SERIALNUMBER] = "1",
};

#define USB_XID(obj) \
    OBJECT_CHECK(USBXIDLightGunState, (obj), TYPE_USB_XID_LIGHT_GUN)

/*
 * Full 20-byte standard XID gamepad input report, as a real EMS TopGun II
 * sends. wButtons is 16-bit: the low byte carries the dpad/start/back
 * digital buttons, and bit 0x2000 (high byte 0x20) is
 * XINPUT_LIGHTGUN_ONSCREEN. Emitting the truncated 16-byte form made
 * Silent Scope treat the device as a plain controller and never enable
 * the scope; the trailing right-thumbstick fields are zero but must be
 * present so bLength is 20.
 */
typedef struct XIDLightGunReport {
    uint8_t bReportId;
    uint8_t bLength;
    uint16_t wButtons;
    uint8_t bAnalogButtons[8]; // The last 2 are the trigger slots
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
} QEMU_PACKED XIDLightGunReport;

typedef struct XIDLightGunCalibrationReport {
    uint8_t bReportId;
    uint8_t bLength;
    int16_t sCenterCalibrationX;
    int16_t sCenterCalibrationY;
    int16_t sTopLeftCalibrationX;
    int16_t sTopLeftCalibrationY;
} QEMU_PACKED XIDLightGunCalibrationReport;

typedef struct USBXIDLightGunState {
    USBDevice dev;
    USBEndpoint *intr;
    const XIDDesc *xid_desc;
    XIDLightGunReport in_state;
    XIDLightGunReport in_state_capabilities;
    XIDLightGunCalibrationReport out_state;
    XIDLightGunCalibrationReport out_state_capabilities;
    uint8_t device_index;
    // Calibration offsets set by the game via XInputSetLightgunCalibration
    // (HID SET_REPORT). Applied to the reported aim the way Cxbx-Reloaded
    // does; center offset near the middle, upper-left offset past half range.
    int16_t cal_center_x, cal_center_y;
    int16_t cal_upp_x, cal_upp_y;
} USBXIDLightGunState;

static const USBDescIface desc_iface_xbox_light_gun = {
    .bInterfaceNumber = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = USB_CLASS_XID,
    .bInterfaceSubClass = 0x42,
    .bInterfaceProtocol = 0x00,
    .eps =
        (USBDescEndpoint[]){
            {
                .bEndpointAddress = USB_DIR_IN | LIGHT_GUN_IN_ENDPOINT_ID,
                .bmAttributes = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize = 0x20,
                .bInterval = 4,
            },
            {
                .bEndpointAddress = USB_DIR_OUT | LIGHT_GUN_OUT_ENDPOINT_ID,
                .bmAttributes = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize = 0x20,
                .bInterval = 4,
            },
        },
};

static const USBDescDevice desc_device_xbox_light_gun = {
    .bcdUSB = 0x0110,
    .bMaxPacketSize0 = 0x40,
    .bNumConfigurations = 1,
    .confs =
        (USBDescConfig[]){
            {
                .bNumInterfaces = 1,
                .bConfigurationValue = 1,
                .bmAttributes = USB_CFG_ATT_ONE,
                .bMaxPower = 50,
                .nif = 1,
                .ifs = &desc_iface_xbox_light_gun,
            },
        },
};

static const USBDesc desc_xbox_light_gun = {
    .id = {
        .idVendor          = USB_VENDOR_EMS,
        .idProduct         = USB_PRODUCT_TOPGUN_II,
        .bcdDevice         = 0x0457,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT,
        .iSerialNumber     = STR_SERIALNUMBER,
    },
    .full = &desc_device_xbox_light_gun,
    .str  = desc_strings_light_gun,
};

static const XIDDesc desc_xid_xbox_light_gun = {
    .bLength = 0x10,
    .bDescriptorType = USB_DT_XID,
    .bcdXid = 0x100,
    .bType = XID_DEVICETYPE_GAMEPAD,
    .bSubType = XID_DEVICESUBTYPE_LIGHT_GUN,
    .bMaxInputReportSize = 20,
    .bMaxOutputReportSize = 6,
    .wAlternateProductIds = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF },
};

static void update_lg_input(USBXIDLightGunState *s)
{
    if (xemu_input_get_test_mode()) {
        // Don't report changes if we are testing the controller while running
        return;
    }

    ControllerState *state = xemu_input_get_bound(s->device_index);
    // assert(state);
    if (state == NULL)
        return;

    xemu_input_update_controller(state);

    s->in_state.wButtons = 0;
    if (state->lg.buttons & CONTROLLER_BUTTON_DPAD_UP)
        s->in_state.wButtons |= 0x01;
    if (state->lg.buttons & CONTROLLER_BUTTON_DPAD_DOWN)
        s->in_state.wButtons |= 0x02;
    if (state->lg.buttons & CONTROLLER_BUTTON_DPAD_LEFT)
        s->in_state.wButtons |= 0x04;
    if (state->lg.buttons & CONTROLLER_BUTTON_DPAD_RIGHT)
        s->in_state.wButtons |= 0x08;
    if (state->lg.buttons & CONTROLLER_BUTTON_START)
        s->in_state.wButtons |= 0x10;
    if (state->lg.buttons & CONTROLLER_BUTTON_BACK)
        s->in_state.wButtons |= 0x20;

    // Onscreen indicator lives in the high byte of the 16-bit wButtons
    // (XINPUT_LIGHTGUN_ONSCREEN == 0x2000); lg.status is 0x20 onscreen,
    // 0x00 offscreen, so shifting it into place reproduces that bit.
    s->in_state.wButtons |= ((uint16_t)state->lg.status) << 8;

    s->in_state.bAnalogButtons[0] =
        (state->lg.buttons & CONTROLLER_BUTTON_A) ? 0xFF : 0x00;
    s->in_state.bAnalogButtons[1] =
        (state->lg.buttons & CONTROLLER_BUTTON_B) ? 0xFF : 0x00;
    s->in_state.bAnalogButtons[2] =
        (state->lg.buttons & CONTROLLER_BUTTON_X) ? 0xFF : 0x00;
    s->in_state.bAnalogButtons[3] =
        (state->lg.buttons & CONTROLLER_BUTTON_Y) ? 0xFF : 0x00;
    s->in_state.bAnalogButtons[4] =
        (state->lg.buttons & CONTROLLER_BUTTON_BLACK) ? 0xFF : 0x00;
    s->in_state.bAnalogButtons[5] =
        (state->lg.buttons & CONTROLLER_BUTTON_WHITE) ? 0xFF : 0x00;
    s->in_state.bAnalogButtons[6] = state->lg.ltrig;
    s->in_state.bAnalogButtons[7] = state->lg.rtrig;

    // Aim is already pixel-perfect; do NOT apply the game's calibration
    // offsets (they destabilise the aim). We still accept the calibration
    // handshake, we just don't distort coordinates with it.
    s->in_state.sThumbLX = state->lg.axis[0];
    s->in_state.sThumbLY = state->lg.axis[1];
    s->in_state.sThumbRX = 0;
    s->in_state.sThumbRY = 0;

    // TEMP diagnostic (grep 'xid-lg-dbg'): log the report only when the
    // digital state changes, so button presses and the onscreen bit
    // (0x2000) are visible without per-frame spam.
    static uint16_t last_wButtons = 0xABCD;
    static uint8_t last_analog[8];
    if (s->in_state.wButtons != last_wButtons ||
        memcmp(s->in_state.bAnalogButtons, last_analog, 8) != 0) {
        last_wButtons = s->in_state.wButtons;
        memcpy(last_analog, s->in_state.bAnalogButtons, 8);
        fprintf(stderr,
                "xid-lg-dbg: wButtons=0x%04x onscreen=%d "
                "A=%d B=%d X=%d Y=%d Blk=%d Wht=%d LT=%d RT=%d aim=%d,%d\n",
                s->in_state.wButtons,
                (s->in_state.wButtons & 0x2000) ? 1 : 0,
                s->in_state.bAnalogButtons[0], s->in_state.bAnalogButtons[1],
                s->in_state.bAnalogButtons[2], s->in_state.bAnalogButtons[3],
                s->in_state.bAnalogButtons[4], s->in_state.bAnalogButtons[5],
                s->in_state.bAnalogButtons[6], s->in_state.bAnalogButtons[7],
                s->in_state.sThumbLX, s->in_state.sThumbLY);
    }
}

// Store the calibration offsets sent by XInputSetLightgunCalibration.
// Games send this on varying report values / pipes, so callers match by
// the report's length rather than a fixed value.
static void usb_xid_light_gun_set_calibration(
    USBXIDLightGunState *s, const XIDLightGunCalibrationReport *report)
{
    s->cal_center_x = le16_to_cpu(report->sCenterCalibrationX);
    s->cal_center_y = le16_to_cpu(report->sCenterCalibrationY);
    s->cal_upp_x = le16_to_cpu(report->sTopLeftCalibrationX);
    s->cal_upp_y = le16_to_cpu(report->sTopLeftCalibrationY);
    fprintf(stderr, "xid-lg-dbg: calibration center=%d,%d upper-left=%d,%d\n",
            s->cal_center_x, s->cal_center_y, s->cal_upp_x, s->cal_upp_y);
}

static void usb_xid_light_gun_handle_control(USBDevice *dev, USBPacket *p,
                                             int request, int value, int index,
                                             int length, uint8_t *data)
{
    USBXIDLightGunState *s = DO_UPCAST(USBXIDLightGunState, dev, dev);

    DPRINTF("xid light_gun handle_control 0x%x 0x%x\n", request, value);

    int ret =
        usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        DPRINTF("xid handled by usb_desc_handle_control: %d\n", ret);
        return;
    }

    // TEMP diagnostic (grep 'xid-lg-dbg'): log the XID-specific control
    // requests a game issues, to see what e.g. Silent Scope asks for.
    fprintf(stderr, "xid-lg-dbg: control request=0x%x value=0x%x index=0x%x "
                    "length=%d\n", request, value, index, length);

    switch (request) {
    /* HID requests */
    case ClassInterfaceRequest | HID_GET_REPORT:
        DPRINTF("xid GET_REPORT 0x%x\n", value);
        update_lg_input(s);
        if (value == 0x0100) { /* input */
            if (length <= s->in_state.bLength) {
                memcpy(data, &s->in_state, s->in_state.bLength);
                p->actual_length = length;
            } else {
                p->status = USB_RET_STALL;
            }
        } else {
            p->status = USB_RET_STALL;
        }
        break;
    case ClassInterfaceOutRequest | HID_SET_REPORT:
        DPRINTF("xid SET_REPORT 0x%x\n", value);
        // Match XInputSetLightgunCalibration by length, not by report value:
        // games send it on different values, and requiring one specific
        // value made the call stall, leaving lightgun features that depend
        // on a successful calibration handshake (e.g. Silent Scope's scope
        // zoom) disabled even though basic aim/fire worked.
        if (length == sizeof(XIDLightGunCalibrationReport)) {
            XIDLightGunCalibrationReport report;
            memcpy(&report, data, sizeof(report));
            usb_xid_light_gun_set_calibration(s, &report);
            p->actual_length = length;
        } else if (value == 0x0200 &&
                   length == sizeof(XIDGamepadOutputReport)) {
            // Rumble; a real light gun has none, accept and ignore.
            p->actual_length = length;
        } else {
            p->status = USB_RET_STALL;
        }
        break;
    /* XID requests */
    case VendorInterfaceRequest | USB_REQ_GET_DESCRIPTOR:
        DPRINTF("xid GET_DESCRIPTOR 0x%x\n", value);
        if (value == 0x4200 && s->xid_desc->bLength <= length) {
            memcpy(data, s->xid_desc, s->xid_desc->bLength);
            p->actual_length = s->xid_desc->bLength;
        } else {
            p->status = USB_RET_STALL;
        }
        break;
    case VendorInterfaceRequest | XID_GET_CAPABILITIES:
        DPRINTF("xid XID_GET_CAPABILITIES 0x%x\n", value);
        if (value == 0x0100) {
            if (length > s->in_state_capabilities.bLength) {
                length = s->in_state_capabilities.bLength;
            }
            memcpy(data, &s->in_state_capabilities, length);
            p->actual_length = length;
        } else if (value == 0x0200) {
            if (length > s->out_state_capabilities.bLength) {
                length = s->out_state_capabilities.bLength;
            }
            memcpy(data, &s->out_state_capabilities, length);
            p->actual_length = length;
        } else {
            p->status = USB_RET_STALL;
        }
        break;
    case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE) << 8) |
        USB_REQ_GET_DESCRIPTOR:
        /* FIXME: ! */
        DPRINTF("xid unknown xpad request 0x%x: value = 0x%x\n", request,
                value);
        memset(data, 0x00, length);
        // FIXME: Intended for the hub: usbd_get_hub_descriptor, UT_READ_CLASS?!
        p->status = USB_RET_STALL;
        // assert(false);
        break;
    case ((USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT) << 8) |
        USB_REQ_CLEAR_FEATURE:
        /* FIXME: ! */
        DPRINTF("xid unknown xpad request 0x%x: value = 0x%x\n", request,
                value);
        memset(data, 0x00, length);
        p->status = USB_RET_STALL;
        break;
    default:
        DPRINTF("xid USB stalled on request 0x%x value 0x%x\n", request, value);
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_xid_light_gun_handle_data(USBDevice *dev, USBPacket *p)
{
    USBXIDLightGunState *s = DO_UPCAST(USBXIDLightGunState, dev, dev);

    DPRINTF("xid light_gun handle_gamepad_data 0x%x %d 0x%zx\n", p->pid,
            p->ep->nr, p->iov.size);

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr == LIGHT_GUN_IN_ENDPOINT_ID) {
            update_lg_input(s);
            usb_packet_copy(p, &s->in_state, s->in_state.bLength);
        } else {
            p->status = USB_RET_STALL;
        }
        break;
    case USB_TOKEN_OUT:
        if (p->ep->nr == LIGHT_GUN_OUT_ENDPOINT_ID) {
            // Calibration may also arrive over the interrupt OUT pipe;
            // match it by size. Anything else (rumble) is accepted+ignored.
            if (p->iov.size == sizeof(XIDLightGunCalibrationReport)) {
                XIDLightGunCalibrationReport report;
                usb_packet_copy(p, &report, sizeof(report));
                usb_xid_light_gun_set_calibration(s, &report);
            }
        } else {
            p->status = USB_RET_STALL;
        }
        break;
    default:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_xid_light_gun_class_initfn(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->handle_reset = usb_xid_handle_reset;
    uc->handle_control = usb_xid_light_gun_handle_control;
    uc->handle_data = usb_xid_light_gun_handle_data;
    // uc->handle_destroy = usb_xid_handle_destroy;
    uc->handle_attach = usb_desc_attach;
}

static void usb_xbox_light_gun_realize(USBDevice *dev, Error **errp)
{
    USBXIDLightGunState *s = USB_XID(dev);
    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, 2);

    s->in_state.bLength = sizeof(s->in_state);
    s->in_state.bReportId = 0;

    s->out_state.bLength = sizeof(s->out_state);
    s->out_state.bReportId = 0;

    s->xid_desc = &desc_xid_xbox_light_gun;

    memset(&s->in_state_capabilities, 0xFF, sizeof(s->in_state_capabilities));
    s->in_state_capabilities.bLength = sizeof(s->in_state_capabilities);
    s->in_state_capabilities.bReportId = 0;

    memset(&s->out_state_capabilities, 0xFF, sizeof(s->out_state_capabilities));
    s->out_state_capabilities.bLength = sizeof(s->out_state_capabilities);
    s->out_state_capabilities.bReportId = 0;
}

static const Property xid_properties[] = {
    DEFINE_PROP_UINT8("index", USBXIDLightGunState, device_index, 0),
};

static const VMStateDescription vmstate_usb_xbox = {
    .name = TYPE_USB_XID_LIGHT_GUN,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){ VMSTATE_USB_DEVICE(dev, USBXIDLightGunState),
                                // FIXME
                                VMSTATE_END_OF_LIST() },
};

static void usb_xbox_light_gun_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc = "EMS TopGun II Light Gun";
    uc->usb_desc = &desc_xbox_light_gun;
    uc->realize = usb_xbox_light_gun_realize;
    uc->unrealize = usb_xbox_gamepad_unrealize;
    usb_xid_light_gun_class_initfn(klass, data);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->vmsd = &vmstate_usb_xbox;
    device_class_set_props(dc, xid_properties);
    dc->desc = "EMS TopGun II Light Gun";
}

static const TypeInfo usb_xbox_light_gun_info = {
    .name = TYPE_USB_XID_LIGHT_GUN,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBXIDLightGunState),
    .class_init = usb_xbox_light_gun_class_initfn,
};

static void usb_xid_register_types(void)
{
    type_register_static(&usb_xbox_light_gun_info);
}

type_init(usb_xid_register_types)