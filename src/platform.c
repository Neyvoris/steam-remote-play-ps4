/* SPDX-License-Identifier: GPL-3.0-only */
#include "platform.h"
#include "startup.h"
#include "ihs_thread.h"
#include "ihs_udp.h"
#include "ihs_buffer.h"
#include "mbedtls/entropy.h"
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int native_clock_gettime(int clock, struct timespec *time) __asm__("sceKernelClockGettime");
static int (*native_random)(void *data, size_t size);

int clock_gettime(clockid_t clock, struct timespec *time)
{
    // The OpenOrbis C headers use musl clock IDs, not the kernel's IDs.
    int native;
    switch (clock) {
    case CLOCK_REALTIME: native = 0; break;
    case CLOCK_MONOTONIC: native = 4; break;
    case CLOCK_THREAD_CPUTIME_ID: native = 14; break;
    default: errno = EINVAL; return -1;
    }
    int result = native_clock_gettime(native, time);
    if (result >= 0) return result;
    errno = (int)((unsigned)result & 0xffffu);
    return -1;
}

int ps4_random_init(void)
{
    startup_log_result("load Random module", sceSysmoduleLoadModule(ORBIS_SYSMODULE_RANDOM));
    OrbisKernelModule handles[256];
    size_t count = 0;
    const size_t capacity = sizeof(handles) / sizeof(handles[0]);
    int result = sceKernelGetModuleList(handles, capacity, &count);
    if (result < 0) return result;
    if (count > capacity) count = capacity;

    void *address = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (sceKernelDlsym((int)handles[i], "sceRandomGetRandomNumber", &address) >= 0 && address) {
            native_random = address;
            return 0;
        }
    }
    return -1;
}

int ps4_random(void *data, size_t size)
{
    return native_random ? native_random(data, size) : -1;
}

int mbedtls_hardware_poll(void *context, unsigned char *output, size_t length, size_t *written)
{
    (void)context;
    *written = 0;
    while (*written < length) {
        size_t chunk = length - *written;
        if (chunk > 64) chunk = 64;
        if (ps4_random(output + *written, chunk) != 0) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        *written += chunk;
    }
    return 0;
}

int ps4_network_init(void)
{
    startup_log_result("load Net module", (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET));
    startup_log_step("sceNetInit");
    startup_log_result("sceNetInit", sceNetInit());
    startup_log_step("sceNetPoolCreate");
    return sceNetPoolCreate("steam-remote", 256 * 1024, 0);
}

struct IHS_Thread {
    pthread_t id;
    IHS_ThreadFunction *function;
    void *context;
};
struct IHS_Mutex { pthread_mutex_t native; };
struct IHS_Cond { pthread_cond_t native; };

static void *thread_entry(void *data)
{
    IHS_Thread *thread = data;
    thread->function(thread->context);
    return NULL;
}
IHS_Thread *IHS_ThreadCreate(IHS_ThreadFunction *function, const char *name, void *context)
{
    (void)name;
    IHS_Thread *thread = malloc(sizeof(*thread));
    if (!thread) return NULL;
    thread->function = function;
    thread->context = context;
    if (pthread_create(&thread->id, NULL, thread_entry, thread) != 0) {
        free(thread);
        return NULL;
    }
    return thread;
}
void IHS_ThreadJoin(IHS_Thread *thread)
{
    if (!thread) return;
    pthread_join(thread->id, NULL);
    free(thread);
}
IHS_Mutex *IHS_MutexCreate(void)
{
    IHS_Mutex *mutex = malloc(sizeof(*mutex));
    if (!mutex) return NULL;
    // OpenOrbis's POSIX attribute type is four bytes. The native PS4 API
    // stores an opaque pointer, which needs eight bytes on this target.
    union {
        void *opaque;
        OrbisPthreadMutexattr sdk;
    } attributes = { .opaque = NULL };
    if (scePthreadMutexattrInit(&attributes.sdk) != 0) {
        free(mutex);
        return NULL;
    }
    // IHSlib's SDL mutexes are recursive. In particular, its disconnect path
    // holds the base lock when it enters the worker-interrupt callback.
    int result = scePthreadMutexattrSettype(&attributes.sdk, PTHREAD_MUTEX_RECURSIVE);
    if (result == 0) result = scePthreadMutexInit(&mutex->native, &attributes.sdk, "steam-remote-play");
    scePthreadMutexattrDestroy(&attributes.sdk);
    if (result != 0) {
        free(mutex);
        return NULL;
    }
    return mutex;
}
void IHS_MutexDestroy(IHS_Mutex *mutex)
{
    pthread_mutex_destroy(&mutex->native);
    free(mutex);
}
bool IHS_MutexLock(IHS_Mutex *mutex) { return pthread_mutex_lock(&mutex->native) == 0; }
bool IHS_MutexUnlock(IHS_Mutex *mutex) { return pthread_mutex_unlock(&mutex->native) == 0; }

int ps4_threading_check(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    IHS_Mutex *lock = IHS_MutexCreate();
    if (!lock) return -2;
    bool locked = IHS_MutexLock(lock);
    bool recursive = locked && pthread_mutex_trylock(&lock->native) == 0;
    if (recursive) IHS_MutexUnlock(lock);
    if (locked) IHS_MutexUnlock(lock);
    IHS_MutexDestroy(lock);
    return recursive ? 0 : -3;
}

IHS_Cond *IHS_CondCreate(void)
{
    IHS_Cond *cond = malloc(sizeof(*cond));
    if (!cond) return NULL;
    if (pthread_cond_init(&cond->native, NULL) != 0) {
        free(cond);
        return NULL;
    }
    return cond;
}
void IHS_CondDestroy(IHS_Cond *cond)
{
    pthread_cond_destroy(&cond->native);
    free(cond);
}
void IHS_CondSignal(IHS_Cond *cond) { pthread_cond_signal(&cond->native); }
bool IHS_CondWait(IHS_Cond *cond, IHS_Mutex *mutex)
{
    return pthread_cond_wait(&cond->native, &mutex->native) == 0;
}

/* The PS4 network API uses BSD sockaddr layouts and option values. */
enum { NET_SOL_SOCKET = 0xffff, NET_SO_BROADCAST = 0x20,
       NET_SO_NONBLOCK = 0x1200, NET_SO_RCVBUF = 0x1002,
       NET_EWOULDBLOCK = (int)0x80410123, NET_EINTR = (int)0x80410104 };
struct IHS_UDPSocket { int fd; uint32_t wait_us; };
typedef struct {
    uint8_t length, family;
    uint16_t port;
    uint8_t ip[4], padding[8];
} NetAddress4;
_Static_assert(sizeof(NetAddress4) == sizeof(OrbisNetSockaddr), "socket address layout");

IHS_UDPSocket *IHS_UDPSocketOpen(bool broadcast)
{
    IHS_UDPSocket *socket = calloc(1, sizeof(*socket));
    if (!socket) return NULL;
    socket->fd = sceNetSocket("steam-remote-play", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_DGRAM, 0);
    if (socket->fd >= 0) {
        int one = 1, size = 512 * 1024;
        if (sceNetSetsockopt(socket->fd, NET_SOL_SOCKET, NET_SO_NONBLOCK, &one, sizeof(one)) < 0) {
            sceNetSocketClose(socket->fd);
            socket->fd = -1;
        } else {
            if (broadcast) sceNetSetsockopt(socket->fd, NET_SOL_SOCKET, NET_SO_BROADCAST, &one, sizeof(one));
            sceNetSetsockopt(socket->fd, NET_SOL_SOCKET, NET_SO_RCVBUF, &size, sizeof(size));
        }
    }
    socket->wait_us = 2000;
    return socket;
}
void IHS_UDPSocketClose(IHS_UDPSocket *socket)
{
    if (!socket) return;
    if (socket->fd >= 0) sceNetSocketClose(socket->fd);
    free(socket);
}
bool IHS_UDPSocketSetBlocking(IHS_UDPSocket *socket, bool blocking)
{
    if (!socket || socket->fd < 0) return false;
    socket->wait_us = blocking ? 2000 : 1000;
    return true;
}
bool IHS_UDPSocketSetRecvTimeout(IHS_UDPSocket *socket, uint32_t timeout_us)
{
    if (!socket || socket->fd < 0) return false;
    socket->wait_us = timeout_us < 2000 ? timeout_us : 2000;
    return true;
}
int IHS_UDPSocketReceive(IHS_UDPSocket *socket, IHS_UDPPacket *packet)
{
    if (!socket || socket->fd < 0) {
        sceKernelUsleep(2000);
        return 0;
    }
    NetAddress4 sender = {0};
    OrbisNetSocklen_t size = sizeof(sender);
    IHS_BufferEnsureMaxSize(&packet->buffer, 2048);
    int result = sceNetRecvfrom(socket->fd, IHS_BufferPointer(&packet->buffer),
                               IHS_BufferMaxSize(&packet->buffer), 0, (OrbisNetSockaddr *)&sender, &size);
    if (result <= 0) {
        if (result && result != NET_EWOULDBLOCK && result != NET_EINTR &&
            !(result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
            // Keep the receive worker alive so the session can unwind its send
            // thread through the normal disconnect path after a network failure.
            sceKernelUsleep(20000);
        } else {
            sceKernelUsleep(socket->wait_us);
        }
        return 0;
    }
    if (size < sizeof(sender) || sender.family != ORBIS_NET_AF_INET) return 0;
    packet->buffer.size = (size_t)result;
    packet->address.ip.family = IHS_IPAddressFamilyIPv4;
    memcpy(packet->address.ip.v4.data, &sender.ip, 4);
    packet->address.port = ntohs(sender.port);
    return 1;
}
bool IHS_UDPSocketSend(IHS_UDPSocket *socket, const IHS_UDPPacket *packet)
{
    if (!socket || socket->fd < 0 || packet->address.ip.family != IHS_IPAddressFamilyIPv4) return false;
    NetAddress4 receiver = {0};
    receiver.length = sizeof(receiver);
    receiver.family = ORBIS_NET_AF_INET;
    receiver.port = htons(packet->address.port);
    memcpy(&receiver.ip, packet->address.ip.v4.data, 4);
    int sent = sceNetSendto(socket->fd, IHS_BufferPointer(&packet->buffer), packet->buffer.size, 0,
                           (const OrbisNetSockaddr *)&receiver, sizeof(receiver));
    return sent >= 0 && (size_t)sent == packet->buffer.size;
}

size_t IHS_UDPBroadcastAddresses(IHS_IPAddress *out, size_t max)
{
    // This PS4 backend uses the global broadcast address for discovery.
    (void)out;
    (void)max;
    return 0;
}
