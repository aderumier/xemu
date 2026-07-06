/*
 * xemu Linux evdev light gun support
 *
 * Reads absolute-position light gun devices directly from /dev/input,
 * enumerated via libudev. Devices tagged with the ID_INPUT_GUN udev
 * property (e.g. by Batocera/Sinden/Gun4IR udev rules) are matched in
 * priority; if none are found, absolute-axis mice are considered (a
 * light gun lacking dedicated udev rules typically reports as one).
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

#ifndef XEMU_INPUT_EVDEV_GUN_H
#define XEMU_INPUT_EVDEV_GUN_H

#include <stdbool.h>
#include <stdint.h>

#define EVDEV_GUN_BTN_TRIGGER (1 << 0)  /* BTN_LEFT */
#define EVDEV_GUN_BTN_RELOAD  (1 << 1)  /* BTN_RIGHT */
#define EVDEV_GUN_BTN_AUX     (1 << 2)  /* BTN_MIDDLE */
#define EVDEV_GUN_BTN_1       (1 << 3)
#define EVDEV_GUN_BTN_2       (1 << 4)
#define EVDEV_GUN_BTN_3       (1 << 5)
#define EVDEV_GUN_BTN_4       (1 << 6)
#define EVDEV_GUN_BTN_5       (1 << 7)
#define EVDEV_GUN_BTN_6       (1 << 8)
#define EVDEV_GUN_BTN_7       (1 << 9)
#define EVDEV_GUN_BTN_8       (1 << 10)

#if defined(__linux__) && defined(CONFIG_LIBUDEV)

/* Lazily scans for devices on first call; true if at least one gun found */
bool xemu_input_evdev_gun_available(void);
int xemu_input_evdev_gun_count(void);

/* Drain pending events from all guns; call once per input update */
void xemu_input_evdev_gun_poll(void);

/*
 * Current aim of gun `index`, normalized to 0..1 (top-left origin).
 * Returns false when offscreen (device reports BTN_TOUCH released) or
 * when the gun doesn't exist.
 */
bool xemu_input_evdev_gun_get_pos(int index, float *x, float *y);

uint32_t xemu_input_evdev_gun_get_buttons(int index);
const char *xemu_input_evdev_gun_get_devnode(int index);

#else

static inline bool xemu_input_evdev_gun_available(void) { return false; }
static inline int xemu_input_evdev_gun_count(void) { return 0; }
static inline void xemu_input_evdev_gun_poll(void) { }
static inline bool xemu_input_evdev_gun_get_pos(int index, float *x, float *y)
{
    return false;
}
static inline uint32_t xemu_input_evdev_gun_get_buttons(int index)
{
    return 0;
}
static inline const char *xemu_input_evdev_gun_get_devnode(int index)
{
    return "";
}

#endif

#endif /* XEMU_INPUT_EVDEV_GUN_H */
