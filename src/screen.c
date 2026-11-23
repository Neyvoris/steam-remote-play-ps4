/* SPDX-License-Identifier: GPL-3.0-only */
#include "screen.h"
#include "startup.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "font8x8.h"
#include "tv_font.h"
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
    const uint32_t border = 0xfff7efdf, accent = 0xfff5b763;
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
    rect(fb, 196, 231, 888, 258, 0xff241911);
    rect(fb, 196, 231, 888, 4, accent);
    text(fb, 224, 257, 4, border, "Screen settings");
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
    rect(fb, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0xff241911);
    rect(fb, area.left, area.top, area.right - area.left, area.bottom - area.top, 0xff493020);
    screen_calibration_overlay(fb, calibration);
    return screen_present() >= 0;
}

/* VideoOut stores red in the low byte. Coordinates use the 720p UI canvas. */
static const uint32_t ink = 0xfffff6ec, muted = 0xffcdb49b;
static const uint32_t focus = 0xfff7cb68, panel = 0xff39291a, backdrop = 0xff23180e;

static uint32_t blend_text(uint32_t background, uint32_t foreground, unsigned alpha)
{
    uint32_t result = 0xff000000;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const unsigned bg = (background >> shift) & 255;
        const unsigned fg = (foreground >> shift) & 255;
        result |= ((bg * (255 - alpha) + fg * alpha + 127) / 255) << shift;
    }
    return result;
}

static void tv_text(uint32_t *fb, int x, int y, int size, int limit, uint32_t color, const char *label)
{
    int target_x = x * output_width / SCREEN_WIDTH;
    const int target_y = y * output_height / SCREEN_HEIGHT;
    const int scale = size * output_width / SCREEN_WIDTH;
    const int right = limit * output_width / SCREEN_WIDTH;
    for (const unsigned char *ch = (const unsigned char *)label; *ch; ++ch) {
        const unsigned glyph = (*ch >= 32 && *ch <= 126 ? *ch : '?') - 32;
        const int advance = (tv_advance[glyph] * scale + 16) / TV_FONT_SIZE;
        if (target_x + advance > right) break;
        if (*ch != ' ') {
            const uint8_t *coverage = tv_coverage + glyph * TV_GLYPH_WIDTH * TV_GLYPH_HEIGHT;
            const int width = TV_GLYPH_WIDTH * scale / TV_FONT_SIZE;
            const int height = TV_GLYPH_HEIGHT * scale / TV_FONT_SIZE;
            for (int row = 0; row < height && target_y + row < output_height; ++row) {
                const uint8_t *source = coverage + row * TV_FONT_SIZE / scale * TV_GLYPH_WIDTH;
                uint32_t *dest = fb + (target_y + row) * output_width + target_x;
                for (int col = 0; col < width && target_x + col < right; ++col) {
                    const unsigned alpha = source[col * TV_FONT_SIZE / scale];
                    if (alpha) dest[col] = blend_text(dest[col], color, alpha);
                }
            }
        }
        target_x += advance;
    }
}

static void tv_shell(uint32_t *fb, unsigned tab, const char *footer)
{
    rect(fb, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, backdrop);
    tv_text(fb, 54, 25, 26, 400, ink, "Steam Remote Play");
    const int x[] = {454, 580, 753};
    const char *names[] = {"Home", "Computers", "Settings"};
    for (unsigned i = 0; i < 3; ++i) {
        tv_text(fb, x[i], 29, 22, x[i] + 145, i == tab ? ink : muted, names[i]);
        if (i == tab) rect(fb, x[i], 64, i == 1 ? 110 : 75, 3, focus);
    }
    tv_text(fb, 1115, 29, 20, 1226, muted, "PS4 0.2.8");
    rect(fb, 54, 77, 1172, 1, 0xff65513f);
    rect(fb, 54, 651, 1172, 1, 0xff65513f);
    tv_text(fb, 54, 668, 19, 1226, muted, footer);
}

static void tv_button(uint32_t *fb, int x, int y, int w, const char *label, bool active)
{
    rect(fb, x, y, w, 60, active ? focus : panel);
    tv_text(fb, x + 20, y + 13, 27, x + w - 16, active ? backdrop : ink, label);
}

static void pc_art(uint32_t *fb)
{
    rect(fb, 780, 170, 362, 230, 0xff68482c);
    rect(fb, 787, 177, 348, 216, 0xff3e2b1b);
    rect(fb, 806, 196, 310, 170, 0xff56391f);
    rect(fb, 935, 400, 52, 54, 0xff68482c);
    rect(fb, 885, 452, 152, 10, 0xff68482c);
    rect(fb, 853, 313, 208, 83, 0xff9c6b3f);
    rect(fb, 860, 320, 194, 69, panel);
    rect(fb, 894, 338, 10, 34, focus);
    rect(fb, 882, 350, 34, 10, focus);
    rect(fb, 1001, 343, 10, 10, focus);
    rect(fb, 1018, 357, 10, 10, focus);
}

static void computer_rows(uint32_t *fb, const char hosts[][64], unsigned count,
                          unsigned selected, bool dim)
{
    tv_text(fb, 54, 108, 48, 1200, dim ? muted : ink, "Computers");
    if (!count) {
        tv_text(fb, 56, 200, 27, 1200, muted, "Looking for Steam PCs on your network...");
        return;
    }
    const unsigned first = selected >= 4 ? selected - 3 : 0;
    for (unsigned i = first; i < count && i < first + 4; ++i) {
        const int y = 190 + (int)(i - first) * 99;
        const bool active = i == selected && !dim;
        rect(fb, 54, y, 1172, 82, active ? focus : panel);
        if (active) rect(fb, 57, y + 3, 1166, 76, panel);
        tv_text(fb, 81, y + 10, 29, 1160, dim ? muted : ink, hosts[i]);
        tv_text(fb, 81, y + 48, 18, 1160, active ? focus : muted, "Available on your network");
    }
}

void screen_tv_menu(unsigned tab, const char hosts[][64], unsigned count, unsigned selected,
                    const char *detail, const char *profile, const char *pin)
{
    uint32_t *fb = screen_pixels();
    if (!fb) return;
    const char *host = count && selected < count ? hosts[selected] : "";
    if (pin) {
        tv_shell(fb, 1, "Circle  Cancel pairing");
        computer_rows(fb, hosts, count, selected, true);
        rect(fb, 302, 185, 676, 380, focus);
        rect(fb, 305, 188, 670, 374, panel);
        tv_text(fb, 374, 224, 42, 927, ink, "Pair with Steam");
        tv_text(fb, 374, 299, 102, 927, ink, pin);
        tv_text(fb, 374, 434, 26, 927, muted, "Enter this PIN on your PC.");
        tv_text(fb, 374, 485, 20, 927, muted, "Steam > Settings > Remote Play > Pair Steam Link");
    } else if (tab == 1) {
        tv_shell(fb, tab, "Left / Right  Change tab     Up / Down  Select     Cross  Connect     Square  Pair again     Circle  Back");
        computer_rows(fb, hosts, count, selected, false);
        tv_text(fb, 54, 604, 19, 1226, muted, detail);
    } else if (tab == 2) {
        tv_shell(fb, tab, "Left / Right  Change tab     Circle  Back");
        tv_text(fb, 54, 108, 48, 1226, ink, "Settings");
        tv_button(fb, 54, 200, 620, "R1   Video profile", false);
        tv_text(fb, 710, 213, 26, 1226, focus, profile);
        tv_button(fb, 54, 280, 620, "Options   Screen calibration", false);
        tv_button(fb, 54, 360, 620, "L1   Check GitHub releases", false);
        tv_button(fb, 54, 440, 620, "Square   Install available update", false);
        tv_text(fb, 54, 535, 22, 1226, muted, detail);
    } else {
        tv_shell(fb, tab, "Left / Right  Change tab     Square  Pair again     Circle  Exit");
        pc_art(fb);
        tv_text(fb, 54, 127, 59, 752, ink, "Play from");
        tv_text(fb, 54, 195, 59, 752, ink, "your PC");
        tv_text(fb, 56, 286, 28, 752, ink, *host ? host : "Looking for Steam PCs...");
        if (*host) tv_text(fb, 56, 327, 20, 752, focus, "Available on your network");
        tv_button(fb, 54, 381, 470, "Cross   Start streaming", *host != 0);
        tv_button(fb, 54, 454, 470, "Triangle   Computers", false);
        tv_text(fb, 56, 554, 20, 1226, muted, detail);
    }
    (void)screen_present();
}

void screen_menu(const char *status, const char *host, const char *detail)
{
    uint32_t *fb = screen_pixels();
    if (!fb) return;
    const bool cancel = strcmp(status, "Connecting") == 0 ||
                        strcmp(status, "Opening Big Picture") == 0;
    tv_shell(fb, 0, cancel ? "Circle  Cancel" : "");
    tv_text(fb, 54, 141, 45, 1226, ink, status);
    if (*host) tv_text(fb, 54, 224, 28, 1226, focus, host);
    tv_text(fb, 54, 314, 23, 1226, muted, detail);
    (void)screen_present();
}
