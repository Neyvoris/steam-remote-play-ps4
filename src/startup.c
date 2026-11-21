/* SPDX-License-Identifier: GPL-3.0-only */
#include "startup.h"
#include <fcntl.h>
#include <orbis/libkernel.h>
#include <stdint.h>

static int log_fd = -1;

void startup_log_open(void)
{
    sceKernelMkdir("/data/steam-remote-ps4", 0700);
    log_fd = sceKernelOpen("/data/steam-remote-ps4/startup.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    startup_log_step("Steam Remote Play PS4 0.2.5: main entered");
}

bool startup_log_available(void)
{
    return log_fd >= 0;
}

static void write_step(const char *name, const char *suffix, size_t suffix_len)
{
    if (log_fd < 0) return;
    char line[256];
    size_t length = 0;
    while (length < sizeof(line) - suffix_len && name[length]) {
        line[length] = name[length];
        ++length;
    }
    for (size_t i = 0; i < suffix_len; ++i) line[length + i] = suffix[i];
    sceKernelWrite(log_fd, line, length + suffix_len);
    sceKernelFsync(log_fd);
}

void startup_log_step(const char *name)
{
    char newline[] = "\n";
    write_step(name, newline, 1);
}

void startup_log_result(const char *name, int result)
{
    char line[] = " = 0x00000000\n";
    const char digits[] = "0123456789abcdef";
    uint32_t code = (uint32_t)result;
    for (unsigned i = 0; i < 8; ++i) line[5 + i] = digits[(code >> (28 - 4 * i)) & 0xf];
    write_step(name, line, sizeof(line) - 1);
}

void startup_log_close(void)
{
    if (log_fd >= 0) sceKernelClose(log_fd);
    log_fd = -1;
}
