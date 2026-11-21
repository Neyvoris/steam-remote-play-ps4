/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Http.h>
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

void updates_check(char *message, size_t capacity)
{
    int net = -1, ssl = -1, http = -1, tmpl = -1, conn = -1, req = -1;
    int result = 0, code = 0;
    unsigned version[3];
    char *headers = NULL;
    size_t length = 0;
    snprintf(message, capacity, "Update check failed. See startup.log.");
    if ((result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL)) < 0 ||
        (result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP)) < 0) goto done;
    if ((result = net = sceNetPoolCreate("steam-update", 256 * 1024, 0)) < 0 ||
        (result = ssl = sceSslInit(512 * 1024)) < 0 ||
        (result = http = sceHttpInit(net, ssl, 256 * 1024)) < 0 ||
        (result = tmpl = sceHttpCreateTemplate(http, "SteamRemotePlayPS4", ORBIS_HTTP_VERSION_1_1, 0)) < 0)
        goto done;
    /* Require server, hostname, certificate lifetime, trust chain, and SNI checks. */
    enum { VERIFY_SERVER = 1, VERIFY_NAME = 4, VERIFY_DATES = 24, VERIFY_CA = 32, TLS_SNI = 128 };
    if ((result = sceHttpsEnableOption(tmpl, VERIFY_SERVER | VERIFY_NAME | VERIFY_DATES | VERIFY_CA | TLS_SNI)) < 0)
        goto done;
    sceHttpSetAutoRedirect(tmpl, 0);
    sceHttpSetConnectTimeOut(tmpl, 3000000);
    sceHttpSetResolveTimeOut(tmpl, 3000000);
    sceHttpSetSendTimeOut(tmpl, 3000000);
    sceHttpSetRecvTimeOut(tmpl, 3000000);
    if ((result = conn = sceHttpCreateConnectionWithURL(tmpl, latest, false)) < 0 ||
        (result = req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, latest, 0)) < 0 ||
        (result = sceHttpSendRequest(req, NULL, 0)) < 0 ||
        (result = sceHttpGetStatusCode(req, &code)) < 0) goto done;
    startup_log_result("GitHub update HTTP status", code);
    if (code < 300 || code >= 400 ||
        (result = sceHttpGetAllResponseHeaders(req, &headers, &length)) < 0 ||
        !headers || !read_release_tag(headers, length, version)) {
        snprintf(message, capacity, "GitHub did not return a supported release tag.");
        goto done;
    }
    snprintf(message, capacity, "GitHub Latest: %u.%u.%u (check only)", version[0], version[1], version[2]);
done:
    if (result < 0) {
        startup_log_result("GitHub update check error", result);
        snprintf(message, capacity, "Update check failed (0x%08x).", (unsigned)result);
    }
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tmpl >= 0) sceHttpDeleteTemplate(tmpl);
    if (http >= 0) sceHttpTerm(http);
    if (ssl >= 0) sceSslTerm(ssl);
    if (net >= 0) sceNetPoolDestroy(net);
}
