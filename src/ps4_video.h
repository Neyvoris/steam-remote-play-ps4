/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <libavutil/frame.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Ps4VideoDecoder Ps4VideoDecoder;

int ps4_video_open(Ps4VideoDecoder **decoder, int width, int height,
                   const uint8_t *parameters, size_t parameter_size);
void ps4_video_close(Ps4VideoDecoder **decoder);

/* 1: decoded NV12 frame, 0: decoder buffered the access unit, <0: error. */
int ps4_video_decode(Ps4VideoDecoder *decoder, const uint8_t *data, size_t size,
                     uint64_t sequence, bool keyframe, AVFrame *image);
