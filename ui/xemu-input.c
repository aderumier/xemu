/*
 * xemu Input Management
 *
 * Copyright (C) 2020-2021 Matt Borgerson
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
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "monitor/qdev.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/timer.h"
#include "qemu/config-file.h"

#include "xemu-input.h"
#include "xemu-input-evdev-gun.h"
#include "xemu-notifications.h"
#include "xemu-settings.h"
#include <stdio.h>
#include <stdlib.h>

#include "system/blockdev.h"
#include "hw/xbox/chihiro-jvs.h"

extern SDL_Window *m_window;
extern int viewport_coords[4];

// #define DEBUG_INPUT

#ifdef DEBUG_INPUT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US  2500
#define XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US 2500

#if 0
static void xemu_input_print_controller_state(ControllerState *state)
{
    DPRINTF("     A = %d,      B = %d,     X = %d,     Y = %d\n"
           "  Left = %d,     Up = %d, Right = %d,  Down = %d\n"
           "  Back = %d,  Start = %d, White = %d, Black = %d\n"
           "Lstick = %d, Rstick = %d, Guide = %d\n"
           "\n"
           "LTrig   = %.3f, RTrig   = %.3f\n"
           "LStickX = %.3f, RStickX = %.3f\n"
           "LStickY = %.3f, RStickY = %.3f\n\n",
        !!(state->buttons & CONTROLLER_BUTTON_A),
        !!(state->buttons & CONTROLLER_BUTTON_B),
        !!(state->buttons & CONTROLLER_BUTTON_X),
        !!(state->buttons & CONTROLLER_BUTTON_Y),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_LEFT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_UP),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_RIGHT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_DOWN),
        !!(state->buttons & CONTROLLER_BUTTON_BACK),
        !!(state->buttons & CONTROLLER_BUTTON_START),
        !!(state->buttons & CONTROLLER_BUTTON_WHITE),
        !!(state->buttons & CONTROLLER_BUTTON_BLACK),
        !!(state->buttons & CONTROLLER_BUTTON_LSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_RSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_GUIDE),
        state->axis[CONTROLLER_AXIS_LTRIG],
        state->axis[CONTROLLER_AXIS_RTRIG],
        state->axis[CONTROLLER_AXIS_LSTICK_X],
        state->axis[CONTROLLER_AXIS_RSTICK_X],
        state->axis[CONTROLLER_AXIS_LSTICK_Y],
        state->axis[CONTROLLER_AXIS_RSTICK_Y]
        );
}
#endif

ControllerStateList available_controllers =
    QTAILQ_HEAD_INITIALIZER(available_controllers);
ControllerState *bound_controllers[4] = { NULL, NULL, NULL, NULL };
const char *bound_drivers[4] = { DRIVER_DUKE, DRIVER_DUKE, DRIVER_DUKE,
                                 DRIVER_DUKE };
int test_mode;

static float m_mouseX;
static float m_mouseY;

static const char **port_index_to_settings_key_map[] = {
    &g_config.input.bindings.port1,
    &g_config.input.bindings.port2,
    &g_config.input.bindings.port3,
    &g_config.input.bindings.port4,
};

static const char **port_index_to_driver_settings_key_map[] = {
    &g_config.input.bindings.port1_driver,
    &g_config.input.bindings.port2_driver,
    &g_config.input.bindings.port3_driver, 
    &g_config.input.bindings.port4_driver
};

static int *peripheral_types_settings_map[4][2] = {
    { &g_config.input.peripherals.port1.peripheral_type_0,
      &g_config.input.peripherals.port1.peripheral_type_1 },
    { &g_config.input.peripherals.port2.peripheral_type_0,
      &g_config.input.peripherals.port2.peripheral_type_1 },
    { &g_config.input.peripherals.port3.peripheral_type_0,
      &g_config.input.peripherals.port3.peripheral_type_1 },
    { &g_config.input.peripherals.port4.peripheral_type_0,
      &g_config.input.peripherals.port4.peripheral_type_1 }
};

static const char **peripheral_params_settings_map[4][2] = {
    { &g_config.input.peripherals.port1.peripheral_param_0,
      &g_config.input.peripherals.port1.peripheral_param_1 },
    { &g_config.input.peripherals.port2.peripheral_param_0,
      &g_config.input.peripherals.port2.peripheral_param_1 },
    { &g_config.input.peripherals.port3.peripheral_param_0,
      &g_config.input.peripherals.port3.peripheral_param_1 },
    { &g_config.input.peripherals.port4.peripheral_param_0,
      &g_config.input.peripherals.port4.peripheral_param_1 }
};

int *g_keyboard_scancode_map[25] = {
    &g_config.input.keyboard_controller_scancode_map.a,
    &g_config.input.keyboard_controller_scancode_map.b,
    &g_config.input.keyboard_controller_scancode_map.x,
    &g_config.input.keyboard_controller_scancode_map.y,
    &g_config.input.keyboard_controller_scancode_map.back,
    &g_config.input.keyboard_controller_scancode_map.guide,
    &g_config.input.keyboard_controller_scancode_map.start,
    &g_config.input.keyboard_controller_scancode_map.lstick_btn,
    &g_config.input.keyboard_controller_scancode_map.rstick_btn,
    &g_config.input.keyboard_controller_scancode_map.white,
    &g_config.input.keyboard_controller_scancode_map.black,
    &g_config.input.keyboard_controller_scancode_map.dpad_up,
    &g_config.input.keyboard_controller_scancode_map.dpad_down,
    &g_config.input.keyboard_controller_scancode_map.dpad_left,
    &g_config.input.keyboard_controller_scancode_map.dpad_right,
    &g_config.input.keyboard_controller_scancode_map.lstick_up,
    &g_config.input.keyboard_controller_scancode_map.lstick_left,
    &g_config.input.keyboard_controller_scancode_map.lstick_right,
    &g_config.input.keyboard_controller_scancode_map.lstick_down,
    &g_config.input.keyboard_controller_scancode_map.ltrigger,
    &g_config.input.keyboard_controller_scancode_map.rstick_up,
    &g_config.input.keyboard_controller_scancode_map.rstick_left,
    &g_config.input.keyboard_controller_scancode_map.rstick_right,
    &g_config.input.keyboard_controller_scancode_map.rstick_down,
    &g_config.input.keyboard_controller_scancode_map.rtrigger,
};

static void check_and_reset_in_range(int *btn, int min, int max,
                                     const char *message)
{
    if (*btn < min || *btn >= max) {
        fprintf(stderr, "%s\n", message);
        *btn = min;
    }
}

static void xemu_input_bindings_set_in_range(ControllerState *con)
{
#define CHECK_RESET_BUTTON(btn)                                            \
    check_and_reset_in_range(&con->controller_map->controller_mapping.btn, \
                             SDL_GAMEPAD_BUTTON_INVALID,                   \
                             SDL_GAMEPAD_BUTTON_COUNT,                     \
                             "Invalid entry for button " #btn ", resetting")

    CHECK_RESET_BUTTON(a);
    CHECK_RESET_BUTTON(b);
    CHECK_RESET_BUTTON(x);
    CHECK_RESET_BUTTON(y);
    CHECK_RESET_BUTTON(dpad_left);
    CHECK_RESET_BUTTON(dpad_up);
    CHECK_RESET_BUTTON(dpad_right);
    CHECK_RESET_BUTTON(dpad_down);
    CHECK_RESET_BUTTON(back);
    CHECK_RESET_BUTTON(start);
    CHECK_RESET_BUTTON(lshoulder);
    CHECK_RESET_BUTTON(rshoulder);
    CHECK_RESET_BUTTON(lstick_btn);
    CHECK_RESET_BUTTON(rstick_btn);
    CHECK_RESET_BUTTON(guide);

#undef CHECK_RESET_BUTTON

#define CHECK_RESET_AXIS(axis)                                              \
    check_and_reset_in_range(&con->controller_map->controller_mapping.axis, \
                             SDL_GAMEPAD_AXIS_INVALID,                      \
                             SDL_GAMEPAD_AXIS_COUNT,                        \
                             "Invalid entry for button " #axis ", resetting")

    CHECK_RESET_AXIS(axis_trigger_left);
    CHECK_RESET_AXIS(axis_trigger_right);
    CHECK_RESET_AXIS(axis_left_x);
    CHECK_RESET_AXIS(axis_left_y);
    CHECK_RESET_AXIS(axis_right_x);
    CHECK_RESET_AXIS(axis_right_y);

#undef CHECK_RESET_AXIS
}

static void xemu_input_bindings_reload_map(ControllerState *con)
{
    assert(con->type == INPUT_DEVICE_SDL_GAMEPAD);

    char guid[35] = { 0 };
    SDL_GUIDToString(con->sdl_joystick_guid, guid, sizeof(guid));
    if (!xemu_settings_load_gamepad_mapping(guid, &con->controller_map)) {
        return;
    }

    // If this controller did not exist in the mapping array, the config will
    // have been reallocated. Any gamepad mapping pointers for other controllers
    // are now invalid, and need to be reloaded.
    ControllerState *iter, *next;
    bool is_new_mapping;
    QTAILQ_FOREACH_SAFE (iter, &available_controllers, entry, next) {
        if (iter == con || iter->type != INPUT_DEVICE_SDL_GAMEPAD) {
            continue;
        }

        memset(guid, 0, sizeof(guid));
        SDL_GUIDToString(iter->sdl_joystick_guid, guid, sizeof(guid));

        is_new_mapping =
            xemu_settings_load_gamepad_mapping(guid, &iter->controller_map);
        assert(!is_new_mapping &&
               "Existing controller GUIDs should exist in the config");

        xemu_input_bindings_set_in_range(iter);
    }
}

static const char *get_bound_driver(int port)
{
    assert(port >= 0 && port <= 3);
    const char *driver = *port_index_to_driver_settings_key_map[port];

    // If the driver in the config is NULL, empty, or unrecognized 
    // then default to DRIVER_DUKE
    if (driver == NULL)
        return DRIVER_DUKE;
    if (strlen(driver) == 0)
        return DRIVER_DUKE;
    if (strcmp(driver, DRIVER_DUKE) == 0)
        return DRIVER_DUKE;
    if (strcmp(driver, DRIVER_S) == 0)
        return DRIVER_S;
    if (strcmp(driver, DRIVER_LIGHT_GUN) == 0)
        return DRIVER_LIGHT_GUN;

    return DRIVER_DUKE;
}

static const int port_map[4] = { 3, 4, 1, 2 };

void xemu_input_init(void)
{
    if (g_config.input.background_input_capture) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "Failed to initialize SDL gamepad subsystem: %s\n", SDL_GetError());
        exit(1);
    }

    // Create the keyboard input (always first)
    ControllerState *new_con = malloc(sizeof(ControllerState));
    memset(new_con, 0, sizeof(ControllerState));
    new_con->type = INPUT_DEVICE_SDL_KEYBOARD;
    new_con->name = "Keyboard";
    new_con->bound = -1;
    new_con->peripheral_types[0] = PERIPHERAL_NONE;
    new_con->peripheral_types[1] = PERIPHERAL_NONE;
    new_con->peripherals[0] = NULL;
    new_con->peripherals[1] = NULL;
    new_con->lg.scaleX = 1.0f;
    new_con->lg.scaleY = 1.0f;

    for (int i = 0; i < 25; i++) {
        static const char *format_str =
            "WARNING: Keyboard controller map scancode out of range "
            "(%d) : Disabled\n";
        char buf[128];
        snprintf(buf, sizeof(buf), format_str, i);
        check_and_reset_in_range(g_keyboard_scancode_map[i],
                                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_COUNT, buf);
    }

    bound_drivers[0] = get_bound_driver(0);
    bound_drivers[1] = get_bound_driver(1);
    bound_drivers[2] = get_bound_driver(2);
    bound_drivers[3] = get_bound_driver(3);

    // Check to see if we should auto-bind the keyboard
    int port = xemu_input_get_controller_default_bind_port(new_con, 0);
    if (port >= 0) {
        xemu_input_bind(port, new_con, 0);
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, 
                 port+1);
        xemu_queue_notification(buf);
        xemu_input_rebind_xmu(port);
    }

    QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
}

int xemu_input_get_controller_default_bind_port(ControllerState *state, 
                                                int start)
{
    char guid[35] = { 0 };
    if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        SDL_GUIDToString(state->sdl_joystick_guid, guid, sizeof(guid));
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        snprintf(guid, sizeof(guid), "keyboard");
    }

    for (int i = start; i < 4; i++) {
        if (strcmp(guid, *port_index_to_settings_key_map[i]) == 0) {
            return i;
        }
    }

    return -1;
}

void xemu_save_peripheral_settings(int player_index, int peripheral_index,
                                   int peripheral_type,
                                   const char *peripheral_parameter)
{
    int *peripheral_type_ptr =
        peripheral_types_settings_map[player_index][peripheral_index];
    const char **peripheral_param_ptr =
        peripheral_params_settings_map[player_index][peripheral_index];

    assert(peripheral_type_ptr);
    assert(peripheral_param_ptr);

    *peripheral_type_ptr = peripheral_type;
    xemu_settings_set_string(
        peripheral_param_ptr,
        peripheral_parameter == NULL ? "" : peripheral_parameter);
}

void xemu_input_process_sdl_events(const SDL_Event *event)
{
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        DPRINTF("Controller Added: %d\n", event->gdevice.which);

        // Attempt to open the added controller
        SDL_Gamepad *sdl_con;
        sdl_con = SDL_OpenGamepad(event->gdevice.which);
        if (sdl_con == NULL) {
            DPRINTF("Could not open joystick %d as a Gamepad\n", event->gdevice.which);
            return;
        }

        // Success! Create a new node to track this controller and continue init
        ControllerState *new_con = malloc(sizeof(ControllerState));
        memset(new_con, 0, sizeof(ControllerState));
        new_con->type                 = INPUT_DEVICE_SDL_GAMEPAD;
        new_con->name                 = SDL_GetGamepadName(sdl_con);
        new_con->sdl_gamepad          = sdl_con;
        new_con->sdl_joystick         = SDL_GetGamepadJoystick(new_con->sdl_gamepad);
        new_con->sdl_joystick_id      = SDL_GetJoystickID(new_con->sdl_joystick);
        new_con->sdl_joystick_guid    = SDL_GetJoystickGUID(new_con->sdl_joystick);
        new_con->bound                = -1;
        new_con->peripheral_types[0] = PERIPHERAL_NONE;
        new_con->peripheral_types[1] = PERIPHERAL_NONE;
        new_con->peripherals[0] = NULL;
        new_con->peripherals[1] = NULL;
        new_con->lg.scaleX = 1.0f;
        new_con->lg.scaleY = 1.0f;

        char guid_buf[35] = { 0 };
        SDL_GUIDToString(new_con->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
        DPRINTF("Opened %s (%s)\n", new_con->name, guid_buf);

        QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
        xemu_input_bindings_reload_map(new_con);

        // Do not replace binding for a currently bound device. In the case that
        // the same GUID is specified multiple times, on different ports, allow
        // any available port to be bound.
        //
        // This can happen naturally with X360 wireless receiver, in which each
        // controller gets the same GUID (go figure). We cannot remember which
        // controller is which in this case, but we can try to tolerate this
        // situation by binding to any previously bound port with this GUID. The
        // upside in this case is that a person can use the same GUID on all
        // ports and just needs to bind to the receiver and never needs to hit
        // this dialog.


        // Attempt to re-bind to port previously bound to
        int port = 0;
        bool did_bind = false;
        while (!did_bind) {
            port = xemu_input_get_controller_default_bind_port(new_con, port);
            if (port < 0) {
                // No (additional) default mappings
                break;
            } else if (!xemu_input_get_bound(port)) {
                xemu_input_bind(port, new_con, 0);
                did_bind = true;
                break;
            } else {
                // Try again for another port
                port++;
            }
        }

        // Try to bind to any open port, and if so remember the binding
        if (!did_bind && g_config.input.auto_bind) {
            for (port = 0; port < 4; port++) {
                if (!xemu_input_get_bound(port)) {
                    xemu_input_bind(port, new_con, 1);
                    did_bind = true;
                    break;
                }
            }
        }

        if (did_bind) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Connected '%s' to port %d", 
                     new_con->name, port+1);
            xemu_queue_notification(buf);
            xemu_input_rebind_xmu(port);
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        DPRINTF("Controller Removed: %d\n", event->gdevice.which);
        int handled = 0;
        ControllerState *iter, *next;
        QTAILQ_FOREACH_SAFE(iter, &available_controllers, entry, next) {
            if (iter->type != INPUT_DEVICE_SDL_GAMEPAD) continue;

            if (iter->sdl_joystick_id == event->gdevice.which) {
                DPRINTF("Device removed: %s\n", iter->name);

                // Disconnect
                if (iter->bound >= 0) {
                    // Queue a notification to inform user controller disconnected
                    // FIXME: Probably replace with a callback registration thing,
                    // but this works well enough for now.
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Port %d disconnected", 
                             iter->bound+1);
                    xemu_queue_notification(buf);

                    // Unbind the controller, but don't save the unbinding in
                    // case the controller is reconnected
                    xemu_input_bind(iter->bound, NULL, 0);
                }

                // Unlink
                QTAILQ_REMOVE (&available_controllers, iter, entry);

                // Deallocate
                if (iter->sdl_gamepad) {
                    SDL_CloseGamepad(iter->sdl_gamepad);
                }

                for (int i = 0; i < 2; i++) {
                    if (iter->peripherals[i])
                        g_free(iter->peripherals[i]);
                }
                free(iter);

                handled = 1;
                break;
            }
        }
        if (!handled) {
            DPRINTF("Could not find handle for joystick instance\n");
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_REMAPPED) {
        DPRINTF("Controller Remapped: %d\n", event->gdevice.which);
    }
}

void xemu_input_update_controller(ControllerState *state)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_input_updated_ts) <
        XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US) {
        return;
    }

    if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_input_update_sdl_kbd_controller_state(state);
    } else if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        xemu_input_update_sdl_controller_state(state);
    }

    state->last_input_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

static void xemu_input_update_jvs_player(ChihiroJVSState *jvs, int player,
                                         bool offscreen, bool trigger,
                                         bool reload, bool start,
                                         bool service, bool coin,
                                         bool push2, bool push3,
                                         float gx, float gy)
{
    static bool coin_prev[JVS_MAX_PLAYERS];
    if (coin && !coin_prev[player]) {
        jvs->coin_count[player]++;
    }
    coin_prev[player] = coin;

    (void)gx;
    (void)gy;

    /*
     * Standard JVS switch layout (per Cxbx-Reloaded JvsIo):
     *   byte0: start=0x80 service=0x40 up/down/left/right=0x20..0x04
     *          push1=0x02 push2=0x01
     *   byte1: push3=0x80 push4=0x40 push5=0x20 push6=0x10 ...
     * HOTD3: push1=trigger, push2=reload, push3=SCREEN-IN sensor.
     */
    uint8_t sw0 = 0;
    if (trigger) sw0 |= 0x02;  /* push 1 */
    if (reload)  sw0 |= 0x01;  /* push 2 */
    if (start)   sw0 |= 0x80;
    if (service) sw0 |= 0x40;
    jvs->player_switches[player][0] = sw0;

    uint8_t sw1 = 0;
    if (!offscreen && !reload) {
        sw1 |= 0x80;  /* push 3: SCREEN-IN gun sensor (HOTD3) */
    }
    if (push2) sw1 |= 0x40;  /* push 4 (e.g. VC3 ES pedal candidate) */
    if (push3) sw1 |= 0x20;  /* push 5 */
    if (player == 0) {
        /* Test keys to identify per-game extra buttons: T=push5 Y=push6 */
        const bool *k = SDL_GetKeyboardState(NULL);
        if (k[SDL_SCANCODE_T]) sw1 |= 0x20;  /* push 5 */
        if (k[SDL_SCANCODE_Y]) sw1 |= 0x10;  /* push 6 */
    }
    jvs->player_switches[player][1] = sw1;
}

/*
 * Aggregated JVS input for one player, OR-combined from every source
 * (light gun, SDL gamepad, mouse, keyboard) so they all work at once.
 */
typedef struct JvsPlayerAgg {
    bool trigger, reload, start, service, coin, push2, push3;
    bool aim_valid;   /* a source is providing absolute aim this frame */
    bool has_gun;     /* a light gun is assigned to this player */
    float ax, ay;     /* aim, normalized 0..1 (top-left origin) */
    bool pad_present;   /* a gamepad is assigned to this player */
    bool pad_stick_aim; /* right stick is being used for gun aim */
    uint16_t steer;     /* JVS analog ch0: left stick X, 0x8000 centered */
    uint16_t accel;     /* JVS analog ch1: right trigger, 0..0xFFFF */
    uint16_t brake;     /* JVS analog ch2: left trigger, 0..0xFFFF */
} JvsPlayerAgg;

/* Add one SDL gamepad's buttons (and, if no other aim source, right-stick
 * aim as a virtual crosshair) to a player's aggregate. */
static void jvs_add_gamepad(JvsPlayerAgg *a, SDL_Gamepad *gp)
{
    a->pad_present = true;

    /* Digital buttons on the face/shoulder buttons only. The triggers
     * are reserved for the analog pedals below, so they don't also fire
     * a digital button (which would double-bind in driving games). */
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH))
        a->trigger = true;   /* shoot / push 1 */
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST))
        a->reload = true;    /* reload / push 2 */
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START))
        a->start = true;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK))
        a->coin = true;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST) ||
        SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
        a->push2 = true;   /* ES / weapon change */
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_NORTH) ||
        SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
        a->push3 = true;
    /* GUIDE is intentionally left for the xemu menu, not JVS service. */

    /* Driving-style analog: left stick steers, triggers are the pedals.
     * Feeds the JVS analog channels for wheel games (Crazy Taxi, Outrun,
     * Wangan). */
    int lx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX);          /* -32768..32767 */
    int rt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);  /* 0..32767 */
    int lt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    a->steer = (uint16_t)MIN(MAX(0x8000 + lx, 0), 0xFFFF);
    a->accel = (uint16_t)((uint32_t)(rt < 0 ? 0 : rt) * 0xFFFF / 32767);
    a->brake = (uint16_t)((uint32_t)(lt < 0 ? 0 : lt) * 0xFFFF / 32767);

    /* Right stick aims (gamepad-as-light-gun); when used it takes over the
     * analog channels from the driving pedals. */
    if (!a->has_gun) {
        float sx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
        float sy = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
        if (sx * sx + sy * sy > 0.04f) {   /* outside a 20% dead zone */
            a->ax = MIN(MAX(0.5f + 0.5f * sx, 0.0f), 1.0f);
            a->ay = MIN(MAX(0.5f + 0.5f * sy, 0.0f), 1.0f);
            a->aim_valid = true;
            a->pad_stick_aim = true;
        }
    }
}

static void xemu_input_update_jvs_lightgun(void)
{
    if (!chihiro_jvs_global) return;
    ChihiroJVSState *jvs = chihiro_jvs_global;

    const bool *kbd = SDL_GetKeyboardState(NULL);

    bool p1_start = kbd[g_config.input.keyboard_controller_scancode_map.start];
    bool p1_service = kbd[SDL_SCANCODE_9];

    JvsPlayerAgg agg[JVS_MAX_PLAYERS] = { 0 };

    /* Source 1: evdev light guns — gun p drives player p (aim + buttons) */
    if (xemu_input_evdev_gun_available()) {
        int num = MIN(xemu_input_evdev_gun_count(), JVS_MAX_PLAYERS);
        for (int p = 0; p < num; p++) {
            JvsPlayerAgg *a = &agg[p];
            a->has_gun = true;
            float gx = 0, gy = 0;
            if (xemu_input_evdev_gun_get_pos(p, &gx, &gy)) {
                a->ax = gx;
                a->ay = gy;
                a->aim_valid = true;
            }
            uint32_t gunBtn = xemu_input_evdev_gun_get_buttons(p);
            if (gunBtn & EVDEV_GUN_BTN_TRIGGER) a->trigger = true;
            if (gunBtn & EVDEV_GUN_BTN_RELOAD)  a->reload = true;
            if (gunBtn & EVDEV_GUN_BTN_AUX)   { a->start = true; a->push2 = true; }
            if (gunBtn & EVDEV_GUN_BTN_1)       a->coin = true;
            if (gunBtn & EVDEV_GUN_BTN_2)       a->push2 = true;
            if (gunBtn & EVDEV_GUN_BTN_3)       a->push3 = true;
        }
    }

    /* Source 2: mouse — player 0 only, and only if no gun owns player 0.
     * A gun and the OS cursor can't both provide absolute aim sanely. */
    if (!agg[0].has_gun) {
        JvsPlayerAgg *a = &agg[0];
        float mx, my;
        uint32_t mouseBtn = SDL_GetMouseState(&mx, &my);

        int32_t winW, winH;
        SDL_GetWindowSize(m_window, &winW, &winH);
        if (viewport_coords[2] > 0 && viewport_coords[3] > 0) {
            int32_t drawW, drawH;
            SDL_GetWindowSizeInPixels(m_window, &drawW, &drawH);
            float scaleW = (float)winW / (float)drawW;
            float scaleH = (float)winH / (float)drawH;
            mx -= viewport_coords[0] * scaleW;
            my -= viewport_coords[1] * scaleH;
            winW = (int)(viewport_coords[2] * scaleW);
            winH = (int)(viewport_coords[3] * scaleH);
        }
        if (mx >= 0 && mx <= winW && my >= 0 && my <= winH && winW > 0 && winH > 0) {
            a->ax = mx / winW;
            a->ay = my / winH;
            a->aim_valid = true;
        }
        if (mouseBtn & SDL_BUTTON_MASK(SDL_BUTTON_LEFT))   a->trigger = true;
        if (mouseBtn & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT))  a->reload = true;
        if (mouseBtn & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) { a->start = true; a->push2 = true; }
        if (mouseBtn & SDL_BUTTON_MASK(SDL_BUTTON_X1))     a->coin = true;
        if (mouseBtn & SDL_BUTTON_MASK(SDL_BUTTON_X2))     a->push2 = true;
    }

    /* Source 3: SDL gamepads, in parallel — gamepad i drives player i.
     * Buttons are always OR'd in; stick aim only fills a player with no
     * gun/mouse aim, so a pad works alongside a gun on the same player. */
    int gp_idx = 0;
    ControllerState *iter;
    QTAILQ_FOREACH (iter, &available_controllers, entry) {
        if (iter->type != INPUT_DEVICE_SDL_GAMEPAD || !iter->sdl_gamepad)
            continue;
        if (gp_idx >= JVS_MAX_PLAYERS)
            break;
        jvs_add_gamepad(&agg[gp_idx], iter->sdl_gamepad);
        gp_idx++;
    }

    /* Source 4: keyboard — player 0 convenience keys */
    if (kbd[SDL_SCANCODE_R]) agg[0].reload = true;
    if (kbd[SDL_SCANCODE_E]) agg[0].push2 = true;
    if (kbd[SDL_SCANCODE_5]) agg[0].coin = true;
    if (p1_start)   agg[0].start = true;
    if (p1_service) agg[0].service = true;

    /* Emit each player's aggregated state */
    for (int p = 0; p < JVS_MAX_PLAYERS; p++) {
        JvsPlayerAgg *a = &agg[p];
        bool offscreen = !a->aim_valid;
        bool trigger = a->trigger;
        bool reload = a->reload;
        /* Shooting offscreen reloads, like on the real cabinet */
        if (offscreen && trigger) {
            reload = true;
            trigger = false;
        }

        /*
         * JVS analog channels.
         *   - A real light gun owns channels p*2 / p*2+1 as X/Y.
         *   - Otherwise, a gamepad with the right stick idle drives
         *     driving-style analog (steering p*2, accel p*2+1, brake ch2
         *     for player 0). This wins over the mouse so a wheel game's
         *     pedals aren't hijacked by the cursor position.
         *   - Otherwise the mouse / right-stick aim owns X/Y.
         */
        bool driving = a->pad_present && !a->has_gun && !a->pad_stick_aim;
        if (a->has_gun) {
            jvs->analog[p * 2 + 0] = a->aim_valid ? (uint16_t)(a->ax * 0xFFFF) : 0;
            jvs->analog[p * 2 + 1] = a->aim_valid ? (uint16_t)(a->ay * 0xFFFF) : 0;
        } else if (driving) {
            jvs->analog[p * 2 + 0] = a->steer;
            jvs->analog[p * 2 + 1] = a->accel;
            if (p == 0) {
                jvs->analog[2] = a->brake;
            }
        } else if (a->aim_valid) {
            jvs->analog[p * 2 + 0] = (uint16_t)(a->ax * 0xFFFF);
            jvs->analog[p * 2 + 1] = (uint16_t)(a->ay * 0xFFFF);
        } else {
            jvs->analog[p * 2 + 0] = 0;
            jvs->analog[p * 2 + 1] = 0;
        }

        xemu_input_update_jvs_player(jvs, p, offscreen, trigger, reload,
                                     a->start, a->service, a->coin,
                                     a->push2, a->push3, a->ax, a->ay);
    }

    jvs->system_switches = kbd[SDL_SCANCODE_F2] ? 0x80 : 0x00;

    /*
     * Test/debug hook: OR extra JVS state from a file, so switch
     * mappings can be exercised without a real input device.
     * XEMU_JVS_TEST_INPUT=<path>; file holds "sw0 sw1 test" hex bytes.
     */
    static const char *jvs_override = (const char *)-1;
    if (jvs_override == (const char *)-1) {
        jvs_override = getenv("XEMU_JVS_TEST_INPUT");
    }
    if (jvs_override) {
        FILE *f = fopen(jvs_override, "r");
        if (f) {
            unsigned s0 = 0, s1 = 0, t = 0;
            if (fscanf(f, "%x %x %x", &s0, &s1, &t) >= 1) {
                jvs->player_switches[0][0] |= (uint8_t)s0;
                jvs->player_switches[0][1] |= (uint8_t)s1;
                if (t) {
                    jvs->system_switches |= 0x80;
                }
            }
            fclose(f);
        }
    }
}

void xemu_input_update_controllers(void)
{
    if (xemu_input_lightgun_active() || chihiro_jvs_global) {
        xemu_input_evdev_gun_poll();
    }

    ControllerState *iter;
    QTAILQ_FOREACH (iter, &available_controllers, entry) {
        xemu_input_update_controller(iter);
    }
    QTAILQ_FOREACH (iter, &available_controllers, entry) {
        xemu_input_update_rumble(iter);
    }
    xemu_input_update_jvs_lightgun();
}

void xemu_input_update_sdl_kbd_controller_state(ControllerState *state)
{
    state->gp.buttons = 0;
    state->lg.buttons = 0;
    memset(state->gp.axis, 0, sizeof(state->gp.axis));
    memset(state->lg.axis, 0, sizeof(state->lg.axis));

    const bool *kbd = SDL_GetKeyboardState(NULL);

    if (state->bound < 0)
        return;

    const char *bound_driver = get_bound_driver(state->bound);
    if (strcmp(bound_driver, DRIVER_LIGHT_GUN) == 0 &&
        xemu_input_evdev_gun_available()) {
        // Aim comes straight from the evdev device (ID_INPUT_GUN in
        // priority); the SDL pointer is bypassed entirely.
        float gx, gy;
        if (xemu_input_evdev_gun_get_pos(0, &gx, &gy)) {
            int32_t x = (int32_t)((gx - 0.5f) * 65535.0f);
            int32_t y = (int32_t)((0.5f - gy) * 65535.0f);
            state->lg.axis[0] = (int16_t)MIN(MAX(x, -32768), 32767);
            state->lg.axis[1] = (int16_t)MIN(MAX(y, -32768), 32767);
            state->lg.status = 0x20; // Light Visible
        } else {
            state->lg.status = 0x00;
        }

        uint32_t gunBtn = xemu_input_evdev_gun_get_buttons(0);
        if (gunBtn & EVDEV_GUN_BTN_TRIGGER)
            state->lg.buttons |= CONTROLLER_BUTTON_A;
        if (gunBtn & EVDEV_GUN_BTN_RELOAD)
            state->lg.buttons |= CONTROLLER_BUTTON_B;
        if (gunBtn & EVDEV_GUN_BTN_AUX)
            state->lg.buttons |= CONTROLLER_BUTTON_START;

        if (kbd[g_config.input.keyboard_controller_scancode_map.a])
            state->lg.buttons |= CONTROLLER_BUTTON_A;
        if (kbd[g_config.input.keyboard_controller_scancode_map.b])
            state->lg.buttons |= CONTROLLER_BUTTON_B;
        if (kbd[g_config.input.keyboard_controller_scancode_map.start])
            state->lg.buttons |= CONTROLLER_BUTTON_START;
        if (kbd[g_config.input.keyboard_controller_scancode_map.back])
            state->lg.buttons |= CONTROLLER_BUTTON_BACK;

    } else if (strcmp(bound_driver, DRIVER_LIGHT_GUN) == 0) {
        uint32_t mouseBtn = SDL_GetMouseState(&m_mouseX, &m_mouseY);

        int32_t windowWidth, windowHeight;
        // Use SDL_GetWindowSize to match SDL_GetMouseState coordinate space
        // (both return logical/window coordinates, not physical/drawable pixels)
        SDL_GetWindowSize(m_window, &windowWidth, &windowHeight);

        DPRINTF("[Lightgun] Window Coordinates: %.0f, %.0f\n", m_mouseX, m_mouseY);

        // Adjust to viewport coordinates if available
        if (viewport_coords[2] > 0 && viewport_coords[3] > 0) {
            // viewport_coords are in drawable (pixel) space.
            // Scale them to window (logical) space for HiDPI compat.
            int32_t drawW, drawH;
            SDL_GetWindowSizeInPixels(m_window, &drawW, &drawH);
            float scaleW = (float)windowWidth / (float)drawW;
            float scaleH = (float)windowHeight / (float)drawH;

            // Switch from Window coordinates to Viewport Coordinates
            m_mouseX -= viewport_coords[0] * scaleW;
            m_mouseY -= viewport_coords[1] * scaleH;
            windowWidth = (int)(viewport_coords[2] * scaleW);
            windowHeight = (int)(viewport_coords[3] * scaleH);
        }

        // Check bounds AFTER viewport adjustment — mouse must be inside
        // the actual game viewport, not just the window
        if (m_mouseX >= 0 && m_mouseX <= windowWidth &&
            m_mouseY >= 0 && m_mouseY <= windowHeight) {

            DPRINTF("[Lightgun] Viewport Coordinates: %.0f, %.0f\n", m_mouseX, m_mouseY);
            // Direct linear mapping - no scale/offset correction needed.
            // Emulated gun provides pixel-perfect coordinates.
            int32_t x = (int32_t)((m_mouseX - (windowWidth / 2)) *
                                  65535 / windowWidth);
            int32_t y = (int32_t)(((windowHeight / 2) - m_mouseY) *
                                  65535 / windowHeight);

            state->lg.axis[0] = (int16_t)MIN(MAX(x, -32768), 32767);
            state->lg.axis[1] = (int16_t)MIN(MAX(y, -32768), 32767);
            state->lg.status = 0x20; // Light Visible

            DPRINTF("[LightGun] X: %d, Y: %d", state->lg.axis[0], state->lg.axis[1]);
        } else {
            state->lg.status = 0x00;
        }

        // Left mouse button is the trigger (A), right mouse button is B
        if (mouseBtn & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) {
            state->lg.buttons |= CONTROLLER_BUTTON_A;
        }
        if (mouseBtn & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) {
            state->lg.buttons |= CONTROLLER_BUTTON_B;
        }

        if (kbd[g_config.input.keyboard_controller_scancode_map.a])
            state->lg.buttons |= CONTROLLER_BUTTON_A;
        if (kbd[g_config.input.keyboard_controller_scancode_map.b])
            state->lg.buttons |= CONTROLLER_BUTTON_B;
        if (kbd[g_config.input.keyboard_controller_scancode_map.x])
            state->lg.buttons |= CONTROLLER_BUTTON_X;
        if (kbd[g_config.input.keyboard_controller_scancode_map.y])
            state->lg.buttons |= CONTROLLER_BUTTON_Y;
        if (kbd[g_config.input.keyboard_controller_scancode_map.start])
            state->lg.buttons |= CONTROLLER_BUTTON_START;
        if (kbd[g_config.input.keyboard_controller_scancode_map.back])
            state->lg.buttons |= CONTROLLER_BUTTON_BACK;
        if (kbd[g_config.input.keyboard_controller_scancode_map.black])
            state->lg.buttons |= CONTROLLER_BUTTON_BLACK;
        if (kbd[g_config.input.keyboard_controller_scancode_map.white])
            state->lg.buttons |= CONTROLLER_BUTTON_WHITE;
        if (kbd[g_config.input.keyboard_controller_scancode_map.dpad_up])
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_UP;
        if (kbd[g_config.input.keyboard_controller_scancode_map.dpad_down])
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_DOWN;
        if (kbd[g_config.input.keyboard_controller_scancode_map.dpad_left])
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_LEFT;
        if (kbd[g_config.input.keyboard_controller_scancode_map.dpad_right])
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_RIGHT;

    } else {
#define KBD_STATE(btn) \
        (kbd[g_config.input.keyboard_controller_scancode_map.btn])

        state->gp.buttons |= KBD_STATE(a) << 0;
        state->gp.buttons |= KBD_STATE(b) << 1;
        state->gp.buttons |= KBD_STATE(x) << 2;
        state->gp.buttons |= KBD_STATE(y) << 3;
        state->gp.buttons |= KBD_STATE(dpad_left) << 4;
        state->gp.buttons |= KBD_STATE(dpad_up) << 5;
        state->gp.buttons |= KBD_STATE(dpad_right) << 6;
        state->gp.buttons |= KBD_STATE(dpad_down) << 7;
        state->gp.buttons |= KBD_STATE(back) << 8;
        state->gp.buttons |= KBD_STATE(start) << 9;
        state->gp.buttons |= KBD_STATE(white) << 10;
        state->gp.buttons |= KBD_STATE(black) << 11;
        state->gp.buttons |= KBD_STATE(lstick_btn) << 12;
        state->gp.buttons |= KBD_STATE(rstick_btn) << 13;
        state->gp.buttons |= KBD_STATE(guide) << 14;

        if (KBD_STATE(lstick_up))
            state->gp.axis[CONTROLLER_AXIS_LSTICK_Y] = 32767;
        if (KBD_STATE(lstick_left))
            state->gp.axis[CONTROLLER_AXIS_LSTICK_X] = -32768;
        if (KBD_STATE(lstick_right))
            state->gp.axis[CONTROLLER_AXIS_LSTICK_X] = 32767;
        if (KBD_STATE(lstick_down))
            state->gp.axis[CONTROLLER_AXIS_LSTICK_Y] = -32768;
        if (KBD_STATE(ltrigger))
            state->gp.axis[CONTROLLER_AXIS_LTRIG] = 32767;

        if (KBD_STATE(rstick_up))
            state->gp.axis[CONTROLLER_AXIS_RSTICK_Y] = 32767;
        if (KBD_STATE(rstick_left))
            state->gp.axis[CONTROLLER_AXIS_RSTICK_X] = -32768;
        if (KBD_STATE(rstick_right))
            state->gp.axis[CONTROLLER_AXIS_RSTICK_X] = 32767;
        if (KBD_STATE(rstick_down))
            state->gp.axis[CONTROLLER_AXIS_RSTICK_Y] = -32768;
        if (KBD_STATE(rtrigger))
            state->gp.axis[CONTROLLER_AXIS_RTRIG] = 32767;

#undef KBD_STATE
    }
}

void xemu_input_update_sdl_controller_state(ControllerState *state)
{
    state->gp.buttons = 0;
    memset(state->gp.axis, 0, sizeof(state->gp.axis));

    if (state->bound < 0)
        return;

    const char *bound_driver = get_bound_driver(state->bound);
    if (strcmp(bound_driver, DRIVER_LIGHT_GUN) == 0) {
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_EAST))
            state->lg.buttons |= CONTROLLER_BUTTON_A;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_SOUTH))
            state->lg.buttons |= CONTROLLER_BUTTON_B;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_WEST))
            state->lg.buttons |= CONTROLLER_BUTTON_X;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_NORTH))
            state->lg.buttons |= CONTROLLER_BUTTON_Y;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_START))
            state->lg.buttons |= CONTROLLER_BUTTON_START;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_BACK))
            state->lg.buttons |= CONTROLLER_BUTTON_BACK;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
            state->lg.buttons |= CONTROLLER_BUTTON_BLACK;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
            state->lg.buttons |= CONTROLLER_BUTTON_WHITE;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_UP;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_DOWN;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_LEFT;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_RIGHT;

        state->lg.axis[0] = SDL_GetGamepadAxis(
            state->sdl_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        state->lg.axis[1] = SDL_GetGamepadAxis(
            state->sdl_gamepad, SDL_GAMEPAD_AXIS_LEFTY);

    } else {
#define SDL_MASK_BUTTON(state, btn, idx)                  \
    (SDL_GetGamepadButton(                                \
         (state)->sdl_gamepad,                            \
         (state)->controller_map->controller_mapping.btn) \
     << idx)

        state->gp.buttons |= SDL_MASK_BUTTON(state, a, 0);
        state->gp.buttons |= SDL_MASK_BUTTON(state, b, 1);
        state->gp.buttons |= SDL_MASK_BUTTON(state, x, 2);
        state->gp.buttons |= SDL_MASK_BUTTON(state, y, 3);
        state->gp.buttons |= SDL_MASK_BUTTON(state, dpad_left, 4);
        state->gp.buttons |= SDL_MASK_BUTTON(state, dpad_up, 5);
        state->gp.buttons |= SDL_MASK_BUTTON(state, dpad_right, 6);
        state->gp.buttons |= SDL_MASK_BUTTON(state, dpad_down, 7);
        state->gp.buttons |= SDL_MASK_BUTTON(state, back, 8);
        state->gp.buttons |= SDL_MASK_BUTTON(state, start, 9);
        state->gp.buttons |= SDL_MASK_BUTTON(state, lshoulder, 10);
        state->gp.buttons |= SDL_MASK_BUTTON(state, rshoulder, 11);
        state->gp.buttons |= SDL_MASK_BUTTON(state, lstick_btn, 12);
        state->gp.buttons |= SDL_MASK_BUTTON(state, rstick_btn, 13);
        state->gp.buttons |= SDL_MASK_BUTTON(state, guide, 14);

#undef SDL_MASK_BUTTON

#define SDL_GET_AXIS(state, axis)    \
    SDL_GetGamepadAxis(              \
        (state)->sdl_gamepad,        \
        (state)->controller_map->controller_mapping.axis)

        state->gp.axis[0] = SDL_GET_AXIS(state, axis_trigger_left);
        state->gp.axis[1] = SDL_GET_AXIS(state, axis_trigger_right);
        state->gp.axis[2] = SDL_GET_AXIS(state, axis_left_x);
        state->gp.axis[3] = SDL_GET_AXIS(state, axis_left_y);
        state->gp.axis[4] = SDL_GET_AXIS(state, axis_right_x);
        state->gp.axis[5] = SDL_GET_AXIS(state, axis_right_y);

#undef SDL_GET_AXIS

// FIXME: Check range
#define INVERT_AXIS(controller_axis) \
        state->gp.axis[controller_axis] = -1 - state->gp.axis[controller_axis]

        if (state->controller_map->controller_mapping.invert_axis_left_x) {
            INVERT_AXIS(CONTROLLER_AXIS_LSTICK_X);
        }

        if (!state->controller_map->controller_mapping.invert_axis_left_y) {
            INVERT_AXIS(CONTROLLER_AXIS_LSTICK_Y);
        }

        if (state->controller_map->controller_mapping.invert_axis_right_x) {
            INVERT_AXIS(CONTROLLER_AXIS_RSTICK_X);
        }

        if (!state->controller_map->controller_mapping.invert_axis_right_y) {
            INVERT_AXIS(CONTROLLER_AXIS_RSTICK_Y);
        }

#undef INVERT_AXIS

        // xemu_input_print_controller_state(state);
    }
}

void xemu_input_update_rumble(ControllerState *state)
{
    if (state->type != INPUT_DEVICE_SDL_GAMEPAD) {
        return;
    }

    if (!state->controller_map->enable_rumble) {
        return;
    }

    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_rumble_updated_ts) <
        XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US) {
        return;
    }

    SDL_RumbleGamepad(state->sdl_gamepad, state->gp.rumble_l, state->gp.rumble_r, 250);
    state->last_rumble_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

ControllerState *xemu_input_get_bound(int index)
{
    return bound_controllers[index];
}

void xemu_input_bind(int index, ControllerState *state, int save)
{
    // FIXME: Attempt to disable rumble when unbinding so it's not left
    // in rumble mode

    // Unbind existing controller
    if (bound_controllers[index]) {
        assert(bound_controllers[index]->device != NULL);
        Error *err = NULL;

        // Unbind any XMUs
        for (int i = 0; i < 2; i++) {
            if (bound_controllers[index]->peripherals[i]) {
                // If this was an XMU, unbind the XMU
                if (bound_controllers[index]->peripheral_types[i] ==
                    PERIPHERAL_XMU)
                    xemu_input_unbind_xmu(index, i);

                // Free up the XmuState and set the peripheral type to none
                g_free(bound_controllers[index]->peripherals[i]);
                bound_controllers[index]->peripherals[i] = NULL;
                bound_controllers[index]->peripheral_types[i] = PERIPHERAL_NONE;
            }
        }

        qdev_unplug((DeviceState *)bound_controllers[index]->device, &err);
        assert(err == NULL);

        bound_controllers[index]->bound = -1;
        bound_controllers[index]->device = NULL;
        bound_controllers[index] = NULL;
    }

    // Save this controller's GUID in settings for auto re-connect
    if (save) {
        char guid_buf[35] = { 0 };
        if (state) {
            if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
                SDL_GUIDToString(state->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
            } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
                snprintf(guid_buf, sizeof(guid_buf), "keyboard");
            }
        }
        xemu_settings_set_string(port_index_to_settings_key_map[index], guid_buf);
        xemu_settings_set_string(port_index_to_driver_settings_key_map[index],
                                 bound_drivers[index]);
    }

    // Bind new controller
    if (state) {
        if (state->bound >= 0) {
            // Device was already bound to another port. Unbind it.
            xemu_input_bind(state->bound, NULL, 1);
        }

        bound_controllers[index] = state;
        bound_controllers[index]->bound = index;

        /* In Chihiro mode, USB ports are used by baseboard AN2131 devices.
         * Skip gamepad hub/controller creation — input comes via JVS I/O. */
        int mem_chk = ((int)g_config.sys.mem_limit + 1) * 64;
        if (mem_chk > 64) {
            return;
        }

        char *tmp;
        QDict *usbhub_qdict = NULL;
        DeviceState *usbhub_dev = NULL;

        // Create controller's internal USB hub.
        usbhub_qdict = qdict_new();
        qdict_put_str(usbhub_qdict, "driver", "usb-hub");
        tmp = g_strdup_printf("1.%d", port_map[index]);
        qdict_put_str(usbhub_qdict, "port", tmp);
        qdict_put_int(usbhub_qdict, "ports", 3);
        QemuOpts *usbhub_opts = qemu_opts_from_qdict(
            qemu_find_opts("device"), usbhub_qdict, &error_abort);
        usbhub_dev = qdev_device_add(usbhub_opts, &error_abort);
        g_free(tmp);

        // Create XID controller. This is connected to Port 1 of the
        // controller's internal USB Hub
        QDict *qdict = qdict_new();

        // Specify device driver
        qdict_put_str(qdict, "driver", bound_drivers[index]);

        // Specify device identifier
        static int id_counter = 0;
        tmp = g_strdup_printf("gamepad_%d", id_counter++);
        qdict_put_str(qdict, "id", tmp);
        g_free(tmp);

        // Specify index/port
        qdict_put_int(qdict, "index", index);
        tmp = g_strdup_printf("1.%d.1", port_map[index]);
        qdict_put_str(qdict, "port", tmp);
        g_free(tmp);

        // Create the device
        QemuOpts *opts = 
            qemu_opts_from_qdict(qemu_find_opts("device"), qdict, &error_abort);
        DeviceState *dev = qdev_device_add(opts, &error_abort);
        assert(dev);

        // Unref for eventual cleanup
        qobject_unref(usbhub_qdict);
        object_unref(OBJECT(usbhub_dev));
        qobject_unref(qdict);
        object_unref(OBJECT(dev));

        state->device = usbhub_dev;
    }
}

bool xemu_input_bind_xmu(int player_index, int expansion_slot_index,
                         const char *filename, bool is_rebind)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);

    ControllerState *player = bound_controllers[player_index];
    enum peripheral_type peripheral_type =
        player->peripheral_types[expansion_slot_index];
    if (peripheral_type != PERIPHERAL_XMU)
        return false;

    XmuState *xmu = (XmuState *)player->peripherals[expansion_slot_index];

    // Unbind existing XMU
    if (xmu->dev != NULL) {
        xemu_input_unbind_xmu(player_index, expansion_slot_index);
    }

    if (filename == NULL)
        return false;

    // Look for any other XMUs that are using this file, and unbind them
    for (int player_i = 0; player_i < 4; player_i++) {
        ControllerState *state = bound_controllers[player_i];
        if (state != NULL) {
            for (int peripheral_i = 0; peripheral_i < 2; peripheral_i++) {
                if (state->peripheral_types[peripheral_i] == PERIPHERAL_XMU) {
                    XmuState *xmu_i =
                        (XmuState *)state->peripherals[peripheral_i];
                    assert(xmu_i);

                    if (xmu_i->filename != NULL &&
                        strcmp(xmu_i->filename, filename) == 0) {
                        char *buf =
                            g_strdup_printf("This XMU is already mounted on "
                                            "player %d slot %c\r\n",
                                            player_i + 1, 'A' + peripheral_i);
                        xemu_queue_notification(buf);
                        g_free(buf);
                        return false;
                    }
                }
            }
        }
    }

    xmu->filename = g_strdup(filename);

    const int xmu_map[2] = { 2, 3 };
    char *tmp;

    static int id_counter = 0;
    tmp = g_strdup_printf("xmu_%d", id_counter++);

    // Add the file as a drive
    QDict *qdict1 = qdict_new();
    qdict_put_str(qdict1, "id", tmp);
    qdict_put_str(qdict1, "format", "raw");
    qdict_put_str(qdict1, "file", filename);

    QemuOpts *drvopts =
        qemu_opts_from_qdict(qemu_find_opts("drive"), qdict1, &error_abort);

    DriveInfo *dinfo = drive_new(drvopts, 0, &error_abort);
    assert(dinfo);

    // Create the usb-storage device
    QDict *qdict2 = qdict_new();

    // Specify device driver
    qdict_put_str(qdict2, "driver", "usb-storage");

    // Specify device identifier
    qdict_put_str(qdict2, "drive", tmp);
    g_free(tmp);

    // Specify index/port
    tmp = g_strdup_printf("1.%d.%d", port_map[player_index],
                          xmu_map[expansion_slot_index]);
    qdict_put_str(qdict2, "port", tmp);
    g_free(tmp);

    // Create the device
    QemuOpts *opts =
        qemu_opts_from_qdict(qemu_find_opts("device"), qdict2, &error_abort);

    DeviceState *dev = qdev_device_add(opts, &error_abort);
    assert(dev);

    xmu->dev = (void *)dev;

    // Unref for eventual cleanup
    qobject_unref(qdict1);
    qobject_unref(qdict2);

    if (!is_rebind) {
        xemu_save_peripheral_settings(player_index, expansion_slot_index,
                                      peripheral_type, xmu->filename);
    }

    return true;
}

void xemu_input_unbind_xmu(int player_index, int expansion_slot_index)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);

    ControllerState *state = bound_controllers[player_index];
    if (state->peripheral_types[expansion_slot_index] != PERIPHERAL_XMU)
        return;

    XmuState *xmu = (XmuState *)state->peripherals[expansion_slot_index];
    if (xmu != NULL) {
        if (xmu->dev != NULL) {
            qdev_unplug((DeviceState *)xmu->dev, &error_abort);
            object_unref(OBJECT(xmu->dev));
            xmu->dev = NULL;
        }

        g_free((void *)xmu->filename);
        xmu->filename = NULL;
    }
}

void xemu_input_rebind_xmu(int port)
{
    // Try to bind peripherals back to controller
    for (int i = 0; i < 2; i++) {
        enum peripheral_type peripheral_type =
            (enum peripheral_type)(*peripheral_types_settings_map[port][i]);

        // If peripheralType is out of range, change the settings for this
        // controller and peripheral port to default
        if (peripheral_type < PERIPHERAL_NONE ||
            peripheral_type >= PERIPHERAL_TYPE_COUNT) {
            xemu_save_peripheral_settings(port, i, PERIPHERAL_NONE, NULL);
            peripheral_type = PERIPHERAL_NONE;
        }

        const char *param = *peripheral_params_settings_map[port][i];

        if (peripheral_type == PERIPHERAL_XMU) {
            if (param != NULL && strlen(param) > 0) {
                // This is an XMU and needs to be bound to this controller
                if (qemu_access(param, R_OK | W_OK) == 0) {
                    bound_controllers[port]->peripheral_types[i] =
                        peripheral_type;
                    bound_controllers[port]->peripherals[i] =
                        g_malloc(sizeof(XmuState));
                    memset(bound_controllers[port]->peripherals[i], 0,
                           sizeof(XmuState));
                    bool did_bind = xemu_input_bind_xmu(port, i, param, true);
                    if (did_bind) {
                        char *buf =
                            g_strdup_printf("Connected XMU %s to port %d%c",
                                            param, port + 1, 'A' + i);
                        xemu_queue_notification(buf);
                        g_free(buf);
                    }
                } else {
                    char *buf =
                        g_strdup_printf("Unable to bind XMU at %s to port %d%c",
                                        param, port + 1, 'A' + i);
                    xemu_queue_error_message(buf);
                    g_free(buf);
                }
            }
        }
    }
}

void xemu_input_set_test_mode(int enabled)
{
    test_mode = enabled;
}

int xemu_input_get_test_mode(void)
{
    return test_mode;
}

void xemu_input_reset_input_mapping(ControllerState *state)
{
    if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        char guid[35] = { 0 };
        SDL_GUIDToString(state->sdl_joystick_guid, guid, sizeof(guid));
        xemu_settings_reset_controller_mapping(guid);
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_settings_reset_keyboard_mapping();
    }
}

int xemu_input_lightgun_active(void)
{
    /* Chihiro JVS gun games use the pointer as a light gun, too:
     * keep right-click reload from opening the xemu menu */
    if (chihiro_jvs_global) {
        return 1;
    }

    for (int i = 0; i < 4; i++) {
        if (bound_drivers[i] &&
            strcmp(bound_drivers[i], DRIVER_LIGHT_GUN) == 0) {
            return 1;
        }
    }
    return 0;
}
