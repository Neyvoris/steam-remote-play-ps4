/* SPDX-License-Identifier: GPL-3.0-only */
#include "gamepad.h"
#include "media.h"
#include "platform.h"
#include "screen.h"
#include "startup.h"
#include "updates.h"
#include <orbis/Net.h>
#include <orbis/libkernel.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { MAX_HOSTS = 12 };
enum { PROFILE_1080_30, PROFILE_720_30, PROFILE_1080_60, PROFILE_720_60, PROFILE_1080_60_HW, PROFILE_720_30_HW, PROFILE_COUNT };
typedef struct {
    unsigned width, height, fps, bitrate;
    IHS_StreamQuality quality;
    const char *label;
    bool hardware;
} StreamProfile;
static const StreamProfile profiles[PROFILE_COUNT] = {
    [PROFILE_1080_30] = {VIDEO_WIDTH, VIDEO_HEIGHT, 30, 14000, IHS_StreamQualityBalanced, "1080p / 30 FPS", false},
    [PROFILE_720_30] = {SCREEN_WIDTH, SCREEN_HEIGHT, 30, 6000, IHS_StreamQualityFast, "720p / 30 FPS", false},
    [PROFILE_1080_60] = {VIDEO_WIDTH, VIDEO_HEIGHT, 60, 20000, IHS_StreamQualityBeautiful, "1080p / 60 FPS", false},
    [PROFILE_720_60] = {SCREEN_WIDTH, SCREEN_HEIGHT, 60, 11000, IHS_StreamQualityBalanced, "720p / 60 FPS", false},
    [PROFILE_1080_60_HW] = {VIDEO_WIDTH, VIDEO_HEIGHT, 60, 20000, IHS_StreamQualityBeautiful,
                          "1080p / 60 FPS (HW)", true},
    [PROFILE_720_30_HW] = {SCREEN_WIDTH, SCREEN_HEIGHT, 30, 6000, IHS_StreamQualityFast,
                         "720p / 30 FPS (HW)", true},
};
static const char profile_path[] = "/data/steam-remote-ps4/video-profile.txt";
static const char profile_temp[] = "/data/steam-remote-ps4/video-profile.tmp";
typedef enum { EVENT_NONE, EVENT_AUTHORIZED, EVENT_AUTH_FAILED, EVENT_STREAM_READY, EVENT_STREAM_FAILED } Event;
typedef enum { BROWSING, REQUESTING, PAIRING } MenuPhase;
typedef struct {
    uint64_t device_id;
    uint8_t secret[32];
} Identity;
typedef struct {
    uint64_t device_id;
    uint64_t steam_id;
    uint8_t secret[32];
} Pairing;

static pthread_mutex_t shared_lock = PTHREAD_MUTEX_INITIALIZER;
static IHS_HostInfo discovered_hosts[MAX_HOSTS];
static unsigned host_count;
static Event event;
static uint64_t event_host;
static int event_result;
static IHS_SessionInfo stream_info;
static Pairing pairing_reply;
static atomic_bool session_done;
static atomic_bool session_connected;
static atomic_bool session_negotiating;
static atomic_uint session_log_count;
static const char *data_dir = "/data/steam-remote-ps4";

static void pairing_path(char path[72], uint64_t client_id)
{
    snprintf(path, 72, "%s/paired-%016" PRIx64 ".bin", data_dir, client_id);
}

static bool load_pairing(uint64_t client_id, uint64_t device_id, Pairing *pairing)
{
    char path[72];
    pairing_path(path, client_id);
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    Pairing saved;
    bool ok = fread(&saved, sizeof(saved), 1, file) == 1 && saved.device_id == device_id;
    fclose(file);
    if (ok) *pairing = saved;
    return ok;
}

static bool save_pairing(uint64_t client_id, const Pairing *pairing)
{
    char path[72], temporary[80];
    pairing_path(path, client_id);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    bool ok = fwrite(pairing, sizeof(*pairing), 1, file) == 1;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(temporary, path) == 0) return true;
    remove(temporary);
    return false;
}

static bool load_identity(Identity *identity)
{
    const char *path = "/data/steam-remote-ps4/identity.bin";
    const char *temporary = "/data/steam-remote-ps4/identity.tmp";
    FILE *file = fopen(path, "rb");
    if (file) {
        bool ok = fread(identity, sizeof(*identity), 1, file) == 1 && identity->device_id != 0;
        fclose(file);
        if (ok) return true;
    }
    if (ps4_random(identity, sizeof(*identity)) != 0) return false;
    identity->device_id |= 1;
    sceKernelMkdir(data_dir, 0700);
    file = fopen(temporary, "wb");
    if (!file) return false;
    bool saved = fwrite(identity, sizeof(*identity), 1, file) == 1;
    if (fclose(file) != 0) saved = false;
    if (!saved || rename(temporary, path) != 0) {
        remove(temporary);
        return false;
    }
    return true;
}

static void found(IHS_Client *client, const IHS_HostInfo *host, void *unused)
{
    (void)client;
    (void)unused;
    if (host->address.ip.family != IHS_IPAddressFamilyIPv4) return;
    pthread_mutex_lock(&shared_lock);
    unsigned i = 0;
    while (i < host_count && discovered_hosts[i].clientId != host->clientId) ++i;
    if (i < MAX_HOSTS) {
        discovered_hosts[i] = *host;
        if (i == host_count) host_count++;
    }
    pthread_mutex_unlock(&shared_lock);
}

static void post(Event next, const IHS_HostInfo *host, int result)
{
    pthread_mutex_lock(&shared_lock);
    event_host = host->clientId;
    event = next;
    event_result = result;
    pthread_mutex_unlock(&shared_lock);
}

static void authorized(IHS_Client *client, const IHS_HostInfo *host, uint64_t steam_id, void *unused)
{
    (void)unused;
    Pairing reply = { .steam_id = steam_id };
    IHS_ClientGetSecretKey(client, reply.secret);
    pthread_mutex_lock(&shared_lock);
    pairing_reply = reply;
    event_host = host->clientId;
    event = EVENT_AUTHORIZED;
    event_result = 0;
    pthread_mutex_unlock(&shared_lock);
}

static void auth_failed(IHS_Client *client, const IHS_HostInfo *host, IHS_AuthorizationResult result, void *unused)
{
    (void)client;
    (void)unused;
    post(EVENT_AUTH_FAILED, host, result);
}

static void stream_ready(IHS_Client *client, const IHS_HostInfo *host, const IHS_SocketAddress *addr,
                         const uint8_t *key, size_t key_size, void *unused)
{
    (void)client;
    (void)unused;
    if (!key || (key_size != 16 && key_size != 24 && key_size != sizeof(stream_info.sessionKey)) || addr->ip.family != IHS_IPAddressFamilyIPv4) {
        post(EVENT_STREAM_FAILED, host, IHS_StreamingFailed);
        return;
    }
    pthread_mutex_lock(&shared_lock);
    stream_info = (IHS_SessionInfo){ .address = *addr, .sessionKeyLen = key_size };
    memcpy(stream_info.sessionKey, key, key_size);
    event_host = host->clientId;
    event = EVENT_STREAM_READY;
    pthread_mutex_unlock(&shared_lock);
}

static void stream_failed(IHS_Client *client, const IHS_HostInfo *host, IHS_StreamingResult result, void *unused)
{
    (void)client;
    (void)unused;
    post(EVENT_STREAM_FAILED, host, result);
}

static unsigned load_profile(void)
{
    FILE *file = fopen(profile_path, "r");
    if (!file) return PROFILE_720_30_HW;
    int value = fgetc(file), end = fgetc(file);
    fclose(file);
    if ((end != '\n' && end != EOF) || value < '0' || value >= '0' + PROFILE_COUNT) return PROFILE_720_30_HW;
    return (unsigned)(value - '0');
}

static bool save_profile(unsigned profile)
{
    FILE *file = fopen(profile_temp, "w");
    if (!file) return false;
    bool ok = fprintf(file, "%u\n", profile) > 0;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(profile_temp, profile_path) == 0) return true;
    remove(profile_temp);
    return false;
}

static bool request_stream(IHS_Client *client, const IHS_HostInfo *host, const StreamProfile *profile)
{
    const IHS_StreamingRequest request = {
        .streamingEnable = { true, true, true },
        .maxResolution = { (int)profile->width, (int)profile->height },
        .maxFramerateNumerator = (int)profile->fps,
        .maxFramerateDenominator = 1,
        .audioChannelCount = 2,
        .streamingInterface = IHS_StreamInterfaceBigPicture,
        .gamepadCount = 1
    };
    return IHS_ClientStreamingRequest(client, host, &request);
}

static bool request_pairing(IHS_Client *client, const IHS_ClientConfig *config,
                            const IHS_HostInfo *host, char pin[5])
{
    uint32_t number;
    if (ps4_random(&number, sizeof(number)) != 0) return false;
    snprintf(pin, 5, "%04u", number % 10000);
    IHS_ClientSetSecretKey(client, config->secretKey);
    return IHS_ClientAuthorizationRequest(client, host, pin);
}

static const char *stream_error(int result)
{
    switch (result) {
    case IHS_StreamingDriversNotInstalled: return "Stream (7): Steam reports missing host drivers.";
    case IHS_StreamingScreenLocked: return "Stream (2): Unlock the desktop on the PC.";
    case IHS_StreamingDisabled: return "Stream (8): Enable Remote Play in Steam.";
    case IHS_StreamingPINRequired: return "Stream (11): Steam requires its Remote Play security PIN.";
    case IHS_StreamingTimeout: return "Steam did not answer the stream request.";
    default: return NULL;
    }
}

static const char *pairing_error(int result)
{
    switch (result) {
    case IHS_AuthorizationDenied: return "Pairing (1): Denied. Retry with the new PIN.";
    case IHS_AuthorizationNotLoggedIn: return "Pairing (2): Log in to Steam on the PC.";
    case IHS_AuthorizationFailed: return "Pairing (7): Steam could not pair this device.";
    default: return NULL;
    }
}

static void configuring(IHS_Session *session, IHS_SessionConfig *config, void *context)
{
    (void)session;
    const StreamProfile *profile = context;
    config->enableAudio = true;
    config->enableHevc = false;
    config->enableHardwareDecoding = profile->hardware;
    config->maxWidth = profile->width;
    config->maxHeight = profile->height;
    config->maxFps = profile->fps;
    config->maxBitrateKbps = profile->bitrate;
    config->quality = profile->quality;
    atomic_store(&session_negotiating, true);
}

static void connected(IHS_Session *session, void *unused)
{
    (void)unused;
    atomic_store(&session_connected, true);
    IHS_SessionHIDNotifyDeviceChange(session);
}

static void finalized(IHS_Session *session, void *unused)
{
    (void)session;
    (void)unused;
    atomic_store(&session_done, true);
}

static void log_warning(IHS_LogLevel level, const char *tag, const char *message)
{
    if (level <= IHS_LogLevelWarn) printf("[Steam Remote Play] %s: %s\n", tag, message);
}

static void log_session(IHS_LogLevel level, const char *tag, const char *message)
{
    if (level > IHS_LogLevelWarn && (level != IHS_LogLevelInfo || strcmp(tag, "Data") != 0)) return;
    if (atomic_fetch_add(&session_log_count, 1) >= 32) return;
    char line[224];
    snprintf(line, sizeof(line), "Session %s: %s", tag, message);
    startup_log_step(line);
}

static void log_video_status(const MediaVideoStatus *video)
{
    startup_log_result("Session negotiation reached", atomic_load(&session_negotiating));
    startup_log_result("Session connected", atomic_load(&session_connected));
    if (video->decoded || video->hardware_active || video->hardware_fallback)
        startup_log_step(video->hardware_active ? "Video decoder: Videodec2" : "Video decoder: FFmpeg");
    if (video->hardware_fallback) startup_log_result("Videodec2 error", video->hardware_error);
    startup_log_result("Video codec", video->codec);
    startup_log_result("Video width", video->width);
    startup_log_result("Video height", video->height);
    if (video->decoded_width > 0) {
        char geometry[96];
        snprintf(geometry, sizeof(geometry), "Decoded image: %dx%d, sample aspect %d:%d",
                 video->decoded_width, video->decoded_height, video->sample_aspect_num, video->sample_aspect_den);
        startup_log_step(geometry);
    }
    startup_log_result("Assembled video frames", (int)video->received);
    startup_log_result("Decoded video frames", (int)video->decoded);
    startup_log_result("Submitted display frames", (int)video->submitted);
    if (video->host_fps_den) {
        char rate[96];
        snprintf(rate, sizeof(rate), "Steam target FPS: %u/%u (limiter flags 0x%x)",
                 video->host_fps_num, video->host_fps_den, video->host_fps_limits);
        startup_log_step(rate);
    }
    if (video->received || video->submitted) {
        char timing[128];
        snprintf(timing, sizeof(timing), "Average video work: decoder %u us/packet, conversion %u us/frame",
                 video->average_decode_us, video->average_render_us);
        startup_log_step(timing);
    }
    startup_log_result("Decoded pixel format", video->frame_format);
    startup_log_result("FFmpeg error", video->decoder_error);
    startup_log_result("Display error", video->display_error);
    startup_log_result("Audio output error", media_audio_error());
}

static void log_stream_rates(const MediaVideoStatus *before, const MediaVideoStatus *after, uint64_t elapsed_us)
{
    if (elapsed_us < 1000000) return;
    const unsigned arrived = (unsigned)((after->received - before->received) * UINT64_C(10000000) / elapsed_us);
    const unsigned decoded = (unsigned)((after->decoded - before->decoded) * UINT64_C(10000000) / elapsed_us);
    const unsigned shown = (unsigned)((after->submitted - before->submitted) * UINT64_C(10000000) / elapsed_us);
    char rates[192];
    snprintf(rates, sizeof(rates), "Video FPS: received %u.%u, decoded %u.%u, displayed %u.%u, Steam target %u/%u, limiter flags 0x%x",
             arrived / 10, arrived % 10, decoded / 10, decoded % 10, shown / 10, shown % 10,
             after->host_fps_num, after->host_fps_den, after->host_fps_limits);
    startup_log_step(rates);
}

static void calibrate_screen(ScreenArea *area, bool live, uint32_t buttons)
{
    const uint32_t arrows = ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT |
                            ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN;
    ScreenCalibration calibration = {.area = *area};
    uint32_t previous = buttons;
    uint64_t repeat_at = 0;
    bool redraw = true;
    while (!live || !atomic_load(&session_done)) {
        buttons = gamepad_buttons();
        const uint32_t pressed = buttons & ~previous;
        const uint32_t direction = buttons & arrows;
        const uint64_t now = sceKernelGetProcessTime();
        if (pressed & ORBIS_PAD_BUTTON_CIRCLE) return;
        if (pressed & ORBIS_PAD_BUTTON_CROSS) {
            if (screen_area_save(calibration.area)) {
                *area = calibration.area;
                return;
            }
            startup_log_step("Could not save the screen area");
            calibration.save_failed = true;
        }
        if (pressed & ORBIS_PAD_BUTTON_L1) calibration.bottom_right = false;
        if (pressed & ORBIS_PAD_BUTTON_R1) calibration.bottom_right = true;
        if (pressed & ORBIS_PAD_BUTTON_SQUARE) calibration.area = screen_area_default();
        if (pressed & ORBIS_PAD_BUTTON_TRIANGLE)
            screen_area_set_fill(&calibration.area, !calibration.area.fill);
        if (direction && ((pressed & arrows) || now >= repeat_at)) {
            int dx = !!(direction & ORBIS_PAD_BUTTON_RIGHT) - !!(direction & ORBIS_PAD_BUTTON_LEFT);
            int dy = !!(direction & ORBIS_PAD_BUTTON_DOWN) - !!(direction & ORBIS_PAD_BUTTON_UP);
            screen_area_move(&calibration.area, calibration.bottom_right, 4 * dx, 4 * dy);
            repeat_at = now + ((pressed & arrows) ? 250000 : 35000);
            redraw = true;
        }
        previous = buttons;
        redraw |= pressed != 0;
        if (live ? media_present(calibration.area, &calibration, redraw, NULL) :
                   redraw && screen_calibration_preview(&calibration)) redraw = false;
        sceKernelUsleep(5000);
    }
}

static const char *play(const IHS_ClientConfig *config, const IHS_SessionInfo *info,
                        const char *name, ScreenArea *area, StreamProfile profile)
{
    static const IHS_StreamSessionCallbacks callbacks = {
        .configuring = configuring, .connected = connected, .finalized = finalized
    };
    if (screen_set_stream_height((int)profile.height) < 0) return "Could not open the video output.";
    IHS_Session *session = IHS_SessionCreate(config, info);
    if (!session) return "Could not create the Steam session.";
    atomic_store(&session_log_count, 0);
    IHS_SessionSetLogFunction(session, log_session);
    IHS_SessionSetSessionCallbacks(session, &callbacks, &profile);
    startup_log_step(profile.label);
    const IHS_StreamClientCapabilities capabilities = {
        .decoderInfo = profile.hardware ? "PS4 Videodec2 H.264" : "PS4 FFmpeg software H.264",
        .decoderThreads = profile.hardware ? 0 : VIDEO_DECODER_THREADS,
        .maximumDecodeBitrateKbps = profile.bitrate,
        .formFactor = IHS_StreamDeviceFormFactorTV,
    };
    IHS_SessionSetClientCapabilities(session, &capabilities);
    media_reset_video_status();
    if (!media_attach(session, profile.hardware)) {
        IHS_SessionDestroy(session);
        return "Could not allocate video frames.";
    }
    gamepad_set_local(true);
    gamepad_attach(session);
    atomic_store(&session_done, false);
    atomic_store(&session_connected, false);
    atomic_store(&session_negotiating, false);
    if (!IHS_SessionConnect(session)) {
        IHS_SessionDestroy(session);
        media_discard();
        return "Could not start the Steam transport.";
    }

    bool got_video = false, stopping = false, cancelled = false;
    bool local_input = true, redraw = false;
    uint64_t started_at = sceKernelGetProcessTime();
    uint64_t playback_started = 0, playback_stopped = 0;
    MediaVideoStatus playback_start = {0}, playback_end = {0};
    uint64_t menu_update = 0, sample_started = 0;
    MediaVideoStatus sample_start = {0}, hud_start = {0};
    uint64_t hud_started = started_at;
    char hud[96] = "Measuring stream FPS...";
    uint32_t previous = gamepad_buttons();
    while (!atomic_load(&session_done)) {
        const uint64_t hud_now = sceKernelGetProcessTime();
        if (hud_now - hud_started >= UINT64_C(1000000)) {
            const MediaVideoStatus status = media_video_status();
            const uint64_t elapsed = hud_now - hud_started;
            snprintf(hud, sizeof(hud), "%s | RX %u DEC %u OUT %u FPS | %u/%u us | LOG %s",
                     status.hardware_fallback ? "SW fallback" :
                     status.hardware_active ? "HW" : "SW",
                     (unsigned)((status.received - hud_start.received) * UINT64_C(1000000) / elapsed),
                     (unsigned)((status.decoded - hud_start.decoded) * UINT64_C(1000000) / elapsed),
                     (unsigned)((status.submitted - hud_start.submitted) * UINT64_C(1000000) / elapsed),
                     status.average_decode_us, status.average_render_us,
                     startup_log_available() ? "open" : "failed");
            hud_start = status;
            hud_started = hud_now;
            redraw = got_video;
        }
        if (media_present(*area, NULL, redraw, hud)) {
            if (!got_video && !stopping) {
                playback_started = sceKernelGetProcessTime();
                playback_start = media_video_status();
            }
            got_video = true;
            redraw = false;
        }
        const uint64_t now = sceKernelGetProcessTime();
        const uint32_t buttons = gamepad_buttons(), pressed = buttons & ~previous;
        previous = buttons;
        if (local_input && buttons == 0) {
            gamepad_set_local(false);
            local_input = false;
        }
        if (!stopping && got_video && (buttons & GAMEPAD_SCREEN) == GAMEPAD_SCREEN &&
            (pressed & GAMEPAD_SCREEN)) {
            gamepad_set_local(true);
            local_input = true;
            calibrate_screen(area, true, buttons);
            previous = gamepad_buttons();
            redraw = true;
            continue;
        }
        const bool wants_close = (buttons & GAMEPAD_EXIT) == GAMEPAD_EXIT ||
                                 (!got_video && (pressed & ORBIS_PAD_BUTTON_CIRCLE));
        if (!stopping && (wants_close || (!got_video && now - started_at > 20000000))) {
            cancelled = wants_close;
            stopping = true;
            playback_stopped = now;
            playback_end = media_video_status();
            IHS_SessionDisconnect(session);
        }
        if (got_video && !stopping) {
            if (!sample_started) {
                sample_started = now;
                sample_start = media_video_status();
            } else if (now - sample_started >= UINT64_C(5000000)) {
                MediaVideoStatus sample_end = media_video_status();
                log_stream_rates(&sample_start, &sample_end, now - sample_started);
                sample_start = sample_end;
                sample_started = now;
            }
        }
        if (!got_video && now >= menu_update) {
            MediaVideoStatus video = media_video_status();
            screen_menu(stopping ? "Closing session" : "Opening Big Picture", name,
                        video.received ? "Decoding video. CIRCLE cancels." :
                        atomic_load(&session_connected) ? "Waiting for video. CIRCLE cancels." :
                        "Connecting to the Steam stream. CIRCLE cancels.");
            menu_update = now + 150000;
        }
        sceKernelUsleep(2000);
    }
    if (!playback_stopped) {
        playback_stopped = sceKernelGetProcessTime();
        playback_end = media_video_status();
    }
    IHS_SessionThreadedJoin(session);
    bool host_stopped = IHS_SessionHostRequestedStop(session);
    MediaVideoStatus video = media_video_status();
    startup_log_result("Steam host requested stop", host_stopped);
    log_video_status(&video);
    if (playback_started && playback_stopped >= playback_started)
        log_stream_rates(&playback_start, &playback_end, playback_stopped - playback_started);
    IHS_SessionDestroy(session);
    media_discard();
    if (got_video && video.hardware_fallback) return "Stream closed. Used FFmpeg. See startup.log.";
    if (got_video && video.hardware_active) return "Stream closed. Used Videodec2.";
    if (got_video || cancelled) return "Stream closed. CROSS to reconnect.";
    if (video.decoded) return "Frames decoded, but display failed. See startup.log.";
    if (video.decoder_error || video.received) return "Video decoding failed. See startup.log.";
    if (host_stopped) return "Steam stopped before sending frames. See startup.log.";
    if (!atomic_load(&session_connected)) return "Steam video connection did not finish. See startup.log.";
    return "Connected, but no video frames arrived. See startup.log.";
}

static void wait_with_error(const char *status, const char *detail)
{
    do {
        screen_menu(status, "", detail);
        sceKernelUsleep(30000);
    } while (!(gamepad_buttons() & ORBIS_PAD_BUTTON_CIRCLE));
}

static void menu(IHS_Client *client, const IHS_ClientConfig *config)
{
    MenuPhase phase = BROWSING;
    ScreenArea area = screen_area_load();
    unsigned video_profile = load_profile();
    unsigned selected = 0, tab = 0;
    char host_names[MAX_HOSTS][64];
    uint32_t previous = gamepad_buttons();
    uint64_t next_draw = 0, deadline = 0;
    IHS_HostInfo target = {0};
    Pairing paired = {0};
    uint64_t paired_host = 0;
    char pin[5] = "", status[108] = "";
    for (;;) {
        const uint32_t buttons = gamepad_buttons(), pressed = buttons & ~previous;
        uint64_t now = sceKernelGetProcessTime();
        previous = buttons;
        IHS_HostInfo host = {0};
        IHS_SessionInfo info = {0};
        pthread_mutex_lock(&shared_lock);
        unsigned count = host_count;
        for (unsigned i = 0; i < count; ++i)
            snprintf(host_names[i], sizeof(host_names[i]), "%.63s", discovered_hosts[i].hostname);
        if (phase == BROWSING && count) {
            if (pressed & ORBIS_PAD_BUTTON_DOWN) selected = (selected + 1) % count;
            if (pressed & ORBIS_PAD_BUTTON_UP) selected = (selected + count - 1) % count;
            host = discovered_hosts[selected];
        }
        Event received = event_host == target.clientId ? event : EVENT_NONE;
        int result = event_result;
        if (received == EVENT_STREAM_READY) info = stream_info;
        if (received == EVENT_AUTHORIZED && phase == PAIRING) {
            paired = pairing_reply;
            paired.device_id = config->deviceId;
            paired_host = target.clientId;
        }
        event = EVENT_NONE;
        pthread_mutex_unlock(&shared_lock);

        if (phase == BROWSING) {
            if (pressed & ORBIS_PAD_BUTTON_LEFT) tab = (tab + 2) % 3;
            if (pressed & ORBIS_PAD_BUTTON_RIGHT) tab = (tab + 1) % 3;
            if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) tab = 1;
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                if (!tab) return;
                tab = 0;
                continue;
            }
            if (pressed & ORBIS_PAD_BUTTON_L1) {
                screen_menu("Checking updates", "", "Contacting GitHub.");
                updates_check(status, sizeof(status));
                next_draw = 0;
                continue;
            }
            if (pressed & ORBIS_PAD_BUTTON_R1) {
                video_profile = (video_profile + 1) % PROFILE_COUNT;
                if (!save_profile(video_profile))
                    snprintf(status, sizeof(status), "Video mode could not be saved.");
                else status[0] = 0;
                next_draw = 0;
            }
            if (pressed & ORBIS_PAD_BUTTON_OPTIONS) {
                calibrate_screen(&area, false, buttons);
                previous = gamepad_buttons();
                next_draw = 0;
                continue;
            }
            if (pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN)) status[0] = 0;
            if (tab != 2 && count && (pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_SQUARE))) {
                target = host;
                if (paired_host != target.clientId || paired.device_id != config->deviceId) {
                    paired = (Pairing){0};
                    if (load_pairing(target.clientId, config->deviceId, &paired)) paired_host = target.clientId;
                }
                if ((pressed & ORBIS_PAD_BUTTON_SQUARE) || paired.device_id != config->deviceId) {
                    if (request_pairing(client, config, &target, pin)) {
                        startup_log_step("Steam pairing requested");
                        phase = PAIRING;
                        deadline = now + 120000000;
                    } else {
                        snprintf(status, sizeof(status), "Could not start pairing. Try again.");
                    }
                } else {
                    IHS_ClientSetSecretKey(client, paired.secret);
                    if (request_stream(client, &target, &profiles[video_profile])) {
                        startup_log_step("Steam stream requested");
                        phase = REQUESTING;
                        deadline = now + 30000000;
                    } else {
                        snprintf(status, sizeof(status), "Could not send the stream request. Try again.");
                    }
                }
            }
        } else if (received == EVENT_STREAM_READY && phase == REQUESTING) {
            startup_log_step("Steam accepted streaming");
            info.steamId = paired.steam_id;
            IHS_ClientConfig session_config = *config;
            session_config.secretKey = paired.secret;
            const char *stream_status = play(&session_config, &info, target.hostname, &area, profiles[video_profile]);
            memset(&info, 0, sizeof(info));
            snprintf(status, sizeof(status), "%s", stream_status);
            phase = BROWSING;
            next_draw = 0;
            previous = gamepad_buttons();
        } else if (received == EVENT_STREAM_FAILED && phase == REQUESTING) {
            startup_log_result("Steam stream response", result);
            if (result == IHS_StreamingUnauthorized) {
                if (request_pairing(client, config, &target, pin)) {
                    startup_log_step("Steam re-pairing requested");
                    phase = PAIRING;
                    deadline = now + 120000000;
                } else {
                    snprintf(status, sizeof(status), "Could not start pairing. Try again.");
                    phase = BROWSING;
                }
            } else {
                const char *message = stream_error(result);
                if (message) snprintf(status, sizeof(status), "%s", message);
                else snprintf(status, sizeof(status), "Steam rejected streaming (%d). Check the PC.", result);
                phase = BROWSING;
            }
        } else if (phase == PAIRING &&
                   (received == EVENT_AUTHORIZED || received == EVENT_AUTH_FAILED ||
                    (pressed & ORBIS_PAD_BUTTON_CIRCLE))) {
            if (received == EVENT_AUTHORIZED) {
                startup_log_step("Steam pairing accepted");
                bool saved = save_pairing(target.clientId, &paired);
                startup_log_result("Save Steam pairing", saved ? 0 : -1);
                if (request_stream(client, &target, &profiles[video_profile])) {
                    startup_log_step("Steam stream requested after pairing");
                    phase = REQUESTING;
                    deadline = now + 30000000;
                } else {
                    snprintf(status, sizeof(status), "Paired. Press CROSS to start streaming.");
                    phase = BROWSING;
                }
            } else {
                if (received == EVENT_NONE) {
                    IHS_ClientAuthorizationCancel(client);
                    snprintf(status, sizeof(status), "Pairing cancelled.");
                } else {
                    startup_log_result("Steam pairing response", result);
                    const char *message = pairing_error(result);
                    if (message) snprintf(status, sizeof(status), "%s", message);
                    else snprintf(status, sizeof(status), "Steam pairing failed (%d). Try again.", result);
                }
                phase = BROWSING;
            }
        } else if (now >= deadline) {
            if (phase == PAIRING) IHS_ClientAuthorizationCancel(client);
            snprintf(status, sizeof(status), "Timed out. Check Steam on the PC and try again.");
            phase = BROWSING;
        }

        if (now >= next_draw) {
            if (phase == REQUESTING) {
                screen_menu("Connecting", target.hostname, "Waiting for the Steam host.");
            } else {
                screen_tv_menu(tab, host_names, count, selected, status,
                               profiles[video_profile].label, phase == PAIRING ? pin : NULL);
            }
            next_draw = now + 80000;
        }
        sceKernelUsleep(5000);
    }
}

int main(void)
{
    startup_log_open();
    startup_log_step("display init");
    int result = screen_init();
    startup_log_result("display init", result);
    if (result < 0) {
        startup_log_close();
        return 0;
    }
    screen_menu("Starting", "", "Initializing the DualShock 4.");
    startup_log_step("gamepad init");
    result = gamepad_init();
    startup_log_result("gamepad init", result);
    if (result < 0) {
        for (int i = 0; i < 40; ++i) {
            screen_menu("No controller", "", "Connect a DualShock 4 and restart the app.");
            sceKernelUsleep(100000);
        }
        screen_shutdown();
        startup_log_close();
        return 0;
    }
    startup_log_step("network init");
    int net = ps4_network_init();
    startup_log_result("Net pool", net);
    Identity identity;
    if (net < 0) {
        wait_with_error("Network unavailable", "Connect the PS4 to your home network.");
    } else {
        startup_log_step("random init");
        result = ps4_random_init();
        startup_log_result("random init", result);
        if (result < 0) {
            wait_with_error("Random service unavailable", "See startup.log for the last startup stage.");
            goto cleanup;
        }
        startup_log_step("load identity");
        bool identity_loaded = load_identity(&identity);
        startup_log_result("load identity", identity_loaded ? 0 : -1);
        if (!identity_loaded) {
            wait_with_error("Identity unavailable", "Check that the app can write to /data.");
            goto cleanup;
        }
        startup_log_step("threading check");
        result = ps4_threading_check();
        startup_log_result("threading check", result);
        if (result < 0) {
            wait_with_error("Threading unavailable", "See startup.log for the last startup stage.");
            goto cleanup;
        }
        const IHS_ClientConfig config = { identity.device_id, identity.secret, "Steam Remote Play on PS4" };
        const IHS_ClientDiscoveryCallbacks discovery = { .discovered = found };
        const IHS_ClientAuthorizationCallbacks auth = { .success = authorized, .failed = auth_failed };
        const IHS_ClientStreamingCallbacks stream = { .success = stream_ready, .failed = stream_failed };
        startup_log_step("IHS init");
        IHS_Init();
        startup_log_step("create Steam client");
        IHS_Client *client = IHS_ClientCreate(&config);
        IHS_ClientSetLogFunction(client, log_warning);
        IHS_ClientSetDiscoveryCallbacks(client, &discovery, NULL);
        IHS_ClientSetAuthorizationCallbacks(client, &auth, NULL);
        IHS_ClientSetStreamingCallbacks(client, &stream, NULL);
        startup_log_step("start Steam discovery");
        IHS_ClientStartDiscovery(client, 3000);
        startup_log_step("Steam discovery menu ready");
        menu(client, &config);
        IHS_ClientStop(client);
        IHS_ClientThreadedJoin(client);
        IHS_ClientDestroy(client);
        IHS_Quit();
    }
cleanup:
    memset(&identity, 0, sizeof(identity));
    startup_log_close();
    if (net >= 0) sceNetPoolDestroy(net);
    gamepad_close();
    screen_shutdown();
    return 0;
}
