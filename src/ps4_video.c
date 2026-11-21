/* SPDX-License-Identifier: GPL-3.0-only */
#include "ps4_video.h"
#include "screen.h"
#include "startup.h"
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <orbis/GnmDriver.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
#include <smmintrin.h>
#include <stdlib.h>
#include <string.h>

/* libSceVideodec2 firmware ABI. OpenOrbis declares only untyped stubs.
 * Layout and setup references are listed in README.md. */
typedef struct {
    uint64_t size;
    uint16_t pipe, queue;
    bool check_memory_type;
} ComputeConfig;

typedef struct {
    uint64_t size, memory_size;
    void *memory;
} ComputeMemory;

typedef struct {
    uint64_t size;
    uint32_t resource_type, codec, profile, level;
    int32_t width, height, reference_frames;
    uint32_t depth;
    void *queue;
    uint64_t affinity;
    int32_t priority;
    bool progressive, check_memory_type;
    void *extra;
} DecoderConfig;

typedef struct {
    uint64_t size, cpu_size;
    void *cpu;
    uint64_t gpu_size;
    void *gpu;
    uint64_t shared_size;
    void *shared;
    uint64_t surface_size;
    uint32_t surface_alignment;
} DecoderMemory;

typedef struct {
    uint64_t size;
    void *bytes;
    uint64_t length, pts, dts, tag;
} DecoderInput;

typedef struct {
    uint64_t size;
    void *data;
    uint64_t capacity;
    bool accepted;
} DecoderSurface;

typedef struct {
    uint64_t size;
    bool valid, damaged;
    uint8_t picture_count;
    uint32_t codec, width, pitch, height;
    void *data;
    uint64_t capacity;
    uint32_t format, pitch_bytes;
} DecoderOutput;

typedef struct {
    uint64_t size;
    bool valid;
    uint64_t pts, dts, tag;
    uint8_t idr, profile, level;
    uint32_t width_in_mbs_minus_one, height_in_mbs_minus_one;
    bool progressive, cropped;
    uint32_t crop_left, crop_right, crop_top, crop_bottom;
    uint8_t has_aspect, aspect;
    uint16_t sar_width, sar_height;
    uint8_t has_signal, video_format, full_range, has_color, primaries, transfer, matrix;
    uint8_t reserved[35];
} AvcPicture;

_Static_assert(sizeof(ComputeConfig) == 16 && sizeof(ComputeMemory) == 24, "Videodec2 queue ABI");
_Static_assert(sizeof(DecoderConfig) == 72 && offsetof(DecoderConfig, extra) == 64, "Videodec2 config ABI");
_Static_assert(sizeof(DecoderMemory) == 72, "Videodec2 memory ABI");
_Static_assert(sizeof(DecoderInput) == 48 && sizeof(DecoderSurface) == 32, "Videodec2 input ABI");
_Static_assert(sizeof(DecoderOutput) == 56 && offsetof(DecoderOutput, data) == 32, "Videodec2 output ABI");
_Static_assert(sizeof(AvcPicture) == 120 && offsetof(AvcPicture, tag) == 32 &&
               offsetof(AvcPicture, sar_width) == 74, "Videodec2 AVC picture ABI");

typedef struct {
    int (*compute_memory)(ComputeMemory *);
    int (*queue_create)(const ComputeConfig *, const ComputeMemory *, void **);
    int (*queue_destroy)(void *);
    int (*decoder_memory)(const DecoderConfig *, DecoderMemory *);
    int (*decoder_create)(const DecoderConfig *, const DecoderMemory *, void **);
    int (*decoder_destroy)(void *);
    int (*decode)(void *, const DecoderInput *, DecoderSurface *, DecoderOutput *);
    int (*picture)(const DecoderOutput *, AvcPicture *, void *);
} DecoderApi;

typedef struct {
    uint8_t *mapping;
    off_t offset;
    size_t size;
} DirectMemory;

enum {
    PAGE_BYTES = 65536, PIPELINE_DEPTH = 2, ACCESS_UNIT_SLOTS = PIPELINE_DEPTH + 1,
    SURFACE_SLOTS = 8, MAX_AU_BYTES = 2 * 1024 * 1024, MAX_PARAMETERS = 64 * 1024
};
static DecoderApi api;
static bool loaded, native_failed_to_close;

struct Ps4VideoDecoder {
    void *queue, *decoder;
    DirectMemory compute, cpu, gpu, shared, surfaces, inputs;
    size_t surface_stride, input_stride;
    unsigned surface_index, input_index;
    int width, height, row_stride;
    unsigned nal_length;
    bool length_prefixed;
    uint8_t parameters[MAX_PARAMETERS];
    size_t parameter_size;
    AVBufferPool *staging;
};

static int load_api(void)
{
    if (loaded) return 0;
    static const char *const dependencies[] = {
        "/system/common/lib/libSceVdecCore.sprx", "/system/common/lib/libSceVdecSavc.sprx",
        "/system/common/lib/libSceVdecSavc2.sprx", "/system/common/lib/libSceVdecwrap.sprx"
    };
    for (size_t i = 0; i < sizeof(dependencies) / sizeof(*dependencies); ++i)
        sceKernelLoadStartModule(dependencies[i], 0, NULL, 0, NULL, NULL);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VDECCORE);
    int module = (int)sceKernelLoadStartModule("/system/common/lib/libSceVideodec2.sprx", 0, NULL, 0, NULL, NULL);
    if (module < 0)
        module = (int)sceKernelLoadStartModule("libSceVideodec2.sprx", 0, NULL, 0, NULL, NULL);
    if (module < 0) return module;
    int error = 0;
#define RESOLVE(field, suffix) do { \
    void *symbol = NULL; \
    error = sceKernelDlsym(module, "sceVideodec2" #suffix, &symbol); \
    if (error || !symbol) goto failed; \
    memcpy(&api.field, &symbol, sizeof(symbol)); \
} while (0)
    RESOLVE(compute_memory, QueryComputeMemoryInfo);
    RESOLVE(queue_create, AllocateComputeQueue);
    RESOLVE(queue_destroy, ReleaseComputeQueue);
    RESOLVE(decoder_memory, QueryDecoderMemoryInfo);
    RESOLVE(decoder_create, CreateDecoder);
    RESOLVE(decoder_destroy, DeleteDecoder);
    RESOLVE(decode, Decode);
#undef RESOLVE
    void *picture = NULL;
    if (sceKernelDlsym(module, "sceVideodec2GetPictureInfo", &picture) == 0 && picture)
        memcpy(&api.picture, &picture, sizeof(picture));
    loaded = true;
    return 0;
failed:
    sceKernelStopUnloadModule(module, 0, NULL, 0, NULL, NULL);
    return error < 0 ? error : AVERROR(ENOSYS);
}

static int allocate_memory(DirectMemory *block, size_t bytes, int type)
{
    if (!bytes) return 0;
    if (bytes > 256u * 1024 * 1024) return AVERROR(EOVERFLOW);
    size_t rounded = (bytes + PAGE_BYTES - 1) & ~(size_t)(PAGE_BYTES - 1);
    int error = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), rounded,
                                              PAGE_BYTES, type, &block->offset);
    if (error < 0) return error;
    void *mapping = NULL;
    error = sceKernelMapDirectMemory(&mapping, rounded, ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_ALL,
                                     0, block->offset, PAGE_BYTES);
    if (error < 0) {
        sceKernelReleaseDirectMemory(block->offset, rounded);
        return error;
    }
    block->mapping = mapping;
    block->size = rounded;
    return 0;
}

static void release_memory(DirectMemory *block)
{
    if (!block->mapping) return;
    sceKernelMunmap(block->mapping, block->size);
    sceKernelReleaseDirectMemory(block->offset, block->size);
}

void ps4_video_close(Ps4VideoDecoder **owner)
{
    Ps4VideoDecoder *state = *owner;
    *owner = NULL;
    if (!state) return;
    av_buffer_pool_uninit(&state->staging);
    if (state->decoder && api.decoder_destroy(state->decoder) < 0) {
        native_failed_to_close = true;
        startup_log_step("Videodec2 could not release decoder");
        free(state);
        return; /* The firmware may still reference its direct memory. */
    }
    release_memory(&state->inputs);
    release_memory(&state->surfaces);
    release_memory(&state->shared);
    release_memory(&state->gpu);
    release_memory(&state->cpu);
    if (!state->queue || api.queue_destroy(state->queue) >= 0) {
        release_memory(&state->compute);
    } else {
        native_failed_to_close = true;
        startup_log_step("Videodec2 could not release compute queue");
    }
    free(state);
}

static const uint8_t start_code[4] = {0, 0, 0, 1};

static bool annex_b(const uint8_t *data, size_t remaining)
{
    if (remaining < 4 || data[0] || data[1]) return false;
    size_t i = 2;
    while (i < remaining && data[i] == 0) ++i;
    return i + 1 < remaining && data[i] == 1;
}

static int read_parameters(Ps4VideoDecoder *state, const uint8_t *data, size_t size)
{
    state->nal_length = 4;
    if (size == 0) return 0;
    if (!data || size > MAX_PARAMETERS) return AVERROR_INVALIDDATA;
    if (annex_b(data, size)) {
        memcpy(state->parameters, data, size);
        state->parameter_size = size;
        return 0;
    }
    if (size < 7 || data[0] != 1) return AVERROR_INVALIDDATA;
    state->nal_length = (data[4] & 3) + 1;
    state->length_prefixed = true;
    size_t cursor = 6;
    unsigned count = data[5] & 31;
    for (unsigned group = 0; group < 2; ++group) {
        if (group == 1) {
            if (cursor == size) return AVERROR_INVALIDDATA;
            count = data[cursor++];
        }
        for (unsigned i = 0; i < count; ++i) {
            if (size - cursor < 2) return AVERROR_INVALIDDATA;
            unsigned length = (unsigned)data[cursor] << 8 | data[cursor + 1];
            cursor += 2;
            if (!length || length > size - cursor ||
                length + sizeof(start_code) > MAX_PARAMETERS - state->parameter_size) return AVERROR_INVALIDDATA;
            memcpy(state->parameters + state->parameter_size, start_code, sizeof(start_code));
            state->parameter_size += sizeof(start_code);
            memcpy(state->parameters + state->parameter_size, data + cursor, length);
            state->parameter_size += length;
            cursor += length;
        }
    }
    return state->parameter_size ? 0 : AVERROR_INVALIDDATA;
}

static int create_compute_queue(Ps4VideoDecoder *state)
{
    ComputeMemory memory = {.size = sizeof(memory)};
    int error = api.compute_memory(&memory);
    if (error < 0) return error;
    if (!memory.memory_size) return AVERROR_INVALIDDATA;
    error = allocate_memory(&state->compute, memory.memory_size, ORBIS_KERNEL_WB_ONION);
    if (error < 0) return error;
    memory.memory = state->compute.mapping;
    const uint16_t slots[][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {3, 0}, {4, 0}};
    for (size_t i = 0; i < sizeof(slots) / sizeof(*slots); ++i) {
        const ComputeConfig config = {
            .size = sizeof(config), .pipe = slots[i][0], .queue = slots[i][1], .check_memory_type = true,
        };
        void *queue = NULL;
        error = api.queue_create(&config, &memory, &queue);
        if (!error && queue) {
            state->queue = queue;
            return 0;
        }
    }
    return error < 0 ? error : AVERROR(EIO);
}

int ps4_video_open(Ps4VideoDecoder **output, int width, int height,
                   const uint8_t *parameters, size_t parameter_size)
{
    if (native_failed_to_close || width <= 0 || height <= 0 || width > VIDEO_WIDTH || height > VIDEO_HEIGHT)
        return AVERROR(ENOSYS);
    Ps4VideoDecoder *state = calloc(1, sizeof(*state));
    if (!state) return AVERROR(ENOMEM);
    state->width = width;
    state->height = height;
    int error = read_parameters(state, parameters, parameter_size);
    if (error < 0) goto failed;
    error = load_api();
    if (error < 0) goto failed;
    error = create_compute_queue(state);
    if (error < 0) goto failed;
    const DecoderConfig config = {
        .size = sizeof(config), .resource_type = 1, .codec = 1, .profile = 100, .level = 51,
        .width = VIDEO_WIDTH, .height = (VIDEO_HEIGHT + 15) & ~15, .reference_frames = 4,
        .depth = PIPELINE_DEPTH, .queue = state->queue, .affinity = 0x3f, .priority = 700,
        .progressive = true
    };
    DecoderMemory memory = {.size = sizeof(memory)};
    error = api.decoder_memory(&config, &memory);
    if (error < 0) goto failed;
    size_t alignment = memory.surface_alignment ? memory.surface_alignment : 256;
    if (alignment > PAGE_BYTES || (alignment & (alignment - 1)) ||
        !memory.surface_size || memory.surface_size > 16u * 1024 * 1024) {
        error = AVERROR_INVALIDDATA;
        goto failed;
    }
    state->surface_stride = (memory.surface_size + alignment - 1) & ~(alignment - 1);
    state->input_stride = MAX_AU_BYTES + MAX_PARAMETERS + PAGE_BYTES;
    error = allocate_memory(&state->cpu, memory.cpu_size, ORBIS_KERNEL_WB_ONION);
    if (error < 0) goto failed;
    error = allocate_memory(&state->gpu, memory.gpu_size, ORBIS_KERNEL_WC_GARLIC);
    if (error < 0) goto failed;
    error = allocate_memory(&state->shared, memory.shared_size, ORBIS_KERNEL_WB_ONION);
    if (error < 0) goto failed;
    error = allocate_memory(&state->surfaces, state->surface_stride * SURFACE_SLOTS, ORBIS_KERNEL_WC_GARLIC);
    if (error < 0) goto failed;
    error = allocate_memory(&state->inputs, state->input_stride * ACCESS_UNIT_SLOTS, ORBIS_KERNEL_WB_ONION);
    if (error < 0) goto failed;
    memory.cpu = state->cpu.mapping;
    memory.gpu = state->gpu.mapping;
    memory.shared = state->shared.mapping;
    error = api.decoder_create(&config, &memory, &state->decoder);
    if (error < 0) goto failed;
    int frame_size = av_image_get_buffer_size(AV_PIX_FMT_NV12, VIDEO_WIDTH, VIDEO_HEIGHT, 64);
    if (!state->decoder || frame_size <= 0) {
        error = AVERROR(EIO);
        goto failed;
    }
    state->staging = av_buffer_pool_init(frame_size, NULL);
    if (!state->staging) {
        error = AVERROR(ENOMEM);
        goto failed;
    }
    state->row_stride = (VIDEO_WIDTH + 63) & ~63;
    *output = state;
    return 0;
failed:
    ps4_video_close(&state);
    return error;
}

static int convert_length_prefixed(const Ps4VideoDecoder *state, uint8_t *dest, size_t length,
                                   const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        uint32_t nal_size = 0;
        if (size - offset < state->nal_length) return AVERROR_INVALIDDATA;
        for (unsigned i = 0; i < state->nal_length; ++i)
            nal_size = nal_size << 8 | data[offset++];
        if (!nal_size || nal_size > size - offset ||
            sizeof(start_code) + nal_size > state->input_stride - length) return AVERROR_INVALIDDATA;
        memcpy(dest + length, start_code, sizeof(start_code));
        length += sizeof(start_code);
        memcpy(dest + length, data + offset, nal_size);
        length += nal_size;
        offset += nal_size;
    }
    return (int)length;
}

static int make_access_unit(const Ps4VideoDecoder *state, uint8_t *dest, const uint8_t *data,
                            size_t size, bool keyframe)
{
    size_t length = keyframe ? state->parameter_size : 0;
    if (length) memcpy(dest, state->parameters, length);
    const bool has_start_code = annex_b(data, size);
    if (state->length_prefixed || !has_start_code) {
        int result = convert_length_prefixed(state, dest, length, data, size);
        if (result >= 0 || !has_start_code) return result;
    }
    memcpy(dest + length, data, size);
    return (int)(length + size);
}

/* The native NV12 surfaces reside in write-combining GPU memory. Move each
 * row to a reusable CPU-cached frame before handing it to the presenter. */
__attribute__((target("sse4.1")))
static void copy_gpu_plane(uint8_t *dest, int stride, const uint8_t *src, int pitch, int width, int rows)
{
    const bool aligned = ((uintptr_t)src & 15) == 0 && (pitch & 15) == 0;
    for (int y = 0; y < rows; ++y) {
        int x = 0;
        if (aligned) {
            for (; x + 16 <= width; x += 16) {
                __m128i bytes = _mm_stream_load_si128((__m128i *)(uintptr_t)(src + x));
                _mm_storeu_si128((__m128i *)(dest + x), bytes);
            }
        }
        memcpy(dest + x, src + x, (size_t)(width - x));
        dest += stride;
        src += pitch;
    }
}

int ps4_video_decode(Ps4VideoDecoder *state, const uint8_t *data, size_t size,
                     uint64_t sequence, bool keyframe, AVFrame *image)
{
    if (!data || !size || size > MAX_AU_BYTES) return AVERROR_INVALIDDATA;
    uint8_t *bytes = state->inputs.mapping + state->input_index * state->input_stride;
    int length = make_access_unit(state, bytes, data, size, keyframe);
    if (length < 0) return length;
    state->input_index = (state->input_index + 1) % ACCESS_UNIT_SLOTS;
    DecoderInput input = {.size = sizeof(input), .bytes = bytes, .length = (uint64_t)length,
                          .pts = sequence, .dts = sequence, .tag = sequence};
    DecoderSurface surface = {.size = sizeof(surface), .capacity = state->surface_stride,
                              .data = state->surfaces.mapping + state->surface_index * state->surface_stride};
    state->surface_index = (state->surface_index + 1) % SURFACE_SLOTS;
    DecoderOutput output = {.size = sizeof(output)};
    int error = api.decode(state->decoder, &input, &surface, &output);
    if (error) return error < 0 ? error : AVERROR(EIO);
    if (output.damaged) return AVERROR_INVALIDDATA;
    if (!output.valid) return 0;
    const unsigned pitch = output.pitch_bytes ? output.pitch_bytes : output.pitch;
    const uintptr_t start = (uintptr_t)state->surfaces.mapping, address = (uintptr_t)output.data;
    if (!output.data || output.format != 0 || !output.width || !output.height ||
        output.width > ((VIDEO_WIDTH + 255) & ~255) || output.height > ((VIDEO_HEIGHT + 15) & ~15) ||
        pitch < output.width || pitch > 4096 || address < start || (address - start) % state->surface_stride ||
        (address - start) / state->surface_stride >= SURFACE_SLOTS ||
        output.capacity > state->surface_stride) return AVERROR_INVALIDDATA;

    sceGnmFlushGarlic();
    AvcPicture picture = {.size = sizeof(picture)};
    if (api.picture && output.picture_count == 1 && output.capacity && state->surface_stride >= sizeof(picture) &&
        output.capacity <= state->surface_stride - sizeof(picture)) {
        if (api.picture(&output, &picture, NULL) < 0) picture.valid = false;
    }
    unsigned width = output.width, height = output.height, left = 0, top = 0;
    if (output.width == (unsigned)((state->width + 15) & ~15) ||
        output.width == (unsigned)((state->width + 255) & ~255)) width = state->width;
    if (output.height == (unsigned)((state->height + 15) & ~15)) height = state->height;
    if (picture.valid && picture.progressive && picture.cropped &&
        picture.width_in_mbs_minus_one < 128 && picture.height_in_mbs_minus_one < 68) {
        unsigned coded_width = (picture.width_in_mbs_minus_one + 1) * 16;
        unsigned coded_height = (picture.height_in_mbs_minus_one + 1) * 16;
        uint64_t crop_x = 2 * ((uint64_t)picture.crop_left + picture.crop_right);
        uint64_t crop_y = 2 * ((uint64_t)picture.crop_top + picture.crop_bottom);
        if (crop_x >= coded_width || crop_y >= coded_height) return AVERROR_INVALIDDATA;
        left = 2 * picture.crop_left;
        top = 2 * picture.crop_top;
        width = coded_width - (unsigned)crop_x;
        height = coded_height - (unsigned)crop_y;
    }
    const unsigned chroma_width = (width + 1) & ~1;
    const size_t y_bytes = (size_t)pitch * output.height;
    const size_t required = y_bytes + (size_t)pitch * ((top + height + 1) / 2);
    if (width > VIDEO_WIDTH || height > VIDEO_HEIGHT || left + chroma_width > pitch ||
        top + height > output.height || required > state->surface_stride ||
        (output.capacity && required > output.capacity)) return AVERROR_INVALIDDATA;

    AVBufferRef *frame = av_buffer_pool_get(state->staging);
    if (!frame) return AVERROR(ENOMEM);
    av_frame_unref(image);
    image->width = (int)width;
    image->height = (int)height;
    image->format = AV_PIX_FMT_NV12;
    image->pts = picture.valid && picture.tag ? (int64_t)picture.tag : AV_NOPTS_VALUE;
    image->color_range = picture.valid && picture.has_signal && picture.full_range ?
                         AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    static const AVRational ratios[] = {
        {0, 1}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11}, {20, 11},
        {32, 11}, {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99}, {4, 3}, {3, 2}, {2, 1},
    };
    if (picture.valid && picture.has_aspect) {
        if (picture.aspect < sizeof(ratios) / sizeof(*ratios))
            image->sample_aspect_ratio = ratios[picture.aspect];
        else if (picture.aspect == 255 && picture.sar_width && picture.sar_height)
            image->sample_aspect_ratio = (AVRational){picture.sar_width, picture.sar_height};
    }
    image->buf[0] = frame;
    image->data[0] = frame->data;
    image->data[1] = frame->data + (size_t)state->row_stride * height;
    image->linesize[0] = image->linesize[1] = state->row_stride;
    const uint8_t *luma = (const uint8_t *)output.data + (size_t)top * pitch + left;
    const uint8_t *chroma = (const uint8_t *)output.data + y_bytes + (size_t)(top / 2) * pitch + left;
    copy_gpu_plane(image->data[0], state->row_stride, luma, pitch, (int)width, (int)height);
    copy_gpu_plane(image->data[1], state->row_stride, chroma, pitch, (int)chroma_width, (height + 1) / 2);
    return 1;
}
