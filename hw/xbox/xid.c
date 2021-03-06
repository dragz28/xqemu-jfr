/*
 * QEMU USB XID Devices
 *
 * Copyright (c) 2013 espes
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/hw.h"
#include "ui/console.h"
#include "hw/usb.h"
#include "hw/usb/desc.h"

#define DEBUG_XID
#ifdef DEBUG_XID
#define DPRINTF printf
#else
#define DPRINTF(...)
#endif

/*
 * http://xbox-linux.cvs.sourceforge.net/viewvc/xbox-linux/kernel-2.6/drivers/usb/input/xpad.c
 * http://euc.jp/periphs/xbox-controller.en.html
 * http://euc.jp/periphs/xbox-pad-desc.txt
 */

#define USB_CLASS_XID  0x58
#define USB_DT_XID     0x42


#define HID_GET_REPORT       0x01
#define HID_SET_REPORT       0x09
#define XID_GET_CAPABILITIES 0x01

typedef struct XIDDesc {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdXid;
    uint8_t bType;
    uint8_t bSubType;
    uint8_t bMaxInputReportSize;
    uint8_t bMaxOutputReportSize;
    uint16_t wAlternateProductIds[4];
} QEMU_PACKED XIDDesc;

typedef struct XIDGamepadReport {
    uint8_t bReportId;
    uint8_t bLength;
    uint16_t wButtons;
    uint8_t bAnalogButtons[8];
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
} QEMU_PACKED XIDGamepadReport;

typedef struct XIDGamepadOutputReport {
    uint8_t report_id; //FIXME: is this correct?
    uint8_t length;
    uint16_t left_actuator_strength;
    uint16_t right_actuator_strength;
} QEMU_PACKED XIDGamepadOutputReport;


typedef struct USBXIDState {
    USBDevice dev;
    USBEndpoint *intr;

    const XIDDesc *xid_desc;

    QEMUPutKbdEntry *kbd_entry;
    XIDGamepadReport in_state;
    XIDGamepadOutputReport out_state;
} USBXIDState;

static const USBDescIface desc_iface_xbox_gamepad = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 2,
    .bInterfaceClass               = USB_CLASS_XID,
    .bInterfaceSubClass            = 0x42,
    .bInterfaceProtocol            = 0x00,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 0x20,
            .bInterval             = 4,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 0x20,
            .bInterval             = 4,
        },
    },
};

static const USBDescDevice desc_device_xbox_gamepad = {
    .bcdUSB                        = 0x0110,
    .bMaxPacketSize0               = 0x40,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .bmAttributes          = 0x80,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_xbox_gamepad,
        },
    },
};

static const USBDesc desc_xbox_gamepad = {
    .id = {
        .idVendor          = 0x045e,
        .idProduct         = 0x0202,
        .bcdDevice         = 0x0100,
    },
    .full = &desc_device_xbox_gamepad,
};

static const XIDDesc desc_xid_xbox_gamepad = {
    .bLength = 0x10,
    .bDescriptorType = USB_DT_XID,
    .bcdXid = 1,
    .bType = 1,
    .bSubType = 1,
    .bMaxInputReportSize = 0x20,
    .bMaxOutputReportSize = 0x6,
    .wAlternateProductIds = {-1, -1, -1, -1},
};


#define GAMEPAD_A                0
#define GAMEPAD_B                1
#define GAMEPAD_X                2
#define GAMEPAD_Y                3
#define GAMEPAD_BLACK            4
#define GAMEPAD_WHITE            5
#define GAMEPAD_LEFT_TRIGGER     6
#define GAMEPAD_RIGHT_TRIGGER    7

#define GAMEPAD_DPAD_UP          8
#define GAMEPAD_DPAD_DOWN        9
#define GAMEPAD_DPAD_LEFT        10
#define GAMEPAD_DPAD_RIGHT       11
#define GAMEPAD_START            12
#define GAMEPAD_BACK             13
#define GAMEPAD_LEFT_THUMB       14
#define GAMEPAD_RIGHT_THUMB      15

static const int gamepad_mapping[] = {
    [0 ... Q_KEY_CODE_MAX] = -1,

    [Q_KEY_CODE_UP]    = GAMEPAD_DPAD_UP,
    [Q_KEY_CODE_KP_8]  = GAMEPAD_DPAD_UP,
    [Q_KEY_CODE_DOWN]  = GAMEPAD_DPAD_DOWN,
    [Q_KEY_CODE_KP_2]  = GAMEPAD_DPAD_DOWN,
    [Q_KEY_CODE_LEFT]  = GAMEPAD_DPAD_LEFT,
    [Q_KEY_CODE_KP_4]  = GAMEPAD_DPAD_LEFT,
    [Q_KEY_CODE_RIGHT] = GAMEPAD_DPAD_RIGHT,
    [Q_KEY_CODE_KP_6]  = GAMEPAD_DPAD_RIGHT,

    [Q_KEY_CODE_RET]   = GAMEPAD_START,
    [Q_KEY_CODE_BACKSPACE] = GAMEPAD_BACK,

    [Q_KEY_CODE_Z]     = GAMEPAD_A,
    [Q_KEY_CODE_X]     = GAMEPAD_B,
    [Q_KEY_CODE_A]     = GAMEPAD_X,
    [Q_KEY_CODE_S]     = GAMEPAD_Y,
};

static void xbox_gamepad_keyboard_event(void *opaque, int keycode)
{
    USBXIDState *s = opaque;
    bool up = keycode & 0x80;
    uint8_t key = keycode & 0x7F;
#if 1
    uint16_t mask = 0x0000;
    if (key == 0x1e) {
        s->in_state.bAnalogButtons[GAMEPAD_A] = up?0:0xff;
    } else if (key == 0x30) {
        s->in_state.bAnalogButtons[GAMEPAD_B] = up?0:0xff;
    } else if (key == 0x2d) {
        s->in_state.bAnalogButtons[GAMEPAD_X] = up?0:0xff;
    } else if ((key == 0x15) || (key == 0x2c)) {
        s->in_state.bAnalogButtons[GAMEPAD_Y] = up?0:0xff;
    } else if (key == 0x38) {
        s->in_state.bAnalogButtons[GAMEPAD_BLACK] = up?0:0xff;
    } else if (key == 0x1d) {
        s->in_state.bAnalogButtons[GAMEPAD_WHITE] = up?0:0xff;
    } else if (key == 0x26) {
        s->in_state.bAnalogButtons[GAMEPAD_LEFT_TRIGGER] = up?0:0xff;
    } else if (key == 0x13) {
        s->in_state.bAnalogButtons[GAMEPAD_RIGHT_TRIGGER] = up?0:0xff;
    } else if (key == 0x53) { //FIXME: These won't work!
        mask = (1 << (GAMEPAD_DPAD_UP-GAMEPAD_DPAD_UP));
    } else if (key == 0x54) {
        mask = (1 << (GAMEPAD_DPAD_DOWN-GAMEPAD_DPAD_UP));
    } else if (key == 0x4f) {
        mask = (1 << (GAMEPAD_DPAD_LEFT-GAMEPAD_DPAD_UP));
    } else if (key == 0x59) {
        mask = (1 << (GAMEPAD_DPAD_RIGHT-GAMEPAD_DPAD_UP));
    } else if (key == 0x1c) {
        mask = (1 << (GAMEPAD_START-GAMEPAD_DPAD_UP));
    } else if (key == 0x0e) {
        mask = (1 << (GAMEPAD_BACK-GAMEPAD_DPAD_UP));
    } else {
        fprintf(stderr,"Unknown key 0x%02x. Broken in v2.0.0 until Gerds new input layer is merged.\n"
                       "Mapping Start (Enter), Back (Backspace), Black (Ctrl-L), White (Alt-L), LT (L), RT (R), Digital-Pad (Arrow keys), A (A), B (B), X (X) and Y (Y/Z) to keyboard.\n",
                        key);
    }
    s->in_state.wButtons &= ~mask;
    if (!up) s->in_state.wButtons |= mask;
#else
    QKeyCode code = index_from_keycode(keycode & 0x7f);
    if (code >= Q_KEY_CODE_MAX) return;

    int button = gamepad_mapping[code];

    DPRINTF("xid keyboard_event %x - %d %d %d\n", keycode, code, button, up);

    uint16_t mask;
    switch (button) {
    case GAMEPAD_A ... GAMEPAD_RIGHT_TRIGGER:
        s->in_state.bAnalogButtons[button] = up?0:0xff;
        break;
    case GAMEPAD_DPAD_UP ... GAMEPAD_RIGHT_THUMB:
        mask = (1 << (button-GAMEPAD_DPAD_UP));
        s->in_state.wButtons &= ~mask;
        if (!up) s->in_state.wButtons |= mask;
        break;
    default:
        break;
    }
#endif
}


static void usb_xid_handle_reset(USBDevice *dev)
{
    DPRINTF("xid reset\n");
}

static void usb_xid_handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    USBXIDState *s = DO_UPCAST(USBXIDState, dev, dev);

    static unsigned int pid = 0;
    static size_t bytes = 0;
    DPRINTF("xid handle_control 0x%x 0x%x, packet %zu, byte %d\n", request, value, ++pid, bytes += length);
#if 0
    if ((request == USB_REQ_SET_CONFIGURATION) && (value == 1)) {
        /* The OpenXDK code will try to set configuration 1 on every read */
        DPRINTF("xid handled by configuration hack\n");
        return;
    }
#endif
    int ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        DPRINTF("xid handled by usb_desc_handle_control: %d\n",ret);
        return;
    }

    switch (request) {
    /* HID requests */
    case ClassInterfaceRequest | HID_GET_REPORT:
        DPRINTF("xid GET_REPORT 0x%x\n", value);
        if (value == 0x100) { /* input */
            assert(s->in_state.bLength <= length);
//            s->in_state.bReportId++; FIXME: Like this?
            memcpy(data, &s->in_state, s->in_state.bLength);
            p->actual_length = s->in_state.bLength;
        } else {
            assert(false);
        }
        break;
    case ClassInterfaceOutRequest | HID_SET_REPORT:
        DPRINTF("xid SET_REPORT 0x%x\n", value);
        if (value == 0x200) { /* output */
            /* Read length, then the entire packet */
            memcpy(&s->out_state, data, sizeof(s->out_state));
            assert(s->out_state.length == sizeof(s->out_state));
            assert(s->out_state.length <= length);
            //FIXME: Check actuator endianess
            printf("Set rumble power to 0x%x, 0x%x\n",
                   s->out_state.left_actuator_strength,
                   s->out_state.right_actuator_strength);
            p->actual_length = s->out_state.length;
        } else {
            assert(false);
        }
        break;
    /* XID requests */
    case VendorInterfaceRequest | USB_REQ_GET_DESCRIPTOR:
        DPRINTF("xid GET_DESCRIPTOR 0x%x\n", value);
        if (value == 0x4200) {
            assert(s->xid_desc->bLength <= length);
            memcpy(data, s->xid_desc, s->xid_desc->bLength);
            p->actual_length = s->xid_desc->bLength;
        } else {
            assert(false);
        }
        break;
    case VendorInterfaceRequest | XID_GET_CAPABILITIES:
        DPRINTF("xid XID_GET_CAPABILITIES 0x%x\n", value);
        //FIXME: !
        p->status = USB_RET_STALL;
        //assert(false);
        break;
    case ((USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_DEVICE)<<8) | 0x06:
        DPRINTF("xid unknown xpad request 1: value = 0x%x\n", value);
        memset(data, 0x00, length);
        //FIXME: Intended for the hub: usbd_get_hub_descriptor, UT_READ_CLASS?!
        p->status = USB_RET_STALL;
        //assert(false);
        break;
    default:
        DPRINTF("xid USB stalled on request 0x%x value 0x%x\n", request, value);
        p->status = USB_RET_STALL;
        assert(false);
        break;
    }
}

static void usb_xid_handle_data(USBDevice *dev, USBPacket *p)
{
    USBXIDState *s = DO_UPCAST(USBXIDState, dev, dev);

    DPRINTF("xid handle_data 0x%x %d 0x%zx\n", p->pid, p->ep->nr, p->iov.size);

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr == 2) {
            usb_packet_copy(p, &s->in_state, s->in_state.bLength);
        } else {
            assert(false);
        }
        break;
    case USB_TOKEN_OUT:
        p->status = USB_RET_STALL;
        break;
    default:
        p->status = USB_RET_STALL;
        assert(false);
        break;
    }
}

static void usb_xid_handle_destroy(USBDevice *dev)
{
    DPRINTF("xid handle_destroy\n");
}

static void usb_xid_class_initfn(ObjectClass *klass, void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->handle_reset   = usb_xid_handle_reset;
    uc->handle_control = usb_xid_handle_control;
    uc->handle_data    = usb_xid_handle_data;
    uc->handle_destroy = usb_xid_handle_destroy;
    uc->handle_attach  = usb_desc_attach;
}

static int usb_xbox_gamepad_initfn(USBDevice *dev)
{
    USBXIDState *s = DO_UPCAST(USBXIDState, dev, dev);
    usb_desc_init(dev);
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, 2);

    s->in_state.bLength = sizeof(s->in_state);
    s->out_state.length = sizeof(s->out_state);
    s->kbd_entry = qemu_add_kbd_event_handler(xbox_gamepad_keyboard_event, s);
    s->xid_desc = &desc_xid_xbox_gamepad;

    return 0;
}

static void usb_xbox_gamepad_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    usb_xid_class_initfn(klass, data);
    uc->init           = usb_xbox_gamepad_initfn;
    uc->product_desc   = "Microsoft Xbox Controller";
    uc->usb_desc       = &desc_xbox_gamepad;
    //dc->vmsd = &vmstate_usb_kbd;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_xbox_gamepad_info = {
    .name          = "usb-xbox-gamepad",
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBXIDState),
    .class_init    = usb_xbox_gamepad_class_initfn,
};

static void usb_xid_register_types(void)
{
    type_register_static(&usb_xbox_gamepad_info);
}

type_init(usb_xid_register_types)
