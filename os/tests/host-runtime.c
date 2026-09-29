#include <string.h>

#include "tinyx-host.h"

static unsigned int logCount;
static unsigned int wakeCount;
static unsigned int operationCount;

static uint32_t
TestTime(void *userdata)
{
    return *(uint32_t *)userdata;
}

static void
TestLog(void *userdata, TinyXHostLogLevel level, const char *message)
{
    (void)userdata;
    if (level == TINYX_HOST_LOG_FATAL && strcmp(message, "expected") == 0)
        logCount++;
}

static void
TestWakeup(void *userdata)
{
    (void)userdata;
    wakeCount++;
}

static void
Succeed(void *closure)
{
    (void)closure;
    operationCount++;
}

static void
Fail(void *closure)
{
    (void)closure;
    TinyXHostFatal("expected");
}

int
main(void)
{
    uint32_t now = 0xfedcba98U;
    TinyXHostOps ops = { TestTime, TestLog, TestWakeup };

    if (!TinyXHostSetOps(&ops, &now))
        return 1;
    if (TinyXHostMonotonicTimeMillis() != now)
        return 2;

    TinyXHostWakeup();
    if (wakeCount != 1)
        return 3;
    if (!TinyXHostRunProtected(Succeed, NULL) || operationCount != 1)
        return 4;
    if (TinyXHostRunProtected(Fail, NULL))
        return 5;
    if (!TinyXHostIsPoisoned() || logCount != 1)
        return 6;
    if (!TinyXHostLastError() || strcmp(TinyXHostLastError(), "expected"))
        return 7;
    if (TinyXHostRunProtected(Succeed, NULL) || operationCount != 1)
        return 8;
    if (TinyXHostSetOps(NULL, NULL))
        return 9;

    return 0;
}
