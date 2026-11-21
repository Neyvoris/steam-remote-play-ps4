/* SPDX-License-Identifier: GPL-3.0-only */
#include "screen.h"
#include "startup.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "font8x8.h"
#include <orbis/VideoOut.h>
#include <orbis/GnmDriver.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

static int handle = -1;
static void *memory;
static off_t offset;
static int reserved;
static unsigned draw_buffer_index;
static int64_t flip = 1;
enum { DISPLAY_BUFFERS = 3 };
static int64_t buffer_flips[DISPLAY_BUFFERS];
static int64_t displayed_flip;
static uint32_t *buffers[DISPLAY_BUFFERS];
static int output_width = SCREEN_WIDTH, output_height = SCREEN_HEIGHT;
static const size_t alignment = 0x200000;
static size_t buffer_size;
static const char area_path[] = "/data/steam-remote-ps4/screen-area.txt";
static const char area_temp[] = "/data/steam-remote-ps4/screen-area.tmp";

enum { SCREEN_LIMIT_X = SCREEN_WIDTH / 4, SCREEN_LIMIT_Y = SCREEN_HEIGHT / 4 };

static int clamp(int value, int min, int max)
{
    return value < min ? min : value > max ? max : value;
}

ScreenArea screen_area_default(void)
{
    return (ScreenArea){.right = SCREEN_WIDTH, .bottom = SCREEN_HEIGHT};
}

ScreenArea screen_area_load(void)
{
    FILE *file = fopen(area_path, "r");
    if (!file) return screen_area_default();
    ScreenArea saved;
    unsigned version, fill;
    int fields = fscanf(file, "%1u %5d %5d %5d %5d %1u", &version, &saved.left, &saved.top,
                        &saved.right, &saved.bottom, &fill);
    fclose(file);
    if (fields != 6 || (version < 1 || version > 4) || fill > 1 ||
        saved.left < -SCREEN_LIMIT_X || saved.top < -SCREEN_LIMIT_Y ||
        saved.right > SCREEN_WIDTH + SCREEN_LIMIT_X || saved.bottom > SCREEN_HEIGHT + SCREEN_LIMIT_Y ||
        saved.left > SCREEN_LIMIT_X || saved.top > SCREEN_LIMIT_Y ||
        saved.right < SCREEN_WIDTH - SCREEN_LIMIT_X || saved.bottom < SCREEN_HEIGHT - SCREEN_LIMIT_Y)
        return screen_area_default();
    /* Start older configurations in Fit to avoid cropping on upgrade. */
    screen_area_set_fill(&saved, version == 4 && fill != 0);
    return saved;
}

bool screen_area_save(ScreenArea area)
{
    FILE *file = fopen(area_temp, "w");
    if (!file) return false;
    bool ok = fprintf(file, "4 %d %d %d %d %u\n", area.left, area.top, area.right, area.bottom,
                      (unsigned)area.fill) > 0;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(area_temp, area_path) == 0) return true;
    remove(area_temp);
    return false;
}

void screen_area_set_fill(ScreenArea *area, bool fill)
{
    area->fill = fill;
    if (fill) return;
    if (area->left < 0) area->left = 0;
    if (area->top < 0) area->top = 0;
    if (area->right > SCREEN_WIDTH) area->right = SCREEN_WIDTH;
    if (area->bottom > SCREEN_HEIGHT) area->bottom = SCREEN_HEIGHT;
}

void screen_area_move(ScreenArea *area, bool bottom_right, int dx, int dy)
{
    const int overscan_x = area->fill ? SCREEN_LIMIT_X : 0;
    const int overscan_y = area->fill ? SCREEN_LIMIT_Y : 0;
    if (bottom_right) {
        area->right = clamp(area->right + dx, SCREEN_WIDTH - SCREEN_LIMIT_X, SCREEN_WIDTH + overscan_x);
        area->bottom = clamp(area->bottom + dy, SCREEN_HEIGHT - SCREEN_LIMIT_Y, SCREEN_HEIGHT + overscan_y);
    } else {
        area->left = clamp(area->left + dx, -overscan_x, SCREEN_LIMIT_X);
        area->top = clamp(area->top + dy, -overscan_y, SCREEN_LIMIT_Y);
    }
}

ScreenArea screen_output_area(ScreenArea area, int width, int height)
{
    if (!area.fill) screen_area_set_fill(&area, false);
    area.left = area.left * width / SCREEN_WIDTH;
    area.right = area.right * width / SCREEN_WIDTH;
    area.top = area.top * height / SCREEN_HEIGHT;
    area.bottom = area.bottom * height / SCREEN_HEIGHT;
    return area;
}

static int register_buffers(int width, int height)
{
    const size_t pixels = (size_t)width * height * sizeof(uint32_t);
    buffer_size = (pixels + alignment - 1) & ~(alignment - 1);
    startup_log_step("allocate display buffers");
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), DISPLAY_BUFFERS * buffer_size,
                                          alignment, 3, &offset);
    if (rc < 0) return rc;
    reserved = 1;
    startup_log_step("map display buffers");
    rc = sceKernelMapDirectMemory(&memory, DISPLAY_BUFFERS * buffer_size, 0x33, 0, offset, alignment);
    if (rc < 0) return rc;
    for (unsigned i = 0; i < DISPLAY_BUFFERS; ++i)
        buffers[i] = (uint32_t *)((char *)memory + i * buffer_size);
    OrbisVideoOutBufferAttribute format = {0};
    sceVideoOutSetBufferAttribute(&format, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
        ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
        width, height, width);
    startup_log_step("register display buffers");
    rc = sceVideoOutRegisterBuffers(handle, 0, (void *const *)buffers, DISPLAY_BUFFERS, &format);
    if (rc >= 0) {
        output_width = width;
        output_height = height;
        startup_log_result("Display buffer width", width);
        startup_log_result("Display buffer height", height);
    }
    return rc;
}

ScreenArea screen_video_area(ScreenArea area, int aspect_width, int aspect_height)
{
    const int available_width = area.right - area.left, available_height = area.bottom - area.top;
    const bool source_wider = (int64_t)aspect_width * available_height > (int64_t)aspect_height * available_width;
    if (source_wider == area.fill) {
        int scaled_width = (int)((int64_t)available_height * aspect_width / aspect_height);
        if (scaled_width < 1) scaled_width = 1;
        area.left += (available_width - scaled_width) / 2;
        area.right = area.left + scaled_width;
    } else {
        int scaled_height = (int)((int64_t)available_width * aspect_height / aspect_width);
        if (scaled_height < 1) scaled_height = 1;
        area.top += (available_height - scaled_height) / 2;
        area.bottom = area.top + scaled_height;
    }
    return area;
}

static int open_display(int height)
{
    startup_log_step("sceVideoOutOpen");
    handle = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
    if (handle < 0) return handle;
    OrbisVideoOutResolutionStatus resolution = {0};
    if (height == VIDEO_HEIGHT &&
        (sceVideoOutGetResolutionStatus(handle, &resolution) < 0 || !resolution.height ||
         resolution.height >= VIDEO_HEIGHT)) {
        int rc = register_buffers(VIDEO_WIDTH, VIDEO_HEIGHT);
        if (rc >= 0) return 0;
        startup_log_result("1080p VideoOut unavailable", rc);
        screen_shutdown();
        if (handle >= 0) return rc;
        handle = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
        if (handle < 0) return handle;
    }
    int rc = register_buffers(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (rc < 0) screen_shutdown();
    return rc < 0 ? rc : 0;
}

int screen_init(void)
{
    startup_log_result("load VideoOut module",
                       (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT));
    return open_display(VIDEO_HEIGHT);
}

int screen_set_stream_height(int height)
{
    if (height == output_height) return 0;
    int previous_height = output_height;
    screen_shutdown();
    if (handle >= 0) {
        startup_log_step("VideoOut kept the previous buffers");
        return 0;
    }
    int result = open_display(height);
    if (result < 0 && handle < 0) {
        startup_log_result("VideoOut mode change failed", result);
        result = open_display(previous_height);
    }
    return result;
}

void screen_shutdown(void)
{
    if (handle >= 0 && sceVideoOutClose(handle) < 0) return;
    handle = -1;
    if (memory) sceKernelMunmap(memory, DISPLAY_BUFFERS * buffer_size);
    if (reserved) sceKernelReleaseDirectMemory(offset, DISPLAY_BUFFERS * buffer_size);
    memory = NULL;
    reserved = 0;
    draw_buffer_index = 0;
    displayed_flip = 0;
    for (unsigned i = 0; i < DISPLAY_BUFFERS; ++i) buffer_flips[i] = 0;
}

void screen_output_size(int *width, int *height)
{
    *width = output_width;
    *height = output_height;
}

uint32_t *screen_pixels(void)
{
    if (handle < 0) return NULL;
    OrbisVideoOutFlipStatus status = {0};
    if (sceVideoOutGetFlipStatus(handle, &status) >= 0 &&
        status.flipArg > displayed_flip && status.flipArg < flip)
        displayed_flip = status.flipArg;
    for (unsigned i = 0; i < DISPLAY_BUFFERS; ++i) {
        /* Reuse only buffers that are neither scanned out nor queued. */
        if (buffer_flips[i] == 0 || buffer_flips[i] < displayed_flip) {
            draw_buffer_index = i;
            return buffers[i];
        }
    }
    return NULL;
}

int screen_present(void)
{
    if (handle < 0) return -1;
    int rc = sceVideoOutSubmitFlip(handle, (int)draw_buffer_index, ORBIS_VIDEO_OUT_FLIP_VSYNC, flip);
    if (rc < 0) return rc;
    buffer_flips[draw_buffer_index] = flip++;
    (void)sceGnmSubmitDone();
    return 0;
}

static void rect(uint32_t *fb, int x, int y, int w, int h, uint32_t color)
{
    const int right = clamp(x + w, 0, SCREEN_WIDTH) * output_width / SCREEN_WIDTH;
    const int bottom = clamp(y + h, 0, SCREEN_HEIGHT) * output_height / SCREEN_HEIGHT;
    const int left = clamp(x, 0, SCREEN_WIDTH) * output_width / SCREEN_WIDTH;
    const int top = clamp(y, 0, SCREEN_HEIGHT) * output_height / SCREEN_HEIGHT;
    for (int row = top; row < bottom; row++) {
        uint32_t *line = fb + row * output_width;
        for (int col = left; col < right; col++) line[col] = color;
    }
}

static void text(uint32_t *fb, int x, int y, int scale, uint32_t color, const char *label)
{
    for (const unsigned char *ch = (const unsigned char *)label;
         *ch && x + 8 * scale < SCREEN_WIDTH - 66; ch++, x += 8 * scale) {
        unsigned glyph = *ch >= 32 && *ch < 127 ? *ch : '?';
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (font8x8_basic[glyph][row] & (1u << col))
                    rect(fb, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

void screen_diagnostics_overlay(uint32_t *fb, const char *label)
{
    rect(fb, 40, 32, 1190, 36, 0xff101010);
    text(fb, 52, 42, 2, 0xffffffff, label);
}

void screen_calibration_overlay(uint32_t *fb, const ScreenCalibration *calibration)
{
    const ScreenArea area = calibration->area;
    const int width = area.right - area.left, height = area.bottom - area.top;
    const uint32_t border = 0xfffff5e8, accent = 0xffffc266;
    rect(fb, area.left, area.top, width, 4, border);
    rect(fb, area.left, area.bottom - 4, width, 4, border);
    rect(fb, area.left, area.top, 4, height, border);
    rect(fb, area.right - 4, area.top, 4, height, border);
    if (calibration->bottom_right) {
        rect(fb, area.right - 38, area.bottom - 8, 38, 8, accent);
        rect(fb, area.right - 8, area.bottom - 38, 8, 38, accent);
    } else {
        rect(fb, area.left, area.top, 38, 8, accent);
        rect(fb, area.left, area.top, 8, 38, accent);
    }
    rect(fb, 196, 231, 888, 258, 0xff231911);
    text(fb, 224, 257, 4, border, "Screen size");
    char detail[56];
    snprintf(detail, sizeof(detail), "%s: %d, %d    Mode: %s",
             calibration->bottom_right ? "Bottom-right" : "Top-left",
             calibration->bottom_right ? area.right : area.left,
             calibration->bottom_right ? area.bottom : area.top,
             area.fill ? "Fill" : "Fit");
    text(fb, 224, 313, 2, accent, detail);
    text(fb, 224, 355, 2, border, "L1 Top-left   R1 Bottom-right   D-pad Move");
    text(fb, 224, 395, 2, border, "TRIANGLE Fit/Fill   SQUARE Reset");
    text(fb, 224, 435, 2, border, calibration->save_failed ?
         "Save failed. CROSS Retry   CIRCLE Cancel" : "CROSS Save   CIRCLE Cancel");
}

bool screen_calibration_preview(const ScreenCalibration *calibration)
{
    uint32_t *fb = screen_pixels();
    if (!fb) return false;
    const ScreenArea area = calibration->area;
    rect(fb, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0xff231911);
    rect(fb, area.left, area.top, area.right - area.left, area.bottom - area.top, 0xff422d1c);
    screen_calibration_overlay(fb, calibration);
    return screen_present() >= 0;
}

static void menu_detail(uint32_t *fb, int x, int y, const char *message)
{
    enum { LINE_CHARS = 59 };
    for (int line = 0; line < 3 && *message; ++line) {
        char buffer[LINE_CHARS + 1];
        size_t visible = strlen(message);
        if (visible > LINE_CHARS) {
            visible = LINE_CHARS;
            while (visible > 0 && message[visible] != ' ') --visible;
            if (!visible) visible = LINE_CHARS;
        }
        memcpy(buffer, message, visible);
        buffer[visible] = 0;
        text(fb, x, y + line * 27, 2, 0xffdcc8ad, buffer);
        message += visible;
        while (*message == ' ') ++message;
    }
}

void screen_host_menu(const char *status, const char *host, const char *detail, const char *profile)
{
    uint32_t *fb = screen_pixels();
    if (!fb) return;

    const uint32_t white = 0xfffff5e8, blue = 0xffffc266, muted = 0xffdcc8ad;
    const bool selecting = profile && strcmp(status, "Choose a Steam PC") == 0;
    rect(fb, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0xff231911);
    rect(fb, 0, 0, SCREEN_WIDTH, 144, 0xff352518);
    rect(fb, 0, 140, SCREEN_WIDTH, 4, 0xff4d3524);
    rect(fb, 64, 55, 8, 46, blue);
    text(fb, 92, 56, 4, white, "Steam Remote Play");
    text(fb, 1024, 70, 2, muted, "PS4  0.2.6");

    rect(fb, 64, 182, 1152, 367, 0xff3d2b1d);
    rect(fb, 64, 182, 1152, 3, blue);
    text(fb, 96, 215, 3, blue, status);

    if (*host) {
        rect(fb, 96, 271, 1088, 76, 0xff563e29);
        rect(fb, 96, 271, 5, 76, blue);
        char name[44];
        snprintf(name, sizeof(name), "%.43s", host);
        text(fb, 121, 295, 2, white, name);
    }
    menu_detail(fb, 96, *host ? 377 : 283, detail);

    if (profile) {
        rect(fb, 96, 476, 1088, 48, 0xff302217);
        text(fb, 115, 492, 2, blue, "R1");
        text(fb, 176, 492, 2, white, profile);
    }

    rect(fb, 0, 581, SCREEN_WIDTH, 139, 0xff1b130d);
    if (selecting) {
        text(fb, 64, 611, 2, white, "D-PAD Select    CROSS Connect    SQUARE Pair again");
        text(fb, 64, 647, 2, muted, "L1 Check updates    OPTIONS Screen size    CIRCLE Exit");
    } else if (profile) {
        text(fb, 64, 611, 2, muted, "L1 Check updates    OPTIONS Screen size    CIRCLE Exit");
    } else if (strcmp(status, "Connecting") == 0 || strcmp(status, "Opening Big Picture") == 0 ||
               strncmp(status, "PIN:", 4) == 0) {
        text(fb, 64, 611, 2, muted, "CIRCLE Cancel");
    }
    (void)screen_present();
}

void screen_menu(const char *status, const char *host, const char *detail)
{
    screen_host_menu(status, host, detail, NULL);
}
