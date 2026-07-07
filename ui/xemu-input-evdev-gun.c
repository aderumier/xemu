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
#include "xemu-settings.h"

#include <libudev.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define MAX_GUNS 4
#define MAX_GUN_NODES 4

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
    int fds[MAX_GUN_NODES];  /* a gun may span several event nodes */
    int num_fds;
    char devnode[128];       /* first node, for display */
    EvdevGunAxis x;
    EvdevGunAxis y;
    uint32_t buttons;
    bool has_abs;
    bool has_rel;
    bool has_touch;   /* some node reports BTN_TOUCH (offscreen indicator) */
    bool touch;
    bool touch_seen;  /* a BTN_TOUCH event (or pressed initial state) was
                         actually observed; some devices advertise BTN_TOUCH
                         in their descriptor but never emit it */
    bool rel_axes;    /* relative mouse: axes are synthetic, driven by
                         integrating REL_X/REL_Y deltas */
} EvdevGun;

/*
 * Virtual axis range and delta scaling for relative mice. With 8x
 * scaling, sweeping the full virtual screen takes ~4096 mouse counts
 * (about 10 cm of travel on a typical 1000 dpi mouse).
 */
#define REL_AXIS_RANGE 32767
#define REL_DELTA_SCALE 8

static EvdevGun guns[MAX_GUNS];
static int num_guns;
static bool scanned;

/*
 * Evdev code names accepted in the input.lightgun.gun_buttons config
 * (raw decimal/hex codes are accepted too).
 */
static const struct {
    const char *name;
    uint16_t code;
} btn_names[] = {
    /* Mouse */
    { "BTN_LEFT", BTN_LEFT },       { "BTN_RIGHT", BTN_RIGHT },
    { "BTN_MIDDLE", BTN_MIDDLE },   { "BTN_SIDE", BTN_SIDE },
    { "BTN_EXTRA", BTN_EXTRA },     { "BTN_FORWARD", BTN_FORWARD },
    { "BTN_BACK", BTN_BACK },       { "BTN_TASK", BTN_TASK },
    /* Joystick */
    { "BTN_TRIGGER", BTN_TRIGGER }, { "BTN_THUMB", BTN_THUMB },
    { "BTN_THUMB2", BTN_THUMB2 },   { "BTN_TOP", BTN_TOP },
    { "BTN_TOP2", BTN_TOP2 },       { "BTN_PINKIE", BTN_PINKIE },
    { "BTN_BASE", BTN_BASE },       { "BTN_BASE2", BTN_BASE2 },
    { "BTN_BASE3", BTN_BASE3 },     { "BTN_BASE4", BTN_BASE4 },
    { "BTN_BASE5", BTN_BASE5 },     { "BTN_BASE6", BTN_BASE6 },
    { "BTN_DEAD", BTN_DEAD },
    /* Gamepad (guns in XInput mode) */
    { "BTN_SOUTH", BTN_SOUTH },     { "BTN_EAST", BTN_EAST },
    { "BTN_NORTH", BTN_NORTH },     { "BTN_WEST", BTN_WEST },
    { "BTN_C", BTN_C },             { "BTN_Z", BTN_Z },
    { "BTN_TL", BTN_TL },           { "BTN_TR", BTN_TR },
    { "BTN_TL2", BTN_TL2 },         { "BTN_TR2", BTN_TR2 },
    { "BTN_SELECT", BTN_SELECT },   { "BTN_START", BTN_START },
    { "BTN_MODE", BTN_MODE },       { "BTN_THUMBL", BTN_THUMBL },
    { "BTN_THUMBR", BTN_THUMBR },
    /* Misc BTN_0..BTN_9 */
    { "BTN_0", BTN_0 }, { "BTN_1", BTN_1 }, { "BTN_2", BTN_2 },
    { "BTN_3", BTN_3 }, { "BTN_4", BTN_4 }, { "BTN_5", BTN_5 },
    { "BTN_6", BTN_6 }, { "BTN_7", BTN_7 }, { "BTN_8", BTN_8 },
    { "BTN_9", BTN_9 },
    /* Digitizer */
    { "BTN_TOUCH", BTN_TOUCH },     { "BTN_STYLUS", BTN_STYLUS },
    { "BTN_STYLUS2", BTN_STYLUS2 },
    { "BTN_TRIGGER_HAPPY1", BTN_TRIGGER_HAPPY1 },
    { "BTN_TRIGGER_HAPPY2", BTN_TRIGGER_HAPPY2 },
    { "BTN_TRIGGER_HAPPY3", BTN_TRIGGER_HAPPY3 },
    { "BTN_TRIGGER_HAPPY4", BTN_TRIGGER_HAPPY4 },
    /* Keys sometimes used by gun keyboard interfaces */
    { "KEY_ENTER", KEY_ENTER },     { "KEY_ESC", KEY_ESC },
    { "KEY_SPACE", KEY_SPACE },     { "KEY_LEFTSHIFT", KEY_LEFTSHIFT },
    { "KEY_LEFTCTRL", KEY_LEFTCTRL }, { "KEY_LEFTALT", KEY_LEFTALT },
    { "KEY_UP", KEY_UP },           { "KEY_DOWN", KEY_DOWN },
    { "KEY_LEFT", KEY_LEFT },       { "KEY_RIGHT", KEY_RIGHT },
    { "KEY_1", KEY_1 }, { "KEY_2", KEY_2 }, { "KEY_3", KEY_3 },
    { "KEY_4", KEY_4 }, { "KEY_5", KEY_5 },
};

static int btn_code_for_name(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(btn_names); i++) {
        if (g_ascii_strcasecmp(name, btn_names[i].name) == 0) {
            return btn_names[i].code;
        }
    }

    /* Raw decimal or 0x-prefixed code */
    char *end;
    long code = strtol(name, &end, 0);
    if (end != name && *end == '\0' && code > 0 && code <= KEY_MAX) {
        return (int)code;
    }
    return -1;
}

static const char *btn_name_for_code(uint16_t code)
{
    for (size_t i = 0; i < ARRAY_SIZE(btn_names); i++) {
        if (btn_names[i].code == code) {
            return btn_names[i].name;
        }
    }
    return NULL;
}

/* evdev code -> Xbox button mask bindings, built from the config */
static struct {
    uint16_t code;
    uint32_t mask;
} code_map[64];
static int code_map_len;

static void code_map_add(uint16_t code, uint32_t mask)
{
    for (int i = 0; i < code_map_len; i++) {
        if (code_map[i].code == code) {
            code_map[i].mask |= mask;
            return;
        }
    }
    if (code_map_len < (int)ARRAY_SIZE(code_map)) {
        code_map[code_map_len].code = code;
        code_map[code_map_len].mask = mask;
        code_map_len++;
    }
}

static uint32_t button_mask_for_code(uint16_t code)
{
    for (int i = 0; i < code_map_len; i++) {
        if (code_map[i].code == code) {
            return code_map[i].mask;
        }
    }
    return 0;
}

static void parse_button_bindings(void)
{
    const struct {
        const char *value;
        uint32_t mask;
        const char *label;
    } bindings[] = {
        { g_config.input.lightgun.gun_buttons.a,     EVDEV_GUN_BTN_A,     "a" },
        { g_config.input.lightgun.gun_buttons.b,     EVDEV_GUN_BTN_B,     "b" },
        { g_config.input.lightgun.gun_buttons.x,     EVDEV_GUN_BTN_X,     "x" },
        { g_config.input.lightgun.gun_buttons.y,     EVDEV_GUN_BTN_Y,     "y" },
        { g_config.input.lightgun.gun_buttons.start, EVDEV_GUN_BTN_START, "start" },
        { g_config.input.lightgun.gun_buttons.back,  EVDEV_GUN_BTN_BACK,  "back" },
        { g_config.input.lightgun.gun_buttons.white, EVDEV_GUN_BTN_WHITE, "white" },
        { g_config.input.lightgun.gun_buttons.black, EVDEV_GUN_BTN_BLACK, "black" },
        { g_config.input.lightgun.gun_buttons.dpad_up,
          EVDEV_GUN_BTN_DPAD_UP, "dpad_up" },
        { g_config.input.lightgun.gun_buttons.dpad_down,
          EVDEV_GUN_BTN_DPAD_DOWN, "dpad_down" },
        { g_config.input.lightgun.gun_buttons.dpad_left,
          EVDEV_GUN_BTN_DPAD_LEFT, "dpad_left" },
        { g_config.input.lightgun.gun_buttons.dpad_right,
          EVDEV_GUN_BTN_DPAD_RIGHT, "dpad_right" },
    };

    code_map_len = 0;

    for (size_t i = 0; i < ARRAY_SIZE(bindings); i++) {
        if (!bindings[i].value || !bindings[i].value[0]) {
            continue;
        }
        char **tokens = g_strsplit(bindings[i].value, ",", -1);
        for (int t = 0; tokens[t]; t++) {
            const char *name = g_strstrip(tokens[t]);
            if (!name[0]) {
                continue;
            }
            int code = btn_code_for_name(name);
            if (code < 0) {
                fprintf(stderr,
                        "evdev-gun: gun_buttons.%s: unknown code '%s'\n",
                        bindings[i].label, name);
            } else {
                code_map_add(code, bindings[i].mask);
            }
        }
        g_strfreev(tokens);
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

static void log_node_buttons(const char *devnode, const unsigned long *keybits)
{
    char buf[512];
    int len = 0;

    for (int code = 0; code <= KEY_MAX && len < (int)sizeof(buf) - 32;
         code++) {
        if (!TEST_BIT(code, keybits)) {
            continue;
        }
        const char *name = btn_name_for_code(code);
        if (name) {
            len += snprintf(buf + len, sizeof(buf) - len, " %s", name);
        } else if (code >= BTN_MISC) {
            len += snprintf(buf + len, sizeof(buf) - len, " 0x%x", code);
        }
    }
    if (len > 0) {
        fprintf(stderr, "evdev-gun:   %s buttons:%s\n", devnode, buf);
    }
}

/*
 * Open one event node and attach it to `gun`. The first node exposing
 * ABS_X/ABS_Y provides the aim axes; every node contributes button
 * events.
 */
static bool gun_node_open(EvdevGun *gun, const char *devnode)
{
    unsigned long absbits[NBITS(ABS_MAX + 1)] = { 0 };
    unsigned long relbits[NBITS(REL_MAX + 1)] = { 0 };
    unsigned long keybits[NBITS(KEY_MAX + 1)] = { 0 };
    struct input_absinfo absinfo;

    if (gun->num_fds >= MAX_GUN_NODES) {
        return false;
    }

    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "evdev-gun: cannot open %s: %s\n", devnode,
                strerror(errno));
        return false;
    }

    bool node_abs = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0 &&
                    TEST_BIT(ABS_X, absbits) && TEST_BIT(ABS_Y, absbits);
    bool node_rel = ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) >= 0 &&
                    TEST_BIT(REL_X, relbits) && TEST_BIT(REL_Y, relbits);

    if (node_abs && !gun->has_abs) {
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
        gun->has_abs = true;
    }
    gun->has_rel |= node_rel;

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
        if (TEST_BIT(BTN_TOUCH, keybits)) {
            gun->has_touch = true;
            unsigned long keystate[NBITS(KEY_MAX + 1)] = { 0 };
            if (ioctl(fd, EVIOCGKEY(sizeof(keystate)), keystate) >= 0 &&
                TEST_BIT(BTN_TOUCH, keystate)) {
                gun->touch = true;
                gun->touch_seen = true;
            }
        }
        log_node_buttons(devnode, keybits);
    }

    if (gun->num_fds == 0) {
        snprintf(gun->devnode, sizeof(gun->devnode), "%s", devnode);
    }
    gun->fds[gun->num_fds++] = fd;
    return true;
}

static void gun_close(EvdevGun *gun)
{
    for (int i = 0; i < gun->num_fds; i++) {
        close(gun->fds[i]);
    }
    memset(gun, 0, sizeof(*gun));
}

/*
 * Finish setting up a gun once all of its nodes are open. Without any
 * absolute axes, synthesize a centered virtual axis driven by relative
 * deltas (allow_relative permitting). Returns false (and rolls back)
 * if the gun ends up unusable.
 */
static bool gun_finalize(EvdevGun *gun, bool allow_relative)
{
    if (gun->num_fds == 0) {
        return false;
    }

    if (!gun->has_abs) {
        if (!(allow_relative && gun->has_rel)) {
            gun_close(gun);
            return false;
        }
        gun->rel_axes = true;
        gun->x.min = 0;
        gun->x.max = REL_AXIS_RANGE;
        gun->x.value = REL_AXIS_RANGE / 2;
        gun->y.min = 0;
        gun->y.max = REL_AXIS_RANGE;
        gun->y.value = REL_AXIS_RANGE / 2;
    }

    fprintf(stderr,
            "evdev-gun: gun %d: %s (%d node%s), ABS_X(%d, %d), "
            "ABS_Y(%d, %d)%s%s\n",
            num_guns, gun->devnode, gun->num_fds,
            gun->num_fds > 1 ? "s" : "", gun->x.min, gun->x.max,
            gun->y.min, gun->y.max,
            gun->rel_axes ? " [relative mouse]" : "",
            gun->has_touch ? ", BTN_TOUCH" : "");
    num_guns++;
    return true;
}

/* Single-node convenience used by the udev auto-detection path */
static bool gun_open(const char *devnode, bool allow_relative)
{
    if (num_guns >= MAX_GUNS) {
        return false;
    }

    EvdevGun *gun = &guns[num_guns];
    memset(gun, 0, sizeof(*gun));
    if (!gun_node_open(gun, devnode)) {
        return false;
    }
    return gun_finalize(gun, allow_relative);
}

/*
 * Enumerate input devices carrying the given udev property and open any
 * that expose absolute X/Y axes, in ascending /dev/input/eventN order.
 */
static void scan_udev_property(struct udev *udev, const char *property,
                               bool allow_relative)
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
        gun_open(devnodes[i], allow_relative);
        g_free(devnodes[i]);
    }

    udev_enumerate_unref(enumerate);
}

/*
 * Accept several spellings for a configured gun device: a devnode
 * (/dev/input/eventN or a /dev/input/by-id/... symlink), a sysfs path
 * (/sys/class/input/eventN), or a bare "eventN".
 */
static const char *resolve_devnode(const char *path, char *buf, size_t len)
{
    if (strncmp(path, "/sys/", 5) == 0) {
        const char *base = strrchr(path, '/');
        snprintf(buf, len, "/dev/input%s", base);
        return buf;
    }
    if (path[0] != '/') {
        snprintf(buf, len, "/dev/input/%s", path);
        return buf;
    }
    return path;
}

/*
 * Open guns explicitly listed in the config
 * (input.lightgun.gunN_device). Each entry may be a comma-separated
 * list of event nodes merged into a single gun (aim and buttons split
 * across nodes). Returns true if at least one device was configured
 * (even if it failed to open), in which case udev auto-detection is
 * skipped: an explicit config fully describes the setup. Gun index
 * follows config order, skipping devices that fail to open.
 */
static bool scan_config_devices(void)
{
    const char *paths[MAX_GUNS] = {
        g_config.input.lightgun.gun1_device,
        g_config.input.lightgun.gun2_device,
        g_config.input.lightgun.gun3_device,
        g_config.input.lightgun.gun4_device,
    };
    bool any_configured = false;

    for (int i = 0; i < MAX_GUNS && num_guns < MAX_GUNS; i++) {
        if (!paths[i] || !paths[i][0]) {
            continue;
        }
        any_configured = true;

        EvdevGun *gun = &guns[num_guns];
        memset(gun, 0, sizeof(*gun));

        char **nodes = g_strsplit(paths[i], ",", -1);
        for (int n = 0; nodes[n]; n++) {
            const char *path = g_strstrip(nodes[n]);
            if (!path[0]) {
                continue;
            }
            char buf[64];
            gun_node_open(gun, resolve_devnode(path, buf, sizeof(buf)));
        }
        g_strfreev(nodes);

        if (!gun_finalize(gun, true)) {
            fprintf(stderr,
                    "evdev-gun: configured gun%d_device '%s' not usable\n",
                    i + 1, paths[i]);
        }
    }
    return any_configured;
}

static void scan_devices(void)
{
    scanned = true;

    parse_button_bindings();

    if (scan_config_devices()) {
        return;
    }

    struct udev *udev = udev_new();
    if (!udev) {
        fprintf(stderr, "evdev-gun: udev initialization failed\n");
        return;
    }

    /*
     * Prefer devices tagged as light guns (ID_INPUT_GUN=1, set by
     * Batocera/Sinden/Gun4IR udev rules); tagging is an explicit
     * opt-in, so relative mice are accepted there too (each becomes an
     * independent virtual pointer, enabling e.g. 2-player with two
     * mice). Only if nothing is tagged, fall back to absolute-axis
     * mice: a light gun without dedicated udev rules usually
     * identifies as one. Untagged relative mice are never matched and
     * keep working through the SDL pointer.
     */
    scan_udev_property(udev, "ID_INPUT_GUN", true);
    if (num_guns == 0) {
        scan_udev_property(udev, "ID_INPUT_MOUSE", false);
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

static void log_unmapped_button(uint16_t code)
{
    static uint16_t seen[32];
    static int seen_len;

    for (int i = 0; i < seen_len; i++) {
        if (seen[i] == code) {
            return;
        }
    }
    if (seen_len < (int)ARRAY_SIZE(seen)) {
        seen[seen_len++] = code;
    }

    const char *name = btn_name_for_code(code);
    if (name) {
        fprintf(stderr,
                "evdev-gun: unmapped button %s (code %d/0x%x) pressed; "
                "bind it via input.lightgun.gun_buttons in xemu.toml\n",
                name, code, code);
    } else {
        fprintf(stderr,
                "evdev-gun: unmapped button code %d/0x%x pressed; "
                "bind it via input.lightgun.gun_buttons in xemu.toml\n",
                code, code);
    }
}

static void gun_drain_node(EvdevGun *gun, int fd)
{
    struct input_event evt;

    for (;;) {
        ssize_t n = read(fd, &evt, sizeof(evt));
        if (n != sizeof(evt)) {
            break;
        }

        switch (evt.type) {
        case EV_KEY: {
            if (evt.code == BTN_TOUCH) {
                gun->touch = evt.value != 0;
                gun->touch_seen = true;
            }
            uint32_t mask = button_mask_for_code(evt.code);
            if (mask) {
                if (evt.value) {
                    gun->buttons |= mask;
                } else {
                    gun->buttons &= ~mask;
                }
            } else if (evt.code != BTN_TOUCH && evt.value == 1) {
                log_unmapped_button(evt.code);
            }
            break;
        }
        case EV_ABS:
            if (evt.code == ABS_X) {
                gun->x.value = evt.value;
            } else if (evt.code == ABS_Y) {
                gun->y.value = evt.value;
            }
            break;
        case EV_REL:
            if (gun->rel_axes) {
                EvdevGunAxis *axis = (evt.code == REL_X) ? &gun->x :
                                     (evt.code == REL_Y) ? &gun->y : NULL;
                if (axis) {
                    int v = axis->value + evt.value * REL_DELTA_SCALE;
                    axis->value = MIN(MAX(v, axis->min), axis->max);
                }
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
        for (int n = 0; n < guns[i].num_fds; n++) {
            gun_drain_node(&guns[i], guns[i].fds[n]);
        }
    }
}

/*
 * Fraction of the axis range near each edge treated as offscreen for
 * absolute guns without (observed) BTN_TOUCH: when such a gun (e.g.
 * Sinden) loses the screen, its position pins at/near the axis
 * extremes.
 */
#define EDGE_OFFSCREEN_MARGIN 0.02f

bool xemu_input_evdev_gun_get_pos(int index, float *x, float *y)
{
    if (index < 0 || index >= num_guns) {
        return false;
    }

    EvdevGun *gun = &guns[index];

    /*
     * Only trust BTN_TOUCH as an offscreen indicator once the device
     * has actually emitted it: plenty of HID descriptors advertise
     * BTN_TOUCH without ever sending it, which would otherwise freeze
     * the aim as permanently offscreen.
     */
    bool touch_valid = gun->has_touch && gun->touch_seen;
    if (touch_valid && !gun->touch) {
        return false;
    }

    int range_x = gun->x.max - gun->x.min;
    int range_y = gun->y.max - gun->y.min;
    if (range_x <= 0 || range_y <= 0) {
        return false;
    }

    float fx = (float)(gun->x.value - gun->x.min) / range_x;
    float fy = (float)(gun->y.value - gun->y.min) / range_y;

    if (!gun->rel_axes && !touch_valid &&
        (fx < EDGE_OFFSCREEN_MARGIN || fx > 1.0f - EDGE_OFFSCREEN_MARGIN ||
         fy < EDGE_OFFSCREEN_MARGIN || fy > 1.0f - EDGE_OFFSCREEN_MARGIN)) {
        return false;
    }

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
