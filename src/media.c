/* SPDX-License-Identifier: GPL-3.0-only */
#include "media.h"
#include "ps4_video.h"
#include "startup.h"
#include <emmintrin.h>
#include <ihslib/frame_ticket.h>
#include <libavcodec/avcodec.h>
#include <opus.h>
#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static AVCodecContext *decoder;
static AVFrame *frame;
static AVFrame *latest_frame, *display_frame;
static AVPacket *packet;
static pthread_mutex_t video_lock = PTHREAD_MUTEX_INITIALIZER;
static bool video_ready;
static bool frame_pending;
static atomic_int stream_codec, stream_width, stream_height, decoder_error, frame_format, display_error;
static atomic_int audio_error;
static atomic_uint received_frames, decoded_frames, submitted_frames;
static atomic_uint_fast64_t decode_us;
static atomic_uint_fast64_t host_framerate;
static atomic_uint host_framerate_limits;
static uint64_t render_us;
static unsigned render_attempts;
static uint64_t session_identity, presentation_serial, previous_present_us;
static bool prefer_hardware;
static atomic_bool hardware_active, hardware_fallback;
static atomic_int hardware_error;
static Ps4VideoDecoder *native_decoder;
/* Codec data is owned by IHSlib until the matching video stop callback. */
static IHS_StreamVideoConfig current_video;
enum { MAX_NATIVE_PENDING = 24 };
static struct {
    uint64_t sequence;
    AVBufferRef *reference;
} native_pending[MAX_NATIVE_PENDING];
static unsigned native_pending_count;
static uint64_t native_sequence;

void media_reset_video_status(void)
{
    atomic_store(&stream_codec, 0);
    atomic_store(&stream_width, 0);
    atomic_store(&stream_height, 0);
    atomic_store(&decoder_error, 0);
    atomic_store(&frame_format, -1);
    atomic_store(&display_error, 0);
    atomic_store(&audio_error, 0);
    atomic_store(&received_frames, 0);
    atomic_store(&decoded_frames, 0);
    atomic_store(&submitted_frames, 0);
    atomic_store(&decode_us, 0);
    atomic_store(&host_framerate, 0);
    atomic_store(&host_framerate_limits, 0);
    atomic_store(&hardware_active, false);
    atomic_store(&hardware_fallback, false);
    atomic_store(&hardware_error, 0);
    render_us = 0;
    render_attempts = 0;
    presentation_serial = 0;
    previous_present_us = 0;
}

int media_audio_error(void)
{
    return atomic_load(&audio_error);
}

MediaVideoStatus media_video_status(void)
{
    uint64_t rate = atomic_load(&host_framerate);
    MediaVideoStatus status = {
        .codec = atomic_load(&stream_codec),
        .width = atomic_load(&stream_width),
        .height = atomic_load(&stream_height),
        .decoder_error = atomic_load(&decoder_error),
        .frame_format = atomic_load(&frame_format),
        .display_error = atomic_load(&display_error),
        .received = atomic_load(&received_frames),
        .decoded = atomic_load(&decoded_frames),
        .submitted = atomic_load(&submitted_frames),
        .host_fps_num = (uint32_t)(rate >> 32),
        .host_fps_den = (uint32_t)rate,
        .host_fps_limits = atomic_load(&host_framerate_limits),
        .hardware_active = atomic_load(&hardware_active),
        .hardware_fallback = atomic_load(&hardware_fallback),
        .hardware_error = atomic_load(&hardware_error)
    };
    if (display_frame && display_frame->data[0]) {
        status.decoded_width = display_frame->width;
        status.decoded_height = display_frame->height;
        status.sample_aspect_num = display_frame->sample_aspect_ratio.num;
        status.sample_aspect_den = display_frame->sample_aspect_ratio.den;
    }
    if (status.received) status.average_decode_us = (unsigned)(atomic_load(&decode_us) / status.received);
    if (render_attempts) status.average_render_us = (unsigned)(render_us / render_attempts);
    return status;
}

static void video_stop(IHS_Session *session, void *context)
{
    (void)session;
    (void)context;
    ps4_video_close(&native_decoder);
    for (unsigned i = 0; i < native_pending_count; ++i)
        av_buffer_unref(&native_pending[i].reference);
    native_pending_count = 0;
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
}

static uint64_t frame_clock_us(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static IHS_FrameTicket *frame_ticket(const AVFrame *image)
{
    return image && image->opaque_ref ? (IHS_FrameTicket *)image->opaque_ref->data : NULL;
}

static void drop_ticket(IHS_FrameTicket *ticket, IHS_VideoFrameResult result)
{
    if (!ticket) return;
    const IHS_FrameOutcome outcome = {.result = result, .completionUs = frame_clock_us()};
    IHS_FrameTicketComplete(ticket, &outcome);
}

static void release_ticket(void *context, uint8_t *data)
{
    (void)context;
    IHS_FrameTicket *ticket = (IHS_FrameTicket *)data;
    drop_ticket(ticket, IHS_VideoFrameResultDroppedReset);
    IHS_FrameTicketRelease(ticket);
}

static AVBufferRef *retain_ticket(IHS_FrameTicket *ticket)
{
    if (!ticket || !IHS_FrameTicketRetain(ticket)) return NULL;
    AVBufferRef *reference = av_buffer_create((uint8_t *)ticket, 1, release_ticket, NULL,
                                              AV_BUFFER_FLAG_READONLY);
    if (!reference) IHS_FrameTicketRelease(ticket);
    return reference;
}

static int open_software_decoder(const IHS_StreamVideoConfig *config)
{
    int result = AVERROR(ENOSYS);
    if (config->codec != IHS_StreamVideoCodecH264) goto failed;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) goto failed;
    result = AVERROR(ENOMEM);
    decoder = avcodec_alloc_context3(codec);
    packet = av_packet_alloc();
    if (!decoder || !packet) goto failed;
    decoder->width = (int)config->width;
    decoder->height = (int)config->height;
    decoder->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_COPY_OPAQUE;
    decoder->thread_count = VIDEO_DECODER_THREADS;
    decoder->max_pixels = VIDEO_WIDTH * ((VIDEO_HEIGHT + 15) & ~15);
    if (config->codecData && config->codecDataLen && config->codecDataLen < 65536) {
        decoder->extradata = av_mallocz(config->codecDataLen + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!decoder->extradata) goto failed;
        memcpy(decoder->extradata, config->codecData, config->codecDataLen);
        decoder->extradata_size = (int)config->codecDataLen;
    }
    result = avcodec_open2(decoder, codec, NULL);
    if (result == 0) return 0;
failed:
    atomic_store(&decoder_error, result);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    return result;
}

static void report_hardware_failure(int error)
{
    atomic_store(&hardware_active, false);
    atomic_store(&hardware_fallback, true);
    atomic_store(&hardware_error, error);
    startup_log_result("Videodec2 fallback to FFmpeg", error);
}

static int video_start(IHS_Session *session, const IHS_StreamVideoConfig *config, void *context)
{
    video_stop(session, context);
    current_video = *config;
    atomic_store(&stream_codec, (int)config->codec);
    atomic_store(&stream_width, (int)config->width);
    atomic_store(&stream_height, (int)config->height);
    atomic_store(&hardware_active, false);
    frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    if (prefer_hardware && !atomic_load(&hardware_fallback) && config->codec == IHS_StreamVideoCodecH264) {
        int result = ps4_video_open(&native_decoder, (int)config->width, (int)config->height,
                                    config->codecData, config->codecDataLen);
        if (!result) {
            native_sequence = 0;
            atomic_store(&hardware_active, true);
            startup_log_step("Videodec2 initialized");
            return 0;
        }
        report_hardware_failure(result);
    }
    int result = open_software_decoder(config);
    if (result < 0) video_stop(session, context);
    return result;
}

static unsigned clip_byte(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : (unsigned)value;
}

static inline __m128i bytes_to_words(__m128i bytes)
{
    return _mm_unpacklo_epi8(bytes, _mm_setzero_si128());
}

static inline __m128i load_chroma4(const uint8_t *samples)
{
    uint32_t four;
    memcpy(&four, samples, sizeof(four));
    return _mm_cvtsi32_si128((int)four);
}

static inline __m128i color_channel(__m128i a, __m128i b, __m128i factors, __m128i bias)
{
    const __m128i low = _mm_madd_epi16(_mm_unpacklo_epi16(a, b), factors);
    const __m128i high = _mm_madd_epi16(_mm_unpackhi_epi16(a, b), factors);
    return _mm_packs_epi32(_mm_srai_epi32(_mm_add_epi32(low, bias), 8),
                           _mm_srai_epi32(_mm_add_epi32(high, bias), 8));
}

typedef struct { __m128i red, blue, green_uv, luma, bias; } ColorMatrix;

static inline void render_pixels8(uint32_t *output, __m128i y, __m128i u, __m128i v,
                                  const ColorMatrix *matrix)
{
    const __m128i zero = _mm_setzero_si128();
    y = bytes_to_words(y);
    u = _mm_sub_epi16(bytes_to_words(u), _mm_set1_epi16(128));
    v = _mm_sub_epi16(bytes_to_words(v), _mm_set1_epi16(128));
    const __m128i red = color_channel(y, v, matrix->red, matrix->bias);
    const __m128i blue = color_channel(y, u, matrix->blue, matrix->bias);
    const __m128i green_low = _mm_add_epi32(
        _mm_madd_epi16(_mm_unpacklo_epi16(y, zero), matrix->luma),
        _mm_madd_epi16(_mm_unpacklo_epi16(u, v), matrix->green_uv));
    const __m128i green_high = _mm_add_epi32(
        _mm_madd_epi16(_mm_unpackhi_epi16(y, zero), matrix->luma),
        _mm_madd_epi16(_mm_unpackhi_epi16(u, v), matrix->green_uv));
    const __m128i green = _mm_packs_epi32(
        _mm_srai_epi32(_mm_add_epi32(green_low, matrix->bias), 8),
        _mm_srai_epi32(_mm_add_epi32(green_high, matrix->bias), 8));
    const __m128i rg = _mm_unpacklo_epi8(_mm_packus_epi16(red, zero), _mm_packus_epi16(green, zero));
    const __m128i ba = _mm_unpacklo_epi8(_mm_packus_epi16(blue, zero), _mm_set1_epi8(-1));
    _mm_storeu_si128((__m128i *)output, _mm_unpacklo_epi16(rg, ba));
    _mm_storeu_si128((__m128i *)(output + 4), _mm_unpackhi_epi16(rg, ba));
}

static void save_image(AVFrame *source)
{
    if (source->width <= 0 || source->height <= 0 || source->width > 4096 || source->height > 4096 ||
        (source->format != AV_PIX_FMT_NV12 && source->format != AV_PIX_FMT_YUV420P &&
         source->format != AV_PIX_FMT_YUVJ420P)) {
        drop_ticket(frame_ticket(source), IHS_VideoFrameResultDroppedDecodeCorrupt);
        return;
    }
    pthread_mutex_lock(&video_lock);
    if (video_ready) drop_ticket(frame_ticket(latest_frame), IHS_VideoFrameResultDroppedLate);
    av_frame_unref(latest_frame);
    av_frame_move_ref(latest_frame, source);
    video_ready = true;
    pthread_mutex_unlock(&video_lock);
}

static void render_image(uint32_t *pixels, const AVFrame *source, ScreenArea area)
{
    const int width = source->width, height = source->height;
    int output_width, output_height;
    screen_output_size(&output_width, &output_height);
    area = screen_output_area(area, output_width, output_height);
    const bool semiplanar = source->format == AV_PIX_FMT_NV12;
    const AVRational sample_aspect = source->sample_aspect_ratio;
    int aspect_width = width, aspect_height = height;
    if (sample_aspect.num > 0 && sample_aspect.den > 0 && sample_aspect.num != sample_aspect.den) {
        int num, den;
        av_reduce(&num, &den, (int64_t)width * sample_aspect.num,
                  (int64_t)height * sample_aspect.den, 16384);
        if (num > 0 && den > 0) {
            aspect_width = num;
            aspect_height = den;
        }
    }
    const ScreenArea scaled = screen_video_area(area, aspect_width, aspect_height);
    const int left = scaled.left > area.left ? scaled.left : area.left;
    const int top = scaled.top > area.top ? scaled.top : area.top;
    const int right = scaled.right < area.right ? scaled.right : area.right;
    const int bottom = scaled.bottom < area.bottom ? scaled.bottom : area.bottom;
    const int x_start = left > 0 ? left : 0, y_start = top > 0 ? top : 0;
    const int x_end = right < output_width ? right : output_width;
    const int y_end = bottom < output_height ? bottom : output_height;
    const int scaled_width = scaled.right - scaled.left, scaled_height = scaled.bottom - scaled.top;
    memset(pixels, 0, (size_t)y_start * output_width * sizeof(*pixels));
    memset(pixels + y_end * output_width, 0, (size_t)(output_height - y_end) * output_width * sizeof(*pixels));
    const bool full_range = source->color_range == AVCOL_RANGE_JPEG;
    const int luma_scale = full_range ? 256 : 298;
    const int luma_bias = full_range ? 0 : 298 * 16;
    const int chroma_step = semiplanar ? 2 : 1;
    const ColorMatrix matrix = {
        .red = _mm_setr_epi16(luma_scale, 459, luma_scale, 459, luma_scale, 459, luma_scale, 459),
        .blue = _mm_setr_epi16(luma_scale, 541, luma_scale, 541, luma_scale, 541, luma_scale, 541),
        .green_uv = _mm_setr_epi16(-55, -136, -55, -136, -55, -136, -55, -136),
        .luma = _mm_setr_epi16(luma_scale, 0, luma_scale, 0, luma_scale, 0, luma_scale, 0),
        .bias = _mm_set1_epi32(128 - luma_bias)
    };
    int horizontal[VIDEO_WIDTH];
    uint8_t scaled_luma[VIDEO_WIDTH], scaled_cb[VIDEO_WIDTH], scaled_cr[VIDEO_WIDTH];
    const bool contiguous = width == scaled_width && (x_start - scaled.left) % 2 == 0;
    for (int x = x_start; x < x_end; ++x)
        horizontal[x] = (int)((int64_t)(x - scaled.left) * width / scaled_width);
    int previous_sy = -2;
    for (int y = y_start; y < y_end; ++y) {
        const int sy = (int)((int64_t)(y - scaled.top) * height / scaled_height);
        uint32_t *dest = pixels + y * output_width;
        if (sy == previous_sy) {
            memcpy(dest, dest - output_width, (size_t)output_width * sizeof(*dest));
            continue;
        }
        const uint8_t *luma = source->data[0] + sy * source->linesize[0];
        const uint8_t *cb = source->data[1] + (sy / 2) * source->linesize[1];
        const uint8_t *cr = semiplanar ? cb + 1 : source->data[2] + (sy / 2) * source->linesize[2];
        if (x_start) memset(dest, 0, (size_t)x_start * sizeof(*pixels));
        if (x_end < output_width) memset(dest + x_end, 0, (size_t)(output_width - x_end) * sizeof(*pixels));
        int x = x_start;
        if (contiguous) {
            for (; x + 8 <= x_end; x += 8) {
                const int sx = x - scaled.left;
                __m128i u, v;
                if (semiplanar) {
                    const __m128i pairs = _mm_loadl_epi64((const __m128i *)(cb + sx));
                    u = _mm_packus_epi16(_mm_and_si128(pairs, _mm_set1_epi16(255)), _mm_setzero_si128());
                    v = _mm_packus_epi16(_mm_srli_epi16(pairs, 8), _mm_setzero_si128());
                } else {
                    u = load_chroma4(cb + sx / 2);
                    v = load_chroma4(cr + sx / 2);
                }
                render_pixels8(dest + x, _mm_loadl_epi64((const __m128i *)(luma + sx)),
                               _mm_unpacklo_epi8(u, u), _mm_unpacklo_epi8(v, v), &matrix);
            }
        } else {
            for (int i = x_start; i < x_end; ++i) scaled_luma[i] = luma[horizontal[i]];
            if (sy / 2 != previous_sy / 2) {
                for (int i = x_start; i < x_end; ++i) {
                    const int chroma = horizontal[i] / 2 * chroma_step;
                    scaled_cb[i] = cb[chroma];
                    scaled_cr[i] = cr[chroma];
                }
            }
            for (; x + 8 <= x_end; x += 8) {
                render_pixels8(dest + x, _mm_loadl_epi64((const __m128i *)(scaled_luma + x)),
                               _mm_loadl_epi64((const __m128i *)(scaled_cb + x)),
                               _mm_loadl_epi64((const __m128i *)(scaled_cr + x)), &matrix);
            }
        }
        previous_sy = sy;
        for (; x < x_end; ++x) {
            int sx = horizontal[x];
            int chroma = (sx / 2) * chroma_step;
            int u = cb[chroma] - 128, v = cr[chroma] - 128;
            int yy = luma_scale * luma[sx] - luma_bias;
            unsigned red = clip_byte((yy + 459 * v + 128) >> 8);
            unsigned green = clip_byte((yy - 55 * u - 136 * v + 128) >> 8);
            unsigned blue = clip_byte((yy + 541 * u + 128) >> 8);
            dest[x] = 0xff000000u | blue << 16 | green << 8 | red;
        }
    }
}

static void publish_decoded_frame(void)
{
    IHS_FrameTicket *ticket = frame_ticket(frame);
    if (ticket) IHS_FrameTicketDecodeStage(ticket, true, frame_clock_us());
    atomic_fetch_add(&decoded_frames, 1);
    atomic_store(&frame_format, frame->format);
    save_image(frame);
    av_frame_unref(frame);
}

static int receive_frames(void)
{
    int result;
    while ((result = avcodec_receive_frame(decoder, frame)) == 0) {
        publish_decoded_frame();
    }
    return result;
}

static IHS_StreamVideoSubmitResult decode_software(uint16_t frame_id, IHS_Buffer *buffer,
                                                  IHS_StreamVideoFrameFlag flags, IHS_FrameTicket *ticket)
{
    if (!decoder || buffer->size == 0 || buffer->size > 4 * 1024 * 1024)
        return IHS_StreamVideoSubmitReportLost;
    int result = av_new_packet(packet, (int)buffer->size);
    if (result < 0) goto lost;
    packet->opaque_ref = retain_ticket(ticket);
    if (packet->opaque_ref) IHS_FrameTicketDecodeStage(ticket, false, frame_clock_us());
    else drop_ticket(ticket, IHS_VideoFrameResultDroppedReset);
    packet->pts = frame_id;
    if (flags & IHS_StreamVideoFrameKeyFrame) packet->flags |= AV_PKT_FLAG_KEY;
    memcpy(packet->data, IHS_BufferPointer(buffer), buffer->size);
    result = avcodec_send_packet(decoder, packet);
    if (result == AVERROR(EAGAIN)) {
        result = receive_frames();
        if (result == AVERROR(EAGAIN)) result = avcodec_send_packet(decoder, packet);
    }
    if (result < 0) drop_ticket(ticket, IHS_VideoFrameResultDroppedDecodeCorrupt);
    av_packet_unref(packet);
    if (result < 0) goto lost;
    result = receive_frames();
    if (result == AVERROR(EAGAIN)) return IHS_StreamVideoSubmitOK;
lost:
    atomic_store(&decoder_error, result);
    return IHS_StreamVideoSubmitReportLost;
}

static IHS_StreamVideoSubmitResult decode_hardware(IHS_Buffer *buffer, IHS_StreamVideoFrameFlag flags,
                                                  IHS_FrameTicket *ticket)
{
    int result = AVERROR_INVALIDDATA;
    if (native_pending_count == MAX_NATIVE_PENDING) goto fallback;
    unsigned slot = native_pending_count++;
    native_pending[slot].sequence = ++native_sequence;
    native_pending[slot].reference = retain_ticket(ticket);
    if (native_pending[slot].reference) IHS_FrameTicketDecodeStage(ticket, false, frame_clock_us());
    else drop_ticket(ticket, IHS_VideoFrameResultDroppedReset);
    result = ps4_video_decode(native_decoder, IHS_BufferPointer(buffer), buffer->size, native_sequence,
                              (flags & IHS_StreamVideoFrameKeyFrame) != 0, frame);
    if (!result) return IHS_StreamVideoSubmitOK;
    if (result < 0) goto fallback;
    /* With picture metadata, use the original tag even across reordered frames.
     * Some firmware revisions only return the surface; use stream order then. */
    unsigned found = 0;
    if (frame->pts != AV_NOPTS_VALUE) {
        while (found < native_pending_count && native_pending[found].sequence != (uint64_t)frame->pts) ++found;
        if (found == native_pending_count) {
            result = AVERROR_INVALIDDATA;
            goto fallback;
        }
    }
    frame->opaque_ref = native_pending[found].reference;
    bool first = !atomic_load(&decoded_frames);
    --native_pending_count;
    memmove(&native_pending[found], &native_pending[found + 1],
            (native_pending_count - found) * sizeof(native_pending[0]));
    publish_decoded_frame();
    if (first) startup_log_step("Videodec2 produced its first NV12 frame");
    return IHS_StreamVideoSubmitOK;

fallback:
    report_hardware_failure(result);
    av_frame_unref(frame);
    ps4_video_close(&native_decoder);
    for (unsigned i = 0; i < native_pending_count; ++i)
        av_buffer_unref(&native_pending[i].reference);
    native_pending_count = 0;
    return open_software_decoder(&current_video) < 0 ? IHS_StreamVideoSubmitError :
           IHS_StreamVideoSubmitReportLost; /* Resume software decode from the next IDR. */
}

static IHS_StreamVideoSubmitResult video_submit(IHS_Session *session, const IHS_VideoEpochInfo *epoch,
                                               uint16_t frame_id, IHS_FrameTicket *ticket, IHS_Buffer *buffer,
                                               IHS_StreamVideoFrameFlag flags, bool *ticket_taken, void *context)
{
    (void)session;
    (void)epoch;
    (void)context;
    uint64_t start = sceKernelGetProcessTime();
    IHS_StreamVideoSubmitResult result = native_decoder ? decode_hardware(buffer, flags, ticket) :
                                         decode_software(frame_id, buffer, flags, ticket);
    if (result != IHS_StreamVideoSubmitOK) drop_ticket(ticket, IHS_VideoFrameResultDroppedDecodeCorrupt);
    *ticket_taken = true;
    atomic_fetch_add(&decode_us, sceKernelGetProcessTime() - start);
    atomic_fetch_add(&received_frames, 1);
    return result;
}

bool media_present(ScreenArea area, const ScreenCalibration *calibration, bool redraw)
{
    uint32_t *pixels = screen_pixels();
    if (!pixels) return false;
    if (pthread_mutex_trylock(&video_lock) != 0) return false;
    bool ready = video_ready;
    if (ready) {
        AVFrame *previous = display_frame;
        if (frame_pending) drop_ticket(frame_ticket(previous), IHS_VideoFrameResultDroppedLate);
        display_frame = latest_frame;
        latest_frame = previous;
        video_ready = false;
        frame_pending = true;
    }
    pthread_mutex_unlock(&video_lock);
    if ((!frame_pending && !redraw) || !display_frame || !display_frame->data[0]) return false;
    IHS_FrameTicket *ticket = frame_pending ? frame_ticket(display_frame) : NULL;
    const uint64_t upload_start = ticket ? frame_clock_us() : 0;
    uint64_t start = sceKernelGetProcessTime();
    render_image(pixels, display_frame, area);
    render_us += sceKernelGetProcessTime() - start;
    ++render_attempts;
    if (calibration) screen_calibration_overlay(pixels, calibration);
    const uint64_t upload_end = ticket ? frame_clock_us() : 0;
    int result = screen_present();
    if (result < 0) {
        atomic_store(&display_error, result);
        return false;
    }
    if (frame_pending) atomic_fetch_add(&submitted_frames, 1);
    if (ticket) {
        const uint64_t now = frame_clock_us();
        const IHS_FrameOutcome outcome = {
            .result = IHS_VideoFrameResultDisplayed, .completionUs = now,
            .uploadBeginUs = upload_start, .uploadEndUs = upload_end, .hasUpload = true,
            .presentationSerial = ++presentation_serial,
            .presentationIntervalUs = previous_present_us ? now - previous_present_us : 0,
            .hasPresentationInterval = previous_present_us != 0,
        };
        IHS_FrameTicketComplete(ticket, &outcome);
        previous_present_us = now;
    }
    frame_pending = false;
    return true;
}

void media_discard(void)
{
    /* Called before connection or after the session's data workers have joined. */
    video_ready = false;
    frame_pending = false;
    av_frame_free(&latest_frame);
    av_frame_free(&display_frame);
}

enum { AUDIO_BLOCK = 256, AUDIO_RING = 16384, OPUS_MAX_SAMPLES = 5760 };
static int16_t samples[AUDIO_RING * 2];
static unsigned read_pos, queued;
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t audio_thread;
static atomic_bool audio_running;
static int audio_port = -1;
static OpusDecoder *audio_decoder;

static void *audio_play(void *unused)
{
    (void)unused;
    while (atomic_load(&audio_running)) {
        int16_t buffer[AUDIO_BLOCK * 2];
        pthread_mutex_lock(&audio_lock);
        if (queued < AUDIO_BLOCK) {
            pthread_mutex_unlock(&audio_lock);
            sceKernelUsleep(2000);
            continue;
        }
        for (unsigned i = 0; i < AUDIO_BLOCK; ++i) {
            unsigned index = (read_pos + i) % AUDIO_RING;
            buffer[2 * i] = samples[2 * index];
            buffer[2 * i + 1] = samples[2 * index + 1];
        }
        read_pos = (read_pos + AUDIO_BLOCK) % AUDIO_RING;
        queued -= AUDIO_BLOCK;
        pthread_mutex_unlock(&audio_lock);
        int result = sceAudioOutOutput(audio_port, buffer);
        if (result < 0) {
            atomic_store(&audio_error, result);
            break;
        }
    }
    return NULL;
}

static void audio_stop(IHS_Session *session, void *context)
{
    (void)session;
    (void)context;
    if (atomic_exchange(&audio_running, false)) pthread_join(audio_thread, NULL);
    if (audio_port >= 0) sceAudioOutClose(audio_port);
    audio_port = -1;
    if (audio_decoder) opus_decoder_destroy(audio_decoder);
    audio_decoder = NULL;
}

static int audio_start(IHS_Session *session, const IHS_StreamAudioConfig *config, void *context)
{
    audio_stop(session, context);
    int error = -EINVAL;
    if (config->codec != IHS_StreamAudioCodecOpus || config->frequency != 48000 || config->channels != 2)
        goto failed;
    audio_decoder = opus_decoder_create(48000, 2, &error);
    if (!audio_decoder || error) {
        if (!error) error = -ENOMEM;
        goto failed;
    }

    int user = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
    (void)sceUserServiceGetInitialUser(&user);
    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
    (void)sceAudioOutInit();
    audio_port = sceAudioOutOpen(user, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, AUDIO_BLOCK, 48000,
                                 ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (audio_port < 0 && user != ORBIS_USER_SERVICE_USER_ID_SYSTEM) {
        audio_port = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
                                     0, AUDIO_BLOCK, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    }
    if (audio_port < 0) {
        error = audio_port;
        goto failed;
    }
    read_pos = queued = 0;
    atomic_store(&audio_error, 0);
    atomic_store(&audio_running, true);
    int thread_error = pthread_create(&audio_thread, NULL, audio_play, NULL);
    if (thread_error == 0) return 0;
    error = -thread_error;
    atomic_store(&audio_running, false);
failed:
    audio_stop(session, context);
    atomic_store(&audio_error, error);
    // IHSlib tears down the entire session on a failed audio start. Keep
    // receiving video and discard audio until a supported output can open.
    return 0;
}

static int audio_submit(IHS_Session *session, IHS_Buffer *buffer, void *context)
{
    (void)session;
    (void)context;
    if (!audio_decoder) return 0;
    if (!buffer->size || buffer->size > 16384) return -1;
    int16_t pcm[OPUS_MAX_SAMPLES * 2];
    int frames = opus_decode(audio_decoder, IHS_BufferPointer(buffer), (opus_int32)buffer->size,
                             pcm, OPUS_MAX_SAMPLES, 0);
    if (frames <= 0) return -1;
    pthread_mutex_lock(&audio_lock);
    if (queued + (unsigned)frames > 4800) {
        read_pos = (read_pos + queued) % AUDIO_RING;
        queued = 0;
    }
    for (int i = 0; i < frames; i++) {
        unsigned index = (read_pos + queued + (unsigned)i) % AUDIO_RING;
        samples[2 * index] = pcm[2 * i];
        samples[2 * index + 1] = pcm[2 * i + 1];
    }
    queued += (unsigned)frames;
    pthread_mutex_unlock(&audio_lock);
    return 0;
}

static void video_target_framerate(IHS_Session *session, uint32_t numerator, uint32_t denominator,
                                   uint32_t reasons, void *context)
{
    (void)session;
    (void)context;
    atomic_store(&host_framerate_limits, reasons);
    atomic_store(&host_framerate, ((uint64_t)numerator << 32) | denominator);
}

static int video_start_tracked(IHS_Session *session, const IHS_VideoEpochInfo *epoch,
                               const IHS_StreamVideoConfig *config, void *context)
{
    (void)epoch;
    return video_start(session, config, context);
}

static void video_stop_tracked(IHS_Session *session, const IHS_VideoEpochInfo *epoch, void *context)
{
    (void)epoch;
    video_stop(session, context);
}

bool media_attach(IHS_Session *session, bool hardware)
{
    static const IHS_StreamVideoCallbacks video = {
        .startTracked = video_start_tracked, .submitTracked = video_submit, .stopTracked = video_stop_tracked,
        .setTargetFramerate = video_target_framerate
    };
    static const IHS_StreamAudioCallbacks audio = {
        .start = audio_start, .submit = audio_submit, .stop = audio_stop
    };
    latest_frame = av_frame_alloc();
    display_frame = av_frame_alloc();
    prefer_hardware = hardware;
    if (!latest_frame || !display_frame || !IHS_SessionSetVideoTrackingIdentity(session, ++session_identity)) {
        media_discard();
        return false;
    }
    IHS_SessionSetVideoCallbacks(session, &video, NULL);
    IHS_SessionSetAudioCallbacks(session, &audio, NULL);
    return true;
}
