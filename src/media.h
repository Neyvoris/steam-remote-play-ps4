#pragma once
#include <stdbool.h>
#include <ihslib.h>
#include "screen.h"

enum { VIDEO_DECODER_THREADS = 3 };

bool media_attach(IHS_Session *session, bool hardware);
bool media_present(ScreenArea area, const ScreenCalibration *calibration, bool redraw, const char *diagnostics, const float *cursor);
void media_discard(void);

typedef struct {
    int codec, width, height, decoder_error, frame_format, display_error;
    int decoded_width, decoded_height, sample_aspect_num, sample_aspect_den;
    unsigned received, decoded, submitted;
    unsigned average_decode_us, average_render_us;
    unsigned host_fps_num, host_fps_den, host_fps_limits;
    bool hardware_active, hardware_fallback;
    int hardware_error;
} MediaVideoStatus;

void media_reset_video_status(void);
MediaVideoStatus media_video_status(void);
int media_audio_error(void);
