#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { SCREEN_WIDTH = 1280, SCREEN_HEIGHT = 720, VIDEO_WIDTH = 1920, VIDEO_HEIGHT = 1080 };
typedef struct {
    int left, top, right, bottom;
    bool fill;
} ScreenArea;

typedef struct {
    ScreenArea area;
    bool bottom_right;
    bool save_failed;
} ScreenCalibration;

int screen_init(void);
int screen_set_stream_height(int height);
void screen_shutdown(void);
uint32_t *screen_pixels(void);
int screen_present(void);
void screen_output_size(int *width, int *height);
void screen_menu(const char *status, const char *host, const char *detail);
void screen_host_menu(const char *status, const char *host, const char *detail, const char *profile);
ScreenArea screen_area_default(void);
ScreenArea screen_area_load(void);
bool screen_area_save(ScreenArea area);
void screen_area_set_fill(ScreenArea *area, bool fill);
void screen_area_move(ScreenArea *area, bool bottom_right, int dx, int dy);
ScreenArea screen_output_area(ScreenArea area, int width, int height);
ScreenArea screen_video_area(ScreenArea area, int aspect_width, int aspect_height);
void screen_calibration_overlay(uint32_t *pixels, const ScreenCalibration *calibration);
bool screen_calibration_preview(const ScreenCalibration *calibration);
