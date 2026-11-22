/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <mbedtls/sha256.h>
#include <orbis/AppInstUtil.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
/* OpenOrbis 0.5.4 exposes this SDK symbol as a placeholder. */
#define sceHttpSetResponseHeaderMaxSize orbis_header_size_placeholder
#include <orbis/Http.h>
#undef sceHttpSetResponseHeaderMaxSize
extern int32_t sceHttpSetResponseHeaderMaxSize(int32_t id, uint64_t size);
#include <orbis/Sysmodule.h>
#include "startup.h"
#include "updates.h"

static const char latest[] =
    "https://github.com/ivanopolo30-dot/steam-remote-play-ps4/releases/latest";
static const char release_prefix[] =
    "https://github.com/ivanopolo30-dot/steam-remote-play-ps4/releases/tag/v";

static bool read_release_tag(const char *headers, size_t size, unsigned version[3])
{
    while (size) {
        const char *end = memchr(headers, '\n', size);
        size_t length = end ? (size_t)(end - headers) : size;
        if (length >= 9 && strncasecmp(headers, "Location:", 9) == 0) {
            const char *value = headers + 9;
            size_t remaining = length - 9;
            while (remaining && (*value == ' ' || *value == '\t')) {
                ++value;
                --remaining;
            }
            if (remaining && value[remaining - 1] == '\r') --remaining;
            const size_t prefix = sizeof(release_prefix) - 1;
            if (remaining <= prefix || memcmp(value, release_prefix, prefix) != 0)
                return false;
            value += prefix;
            remaining -= prefix;
            for (unsigned i = 0; i < 3; ++i) {
                unsigned number = 0, digits = 0;
                while (remaining && *value >= '0' && *value <= '9') {
                    if (++digits > 4) return false;
                    number = number * 10 + (unsigned)(*value++ - '0');
                    --remaining;
                }
                if (!digits) return false;
                version[i] = number;
                if (i != 2) {
                    if (!remaining || *value++ != '.') return false;
                    --remaining;
                }
            }
            return remaining == 0;
        }
        if (!end) break;
        size -= length + 1;
        headers = end + 1;
    }
    return false;
}

typedef struct { int net, ssl, http, tmpl, conn, req; } UpdateHttp;
static unsigned available[3];
static const unsigned installed[] = {0, 2, 7};
static const char download_base[] =
    "https://github.com/ivanopolo30-dot/steam-remote-play-ps4/releases/download/";
static const char partial_path[] = "/data/steam-remote-ps4/update.part";
static const char pkg_path[] = "/data/steam-remote-ps4/update.pkg";

static void close_request(UpdateHttp *h)
{
    if (h->req >= 0) sceHttpDeleteRequest(h->req);
    if (h->conn >= 0) sceHttpDeleteConnection(h->conn);
    h->req = h->conn = -1;
}

static void close_http(UpdateHttp *h)
{
    close_request(h);
    if (h->tmpl >= 0) sceHttpDeleteTemplate(h->tmpl);
    if (h->http >= 0) sceHttpTerm(h->http);
    if (h->ssl >= 0) sceSslTerm(h->ssl);
    if (h->net >= 0) sceNetPoolDestroy(h->net);
}

static int open_http(UpdateHttp *h)
{
    *h = (UpdateHttp){-1, -1, -1, -1, -1, -1};
    int rc;
    if ((rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL)) < 0 ||
        (rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP)) < 0 ||
        (rc = h->net = sceNetPoolCreate("steam-update", 256 * 1024, 0)) < 0 ||
        (rc = h->ssl = sceSslInit(512 * 1024)) < 0 ||
        (rc = h->http = sceHttpInit(h->net, h->ssl, 256 * 1024)) < 0 ||
        (rc = h->tmpl = sceHttpCreateTemplate(h->http, "SteamRemotePlayPS4", ORBIS_HTTP_VERSION_1_1, 0)) < 0)
        return rc;
    enum { VERIFY_SERVER = 1, VERIFY_NAME = 4, VERIFY_DATES = 24, VERIFY_CA = 32, TLS_SNI = 128 };
    if ((rc = sceHttpsEnableOption(h->tmpl, VERIFY_SERVER | VERIFY_NAME | VERIFY_DATES | VERIFY_CA | TLS_SNI)) < 0)
        return rc;
    sceHttpSetAutoRedirect(h->tmpl, 0);
    sceHttpSetConnectTimeOut(h->tmpl, 5000000);
    sceHttpSetResolveTimeOut(h->tmpl, 5000000);
    sceHttpSetSendTimeOut(h->tmpl, 5000000);
    sceHttpSetRecvTimeOut(h->tmpl, 5000000);
    return 0;
}

static int send_request(UpdateHttp *h, const char *url, int *code)
{
    close_request(h);
    int rc;
    if ((rc = h->conn = sceHttpCreateConnectionWithURL(h->tmpl, url, false)) < 0 ||
        (rc = h->req = sceHttpCreateRequestWithURL(h->conn, ORBIS_METHOD_GET, url, 0)) < 0 ||
        (rc = sceHttpSetResponseHeaderMaxSize(h->req, 32 * 1024)) < 0 ||
        (rc = sceHttpSendRequest(h->req, NULL, 0)) < 0)
        return rc;
    return sceHttpGetStatusCode(h->req, code);
}

static bool newer(const unsigned version[3])
{
    for (unsigned i = 0; i < 3; ++i)
        if (version[i] != installed[i]) return version[i] > installed[i];
    return false;
}

void updates_check(char *message, size_t capacity)
{
    UpdateHttp h;
    int code = 0;
    int rc = open_http(&h);
    char *headers = NULL;
    size_t length = 0;
    memset(available, 0, sizeof(available));
    if (rc >= 0) rc = send_request(&h, latest, &code);
    if (rc >= 0 && code >= 300 && code < 400) {
        rc = sceHttpGetAllResponseHeaders(h.req, &headers, &length);
        if (rc >= 0 && (!headers || !read_release_tag(headers, length, available))) rc = -1;
    } else if (rc >= 0) rc = -1;
    close_http(&h);
    if (rc < 0) {
        startup_log_result("GitHub update check error", rc);
        snprintf(message, capacity, "Update check failed (0x%08x).", (unsigned)rc);
    } else {
        snprintf(message, capacity, newer(available) ?
                 "GitHub Latest: %u.%u.%u. Press SQUARE in Settings to install." :
                 "GitHub Latest: %u.%u.%u. No newer update.",
                 available[0], available[1], available[2]);
    }
}

static bool asset_redirect(UpdateHttp *h, char *url, size_t capacity)
{
    char *headers = NULL;
    size_t size = 0;
    if (sceHttpGetAllResponseHeaders(h->req, &headers, &size) < 0 || !headers)
        return false;
    while (size) {
        const char *end = memchr(headers, '\n', size);
        size_t len = end ? (size_t)(end - headers) : size;
        if (len >= 9 && strncasecmp(headers, "Location:", 9) == 0) {
            const char *target = headers + 9;
            len -= 9;
            while (len && (*target == ' ' || *target == '\t')) { ++target; --len; }
            if (len && target[len - 1] == '\r') --len;
            static const char host[] = "https://release-assets.githubusercontent.com/";
            if (len >= capacity || len <= sizeof(host) - 1 || memcmp(target, host, sizeof(host) - 1))
                return false;
            for (size_t i = 0; i < len; ++i)
                if ((unsigned char)target[i] <= 32 || (unsigned char)target[i] >= 127) return false;
            memcpy(url, target, len);
            url[len] = 0;
            return true;
        }
        if (!end) break;
        headers = end + 1;
        size -= len + 1;
    }
    return false;
}

static bool open_asset(UpdateHttp *h, const char *tag, const char *name)
{
    char url[4096];
    int code;
    snprintf(url, sizeof(url), "%s%s/%s", download_base, tag, name);
    if (send_request(h, url, &code) < 0) return false;
    if (code >= 300 && code < 400) {
        if (!asset_redirect(h, url, sizeof(url)) || send_request(h, url, &code) < 0)
            return false;
    }
    return code == 200;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_checksum(UpdateHttp *h, const char *name, unsigned char digest[32])
{
    char line[256] = {0};
    size_t used = 0;
    for (;;) {
        if (used == sizeof(line) - 1) return false;
        int n = sceHttpReadData(h->req, line + used, sizeof(line) - 1 - used);
        if (n < 0) return false;
        if (!n) break;
        used += (size_t)n;
    }
    if (used < 67 || line[64] != ' ' || line[65] != ' ') return false;
    while (used && (line[used - 1] == '\r' || line[used - 1] == '\n')) line[--used] = 0;
    const char *file = line + 66;
    if (!strncmp(file, "dist/", 5)) file += 5;
    if (strcmp(file, name)) return false;
    for (unsigned i = 0; i < 32; ++i) {
        int hi = hex_digit(line[2 * i]), lo = hex_digit(line[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        digest[i] = (unsigned char)(hi * 16 + lo);
    }
    return true;
}

static bool save_package(UpdateHttp *h, const unsigned char expected[32])
{
    FILE *file = fopen(partial_path, "wb");
    if (!file) return false;
    unsigned char buffer[16384], digest[32];
    size_t total = 0;
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    bool ok = mbedtls_sha256_starts_ret(&hash, 0) == 0;
    while (ok) {
        int n = sceHttpReadData(h->req, buffer, sizeof(buffer));
        if (!n) break;
        if (n < 0 || (size_t)n > 64u * 1024u * 1024u - total) { ok = false; break; }
        total += (size_t)n;
        ok = fwrite(buffer, 1, (size_t)n, file) == (size_t)n &&
             mbedtls_sha256_update_ret(&hash, buffer, (size_t)n) == 0;
    }
    if (ok) ok = total > 0 && mbedtls_sha256_finish_ret(&hash, digest) == 0 &&
                 memcmp(digest, expected, sizeof(digest)) == 0;
    mbedtls_sha256_free(&hash);
    if (fclose(file) != 0) ok = false;
    if (ok) ok = rename(partial_path, pkg_path) == 0;
    if (!ok) unlink(partial_path);
    return ok;
}

bool updates_install(char *message, size_t capacity)
{
    if (!newer(available)) {
        snprintf(message, capacity, "Check for a newer release with L1 first.");
        return false;
    }
    char tag[24], name[64], title[32] = {0};
    snprintf(tag, sizeof(tag), "v%u.%u.%u", available[0], available[1], available[2]);
    snprintf(name, sizeof(name), "SteamRemotePlay-PS4-%s.pkg", tag + 1);
    UpdateHttp h;
    unsigned char expected[32];
    bool ok = open_http(&h) >= 0 && open_asset(&h, tag, "SHA256SUMS") &&
              read_checksum(&h, name, expected);
    if (!ok) {
        close_http(&h);
        snprintf(message, capacity, "Could not read the release checksum.");
        return false;
    }
    ok = open_asset(&h, tag, name) && save_package(&h, expected);
    close_http(&h);
    if (!ok) {
        snprintf(message, capacity, "Download failed or the package checksum did not match.");
        return false;
    }

    int rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL);
    if (rc >= 0) rc = sceAppInstUtilInitialize();
    if (rc >= 0) {
        int32_t is_app = 0;
        rc = sceAppInstUtilGetTitleIdFromPkg(pkg_path, title, &is_app);
        if (rc >= 0 && (!is_app || memcmp(title, "SRPL00001", sizeof("SRPL00001")))) rc = -1;
        if (rc >= 0) rc = sceAppInstUtilAppInstallPkg(pkg_path, NULL);
        sceAppInstUtilTerminate();
    }
    startup_log_result("PS4 update installer result", rc);
    if (rc < 0) {
        snprintf(message, capacity, "Installer returned 0x%08x. Verified PKG kept.", (unsigned)rc);
        return false;
    }
    return true;
}
