/*
 * xemu Linux evdev light gun support
 *
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#if defined(__linux__) && defined(CONFIG_LIBUDEV)

#include "xemu-input-evdev-gun.h"

#include <libudev.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define MAX_GUNS 4

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, array) \
    (((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

typedef struct EvdevGunAxis {
    int value;
    int min;
    int max;
} EvdevGunAxis;

typedef struct EvdevGun {
    int fd;
    char devnode[64];
    EvdevGunAxis x;
    EvdevGunAxis y;
    uint32_t buttons;
    bool has_touch;   /* device reports BTN_TOUCH (offscreen indicator) */
    bool touch;
} EvdevGun;

static EvdevGun guns[MAX_GUNS];
static int num_guns;
static bool scanned;

static uint32_t button_mask_for_code(uint16_t code)
{
    switch (code) {
    case BTN_LEFT:   return EVDEV_GUN_BTN_TRIGGER;
    case BTN_RIGHT:  return EVDEV_GUN_BTN_RELOAD;
    case BTN_MIDDLE: return EVDEV_GUN_BTN_AUX;
    case BTN_1:      return EVDEV_GUN_BTN_1;
    case BTN_2:      return EVDEV_GUN_BTN_2;
    case BTN_3:      return EVDEV_GUN_BTN_3;
    case BTN_4:      return EVDEV_GUN_BTN_4;
    case BTN_5:      return EVDEV_GUN_BTN_5;
    case BTN_6:      return EVDEV_GUN_BTN_6;
    case BTN_7:      return EVDEV_GUN_BTN_7;
    case BTN_8:      return EVDEV_GUN_BTN_8;
    default:         return 0;
    }
}

/* Sort /dev/input/eventX by X so gun order is stable across scans */
static int devnode_cmp(const void *a, const void *b)
{
    const char *x = *(const char *const *)a;
    const char *y = *(const char *const *)b;
    int n = 0;

    while (x[n] && x[n] == y[n]) {
        n++;
    }
    if (isdigit((unsigned char)x[n]) && isdigit((unsigned char)y[n])) {
        return atoi(x + n) - atoi(y + n);
    }
    return strcmp(x, y);
}

static bool gun_open(const char *devnode)
{
    unsigned long absbits[NBITS(ABS_MAX + 1)] = { 0 };
    unsigned long keybits[NBITS(KEY_MAX + 1)] = { 0 };
    struct input_absinfo absinfo;
    EvdevGun *gun;

    if (num_guns >= MAX_GUNS) {
        return false;
    }

    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "evdev-gun: cannot open %s: %s\n", devnode,
                strerror(errno));
        return false;
    }

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0 ||
        !TEST_BIT(ABS_X, absbits) || !TEST_BIT(ABS_Y, absbits)) {
        close(fd);
        return false;
    }

    gun = &guns[num_guns];
    memset(gun, 0, sizeof(*gun));
    gun->fd = fd;
    snprintf(gun->devnode, sizeof(gun->devnode), "%s", devnode);

    if (ioctl(fd, EVIOCGABS(ABS_X), &absinfo) == 0) {
        gun->x.min = absinfo.minimum;
        gun->x.max = absinfo.maximum;
        gun->x.value = absinfo.value;
    }
    if (ioctl(fd, EVIOCGABS(ABS_Y), &absinfo) == 0) {
        gun->y.min = absinfo.minimum;
        gun->y.max = absinfo.maximum;
        gun->y.value = absinfo.value;
    }

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
        gun->has_touch = TEST_BIT(BTN_TOUCH, keybits);
    }
    if (gun->has_touch) {
        unsigned long keystate[NBITS(KEY_MAX + 1)] = { 0 };
        if (ioctl(fd, EVIOCGKEY(sizeof(keystate)), keystate) >= 0) {
            gun->touch = TEST_BIT(BTN_TOUCH, keystate);
        }
    }

    fprintf(stderr,
            "evdev-gun: gun %d: %s, ABS_X(%d, %d), ABS_Y(%d, %d)%s\n",
            num_guns, devnode, gun->x.min, gun->x.max, gun->y.min,
            gun->y.max, gun->has_touch ? ", BTN_TOUCH" : "");
    num_guns++;
    return true;
}

/*
 * Enumerate input devices carrying the given udev property and open any
 * that expose absolute X/Y axes, in ascending /dev/input/eventN order.
 */
static void scan_udev_property(struct udev *udev, const char *property)
{
    struct udev_enumerate *enumerate = udev_enumerate_new(udev);
    if (!enumerate) {
        return;
    }

    udev_enumerate_add_match_property(enumerate, property, "1");
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);

    char *devnodes[MAX_GUNS * 4];
    int count = 0;

    struct udev_list_entry *item;
    udev_list_entry_foreach(item,
                            udev_enumerate_get_list_entry(enumerate)) {
        if (count >= (int)ARRAY_SIZE(devnodes)) {
            break;
        }
        struct udev_device *dev = udev_device_new_from_syspath(
            udev, udev_list_entry_get_name(item));
        if (!dev) {
            continue;
        }
        const char *devnode = udev_device_get_devnode(dev);
        if (devnode) {
            devnodes[count++] = g_strdup(devnode);
        }
        udev_device_unref(dev);
    }

    qsort(devnodes, count, sizeof(devnodes[0]), devnode_cmp);

    for (int i = 0; i < count; i++) {
        gun_open(devnodes[i]);
        g_free(devnodes[i]);
    }

    udev_enumerate_unref(enumerate);
}

static void scan_devices(void)
{
    scanned = true;

    struct udev *udev = udev_new();
    if (!udev) {
        fprintf(stderr, "evdev-gun: udev initialization failed\n");
        return;
    }

    /*
     * Prefer devices tagged as light guns (ID_INPUT_GUN=1, set by
     * Batocera/Sinden/Gun4IR udev rules). Only if none are tagged,
     * fall back to absolute-axis mice: a light gun without dedicated
     * udev rules usually identifies as one. Regular relative mice are
     * never matched here and keep working through the SDL pointer.
     */
    scan_udev_property(udev, "ID_INPUT_GUN");
    if (num_guns == 0) {
        scan_udev_property(udev, "ID_INPUT_MOUSE");
    }

    udev_unref(udev);
}

bool xemu_input_evdev_gun_available(void)
{
    if (!scanned) {
        scan_devices();
    }
    return num_guns > 0;
}

int xemu_input_evdev_gun_count(void)
{
    if (!scanned) {
        scan_devices();
    }
    return num_guns;
}

static void gun_drain_events(EvdevGun *gun)
{
    struct input_event evt;

    for (;;) {
        ssize_t n = read(gun->fd, &evt, sizeof(evt));
        if (n != sizeof(evt)) {
            break;
        }

        switch (evt.type) {
        case EV_KEY:
            if (evt.code == BTN_TOUCH) {
                gun->touch = evt.value != 0;
            } else {
                uint32_t mask = button_mask_for_code(evt.code);
                if (evt.value) {
                    gun->buttons |= mask;
                } else {
                    gun->buttons &= ~mask;
                }
            }
            break;
        case EV_ABS:
            if (evt.code == ABS_X) {
                gun->x.value = evt.value;
            } else if (evt.code == ABS_Y) {
                gun->y.value = evt.value;
            }
            break;
        default:
            break;
        }
    }
}

void xemu_input_evdev_gun_poll(void)
{
    for (int i = 0; i < num_guns; i++) {
        gun_drain_events(&guns[i]);
    }
}

bool xemu_input_evdev_gun_get_pos(int index, float *x, float *y)
{
    if (index < 0 || index >= num_guns) {
        return false;
    }

    EvdevGun *gun = &guns[index];
    if (gun->has_touch && !gun->touch) {
        return false;
    }

    int range_x = gun->x.max - gun->x.min;
    int range_y = gun->y.max - gun->y.min;
    if (range_x <= 0 || range_y <= 0) {
        return false;
    }

    float fx = (float)(gun->x.value - gun->x.min) / range_x;
    float fy = (float)(gun->y.value - gun->y.min) / range_y;
    *x = MIN(MAX(fx, 0.0f), 1.0f);
    *y = MIN(MAX(fy, 0.0f), 1.0f);
    return true;
}

uint32_t xemu_input_evdev_gun_get_buttons(int index)
{
    if (index < 0 || index >= num_guns) {
        return 0;
    }
    return guns[index].buttons;
}

const char *xemu_input_evdev_gun_get_devnode(int index)
{
    if (index < 0 || index >= num_guns) {
        return "";
    }
    return guns[index].devnode;
}

#endif /* __linux__ && CONFIG_LIBUDEV */
