/*
 * QEMU Chihiro USB Devices
 *
 * Copyright (c) 2016 espes
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

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "ui/console.h"
#include "hw/usb.h"
#include "hw/usb/desc.h"
#include "qapi/error.h"

#include "qemu/timer.h"
#include "system/system.h"
#include "ui/xemu-settings.h"
#include "chihiro-firmware.h"
#include "chihiro-jvs.h"
#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))

/*
 * The baseboard has a single 24LC024 (ic11). Both AN2131 chips (QC and SC)
 * reach the same physical part, so the contents must be shared state rather
 * than a per-device copy — otherwise a write serviced by one chip is invisible
 * to reads serviced by the other, and each chip flushes its own divergent copy
 * over the backup file.
 */
#define CHIHIRO_IC11_SIZE 512

static void chihiro_ic11_init(void);
static void chihiro_ic11_mark_dirty(void);

#define DEBUG_CUSB
#ifdef DEBUG_CUSB
#define DPRINTF(s, ...) do { } while(0)
#else
#define DPRINTF(...)
#endif

typedef struct ChihiroUSBState {
    USBDevice dev;

    /* Device identity (set at realize, safe to use in all callbacks) */
    bool is_qc;  /* true = AN2131QC (PID 0x0002), false = AN2131SC (PID 0x0003) */

    /* I2C EEPROM data (8KB): ic10 for QC, pc20 for SC — loaded at realize */
    uint8_t eeprom[8192];

    /* Per-endpoint bulk IN buffers (EP1–EP5, index 0 unused).
     *
     * Sized for the largest transfer a single vendor request can queue: a read
     * of the AN2131's external memory (0x18), which spans the whole 64KB xdata
     * space. Crazy Taxi asks for 11752 bytes of it in one go once it finds
     * saved settings; a smaller buffer silently truncates the reply, the next
     * bulk IN stalls, and the game blocks forever waiting for the rest. */
    #define CHIHIRO_USB_MAX_EP 6
    #define CHIHIRO_USB_EP_BUFSZ 65536
    struct {
        uint8_t buf[CHIHIRO_USB_EP_BUFSZ];
        int pending;
        int offset;
    } ep_in[CHIHIRO_USB_MAX_EP];

    /* EZ-USB firmware download state (ANCHOR_LOAD / bRequest 0xA0) */
    uint32_t fw_bytes_written;  /* total bytes received via 0xA0 */
    bool fw_cpu_held;           /* true if CPUCS register set to hold CPU */

    /* Pending write tracking for 0x1E (ic11 via EP2) and 0x1F (extmem via EP3) */
    uint16_t write_1e_addr;
    uint16_t write_1f_addr;

    /* SC UART buffers (for JVS communication) */
    uint8_t uart0_rx[256];  /* UART0 receive buffer */
    int uart0_rx_len;
    uint8_t uart1_rx[256];  /* UART1 / JVS receive buffer */
    int uart1_rx_len;

    /* v202: instrumentation counters (read/reset by DIAG timer) */
    uint32_t nak_count;    /* bulk IN NAK count since last report */
    uint32_t bulk_in_count;  /* successful bulk IN count */
    uint32_t bulk_out_count; /* bulk OUT count */

    /* v302: EZ-USB firmware reboot simulation timers.
     * Real AN2131 loads firmware from EEPROM after initial enumeration,
     * then disconnects and reconnects. SEGABOOT waits for the CSC. */
    QEMUTimer *ezusb_disconnect_timer;
    QEMUTimer *ezusb_reconnect_timer;
    bool ezusb_rebooted;

    /* JVS I/O board emulation state (shared between QC and SC paths) */
    ChihiroJVSState jvs;
} ChihiroUSBState;

/* The one baseboard ic11, shared by QC and SC. "ACBU0001" + game ID. */
static uint8_t chihiro_ic11[CHIHIRO_IC11_SIZE];

/*
 * The AN2131's external memory, likewise one physical thing behind both chips.
 *
 * The upper half is the baseboard's backup RAM: battery-backed on real
 * hardware, and where the games actually keep their settings and bookkeeping.
 * Ghost Squad holds its state at 0x8000..0x83FF and Crazy Taxi at
 * 0x8400..0xB22B; neither ever touches an address below CHIHIRO_BRAM_BASE, so
 * only the upper half is persisted — the lower half is 8051 working memory
 * that has no business surviving a reboot.
 *
 * Without this, a game finds the region zeroed on every boot, concludes it has
 * never been configured, and drops the operator into the service menu — no
 * matter what was saved in ic11.
 */
static uint8_t chihiro_extmem[65536];

#define CHIHIRO_BRAM_BASE 0x8000
#define CHIHIRO_BRAM_SIZE (sizeof(chihiro_extmem) - CHIHIRO_BRAM_BASE)

static void chihiro_bram_init(void);
static void chihiro_bram_mark_dirty(uint32_t addr);

enum chihiro_usb_strings {
    STRING_SERIALNUMBER,
    STRING_MANUFACTURER,
    STRING_PRODUCT,
};

static const USBDescStrings chihiro_usb_stringtable = {
    [STRING_SERIALNUMBER]       = "\x00",
    [STRING_MANUFACTURER]       = "SEGA",
    [STRING_PRODUCT]            = "BASEBD" // different for qc?
};

static const USBDescIface desc_iface_chihiro_an2131qc = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 10,
    .bInterfaceClass               = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass            = 0x00,
    .bInterfaceProtocol            = 0x00,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x04,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x05,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x04,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x05,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
    },
};

static const USBDescDevice desc_device_chihiro_an2131qc = {
    .bcdUSB                        = 0x0100,
    .bDeviceClass                  = 0x60,
    .bDeviceSubClass               = 0x00,
    .bDeviceProtocol               = 0x00,
    .bMaxPacketSize0               = 0x40,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .bmAttributes          = 0x80,
            .bMaxPower             = 0x96,
            .nif = 1,
            .ifs = &desc_iface_chihiro_an2131qc,
        },
    },
};

static const USBDesc desc_chihiro_an2131qc = {
    .id = {
        .idVendor          = 0x0CA3,
        .idProduct         = 0x0002,
        .bcdDevice         = 0x0108,
        .iManufacturer     = STRING_MANUFACTURER,
        .iProduct          = STRING_PRODUCT,
        .iSerialNumber     = STRING_SERIALNUMBER,
    },
    .full = &desc_device_chihiro_an2131qc,
    .str  = chihiro_usb_stringtable,
};

static const USBDescIface desc_iface_chihiro_an2131sc = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 6,
    .bInterfaceClass               = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass            = 0x00,
    .bInterfaceProtocol            = 0x00,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
    },
};

static const USBDescDevice desc_device_chihiro_an2131sc = {
    .bcdUSB                        = 0x0100,
    .bDeviceClass                  = 0x60,
    .bDeviceSubClass               = 0x01,
    .bDeviceProtocol               = 0x00,
    .bMaxPacketSize0               = 0x40,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .bmAttributes          = 0x80,
            .bMaxPower             = 0x96,
            .nif = 1,
            .ifs = &desc_iface_chihiro_an2131sc,
        },
    },
};

static const USBDesc desc_chihiro_an2131sc = {
    .id = {
        .idVendor          = 0x0CA3,
        .idProduct         = 0x0003,
        .bcdDevice         = 0x0110,
        .iManufacturer     = STRING_MANUFACTURER,
        .iProduct          = STRING_PRODUCT,
        .iSerialNumber     = STRING_SERIALNUMBER,
    },
    .full = &desc_device_chihiro_an2131sc,
    .str  = chihiro_usb_stringtable,
};

/* v302: EZ-USB firmware reboot — disconnect callback */
static void ezusb_disconnect_cb(void *opaque)
{
    ChihiroUSBState *s = (ChihiroUSBState *)opaque;
    USBDevice *dev = &s->dev;
    const char *id = s->is_qc ? "QC" : "SC";

    if (!dev->attached) {
        if(0) printf("[%07lld] chihiro-usb [%s]: v302 disconnect skipped (already detached)\n", TS_MS, id);
        return;
    }

    if(0) printf("[%07lld] chihiro-usb [%s]: ★ v302 EZ-USB firmware reboot — DISCONNECT (CSC will fire)\n", TS_MS, id);
    usb_device_detach(dev);

    /* Schedule reconnect 50ms later */
    timer_mod(s->ezusb_reconnect_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
}

/* v302: EZ-USB firmware reboot — reconnect callback */
static void ezusb_reconnect_cb(void *opaque)
{
    ChihiroUSBState *s = (ChihiroUSBState *)opaque;
    USBDevice *dev = &s->dev;
    const char *id = s->is_qc ? "QC" : "SC";

    if (dev->attached) {
        if(0) printf("[%07lld] chihiro-usb [%s]: v302 reconnect skipped (already attached)\n", TS_MS, id);
        return;
    }

    if(0) printf("[%07lld] chihiro-usb [%s]: ★ v302 EZ-USB firmware reboot — RECONNECT (CSC will fire → Phase 2)\n", TS_MS, id);
    usb_device_attach(dev, &error_abort);
}

static void handle_reset(USBDevice *dev)
{
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";
    if(0) printf("[%07lld] chihiro-usb [%s]: device reset\n", TS_MS, id);
    fflush(stdout);
}

static uint64_t jvs_send_count = 0;
static uint64_t jvs_recv_count = 0;
static uint64_t jvs_recv_has_data = 0;
static uint64_t vendor_req_counts[256] = {0};

static void handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    extern uint64_t perf_cnt_usb_control;
    perf_cnt_usb_control++;
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";

    if(0) printf("[%07lld] chihiro-usb [%s]: control req=0x%04X val=0x%04X idx=0x%04X len=%d [%s]\n", TS_MS,
           id, request, value, index, length,
           (request == 0x8006 && (value >> 8) == 1) ? "GET_DESCRIPTOR(DEVICE)" :
           (request == 0x8006 && (value >> 8) == 2) ? "GET_DESCRIPTOR(CONFIG)" :
           (request == 0x8006 && (value >> 8) == 3) ? "GET_DESCRIPTOR(STRING)" :
           (request == 0x0005) ? "SET_ADDRESS" :
           (request == 0x0009) ? "SET_CONFIG" :
           (request == 0x010B) ? "SET_INTERFACE" :
           (request == 0x0001) ? "CLEAR_FEATURE" :
           (request == 0x0003) ? "SET_FEATURE" :
           (request == 0x8000) ? "GET_STATUS" :
           ((request >> 8) == 0x40 || (request >> 8) == 0xC0) ? "VENDOR" :
           "OTHER");
    fflush(stdout);

    int ret = usb_desc_handle_control(dev, p, request, value, index,
                                      length, data);
    if (ret >= 0) {
        int actual = p->actual_length;
        if(0) {
            if(0) printf("[%07lld] chihiro-usb [%s]: std handled actual=%d addr=%d", TS_MS, id, actual, dev->addr);
            if (actual > 0) {
                if(0) printf(" data=");
                if(0) for (int i = 0; i < actual && i < 18; i++) printf("%02X", data[i]);
            }
            if(0) printf("\n");
        }

        /* v302: After SET_ADDRESS completes, schedule EZ-USB firmware reboot.
         * Real AN2131 loads firmware from EEPROM, then disconnects+reconnects.
         * SEGABOOT waits for the CSC from reconnect to start Phase 2. */
        if (request == (DeviceOutRequest | USB_REQ_SET_ADDRESS) && !s->ezusb_rebooted) {
            s->ezusb_rebooted = true;
            if(0) printf("[%07lld] chihiro-usb [%s]: v302 SET_ADDRESS done (addr=%d) → scheduling EZ-USB reboot in 100ms\n",
                   TS_MS, id, dev->addr);
            timer_mod(s->ezusb_disconnect_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
        }

        return;
    }

    /* v202: Log when standard handler rejects a request */
    {
        uint8_t bmRequestType = request >> 8;
        uint8_t reqType = (bmRequestType >> 5) & 0x03;  /* 0=std, 1=class, 2=vendor */
        if (reqType != 2) {
            /* Non-vendor request rejected by usb_desc — could indicate descriptor issue */
            if(0) printf("[%07lld] chihiro-usb [%s]: ⚠ std handler REJECTED req=0x%04X (ret=%d) — "
                   "falling through to vendor handler\n", TS_MS, id, request, ret);
        }
    }

    /* Vendor request — extract bRequest from combined field.
     * QEMU encodes: request = (bmRequestType << 8) | bRequest */
    int bRequest = request & 0xFF;
    vendor_req_counts[(uint8_t)bRequest]++;

    if (0) { /* JVS vendor request logging — enable for debugging */
        if (bRequest >= 0x15 && bRequest <= 0x20) {
            printf("[%07lld] chihiro-usb [%s]: VENDOR 0x%02X val=0x%04X idx=0x%04X len=%d",
                   TS_MS, id, bRequest, value, index, length);
            if (!((request >> 8) & 0x80) && length > 0) {
                printf(" OUT_DATA=");
                for (int i = 0; i < length && i < 32; i++) printf("%02X", data[i]);
            }
            printf("\n");
        }
    }

    /* Save original host data for OUT (host-to-device) vendor requests.
     * The default fill below overwrites data[], so we need a copy for
     * requests like 0x20/0x23 that carry JVS payload. */
    uint8_t host_data[256];
    int host_len = MIN(length, (int)sizeof(host_data));
    if (!((request >> 8) & 0x80)) {
        memcpy(host_data, data, host_len);
    }

    /* Default response (MAME: every vendor request gets this) */
    for (int n = 0; n < length && n < 6; n++) {
        data[n] = 0x50 ^ n;
    }
    data[0] = 0x00;  /* success */
    data[1] = 0xCB;  /* PINSA (active low: 0=pressed/ON, 1=released/OFF)
                      * bit0=1 DIP1 OFF
                      * bit1=1 DIP2 OFF
                      * bit2=0 DIP3 ON  (horiz freq — required to avoid CAUTION 51)
                      * bit3=1 CS pin (ignored)
                      * bit4=0 DIP4 ON  (horiz freq — required to avoid CAUTION 51)
                      * bit5=0 DIP5 ON
                      * bit6=1 TEST released
                      * bit7=1 SERVICE released */
    data[2] = 0x52 | (s->jvs.sense & 0x03);  /* PINSB with current JVS sense */
    data[3] = 0x53;  /* OUTB register */

    /*
     * 0x1A/0x1B/0x22/0x23 drive the SC's two UARTs. The QC has none — it
     * carries JVS over 0x19/0x20 instead, and implements only 0x16..0x1F plus
     * 0x20/0x24/0x30 (matching MAME's ohci_hlean2131qc_device).
     *
     * Answering them on the QC is not harmless: 0x1B hands back the pending
     * JVS response and clears it, so a game polling 0x1B alongside 0x19 (Sega
     * Golf does, ~1500 times a boot) can have responses stolen from the path
     * it is actually waiting on.
     */
    if (s->is_qc && (bRequest == 0x1A || bRequest == 0x1B ||
                     bRequest == 0x22 || bRequest == 0x23)) {
        p->actual_length = 0;
        return;
    }

    switch (bRequest) {
    case 0x16: /* Read ic10 EEPROM #1 — queue for bulk EP1 IN */
    {
        int addr = value;     /* wValue = start address in ic10 */
        int count = index;    /* wIndex = byte count */
        if (count > CHIHIRO_USB_EP_BUFSZ) count = CHIHIRO_USB_EP_BUFSZ;
        if (addr + count > 8192) count = 8192 - addr;
        if (addr >= 0 && count > 0) {
            memcpy(s->ep_in[1].buf, s->eeprom + addr, count);
        } else {
            memset(s->ep_in[1].buf, 0xFF, count);
        }
        s->ep_in[1].pending = count;
        s->ep_in[1].offset = 0;
        if(0) printf("[%07lld] chihiro-usb [%s]: READ EEPROM1 addr=0x%04X count=%d → queued for EP1\n", TS_MS,
               id, addr, count);
        break;
    }
    case 0x17: /* Read baseboard EEPROM ic11 (512 bytes, 24LC024) */
    {
        int addr = value;
        int count = index;
        if (count > CHIHIRO_USB_EP_BUFSZ) count = CHIHIRO_USB_EP_BUFSZ;
        if (addr + count > CHIHIRO_IC11_SIZE) count = CHIHIRO_IC11_SIZE - addr;
        if (addr >= 0 && addr < CHIHIRO_IC11_SIZE && count > 0) {
            memcpy(s->ep_in[2].buf, chihiro_ic11 + addr, count);
        } else {
            memset(s->ep_in[2].buf, 0xFF, count);
            count = (count > 0) ? count : 0;
        }
        s->ep_in[2].pending = count;
        s->ep_in[2].offset = 0;
        if(0) {
            if(0) printf("[%07lld] chihiro-usb [%s]: READ ic11 EEPROM addr=0x%02X count=%d data=", TS_MS, id, addr, count);
            for (int i = 0; i < count && i < 16; i++) printf("%02X", s->ep_in[2].buf[i]);
            if(0) printf("\n");
        }
        break;
    }
    case 0x19: { /* Get JVS responses (QC path) */
        jvs_recv_count++;
        data[0] = 0x00;  /* not busy */
        /* Update sense in PINSB */
        data[2] = (data[2] & 0xFC) | (s->jvs.sense & 0x03);
        if (s->jvs.response_len > 0) {
            jvs_recv_has_data++;
            int rlen = s->jvs.response_len;
            /* Wrap raw JVS frame in AN2131QC format for EP4 IN:
             * [0]=0x00 [1]=pkt_count [2]=dest [3]=0x00
             * [4..5]=frame_len(LE) [6..]=raw JVS frame */
            uint8_t *ep = s->ep_in[4].buf;
            int wrapped = 0;
            uint8_t resp_dest = (s->jvs.last_target == JVS_BROADCAST) ? 0
                                                                     : s->jvs.device_id;
            ep[wrapped++] = 0x00;       /* status */
            ep[wrapped++] = 0x01;       /* 1 packet */
            ep[wrapped++] = resp_dest;  /* dest: 0 for broadcast, device_id for addressed */
            ep[wrapped++] = 0x00;       /* dummy */
            ep[wrapped++] = rlen & 0xFF;
            ep[wrapped++] = (rlen >> 8) & 0xFF;
            int copy = MIN(rlen, (int)sizeof(s->ep_in[4].buf) - wrapped);
            memcpy(ep + wrapped, s->jvs.response, copy);
            wrapped += copy;
            data[4] = wrapped & 0xFF;
            data[5] = (wrapped >> 8) & 0xFF;
            s->ep_in[4].pending = wrapped;
            s->ep_in[4].offset = 0;
            s->jvs.response_len = 0;
            if(0) printf("[%07lld] chihiro-usb [%s]: JVS RECV wrapped %d bytes (raw %d, dest=%d)\n",
                   TS_MS, id, wrapped, rlen, resp_dest);
        } else {
            data[4] = 0;
            data[5] = 0;
        }
        break;
    }
    case 0x20: { /* Send JVS packets (QC path) */
        jvs_send_count++;
        if(0) { printf("[%07lld] chihiro-usb [%s]: JVS SEND %d bytes:", TS_MS, id, host_len);
        for (int i = 0; i < host_len && i < 16; i++) printf(" %02X", host_data[i]);
        printf("\n"); }
        /* AN2131QC format: byte 0 = sequence counter, bytes 1+ = JVS frame */
        uint8_t *jvs_data = host_data;
        int jvs_len = host_len;
        if (jvs_len >= 2 && host_data[0] != JVS_SYNC && host_data[1] == JVS_SYNC) {
            jvs_data = host_data + 1;
            jvs_len -= 1;
        }
        if (jvs_len > 0 && jvs_data[0] == JVS_SYNC) {
            int rlen = chihiro_jvs_process(&s->jvs, jvs_data, jvs_len,
                                            s->jvs.response, sizeof(s->jvs.response));
            s->jvs.response_len = rlen;
            if(0) printf("[%07lld] chihiro-usb [%s]: JVS RESP %d bytes\n", TS_MS, id, rlen);
        }
        break;
    }
    case 0x30: /* External interrupt control */
        data[4] = (value & 0xFF) > 0 ? 1 : 0;  /* enabled? */
        data[5] = 0;  /* IRQ counter */
        if(0) printf("[%07lld] chihiro-usb [%s]: EXT IRQ control val=%d\n", TS_MS, id, value);
        break;
    case 0x1C: /* Read RTC — queue BCD time for bulk IN EP4 (data[0]=0 success status) */
    {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        #define TO_BCD(v) ((uint8_t)((v) + 6 * ((v) / 10)))
        int rtc_count = index;
        if (rtc_count > CHIHIRO_USB_EP_BUFSZ) rtc_count = CHIHIRO_USB_EP_BUFSZ;
        memset(s->ep_in[5].buf, 0, rtc_count);
        s->ep_in[5].buf[0] = TO_BCD(t->tm_sec);
        s->ep_in[5].buf[1] = TO_BCD(t->tm_min);
        s->ep_in[5].buf[2] = TO_BCD(t->tm_hour);
        s->ep_in[5].buf[3] = 0;
        s->ep_in[5].buf[4] = TO_BCD(t->tm_mday);
        s->ep_in[5].buf[5] = TO_BCD(t->tm_mon + 1);
        s->ep_in[5].buf[6] = TO_BCD(t->tm_year - 100);
        s->ep_in[5].buf[7] = 0;
        s->ep_in[5].pending = rtc_count;
        s->ep_in[5].offset = 0;
        #undef TO_BCD
        if(0) printf("[%07lld] chihiro-usb [%s]: RTC READ → %02X:%02X:%02X %02X/%02X/%02X (%d bytes queued EP5)\n",
               TS_MS, id, s->ep_in[5].buf[2], s->ep_in[5].buf[1], s->ep_in[5].buf[0],
               s->ep_in[5].buf[4], s->ep_in[5].buf[5], s->ep_in[5].buf[6], rtc_count);
        break;
    }
    case 0x1D: /* Write ic10 EEPROM #1 — accept */
        break;
    case 0x1E: /* Write ic11 EEPROM #2 via EP2 OUT */
        s->write_1e_addr = value;
        break;
    case 0x1F: /* Write external memory via EP3 OUT */
        s->write_1f_addr = value;
        break;
    case 0x24: /* Write RTC — accept */
        break;
    case 0x18: /* Read external memory / write-complete status poll */
    {
        int count = index;
        if (count == 0) {
            /* Status poll — default fill already has data[0]=0 (not busy) */
            break;
        }
        if (count > CHIHIRO_USB_EP_BUFSZ) count = CHIHIRO_USB_EP_BUFSZ;
        int addr = value;
        if (addr + count > 65536) count = 65536 - addr;
        if (addr >= 0 && count > 0) {
            memcpy(s->ep_in[3].buf, chihiro_extmem + addr, count);
        } else {
            memset(s->ep_in[3].buf, 0, count > 0 ? count : 0);
        }
        s->ep_in[3].pending = count;
        s->ep_in[3].offset = 0;
        break;
    }
    case 0xA0: /* ANCHOR_LOAD — EZ-USB firmware download (Cypress AN2131) */
    {
        uint16_t ram_addr = value;  /* wValue = target address in 8051 RAM */
        int count = length;
        if (ram_addr == 0x7F92) {
            /* CPUCS register: bit 0 = 1 → hold CPU in reset, 0 → run */
            bool hold = (count > 0 && data[0] & 0x01);
            if(0) printf("[%07lld] chihiro-usb [%s]: ANCHOR_LOAD CPUCS=%s (total %u bytes downloaded)\n",
                   TS_MS, id, hold ? "HOLD" : "RUN", s->fw_bytes_written);
            if (s->fw_cpu_held && !hold) {
                if(0) printf("[%07lld] chihiro-usb [%s]: ★ EZ-USB firmware loaded — CPU released\n",
                       TS_MS, id);
            }
            s->fw_cpu_held = hold;
        } else {
            s->fw_bytes_written += count;
            if (s->fw_bytes_written <= count) {
                if(0) printf("[%07lld] chihiro-usb [%s]: ANCHOR_LOAD start addr=0x%04X len=%d\n",
                       TS_MS, id, ram_addr, count);
            }
        }
        break;
    }
    /* === SC-specific handlers (UART / JVS) === */
    case 0x1A: /* Get UART0 data (SC only) */
    {
        int avail = s->uart0_rx_len;
        if (avail > 0 && avail <= length) {
            memcpy(data, s->uart0_rx, avail);
            p->actual_length = avail;
            s->uart0_rx_len = 0;
        } else {
            p->actual_length = 0;
        }
        if(0) printf("[%07lld] chihiro-usb [%s]: GET UART0 → %d bytes\n", TS_MS, id, avail);
        return;
    }
    case 0x1B: /* Get UART1 / JVS response (SC only) */
    {
        int avail = s->jvs.response_len;
        if (avail > 0 && avail <= length) {
            memcpy(data, s->jvs.response, avail);
            p->actual_length = avail;
            s->jvs.response_len = 0;
        } else if (s->uart1_rx_len > 0 && s->uart1_rx_len <= length) {
            memcpy(data, s->uart1_rx, s->uart1_rx_len);
            p->actual_length = s->uart1_rx_len;
            s->uart1_rx_len = 0;
        } else {
            p->actual_length = 0;
        }
        return;
    }
    case 0x22: /* Send UART0 data (SC only) — accept and discard */
        if(0) printf("[%07lld] chihiro-usb [%s]: SEND UART0 len=%d (stub)\n", TS_MS, id, length);
        break;
    case 0x23: { /* Send UART1 / JVS command (SC only) */
        int jvs_len = host_len;
        if (jvs_len > 0 && host_data[0] == JVS_SYNC) {
            int rlen = chihiro_jvs_process(&s->jvs, host_data, jvs_len,
                                            s->jvs.response, sizeof(s->jvs.response));
            s->jvs.response_len = rlen;
        }
        break;
    }
    case 0x25: /* UART config (SC only) — accept */
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x29:
    case 0x2A:
    case 0x2B:
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        if(0) printf("[%07lld] chihiro-usb [%s]: UART/GPIO config 0x%02X (stub)\n", TS_MS, id, bRequest);
        break;
    case 0x31: /* Set PORTB pins (SC only) — accept */
        if(0) printf("[%07lld] chihiro-usb [%s]: SET PORTB val=0x%04X (stub)\n", TS_MS, id, value);
        break;
    default:
        if(0) printf("[%07lld] chihiro-usb [%s]: UNHANDLED vendor req 0x%02X val=0x%04X idx=0x%04X len=%d → accepting\n",
               TS_MS, id, bRequest, value, index, length);
        break;
    }

    if (0) { /* Diagnostic: log vendor 0x15 (PINSB/sense check) */
        if (bRequest == 0x15) {
            printf("[%07lld] chihiro-usb [%s]: VENDOR 0x15 → data[0..3]=%02X %02X %02X %02X sense=%d\n",
                   TS_MS, id, data[0], data[1], data[2], data[3], s->jvs.sense);
        }
    }

    p->actual_length = length;
}

static void handle_data(USBDevice *dev, USBPacket *p)
{
    extern uint64_t perf_cnt_usb_handle;
    perf_cnt_usb_handle++;
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";
    int ep = p->ep->nr;

    if (p->pid == USB_TOKEN_IN) {
        /* Bulk IN — return queued data from per-endpoint buffer */
        if (ep >= 1 && ep < CHIHIRO_USB_MAX_EP && s->ep_in[ep].pending > 0) {
            int len = MIN(p->iov.size, s->ep_in[ep].pending);
            usb_packet_copy(p, s->ep_in[ep].buf + s->ep_in[ep].offset, len);
            s->ep_in[ep].offset += len;
            s->ep_in[ep].pending -= len;
            s->bulk_in_count++;
            if(0) printf("[%07lld] chihiro-usb [%s]: BULK IN EP%d → %d bytes (remain=%d)\n",
                   TS_MS, id, ep, len, s->ep_in[ep].pending);
        } else {
            s->nak_count++;
            p->status = USB_RET_STALL;
            if(0) printf("[%07lld] chihiro-usb [%s]: BULK IN EP%d → STALL (no data)\n",
                   TS_MS, id, ep);
        }
    } else {
        /* Bulk OUT — capture data for JVS processing */
        if(0) printf("[%07lld] chihiro-usb [%s]: BULK OUT EP%d %d bytes\n",
               TS_MS, id, ep, (int)p->iov.size);
        int len = p->iov.size;
        uint8_t buf[256];
        int total = 0;
        while (len > 0) {
            int chunk = MIN(len, (int)sizeof(buf) - total);
            if (chunk <= 0) {
                uint8_t discard[64];
                chunk = MIN(len, (int)sizeof(discard));
                usb_packet_copy(p, discard, chunk);
            } else {
                usb_packet_copy(p, buf + total, chunk);
                total += chunk;
            }
            len -= chunk;
        }
        if (total > 0) {
            if(0) { printf("[%07lld] chihiro-usb [%s]: BULK OUT data:", TS_MS, id);
            for (int i = 0; i < total && i < 32; i++) printf(" %02X", buf[i]);
            printf("\n"); }

            if (ep == 2) {
                /* EP2 OUT: ic11 EEPROM write (from vendor 0x1E) */
                uint16_t addr = s->write_1e_addr;
                int copy = MIN(total, CHIHIRO_IC11_SIZE - (int)addr);
                if (copy > 0) {
                    memcpy(chihiro_ic11 + addr, buf, copy);
                    s->write_1e_addr += copy;
                    /* Games update ic11 as a sequence of small chunks (the
                     * record at 0x00, then its mirror at 0x40). Persisting
                     * each chunk as it lands can capture the EEPROM halfway
                     * through that sequence, leaving the two copies out of
                     * sync on disk — which hangs the next boot. Defer the
                     * flush until the writes stop. */
                    chihiro_ic11_mark_dirty();
                }
            } else if (ep == 3) {
                /* EP3 OUT: external memory write (from vendor 0x1F) */
                uint16_t addr = s->write_1f_addr;
                int copy = MIN(total, (int)sizeof(chihiro_extmem) - (int)addr);
                if (copy > 0) {
                    memcpy(chihiro_extmem + addr, buf, copy);
                    s->write_1f_addr += copy;
                    chihiro_bram_mark_dirty(addr);
                }
            } else if (ep == 4) {
                /* EP4 OUT: JVS data with 3-byte AN2131QC header */
                uint8_t *jvs_p = NULL;
                int jvs_n = 0;
                if (total >= 4 && buf[3] == JVS_SYNC) {
                    jvs_p = buf + 3;
                    jvs_n = total - 3;
                } else if (total >= 1 && buf[0] == JVS_SYNC) {
                    jvs_p = buf;
                    jvs_n = total;
                }
                if (jvs_p && jvs_n > 0) {
                    if(0) { printf("[%07lld] chihiro-usb [%s]: JVS frame %d bytes:", TS_MS, id, jvs_n);
                    for (int i = 0; i < jvs_n && i < 16; i++) printf(" %02X", jvs_p[i]);
                    printf("\n"); }
                    int rlen = chihiro_jvs_process(&s->jvs, jvs_p, jvs_n,
                                                    s->jvs.response, sizeof(s->jvs.response));
                    s->jvs.response_len = rlen;
                    if(0) printf("[%07lld] chihiro-usb [%s]: JVS response %d bytes\n", TS_MS, id, rlen);
                }
            }
        }
        s->bulk_out_count++;
    }
}

/* v202: Counter accessors for DIAG timer in chihiro.c */
void chihiro_usb_get_counters(USBDevice *dev, uint32_t *nak, uint32_t *bulk_in, uint32_t *bulk_out)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    if (nak)      *nak      = s->nak_count;
    if (bulk_in)  *bulk_in  = s->bulk_in_count;
    if (bulk_out) *bulk_out = s->bulk_out_count;
}

void chihiro_usb_reset_counters(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->nak_count = 0;
    s->bulk_in_count = 0;
    s->bulk_out_count = 0;
}

void chihiro_usb_get_jvs_counters(uint64_t *send, uint64_t *recv, uint64_t *recv_data)
{
    if (send)      *send      = jvs_send_count;
    if (recv)      *recv      = jvs_recv_count;
    if (recv_data) *recv_data = jvs_recv_has_data;
}

void chihiro_usb_dump_vendor_histogram(void)
{
    printf("=== VENDOR REQUEST HISTOGRAM ===\n");
    for (int i = 0; i < 256; i++) {
        if (vendor_req_counts[i] > 0) {
            printf("  req 0x%02X: %lu\n", i, (unsigned long)vendor_req_counts[i]);
        }
    }
    printf("================================\n");
}

/*
 * Pick the baseboard region (ic10[0x1F00]: 0x01=JPN, 0x02=USA, 0x03=EXP)
 * from the game's boot.id regionFlags (offset 0x38, layout from
 * Cxbx-Reloaded MediaBoard.h). SEGABOOT accepts the game when
 * regionFlags has bit (region - 1) set, so choose the lowest region the
 * game advertises; otherwise SEGABOOT shows "THIS GAME IS NOT
 * ACCEPTABLE BY MAIN BOARD". Defaults to USA (matches the HOTD3 US
 * dump, regionFlags=0xFFFFFF0E) when boot.id is missing or invalid.
 */
static uint8_t chihiro_region_from_bootid(void)
{
    extern char chihiro_game_dir[1024];
    uint8_t region = 0x02;

    if (!chihiro_game_dir[0]) {
        return region;
    }

    char path[1100];
    snprintf(path, sizeof(path), "%s/boot.id", chihiro_game_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return region;
    }

    uint8_t bid[0x40];
    if (fread(bid, 1, sizeof(bid), f) == sizeof(bid) &&
        memcmp(bid, "BTID", 4) == 0) {
        uint32_t flags = bid[0x38] | (bid[0x39] << 8) |
                         (bid[0x3A] << 16) | ((uint32_t)bid[0x3B] << 24);
        /*
         * regionFlags bits (from Cxbx-Reloaded MediaBoard.h):
         *   0x2 = Japan, 0x4 = USA, 0x8 = Export.
         * The baseboard region byte SEGABOOT checks against is
         *   0x01 = Japan, 0x02 = USA, 0x03 = Export,
         * i.e. region R is accepted iff bit (1 << R) is set in flags.
         * Prefer USA, then Export, then Japan so multi-region titles
         * come up in English.
         */
        if (flags & 0x4) {
            region = 0x02;  /* USA */
        } else if (flags & 0x8) {
            region = 0x03;  /* Export */
        } else if (flags & 0x2) {
            region = 0x01;  /* Japan */
        }
        printf("[%07lld] Chihiro QC: boot.id regionFlags=0x%08X -> "
               "region 0x%02X\n", TS_MS, flags, region);
    }
    fclose(f);
    return region;
}

/*
 * Backup memory (ic11 baseboard EEPROM) persistence.
 *
 * Chihiro games save test-menu settings, coin/credit config and
 * bookkeeping into the 24LC024 baseboard EEPROM (ic11). To make those
 * survive across sessions, the ic11 buffer is loaded from and flushed
 * to a per-game file in xemu's data directory, named by the game ID
 * read from boot.id (offset 0x30). Falls back to the baked-in default
 * dump when no backup exists yet.
 */
/* Read the game ID from the game's boot.id (offset 0x30, alnum only);
 * writes "default" if unavailable. */
static void chihiro_backup_game_id(char *game_id, size_t len)
{
    extern char chihiro_game_dir[1024];
    snprintf(game_id, len, "default");

    if (!chihiro_game_dir[0]) {
        return;
    }
    char bootid[1100];
    snprintf(bootid, sizeof(bootid), "%s/boot.id", chihiro_game_dir);
    FILE *f = fopen(bootid, "rb");
    if (!f) {
        return;
    }
    uint8_t bid[0x40];
    if (fread(bid, 1, sizeof(bid), f) == sizeof(bid) &&
        memcmp(bid, "BTID", 4) == 0) {
        int n = 0;
        for (int i = 0; i < 8 && n < (int)len - 1; i++) {
            uint8_t c = bid[0x30 + i];
            if (g_ascii_isalnum(c)) {
                game_id[n++] = c;
            }
        }
        if (n > 0) {
            game_id[n] = '\0';
        }
    }
    fclose(f);
}

/*
 * Whether to persist this game's backup memory. Off unless the title has been
 * checked to survive a save/restore cycle; enabled explicitly via
 * sys.chihiro_backup, or automatically for the titles known to restore
 * cleanly.
 */
static bool chihiro_backup_enabled(void)
{
    if (g_config.sys.chihiro_backup) {
        return true;
    }
    static const char *const known_good[] = {
        "SBFN",  /* House of the Dead 3 */
        "SBFY",  /* Crazy Taxi */
        "SBFZ",  /* Virtua Cop 3 */
        "SBHF",  /* Ollie King */
        "SBHU",  /* Ghost Squad */
    };
    char game_id[16];
    chihiro_backup_game_id(game_id, sizeof(game_id));
    for (size_t i = 0; i < ARRAY_SIZE(known_good); i++) {
        if (strcmp(game_id, known_good[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool chihiro_backup_path_for(char *out, size_t out_len, const char *what)
{
    char game_id[16];
    chihiro_backup_game_id(game_id, sizeof(game_id));

    /* Save alongside the EEPROM file (a proper saves directory, e.g.
     * /userdata/saves/chihiro on Batocera); fall back to xemu's data
     * directory when eeprom_path is unset. */
    const char *eeprom = g_config.sys.files.eeprom_path;
    if (eeprom && eeprom[0]) {
        const char *slash = strrchr(eeprom, '/');
#ifdef _WIN32
        const char *bslash = strrchr(eeprom, '\\');
        if (bslash && (!slash || bslash > slash)) {
            slash = bslash;
        }
#endif
        if (slash) {
            int dir_len = (int)(slash - eeprom) + 1;
            snprintf(out, out_len, "%.*schihiro_%s_%s.bin",
                     dir_len, eeprom, game_id, what);
            return true;
        }
    }

    const char *base = xemu_settings_get_base_path();
    if (!base) {
        return false;
    }
    snprintf(out, out_len, "%schihiro_%s_%s.bin", base, game_id, what);
    return true;
}

static bool chihiro_backup_path(char *out, size_t out_len)
{
    return chihiro_backup_path_for(out, out_len, "backup");
}

/*
 * ic11 is a series of records, each laid out the same way:
 *
 *   +0x00  8-byte ASCII tag   ("ACBU0001", or the game id, NUL padded)
 *   +0x08  u16 entry count
 *   +0x0A  u16 checksum, little-endian: the sum of the data bytes
 *   +0x0C  data, four bytes per entry
 *
 * So a record's length is derived from its own entry count, and it varies per
 * game: the baseboard record holds 13 entries, Ollie King 3, Crazy Taxi 16,
 * Ghost Squad 24. The baseboard record sits at 0x00 and is mirrored at 0x40;
 * the game's own settings follow at 0x80. (Layout and checksum verified
 * against real saves from all four titles.)
 *
 * A record that fails its own checksum — truncated on disk, or captured while
 * the game was partway through rewriting it — is not worth restoring, so fall
 * back to the default dump, which always boots.
 */
#define CHIHIRO_IC11_TAG_LEN     8
#define CHIHIRO_IC11_COUNT_OFF   0x08
#define CHIHIRO_IC11_CKSUM_OFF   0x0A
#define CHIHIRO_IC11_DATA_OFF    0x0C
#define CHIHIRO_IC11_ENTRY_SIZE  4

#define CHIHIRO_IC11_BASEBOARD_TAG "ACBU0001"

/* avail is how much room the record has before the end of ic11; a count that
 * would run past it means the record is corrupt, not merely unrecognized. */
static bool chihiro_ic11_record_valid(const uint8_t *rec, size_t avail)
{
    uint16_t count = rec[CHIHIRO_IC11_COUNT_OFF] |
                     (rec[CHIHIRO_IC11_COUNT_OFF + 1] << 8);
    size_t data_len = (size_t)count * CHIHIRO_IC11_ENTRY_SIZE;

    if (CHIHIRO_IC11_DATA_OFF + data_len > avail) {
        return false;
    }

    uint16_t sum = 0;
    for (size_t i = 0; i < data_len; i++) {
        sum += rec[CHIHIRO_IC11_DATA_OFF + i];
    }

    uint16_t stored = rec[CHIHIRO_IC11_CKSUM_OFF] |
                      (rec[CHIHIRO_IC11_CKSUM_OFF + 1] << 8);
    return sum == stored;
}

/* The game record is tagged with the game's own id, so key off the tag being
 * printable rather than hardcoding one title. An all-zero slot just means the
 * game has not written its settings yet, which is fine. */
static bool chihiro_ic11_game_record_valid(const uint8_t *rec, size_t avail)
{
    bool empty = true;

    for (size_t i = 0; i < CHIHIRO_IC11_TAG_LEN; i++) {
        if (rec[i] != 0) {
            empty = false;
        }
        if (rec[i] != 0 && !g_ascii_isalnum(rec[i])) {
            return false;
        }
    }

    return empty || chihiro_ic11_record_valid(rec, avail);
}

/*
 * Every record must check out. In particular a valid baseboard header paired
 * with a half-written game record is exactly the inconsistency that sends the
 * game into its error path on the next boot.
 */
static bool chihiro_ic11_valid(const uint8_t *ic11)
{
    /* Baseboard record and its mirror. */
    for (int off = 0x00; off <= 0x40; off += 0x40) {
        if (memcmp(ic11 + off, CHIHIRO_IC11_BASEBOARD_TAG,
                   CHIHIRO_IC11_TAG_LEN) != 0 ||
            !chihiro_ic11_record_valid(ic11 + off, 0x40)) {
            return false;
        }
    }

    /* Game settings record. */
    return chihiro_ic11_game_record_valid(ic11 + 0x80,
                                          CHIHIRO_IC11_SIZE - 0x80);
}

static void chihiro_backup_load(uint8_t *ic11, size_t len)
{
    if (!chihiro_backup_enabled()) {
        return;
    }
    char path[1024];
    if (!chihiro_backup_path(path, sizeof(path))) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }

    uint8_t buf[CHIHIRO_IC11_SIZE] = { 0 };
    size_t rd = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    if (rd != len || !chihiro_ic11_valid(buf)) {
        fprintf(stderr, "Chihiro: ignoring damaged backup memory %s "
                        "(%zu of %zu bytes, header %s) — using defaults\n",
                path, rd, len, chihiro_ic11_valid(buf) ? "ok" : "bad");
        return;
    }

    memcpy(ic11, buf, len);
    printf("[%07lld] Chihiro: loaded backup memory (%zu bytes) from %s\n",
           TS_MS, rd, path);
}

static void chihiro_backup_save(const uint8_t *ic11, size_t len)
{
    if (!chihiro_backup_enabled()) {
        return;
    }
    char path[1024];
    if (!chihiro_backup_path(path, sizeof(path))) {
        return;
    }

    /* Write a temporary alongside the target and rename over it, so an exit
     * mid-write leaves the previous good backup in place instead of a
     * truncated file that the next boot would try to restore. */
    char tmp[1024 + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "Chihiro: cannot write backup memory to %s: %s\n",
                tmp, strerror(errno));
        return;
    }
    bool ok = fwrite(ic11, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    if (ok) {
        qemu_fdatasync(fileno(f));
    }
    fclose(f);

    if (!ok) {
        fprintf(stderr, "Chihiro: backup memory write to %s failed: %s\n",
                tmp, strerror(errno));
        unlink(tmp);
        return;
    }

#ifdef _WIN32
    /* rename() will not replace an existing file on Windows. */
    unlink(path);
#endif
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "Chihiro: cannot commit backup memory to %s: %s\n",
                path, strerror(errno));
        unlink(tmp);
    }
}

/*
 * Deferred flush: a game rewrites ic11 as several small EP2 chunks, and only
 * the state after the last one is consistent. Coalesce the writes and flush
 * once they have been quiet for a moment, plus unconditionally on exit.
 */
#define CHIHIRO_IC11_FLUSH_DELAY_MS 500

static QEMUTimer *chihiro_ic11_flush_timer;
static bool chihiro_ic11_dirty;

static void chihiro_ic11_flush(void)
{
    if (!chihiro_ic11_dirty) {
        return;
    }

    /* Still mid-rewrite (or the game left it inconsistent): keep the previous
     * good backup and stay dirty, so the next write or exit retries. */
    if (!chihiro_ic11_valid(chihiro_ic11)) {
        return;
    }

    chihiro_ic11_dirty = false;
    chihiro_backup_save(chihiro_ic11, sizeof(chihiro_ic11));
}

static void chihiro_ic11_flush_cb(void *opaque)
{
    chihiro_ic11_flush();
}

static void chihiro_ic11_exit_notify(Notifier *n, void *opaque)
{
    chihiro_ic11_flush();
}

static Notifier chihiro_ic11_exit_notifier = {
    .notify = chihiro_ic11_exit_notify,
};

static void chihiro_ic11_mark_dirty(void)
{
    chihiro_ic11_dirty = true;
    if (chihiro_ic11_flush_timer) {
        timer_mod(chihiro_ic11_flush_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      CHIHIRO_IC11_FLUSH_DELAY_MS);
    }
}

/* Initialize the single shared ic11: default dump, then the persisted backup
 * on top if one exists. Both AN2131 realize paths call this; only the first
 * does the work. */
static void chihiro_ic11_init(void)
{
    static bool initialized;

    if (initialized) {
        return;
    }
    initialized = true;

    _Static_assert(sizeof(hotd3_ic11_24lc024) == 128,
                   "ic11 EEPROM dump must be exactly 128 bytes");
    memset(chihiro_ic11, 0, sizeof(chihiro_ic11));
    memcpy(chihiro_ic11, hotd3_ic11_24lc024, sizeof(hotd3_ic11_24lc024));

    chihiro_backup_load(chihiro_ic11, sizeof(chihiro_ic11));

    chihiro_ic11_flush_timer =
        timer_new_ms(QEMU_CLOCK_VIRTUAL, chihiro_ic11_flush_cb, NULL);
    qemu_add_exit_notifier(&chihiro_ic11_exit_notifier);
}

/*
 * Backup RAM persistence, same shape as ic11's: coalesce writes, flush once
 * they go quiet and on exit, and commit via a temp file and rename.
 *
 * Unlike ic11 there is nothing to validate — the layout is the game's own and
 * it checksums the region itself, re-initializing it when the contents do not
 * add up. So restore it verbatim, and only insist that the file is the size we
 * wrote.
 */
static QEMUTimer *chihiro_bram_flush_timer;
static bool chihiro_bram_dirty;

static void chihiro_bram_save(void)
{
    if (!chihiro_backup_enabled()) {
        return;
    }
    char path[1024];
    if (!chihiro_backup_path_for(path, sizeof(path), "bram")) {
        return;
    }

    char tmp[1024 + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "Chihiro: cannot write backup RAM to %s: %s\n",
                tmp, strerror(errno));
        return;
    }
    bool ok = fwrite(chihiro_extmem + CHIHIRO_BRAM_BASE, 1,
                     CHIHIRO_BRAM_SIZE, f) == CHIHIRO_BRAM_SIZE;
    ok = (fflush(f) == 0) && ok;
    if (ok) {
        qemu_fdatasync(fileno(f));
    }
    fclose(f);

    if (!ok) {
        fprintf(stderr, "Chihiro: backup RAM write to %s failed: %s\n",
                tmp, strerror(errno));
        unlink(tmp);
        return;
    }

#ifdef _WIN32
    unlink(path);
#endif
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "Chihiro: cannot commit backup RAM to %s: %s\n",
                path, strerror(errno));
        unlink(tmp);
    }
}

static void chihiro_bram_flush(void)
{
    if (!chihiro_bram_dirty) {
        return;
    }
    chihiro_bram_dirty = false;
    chihiro_bram_save();
}

static void chihiro_bram_flush_cb(void *opaque)
{
    chihiro_bram_flush();
}

static void chihiro_bram_exit_notify(Notifier *n, void *opaque)
{
    chihiro_bram_flush();
}

static Notifier chihiro_bram_exit_notifier = {
    .notify = chihiro_bram_exit_notify,
};

/* Only writes into the battery-backed half are worth persisting. */
static void chihiro_bram_mark_dirty(uint32_t addr)
{
    if (addr < CHIHIRO_BRAM_BASE) {
        return;
    }

    chihiro_bram_dirty = true;
    if (chihiro_bram_flush_timer) {
        timer_mod(chihiro_bram_flush_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      CHIHIRO_IC11_FLUSH_DELAY_MS);
    }
}

static void chihiro_bram_init(void)
{
    static bool initialized;

    if (initialized) {
        return;
    }
    initialized = true;

    memset(chihiro_extmem, 0, sizeof(chihiro_extmem));

    chihiro_bram_flush_timer =
        timer_new_ms(QEMU_CLOCK_VIRTUAL, chihiro_bram_flush_cb, NULL);
    qemu_add_exit_notifier(&chihiro_bram_exit_notifier);

    if (!chihiro_backup_enabled()) {
        return;
    }
    char path[1024];
    if (!chihiro_backup_path_for(path, sizeof(path), "bram")) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    size_t rd = fread(chihiro_extmem + CHIHIRO_BRAM_BASE, 1,
                      CHIHIRO_BRAM_SIZE, f);
    fclose(f);

    if (rd != CHIHIRO_BRAM_SIZE) {
        fprintf(stderr, "Chihiro: ignoring short backup RAM %s "
                        "(%zu of %zu bytes) — using defaults\n",
                path, rd, (size_t)CHIHIRO_BRAM_SIZE);
        memset(chihiro_extmem + CHIHIRO_BRAM_BASE, 0, CHIHIRO_BRAM_SIZE);
        return;
    }

    printf("[%07lld] Chihiro: loaded backup RAM (%zu bytes) from %s\n",
           TS_MS, rd, path);
}

static void chihiro_an2131qc_realize(USBDevice *dev, Error **errp)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->is_qc = true;
    usb_desc_init(dev);
    dev->auto_attach = 0;  /* Attach later via hotplug timer */

    /* Load real ic10 QC EEPROM firmware (8192 bytes from MAME hotd3.zip).
     * Contains AN2131 8051 firmware code + region/serial/game data.
     * SEGABOOT reads this via vendor request 0x16 + bulk IN EP1. */
    _Static_assert(sizeof(hotd3_ic10_g24lc64) == 8192,
                   "ic10 firmware must be exactly 8KB");
    memcpy(s->eeprom, hotd3_ic10_g24lc64, sizeof(s->eeprom));

    /* Region from the game's boot.id (original ic10 dump has 0x01/Japan,
     * which fails the SEGABOOT region check for non-JP games) */
    s->eeprom[0x1F00] = chihiro_region_from_bootid();

    /* Initialize per-endpoint bulk transfer state */
    memset(s->ep_in, 0, sizeof(s->ep_in));

    /* Baseboard EEPROM: shared with the SC, initialized by whichever comes first */
    chihiro_ic11_init();
    chihiro_bram_init();
    s->write_1e_addr = 0;
    s->write_1f_addr = 0;

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    /* v302: EZ-USB firmware reboot timers */
    s->ezusb_disconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_disconnect_cb, s);
    s->ezusb_reconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_reconnect_cb, s);
    s->ezusb_rebooted = true;  /* v307: reconnect disabled — Path B fix makes it unnecessary */

    chihiro_jvs_init(&s->jvs);
    chihiro_jvs_global = &s->jvs;

    printf("[%07lld] Chihiro QC: loaded ic10 firmware (8192B) + ic11 (128B), "
           "region patched to USA (0x02), serial=%.16s\n",
           TS_MS, (const char *)&s->eeprom[0x1F10]);
}

static void chihiro_an2131qc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->ezusb_disconnect_timer);
    timer_free(s->ezusb_reconnect_timer);
}

static void chihiro_an2131qc_class_init(ObjectClass *klass, const void *data)
{
    // DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = chihiro_an2131qc_realize;
    uc->unrealize      = chihiro_an2131qc_unrealize;
    uc->product_desc   = "Chihiro an2131qc";
    uc->usb_desc       = &desc_chihiro_an2131qc;

    uc->handle_reset   = handle_reset;
    uc->handle_control = handle_control;
    uc->handle_data    = handle_data;
    uc->handle_attach  = usb_desc_attach;

    //dc->vmsd = &vmstate_usb_kbd;
}

static const TypeInfo chihiro_an2131qc_info = {
    .name          = "chihiro-an2131qc",
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(ChihiroUSBState),
    .class_init    = chihiro_an2131qc_class_init,
};

static void chihiro_an2131sc_realize(USBDevice *dev, Error **errp)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->is_qc = false;
    usb_desc_init(dev);
    dev->auto_attach = 0;  /* Attach later via hotplug timer */

    /* Load real pc20 SC EEPROM firmware (8192 bytes from MAME hotd3.zip). */
    _Static_assert(sizeof(hotd3_pc20_g24lc64) == 8192,
                   "pc20 firmware must be exactly 8KB");
    memcpy(s->eeprom, hotd3_pc20_g24lc64, sizeof(s->eeprom));

    /* Initialize per-endpoint bulk transfer state */
    memset(s->ep_in, 0, sizeof(s->ep_in));

    /* Baseboard EEPROM: shared with the QC, initialized by whichever comes first */
    chihiro_ic11_init();
    chihiro_bram_init();
    s->write_1e_addr = 0;
    s->write_1f_addr = 0;

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    /* Initialize UART buffers */
    s->uart0_rx_len = 0;
    s->uart1_rx_len = 0;

    /* v302: EZ-USB firmware reboot timers */
    s->ezusb_disconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_disconnect_cb, s);
    s->ezusb_reconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_reconnect_cb, s);
    s->ezusb_rebooted = true;  /* v307: reconnect disabled — Path B fix makes it unnecessary */

    chihiro_jvs_init(&s->jvs);

    printf("[%07lld] Chihiro SC: loaded pc20 firmware (8192B) + ic11 (128B)\n", TS_MS);
}

static void chihiro_an2131sc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->ezusb_disconnect_timer);
    timer_free(s->ezusb_reconnect_timer);
}

static void chihiro_an2131sc_class_init(ObjectClass *klass, const void *data)
{
    // DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = chihiro_an2131sc_realize;
    uc->unrealize      = chihiro_an2131sc_unrealize;
    uc->product_desc   = "Chihiro an2131sc";
    uc->usb_desc       = &desc_chihiro_an2131sc;

    uc->handle_reset   = handle_reset;
    uc->handle_control = handle_control;
    uc->handle_data    = handle_data;
    uc->handle_attach  = usb_desc_attach;

    //dc->vmsd = &vmstate_usb_kbd;
}

static const TypeInfo chihiro_an2131sc_info = {
    .name          = "chihiro-an2131sc",
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(ChihiroUSBState),
    .class_init    = chihiro_an2131sc_class_init,
};

static void chihiro_usb_register_types(void)
{
    type_register_static(&chihiro_an2131qc_info);
    type_register_static(&chihiro_an2131sc_info);
}

type_init(chihiro_usb_register_types)
