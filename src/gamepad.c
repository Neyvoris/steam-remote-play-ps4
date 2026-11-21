/* SPDX-License-Identifier: GPL-3.0-only
 * Report layout follows the LGPL-3.0-or-later SDL provider in IHSlib,
 * copyright 2022 Mariotaku. */
#include "gamepad.h"
#include "hid/device.h"
#include "ihs_buffer.h"
#include "startup.h"
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int handle = -1;
static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool local_input;
static char device_path[] = "sdl://0";

int gamepad_init(void)
{
    startup_log_result("load UserService module",
                       (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE));
    startup_log_result("load Pad module",
                       (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD));
    startup_log_step("scePadInit");
    int result = scePadInit();
    if (result < 0) return result;
    int user = -1;
    OrbisUserServiceInitializeParams params = { .priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST };
    startup_log_step("sceUserServiceInitialize");
    startup_log_result("sceUserServiceInitialize", sceUserServiceInitialize(&params));
    startup_log_step("sceUserServiceGetInitialUser");
    result = sceUserServiceGetInitialUser(&user);
    if (result < 0) return result;
    startup_log_step("scePadOpen");
    handle = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    return handle;
}

void gamepad_close(void)
{
    if (handle >= 0) scePadClose(handle);
    handle = -1;
}

static OrbisPadData read_pad(void)
{
    OrbisPadData state = {0};
    if (handle < 0) return state;
    pthread_mutex_lock(&pad_lock);
    int result = scePadReadState(handle, &state);
    pthread_mutex_unlock(&pad_lock);
    return result >= 0 && state.connected ? state : (OrbisPadData){0};
}

uint32_t gamepad_buttons(void)
{
    return read_pad().buttons;
}

void gamepad_set_local(bool local)
{
    atomic_store(&local_input, local);
}

/* Generic Gamepad RAW V2, as advertised by IHSlib. */
typedef struct __attribute__((packed)) {
    int16_t axes[6];
    uint32_t flags;
    uint32_t buttons;
    uint8_t reserved[7];
    uint8_t version;
    uint8_t unused[44];
} PadReport;
_Static_assert(sizeof(PadReport) == 72, "Steam HID report size");
_Static_assert(offsetof(PadReport, buttons) == 16, "Steam HID button offset");
_Static_assert(offsetof(PadReport, version) == 27, "Steam HID version offset");

typedef struct {
    IHS_HIDDevice base;
    PadReport previous;
    size_t report_length;
} PadDevice;

static int16_t center_axis(uint8_t value)
{
    int offset = (int)value - 128;
    if (offset >= -2 && offset <= 2) return 0;
    return (int16_t)(offset * (offset < 0 ? 256 : 258));
}

static PadReport current_report(void)
{
    const OrbisPadData state = read_pad();
    PadReport report = {.version = 3};
    if (!state.connected || atomic_load(&local_input)) return report;
    /* Steam uses SDL's button positions in this report. */
    static const uint32_t map[] = {
        ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CIRCLE, ORBIS_PAD_BUTTON_SQUARE,
        ORBIS_PAD_BUTTON_TRIANGLE, ORBIS_PAD_BUTTON_TOUCH_PAD, 0,
        ORBIS_PAD_BUTTON_OPTIONS, ORBIS_PAD_BUTTON_L3, ORBIS_PAD_BUTTON_R3,
        ORBIS_PAD_BUTTON_L1, ORBIS_PAD_BUTTON_R1, ORBIS_PAD_BUTTON_UP,
        ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_LEFT, ORBIS_PAD_BUTTON_RIGHT
    };
    uint32_t buttons = state.buttons;
    if ((buttons & GAMEPAD_EXIT) == GAMEPAD_EXIT) buttons &= ~GAMEPAD_EXIT;
    if ((buttons & GAMEPAD_SCREEN) == GAMEPAD_SCREEN) buttons &= ~GAMEPAD_SCREEN;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (buttons & map[i]) report.buttons |= 1u << i;
    }
    report.axes[0] = center_axis(state.leftStick.x);
    report.axes[1] = center_axis(state.leftStick.y);
    report.axes[2] = center_axis(state.rightStick.x);
    report.axes[3] = center_axis(state.rightStick.y);
    report.axes[4] = (int16_t)(state.analogButtons.l2 * 128u);
    report.axes[5] = (int16_t)(state.analogButtons.r2 * 128u);
    return report;
}

/* IHSlib holds the managed-device lock in all three report callbacks. */
static int full_report(IHS_HIDDevice *device)
{
    PadDevice *pad = (PadDevice *)device;
    if (!pad->report_length) return -1;
    pad->previous = current_report();
    IHS_HIDDeviceReportReplaceWithFullForced(device, (const uint8_t *)&pad->previous, pad->report_length);
    return 0;
}

static int begin_reports(IHS_HIDDevice *device, size_t length)
{
    PadDevice *pad = (PadDevice *)device;
    const size_t minimum = offsetof(PadReport, version) + 1;
    pad->report_length = length < sizeof(PadReport) ? length : sizeof(PadReport);
    if (length < minimum) pad->report_length = 0;
    startup_log_result("Steam HID requested report bytes", (int)length);
    startup_log_result("Steam HID RAW V2 report bytes", (int)pad->report_length);
    return full_report(device);
}

static int poll_pad(IHS_HIDDevice *device)
{
    PadDevice *pad = (PadDevice *)device;
    if (!pad->report_length) return 0;
    PadReport report = current_report();
    if (memcmp(&pad->previous, &report, sizeof(report)) == 0) return 0;
    IHS_HIDDeviceReportAddDelta(device, (const uint8_t *)&pad->previous,
                               (const uint8_t *)&report, pad->report_length);
    pad->previous = report;
    return 1;
}

static IHS_HIDDevice *alloc_device(const IHS_HIDDeviceClass *cls)
{
    PadDevice *device = calloc(1, sizeof(*device));
    if (!device) return NULL;
    device->base.cls = cls;
    return &device->base;
}
static void free_device(IHS_HIDDevice *device) { free(device); }
static void close_device(IHS_HIDDevice *device) { (void)device; }

static int feature(IHS_HIDDevice *device, const uint8_t *request, size_t size,
                   IHS_Buffer *out, size_t limit)
{
    (void)device;
    if (!size || limit < 2) return -1;
    uint8_t response[21] = {0};
    response[0] = request[0];
    size_t length = 2;
    if (response[0] == 4) {
        response[1] = 1;  /* valid */
        response[3] = 34; /* Steam controller type PS4 */
        response[11] = 1; /* HID device */
        length = sizeof(response);
    } else if (response[0] == 2) {
        response[1] = 0xff;
    }
    if (length > limit) return -1;
    IHS_BufferWriteMem(out, 0, response, length);
    return 0;
}

static int no_feature(IHS_HIDDevice *device, const uint8_t *data, size_t length)
{
    (void)device;
    (void)data;
    (void)length;
    return -1;
}

static int write_pad(IHS_HIDDevice *device, const uint8_t *data, size_t length)
{
    (void)device;
    (void)data;
    return (int)length;
}

static int read_report(IHS_HIDDevice *device, IHS_Buffer *out, size_t length, uint32_t timeout)
{
    (void)device;
    (void)timeout;
    PadReport report = current_report();
    if (length > sizeof(report)) length = sizeof(report);
    IHS_BufferWriteMem(out, 0, (const uint8_t *)&report, length);
    return (int)length;
}

static int empty_string(IHS_HIDDevice *device, IHS_Buffer *out)
{
    (void)device;
    const uint8_t empty = 0;
    IHS_BufferWriteMem(out, 0, &empty, 1);
    return 0;
}

static int disconnect_device(IHS_HIDDevice *device, int method, const uint8_t *data, size_t length)
{
    (void)device;
    (void)method;
    (void)data;
    (void)length;
    return 0;
}

static IHS_HIDDevice *open_device(IHS_HIDProvider *provider, const char *path)
{
    (void)provider;
    (void)path;
    static const IHS_HIDDeviceClass cls = {
        .alloc = alloc_device, .free = free_device, .close = close_device, .write = write_pad,
        .read = read_report, .sendFeatureReport = no_feature, .getFeatureReport = feature,
        .getVendorString = empty_string, .getProductString = empty_string,
        .getSerialNumberString = empty_string, .startInputReports = begin_reports,
        .requestFullReport = full_report, .requestDisconnect = disconnect_device, .poll = poll_pad
    };
    return alloc_device(&cls);
}

static bool supported(IHS_HIDProvider *provider, const char *path)
{
    (void)provider;
    return strcmp(path, device_path) == 0;
}
static bool has_change(IHS_HIDProvider *provider) { (void)provider; return false; }
static IHS_Enumeration *enumerate(IHS_HIDProvider *provider)
{
    (void)provider;
    return IHS_EnumerationArrayCreate(device_path, sizeof(device_path), 1, NULL);
}
static void device_info(IHS_HIDProvider *provider, IHS_Enumeration *enumeration, IHS_HIDDeviceInfo *info)
{
    (void)provider;
    (void)enumeration;
    *info = (IHS_HIDDeviceInfo){
        .path = device_path, .product_string = "DualShock 4", .serial_number = "",
        .vendor_id = 0x054c, .product_id = 0x09cc, .product_version = 1
    };
}

void gamepad_attach(IHS_Session *session)
{
    static const IHS_HIDProviderClass cls = {
        .supportsDevice = supported, .openDevice = open_device,
        .hasChange = has_change, .enumerateDevices = enumerate, .deviceInfo = device_info
    };
    static IHS_HIDProvider provider = { .cls = &cls };
    IHS_SessionHIDAddProvider(session, &provider);
}
