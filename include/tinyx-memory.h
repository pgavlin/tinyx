/* Provisional in-process client byte-stream interface. */
#ifndef TINYX_MEMORY_H
#define TINYX_MEMORY_H

#include <stddef.h>

typedef struct TinyXMemoryClient TinyXMemoryClient;

/*
 * Open a logical X11 client on the current server generation.  A zero output
 * limit allows unbounded buffering; otherwise writes apply backpressure once
 * that many unread output bytes are queued.
 */
TinyXMemoryClient *TinyXMemoryClientOpen(size_t outputLimit);

/* Append an arbitrary fragment of client-to-server protocol bytes. */
int TinyXMemoryClientFeed(TinyXMemoryClient *client, const void *data,
                          size_t size);

/* Drain up to size server-to-client bytes. Returns the number copied. */
size_t TinyXMemoryClientDrain(TinyXMemoryClient *client, void *data,
                              size_t size);

/* Signal end-of-input. Queued input is consumed before the client closes. */
void TinyXMemoryClientCloseInput(TinyXMemoryClient *client);

/* True after the server has closed its side of the logical connection. */
int TinyXMemoryClientIsClosed(const TinyXMemoryClient *client);

/* Release the handle after it is closed and all desired output is drained. */
void TinyXMemoryClientDestroy(TinyXMemoryClient *client);

#endif /* TINYX_MEMORY_H */
