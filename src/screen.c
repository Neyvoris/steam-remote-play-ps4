/* SPDX-License-Identifier: GPL-3.0-only */
#include "screen.h"
#include "startup.h"
#include <stddef.h>
#include <stdio.h>
#include "font8x8.h"
#include <orbis/VideoOut.h>
#include <orbis/GnmDriver.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

static int handle = -1;
static void *memory;
static off_t offset;
static int reserved;
static unsigned index;
static int64_t flip = 1;
static int64_t pending;
static uint32_t *buffers[2];
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
    if (fields != 6 || (version != 1 && version != 2) || fill > 1 ||
        saved.left < -SCREEN_LIMIT_X || saved.top < -SCREEN_LIMIT_Y ||
        saved.right > SCREEN_WIDTH + SCREEN_LIMIT_X || saved.bottom > SCREEN_HEIGHT + SCREEN_LIMIT_Y ||
        saved.left > SCREEN_LIMIT_X || saved.top > SCREEN_LIMIT_Y ||
        saved.right < SCREEN_WIDTH - SCREEN_LIMIT_X || saved.bottom < SCREEN_HEIGHT - SCREEN_LIMIT_Y)
        return screen_area_default();
    /* Version 1 used Fill by default, even for a calibrated screen. */
    screen_area_set_fill(&saved, version == 2 && fill != 0);
    return saved;
}

bool screen_area_save(ScreenArea area)
{
    FILE *file = fopen(area_temp, "w");
    if (!file) return false;
    bool ok = fprintf(file, "2 %d %d %d %d %u\n", area.left, area.top, area.right, area.bottom,
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
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), 2 * buffer_size,
                                          alignment, 3, &offset);
    if (rc < 0) return rc;
    reserved = 1;
    startup_log_step("map display buffers");
    rc = sceKernelMapDirectMemory(&memory, 2 * buffer_size, 0x33, 0, offset, alignment);
    if (rc < 0) return rc;
    buffers[0] = memory;
    buffers[1] = (uint32_t *)((char *)memory + buffer_size);
    OrbisVideoOutBufferAttribute format = {0};
    sceVideoOutSetBufferAttribute(&format, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
        ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
        width, height, width);
    startup_log_step("register display buffers");
    rc = sceVideoOutRegisterBuffers(handle, 0, (void *const *)buffers, 2, &format);
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
    if (memory) sceKernelMunmap(memory, 2 * buffer_size);
    if (reserved) sceKernelReleaseDirectMemory(offset, 2 * buffer_size);
    memory = NULL;
    reserved = 0;
    index = 0;
    pending = 0;
}

void screen_output_size(int *width, int *height)
{
    *width = output_width;
    *height = output_height;
}

uint32_t *screen_pixels(void)
{
    if (handle < 0) return NULL;
    if (pending) {
        OrbisVideoOutFlipStatus status = {0};
        if (sceVideoOutGetFlipStatus(handle, &status) < 0 || status.flipArg != pending) return NULL;
        pending = 0;
        index ^= 1;
    }
    return buffers[index];
}

int screen_present(void)
{
    if (handle < 0 || pending) return -1;
    int rc = sceVideoOutSubmitFlip(handle, (int)index, ORBIS_VIDEO_OUT_FLIP_VSYNC, flip);
    if (rc < 0) return rc;
    pending = flip++;
    return sceGnmSubmitDone();
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
    const uint32_t border = 0xfff9f5ff, accent = 0xfffea8d3;
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
    rect(fb, 196, 231, 888, 258, 0xff181319);
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
    rect(fb, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0xff181319);
    rect(fb, area.left, area.top, area.right - area.left, area.bottom - area.top, 0xff302534);
    screen_calibration_overlay(fb, calibration);
    return screen_present() >= 0;
}

void screen_host_menu(const char *status, const char *host, const char *detail, const char *profile)
{
    uint32_t *fb = screen_pixels();
    if (!fb) return;
    const uint32_t white = 0xfff9f5ff, lavender = 0xfffea8d3, dim = 0xffc2afaa;
    rect(fb, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0xff181319);
    rect(fb, 72, 76, 7, 52, 0xffe180b6);
    text(fb, 104, 80, 5, white, "Steam Remote Play");
    text(fb, 1020, 100, 2, dim, "0.2.1");
    rect(fb, 72, 183, 1136, 365, 0xff302534);
    text(fb, 106, 217, 4, lavender, status);
    text(fb, 106, 303, 3, white, host);
    text(fb, 106, 392, 2, dim, detail);
    text(fb, 72, 598, 2, dim, "UP/DOWN  PC     CROSS  Connect     CIRCLE  Back / Exit");
    if (profile) {
        char hint[56];
        snprintf(hint, sizeof(hint), "R1  Video mode: %s", profile);
        text(fb, 72, 636, 2, white, hint);
    }
    (void)screen_present();
}

void screen_menu(const char *status, const char *host, const char *detail)
{
    screen_host_menu(status, host, detail, NULL);
}
