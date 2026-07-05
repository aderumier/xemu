/*
 * xemu Raw Input mouse support
 *
 * Enumerates HID mice individually via the Windows Raw Input API so that
 * multiple pointer devices (e.g. Sinden Lightguns, which appear as absolute
 * mice) can be told apart and bound to different controller ports.
 *
 * Copyright (C) 2026 xemu contributors
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

#include "xemu-rawinput.h"
#include "xemu-input.h"
#include "xemu-notifications.h"

#ifdef _WIN32

#include <math.h>
#include <windows.h>
#include <SDL3/SDL_system.h>

// #define DEBUG_RAWINPUT
#ifdef DEBUG_RAWINPUT
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

// HidD_GetProductString, loaded dynamically to avoid a hid.dll link dep
typedef BOOLEAN(WINAPI *HidD_GetProductString_t)(HANDLE, PVOID, ULONG);
static HidD_GetProductString_t p_HidD_GetProductString;

static HWND g_hwnd;
static bool g_initialized;

static uint32_t fnv1a_hash(const char *s)
{
    uint32_t h = 0x811c9dc5;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193;
    }
    return h;
}

static ControllerState *rawinput_find_controller(HANDLE hdev)
{
    ControllerState *iter;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        if (iter->type == INPUT_DEVICE_RAWINPUT_MOUSE &&
            iter->rawinput_handle == (void *)hdev) {
            return iter;
        }
    }
    return NULL;
}

static char *rawinput_get_device_path(HANDLE hdev)
{
    UINT size = 0;
    if (GetRawInputDeviceInfoW(hdev, RIDI_DEVICENAME, NULL, &size) != 0 ||
        size == 0) {
        return NULL;
    }
    WCHAR *wpath = g_malloc((size + 1) * sizeof(WCHAR));
    if (GetRawInputDeviceInfoW(hdev, RIDI_DEVICENAME, wpath, &size) ==
        (UINT)-1) {
        g_free(wpath);
        return NULL;
    }
    wpath[size] = 0;
    char *path = g_utf16_to_utf8((const gunichar2 *)wpath, -1, NULL, NULL,
                                 NULL);
    g_free(wpath);
    return path;
}

static char *rawinput_get_product_name(const char *path)
{
    char *name = NULL;

    if (p_HidD_GetProductString) {
        HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WCHAR product[128] = { 0 };
            if (p_HidD_GetProductString(h, product, sizeof(product) - 2)) {
                if (product[0]) {
                    name = g_utf16_to_utf8((const gunichar2 *)product, -1,
                                           NULL, NULL, NULL);
                }
            }
            CloseHandle(h);
        }
    }

    if (name == NULL || name[0] == '\0') {
        g_free(name);
        name = g_strdup("HID Mouse");
    }
    return name;
}

static void rawinput_add_device(HANDLE hdev)
{
    RID_DEVICE_INFO info = { .cbSize = sizeof(info) };
    UINT info_size = sizeof(info);

    if (rawinput_find_controller(hdev)) {
        return;
    }
    if (GetRawInputDeviceInfoW(hdev, RIDI_DEVICEINFO, &info, &info_size) ==
            (UINT)-1 ||
        info.dwType != RIM_TYPEMOUSE) {
        return;
    }

    char *path = rawinput_get_device_path(hdev);
    if (path == NULL) {
        return;
    }

    // Skip the terminal services virtual mouse
    if (strstr(path, "RDP_MOU") != NULL) {
        g_free(path);
        return;
    }

    char *product = rawinput_get_product_name(path);

    // Disambiguate devices with identical product strings
    int same_name = 0;
    ControllerState *iter;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        if (iter->type == INPUT_DEVICE_RAWINPUT_MOUSE &&
            strncmp(iter->name, product, strlen(product)) == 0) {
            same_name++;
        }
    }
    char *name;
    if (same_name > 0) {
        name = g_strdup_printf("%s #%d", product, same_name + 1);
        g_free(product);
    } else {
        name = product;
    }

    ControllerState *new_con = malloc(sizeof(ControllerState));
    memset(new_con, 0, sizeof(ControllerState));
    new_con->type = INPUT_DEVICE_RAWINPUT_MOUSE;
    new_con->name = name;
    new_con->rawinput_handle = (void *)hdev;
    new_con->rawinput_path = path;
    snprintf(new_con->rawinput_guid, sizeof(new_con->rawinput_guid),
             "mouse:%08x", fnv1a_hash(path));
    new_con->bound = -1;
    new_con->peripheral_types[0] = PERIPHERAL_NONE;
    new_con->peripheral_types[1] = PERIPHERAL_NONE;

    QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
    DPRINTF("rawinput: added '%s' (%s) as %s\n", new_con->name, path,
            new_con->rawinput_guid);

    // Re-bind to a previously saved port. Unlike gamepads, never auto-bind
    // a mouse to a free port: every system has at least one regular mouse
    // and grabbing a controller port with it would be surprising.
    int port = 0;
    while (1) {
        port = xemu_input_get_controller_default_bind_port(new_con, port);
        if (port < 0) {
            break;
        }
        if (!xemu_input_get_bound(port)) {
            xemu_input_bind(port, new_con, 0);
            char buf[128];
            snprintf(buf, sizeof(buf), "Connected '%s' to port %d",
                     new_con->name, port + 1);
            xemu_queue_notification(buf);
            break;
        }
        port++;
    }
}

static void rawinput_remove_device(HANDLE hdev)
{
    ControllerState *con = rawinput_find_controller(hdev);
    if (con == NULL) {
        return;
    }

    DPRINTF("rawinput: removed '%s'\n", con->name);

    if (con->bound >= 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Port %d disconnected", con->bound + 1);
        xemu_queue_notification(buf);

        // Unbind, but don't save the unbinding so the device is bound to
        // the same port again when reconnected
        xemu_input_bind(con->bound, NULL, 0);
    }

    QTAILQ_REMOVE(&available_controllers, con, entry);
    g_free(con->rawinput_path);
    g_free((char *)con->name);
    free(con);
}

static void rawinput_handle_mouse_input(HANDLE hdev, const RAWMOUSE *mouse)
{
    ControllerState *con = rawinput_find_controller(hdev);
    if (con == NULL) {
        return;
    }

    if (mouse->usFlags & MOUSE_MOVE_ABSOLUTE) {
        // Absolute coordinates, normalized to [0, 65535] over the screen
        // (or the whole virtual desktop if MOUSE_VIRTUAL_DESKTOP is set).
        // This is what Sinden Lightguns report.
        bool virt = mouse->usFlags & MOUSE_VIRTUAL_DESKTOP;
        int ox = virt ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
        int oy = virt ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
        int w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        int h = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);

        POINT pt = { ox + MulDiv(mouse->lLastX, w, 65535),
                     oy + MulDiv(mouse->lLastY, h, 65535) };
        ScreenToClient(g_hwnd, &pt);
        con->rawinput_client_x = pt.x;
        con->rawinput_client_y = pt.y;
        con->rawinput_has_abs = true;
    }
    // Relative devices are handled in
    // xemu_rawinput_update_controller_state() by following the system
    // cursor, which tracks them anyway.

    USHORT f = mouse->usButtonFlags;
    if (f & RI_MOUSE_LEFT_BUTTON_DOWN)
        con->rawinput_buttons |= XEMU_RAWINPUT_BUTTON_LEFT;
    if (f & RI_MOUSE_LEFT_BUTTON_UP)
        con->rawinput_buttons &= ~XEMU_RAWINPUT_BUTTON_LEFT;
    if (f & RI_MOUSE_RIGHT_BUTTON_DOWN)
        con->rawinput_buttons |= XEMU_RAWINPUT_BUTTON_RIGHT;
    if (f & RI_MOUSE_RIGHT_BUTTON_UP)
        con->rawinput_buttons &= ~XEMU_RAWINPUT_BUTTON_RIGHT;
    if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN)
        con->rawinput_buttons |= XEMU_RAWINPUT_BUTTON_MIDDLE;
    if (f & RI_MOUSE_MIDDLE_BUTTON_UP)
        con->rawinput_buttons &= ~XEMU_RAWINPUT_BUTTON_MIDDLE;
    if (f & RI_MOUSE_BUTTON_4_DOWN)
        con->rawinput_buttons |= XEMU_RAWINPUT_BUTTON_X1;
    if (f & RI_MOUSE_BUTTON_4_UP)
        con->rawinput_buttons &= ~XEMU_RAWINPUT_BUTTON_X1;
    if (f & RI_MOUSE_BUTTON_5_DOWN)
        con->rawinput_buttons |= XEMU_RAWINPUT_BUTTON_X2;
    if (f & RI_MOUSE_BUTTON_5_UP)
        con->rawinput_buttons &= ~XEMU_RAWINPUT_BUTTON_X2;
}

// Hotplug events arrive while SDL pumps the message loop, outside the QEMU
// main loop lock. Binding/unbinding devices there is unsafe, so queue them
// and let xemu_rawinput_process_pending() handle them under the lock.
#define MAX_PENDING_DEVICE_CHANGES 16
static struct {
    HANDLE hdev;
    bool arrival;
} g_pending_changes[MAX_PENDING_DEVICE_CHANGES];
static int g_num_pending_changes;

static bool rawinput_message_hook(void *userdata, MSG *msg)
{
    if (msg->message == WM_INPUT) {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData((HRAWINPUT)msg->lParam, RID_INPUT, &raw, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            raw.header.dwType == RIM_TYPEMOUSE) {
            rawinput_handle_mouse_input(raw.header.hDevice, &raw.data.mouse);
        }
    } else if (msg->message == WM_INPUT_DEVICE_CHANGE) {
        if ((msg->wParam == GIDC_ARRIVAL || msg->wParam == GIDC_REMOVAL) &&
            g_num_pending_changes < MAX_PENDING_DEVICE_CHANGES) {
            g_pending_changes[g_num_pending_changes].hdev =
                (HANDLE)msg->lParam;
            g_pending_changes[g_num_pending_changes].arrival =
                msg->wParam == GIDC_ARRIVAL;
            g_num_pending_changes++;
        }
    }
    return true; // let SDL continue processing the message
}

void xemu_rawinput_process_pending(void)
{
    for (int i = 0; i < g_num_pending_changes; i++) {
        if (g_pending_changes[i].arrival) {
            rawinput_add_device(g_pending_changes[i].hdev);
        } else {
            rawinput_remove_device(g_pending_changes[i].hdev);
        }
    }
    g_num_pending_changes = 0;
}

void xemu_rawinput_init(SDL_Window *window)
{
    assert(!g_initialized);

    g_hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                          SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                          NULL);
    if (g_hwnd == NULL) {
        fprintf(stderr, "rawinput: could not get window handle\n");
        return;
    }

    HMODULE hid = LoadLibraryA("hid.dll");
    if (hid) {
        p_HidD_GetProductString = (HidD_GetProductString_t)GetProcAddress(
            hid, "HidD_GetProductString");
    }

    // Enumerate mice already connected
    UINT num_devices = 0;
    if (GetRawInputDeviceList(NULL, &num_devices,
                              sizeof(RAWINPUTDEVICELIST)) == 0 &&
        num_devices > 0) {
        RAWINPUTDEVICELIST *list =
            g_new0(RAWINPUTDEVICELIST, num_devices);
        UINT n = GetRawInputDeviceList(list, &num_devices,
                                       sizeof(RAWINPUTDEVICELIST));
        if (n != (UINT)-1) {
            for (UINT i = 0; i < n; i++) {
                if (list[i].dwType == RIM_TYPEMOUSE) {
                    rawinput_add_device(list[i].hDevice);
                }
            }
        }
        g_free(list);
    }

    // Receive WM_INPUT for mice (also when unfocused) plus hotplug events
    RAWINPUTDEVICE rid = {
        .usUsagePage = 0x01, // HID_USAGE_PAGE_GENERIC
        .usUsage = 0x02,     // HID_USAGE_GENERIC_MOUSE
        .dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
        .hwndTarget = g_hwnd,
    };
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        fprintf(stderr, "rawinput: RegisterRawInputDevices failed (%lu)\n",
                GetLastError());
        return;
    }

    SDL_SetWindowsMessageHook(rawinput_message_hook, NULL);
    g_initialized = true;
}

void xemu_rawinput_update_controller_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

    uint32_t mb = state->rawinput_buttons;
    if (mb & XEMU_RAWINPUT_BUTTON_LEFT) { // Trigger
        state->buttons |= CONTROLLER_BUTTON_A;
    }
    if (mb & XEMU_RAWINPUT_BUTTON_RIGHT) { // Grip / reload
        state->buttons |= CONTROLLER_BUTTON_B;
    }
    if (mb & XEMU_RAWINPUT_BUTTON_MIDDLE) {
        state->buttons |= CONTROLLER_BUTTON_START;
    }
    if (mb & XEMU_RAWINPUT_BUTTON_X1) {
        state->buttons |= CONTROLLER_BUTTON_BACK;
    }
    if (mb & XEMU_RAWINPUT_BUTTON_X2) {
        state->buttons |= CONTROLLER_BUTTON_X;
    }

    // Aim position in window client pixels
    int px, py;
    if (state->rawinput_has_abs) {
        px = state->rawinput_client_x;
        py = state->rawinput_client_y;
    } else {
        // Relative mice steer the system cursor; follow it
        POINT pt;
        if (!GetCursorPos(&pt) || g_hwnd == NULL) {
            return;
        }
        ScreenToClient(g_hwnd, &pt);
        px = pt.x;
        py = pt.y;
    }

    int rx, ry, rw, rh;
    xemu_input_get_game_display_rect(&rx, &ry, &rw, &rh);
    if (rw <= 0 || rh <= 0) {
        // Game not rendering yet; fall back to the whole client area
        RECT cr;
        if (g_hwnd == NULL || !GetClientRect(g_hwnd, &cr) ||
            cr.right <= 0 || cr.bottom <= 0) {
            return;
        }
        rx = ry = 0;
        rw = cr.right;
        rh = cr.bottom;
    }

    float nx = 2.0f * (px - rx) / rw - 1.0f; // [-1,1], left -> right
    float ny = 1.0f - 2.0f * (py - ry) / rh; // [-1,1], bottom -> top

    // Smooth the aim to tame hand/camera jitter, using a "1-euro filter":
    // an adaptive low-pass whose cutoff rises with speed. While nearly
    // still it filters hard (rock-solid crosshair); on fast moves it opens
    // up completely, so there is no perceivable lag.
    float smoothing = g_config.input.lightgun_smoothing;
    if (smoothing > 0.0f) {
        smoothing = MIN(smoothing, 1.0f);
        uint64_t now = SDL_GetTicksNS();
        if (!state->rawinput_smooth_valid) {
            state->rawinput_smooth_nx = nx;
            state->rawinput_smooth_ny = ny;
            state->rawinput_smooth_dx = 0.0f;
            state->rawinput_smooth_dy = 0.0f;
            state->rawinput_smooth_valid = true;
        } else {
            float dt = (now - state->rawinput_smooth_ts) / 1e9f;
            dt = MIN(MAX(dt, 1e-4f), 0.1f);

            // Filtered velocity (1 Hz cutoff): jitter averages out to ~0,
            // real hand movement produces a sustained value
            const float two_pi = 6.28318531f;
            float ad = 1.0f / (1.0f + 1.0f / (two_pi * 1.0f * dt));
            float raw_dx = (nx - state->rawinput_smooth_nx) / dt;
            float raw_dy = (ny - state->rawinput_smooth_ny) / dt;
            state->rawinput_smooth_dx +=
                ad * (raw_dx - state->rawinput_smooth_dx);
            state->rawinput_smooth_dy +=
                ad * (raw_dy - state->rawinput_smooth_dy);

            // Cutoff frequency: low while still (strong smoothing, more so
            // at higher smoothing settings), raised in proportion to speed
            // so fast moves pass through unfiltered
            float min_cutoff = 6.0f - 5.5f * smoothing; // 6 Hz .. 0.5 Hz
            const float beta = 5.0f;
            float cx = min_cutoff + beta * fabsf(state->rawinput_smooth_dx);
            float cy = min_cutoff + beta * fabsf(state->rawinput_smooth_dy);
            float ax = 1.0f / (1.0f + 1.0f / (two_pi * cx * dt));
            float ay = 1.0f / (1.0f + 1.0f / (two_pi * cy * dt));
            state->rawinput_smooth_nx +=
                ax * (nx - state->rawinput_smooth_nx);
            state->rawinput_smooth_ny +=
                ay * (ny - state->rawinput_smooth_ny);
        }
        state->rawinput_smooth_ts = now;
        nx = state->rawinput_smooth_nx;
        ny = state->rawinput_smooth_ny;
    } else {
        state->rawinput_smooth_valid = false;
    }

    // Sensitivity scales the aim range around the screen center
    float sensitivity = g_config.input.lightgun_sensitivity;
    if (sensitivity > 0.0f) {
        nx *= sensitivity;
        ny *= sensitivity;
    }

    if (nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f) {
        state->buttons |= CONTROLLER_BUTTON_LIGHTGUN_ONSCREEN;
    }

    nx = MIN(MAX(nx, -1.0f), 1.0f);
    ny = MIN(MAX(ny, -1.0f), 1.0f);
    state->axis[CONTROLLER_AXIS_LSTICK_X] = (int16_t)(nx * 32767.0f);
    state->axis[CONTROLLER_AXIS_LSTICK_Y] = (int16_t)(ny * 32767.0f);
}

#else // !_WIN32

void xemu_rawinput_init(SDL_Window *window)
{
    (void)window;
}

void xemu_rawinput_process_pending(void)
{
}

void xemu_rawinput_update_controller_state(ControllerState *state)
{
    (void)state;
}

#endif
