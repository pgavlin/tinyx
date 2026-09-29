#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <X11/Xos.h>

#include "tinyx-host.h"

static TinyXHostOps hostOps;
static void *hostData;
static int customHost;
static jmp_buf fatalBoundary;
static int boundaryActive;
static int poisoned;
static char lastError[1024];

static uint32_t
NativeMonotonicTimeMillis(void)
{
    struct timeval tv;

#ifdef MONOTONIC_CLOCK
    struct timespec tp;
    if (clock_gettime(CLOCK_MONOTONIC, &tp) == 0)
        return (uint32_t)(tp.tv_sec * 1000 + tp.tv_nsec / 1000000L);
#endif

    X_GETTIMEOFDAY(&tv);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

int
TinyXHostSetOps(const TinyXHostOps *ops, void *userdata)
{
    if (boundaryActive || poisoned)
        return 0;

    if (ops) {
        hostOps = *ops;
        customHost = 1;
    }
    else {
        memset(&hostOps, 0, sizeof(hostOps));
        customHost = 0;
    }
    hostData = userdata;
    poisoned = 0;
    lastError[0] = '\0';
    return 1;
}

uint32_t
TinyXHostMonotonicTimeMillis(void)
{
    if (customHost && hostOps.monotonicTimeMillis)
        return hostOps.monotonicTimeMillis(hostData);
    return NativeMonotonicTimeMillis();
}

void
TinyXHostWakeup(void)
{
    if (customHost && hostOps.wakeup)
        hostOps.wakeup(hostData);
}

void
TinyXHostVLog(TinyXHostLogLevel level, const char *format, va_list args)
{
    va_list copy;
    char stackBuffer[1024];
    char *message = stackBuffer;
    int length;

    if (!customHost) {
        vfprintf(stderr, format, args);
        return;
    }
    if (!hostOps.log)
        return;

    va_copy(copy, args);
    length = vsnprintf(stackBuffer, sizeof(stackBuffer), format, copy);
    va_end(copy);
    if (length < 0)
        return;
    if ((size_t)length >= sizeof(stackBuffer)) {
        message = malloc((size_t)length + 1);
        if (!message)
            return;
        va_copy(copy, args);
        vsnprintf(message, (size_t)length + 1, format, copy);
        va_end(copy);
    }
    hostOps.log(hostData, level, message);
    if (message != stackBuffer)
        free(message);
}

void
TinyXHostLog(TinyXHostLogLevel level, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    TinyXHostVLog(level, format, args);
    va_end(args);
}

int
TinyXHostRunProtected(TinyXHostOperation operation, void *closure)
{
    if (!operation || boundaryActive || poisoned)
        return 0;

    /* Native FatalError retains its traditional process-terminating policy. */
    if (!customHost) {
        operation(closure);
        return 1;
    }

    boundaryActive = 1;
    if (setjmp(fatalBoundary) == 0) {
        operation(closure);
        boundaryActive = 0;
        return 1;
    }
    boundaryActive = 0;
    return 0;
}

int
TinyXHostIsPoisoned(void)
{
    return poisoned;
}

int
TinyXHostHasCustomOps(void)
{
    return customHost;
}

const char *
TinyXHostLastError(void)
{
    return lastError[0] ? lastError : NULL;
}

void
TinyXHostClearCallbacksAfterFatal(void)
{
    if (!poisoned)
        return;
    memset(&hostOps, 0, sizeof(hostOps));
    hostData = NULL;
}

void
TinyXHostFatal(const char *message)
{
    poisoned = 1;
    if (!message)
        message = "fatal server error";
    snprintf(lastError, sizeof(lastError), "%s", message);

    if (customHost && hostOps.log)
        hostOps.log(hostData, TINYX_HOST_LOG_FATAL, lastError);

    if (customHost && boundaryActive)
        longjmp(fatalBoundary, 1);

    /* A fatal core path cannot safely return without an unwind boundary. */
    abort();
}
