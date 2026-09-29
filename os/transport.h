/*
 * Internal byte-stream transport interface.
 *
 * Protocol framing and buffering must not depend on a descriptor or Xtrans.
 * A transport operation reports progress explicitly so implementations do not
 * need to communicate would-block state through errno.
 */
#ifndef TINYX_OS_TRANSPORT_H
#define TINYX_OS_TRANSPORT_H

#include <stddef.h>

typedef enum {
    TINYX_TRANSPORT_PROGRESS,
    TINYX_TRANSPORT_WOULD_BLOCK,
    TINYX_TRANSPORT_CLOSED,
    TINYX_TRANSPORT_ERROR
} TinyXTransportResult;

typedef struct {
    TinyXTransportResult (*read)(void *connection, void *buffer, size_t size,
                                 size_t *count);
    TinyXTransportResult (*write)(void *connection, const void *buffer,
                                  size_t size, size_t *count);
    void (*close)(void *connection);
} TinyXTransportOps;

struct _osComm;
struct _XtransConnInfo;

TinyXTransportResult TinyXTransportRead(struct _osComm *oc, void *buffer,
                                        size_t size, size_t *count);
TinyXTransportResult TinyXTransportWrite(struct _osComm *oc,
                                         const void *buffer, size_t size,
                                         size_t *count);
void TinyXTransportClose(struct _osComm *oc);
void TinyXInitXtransTransport(struct _osComm *oc,
                             struct _XtransConnInfo *connection);

#endif /* TINYX_OS_TRANSPORT_H */
