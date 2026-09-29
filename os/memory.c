#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <X11/X.h>

#include "misc.h"
#include "dixstruct.h"
#include "osdep.h"
#include "opaque.h"
#include "tinyx-memory.h"
#include "transport.h"

struct TinyXMemoryClient {
    unsigned char *input;
    size_t inputCount;
    size_t inputCapacity;
    unsigned char *output;
    size_t outputCount;
    size_t outputCapacity;
    size_t outputLimit;
    OsCommPtr osComm;
    Bool inputClosed;
    Bool serverClosed;
};

static Bool
GrowBuffer(unsigned char **buffer, size_t *capacity, size_t needed)
{
    unsigned char *newBuffer;
    size_t newCapacity = *capacity ? *capacity : 256;

    while (newCapacity < needed) {
        if (newCapacity > ((size_t)-1) / 2) {
            newCapacity = needed;
            break;
        }
        newCapacity *= 2;
    }
    newBuffer = realloc(*buffer, newCapacity);
    if (!newBuffer)
        return FALSE;
    *buffer = newBuffer;
    *capacity = newCapacity;
    return TRUE;
}

static TinyXTransportResult
MemoryRead(void *connection, void *buffer, size_t size, size_t *count)
{
    TinyXMemoryClient *memory = connection;

    *count = 0;
    if (memory->inputCount) {
        if (size > memory->inputCount)
            size = memory->inputCount;
        memcpy(buffer, memory->input, size);
        memory->inputCount -= size;
        if (memory->inputCount)
            memmove(memory->input, memory->input + size, memory->inputCount);
        *count = size;
        return TINYX_TRANSPORT_PROGRESS;
    }
    return memory->inputClosed ? TINYX_TRANSPORT_CLOSED :
                                 TINYX_TRANSPORT_WOULD_BLOCK;
}

static TinyXTransportResult
MemoryWrite(void *connection, const void *buffer, size_t size, size_t *count)
{
    TinyXMemoryClient *memory = connection;
    size_t available = size;

    *count = 0;
    if (memory->serverClosed)
        return TINYX_TRANSPORT_CLOSED;
    if (memory->outputLimit) {
        if (memory->outputCount >= memory->outputLimit)
            return TINYX_TRANSPORT_WOULD_BLOCK;
        available = memory->outputLimit - memory->outputCount;
        if (size > available)
            size = available;
    }
    if (!size)
        return TINYX_TRANSPORT_WOULD_BLOCK;
    if (size > (size_t)-1 - memory->outputCount)
        return TINYX_TRANSPORT_ERROR;
    if (!GrowBuffer(&memory->output, &memory->outputCapacity,
                    memory->outputCount + size))
        return TINYX_TRANSPORT_ERROR;
    memcpy(memory->output + memory->outputCount, buffer, size);
    memory->outputCount += size;
    *count = size;
    return TINYX_TRANSPORT_PROGRESS;
}

static void
MemoryClose(void *connection)
{
    TinyXMemoryClient *memory = connection;

    memory->serverClosed = TRUE;
    memory->osComm = NULL;
}

static const TinyXTransportOps memoryOps = {
    MemoryRead,
    MemoryWrite,
    MemoryClose
};

TinyXMemoryClient *
TinyXMemoryClientOpen(size_t outputLimit)
{
    TinyXMemoryClient *memory = calloc(1, sizeof(*memory));
    ClientPtr client;

    if (!memory)
        return NULL;
    memory->outputLimit = outputLimit;
    client = AllocNewConnection(&memoryOps, memory, NULL, -1,
                                GetTimeInMillis());
    if (!client) {
        free(memory);
        return NULL;
    }
    memory->osComm = (OsCommPtr)client->osPrivate;
    return memory;
}

int
TinyXMemoryClientFeed(TinyXMemoryClient *memory, const void *data, size_t size)
{
    if (!memory || memory->inputClosed || memory->serverClosed ||
        size > (size_t)-1 - memory->inputCount)
        return 0;
    if (size && !GrowBuffer(&memory->input, &memory->inputCapacity,
                            memory->inputCount + size))
        return 0;
    if (size) {
        memcpy(memory->input + memory->inputCount, data, size);
        memory->inputCount += size;
        OsCommSetInputReady(memory->osComm, TRUE);
    }
    return 1;
}

size_t
TinyXMemoryClientDrain(TinyXMemoryClient *memory, void *data, size_t size)
{
    size_t count;

    if (!memory || !size)
        return 0;
    count = size < memory->outputCount ? size : memory->outputCount;
    memcpy(data, memory->output, count);
    memory->outputCount -= count;
    if (memory->outputCount)
        memmove(memory->output, memory->output + count, memory->outputCount);
    if (count && memory->osComm)
        OsCommNotifyWritable(memory->osComm);
    return count;
}

void
TinyXMemoryClientCloseInput(TinyXMemoryClient *memory)
{
    if (!memory || memory->inputClosed)
        return;
    memory->inputClosed = TRUE;
    if (memory->osComm)
        OsCommSetInputReady(memory->osComm, TRUE);
}

int
TinyXMemoryClientIsClosed(const TinyXMemoryClient *memory)
{
    return !memory || memory->serverClosed;
}

void
TinyXMemoryClientDestroy(TinyXMemoryClient *memory)
{
    if (!memory)
        return;
    if (memory->osComm) {
        ClientPtr client = clients[memory->osComm->clientIndex];
        if (client)
            CloseDownClient(client);
    }
    free(memory->input);
    free(memory->output);
    free(memory);
}
