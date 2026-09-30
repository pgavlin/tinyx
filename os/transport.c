#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#ifndef TINYX_MEMORY_ONLY
#include <errno.h>
#define XSERV_t
#define TRANS_SERVER
#define TRANS_REOPEN
#include <X11/Xtrans/Xtrans.h>
#endif
#include <X11/X.h>

#include "misc.h"
#include "dixstruct.h"
#include "osdep.h"
#include "transport.h"

#ifndef TINYX_MEMORY_ONLY
#if defined(EAGAIN) && defined(EWOULDBLOCK)
#define WOULD_BLOCK(err) ((err) == EAGAIN || (err) == EWOULDBLOCK)
#elif defined(EAGAIN)
#define WOULD_BLOCK(err) ((err) == EAGAIN)
#else
#define WOULD_BLOCK(err) ((err) == EWOULDBLOCK)
#endif

static TinyXTransportResult
XtransRead(void *connection, void *buffer, size_t size, size_t *count)
{
    int result = _XSERVTransRead((XtransConnInfo)connection, buffer, size);

    *count = 0;
    if (result > 0) {
        *count = result;
        return TINYX_TRANSPORT_PROGRESS;
    }
    if (result == 0)
        return TINYX_TRANSPORT_CLOSED;
    if (WOULD_BLOCK(errno))
        return TINYX_TRANSPORT_WOULD_BLOCK;
    return TINYX_TRANSPORT_ERROR;
}

static TinyXTransportResult
XtransWrite(void *connection, const void *buffer, size_t size, size_t *count)
{
    int result = _XSERVTransWrite((XtransConnInfo)connection, buffer, size);

    *count = 0;
    if (result > 0) {
        *count = result;
        return TINYX_TRANSPORT_PROGRESS;
    }
    if (result == 0 || WOULD_BLOCK(errno))
        return TINYX_TRANSPORT_WOULD_BLOCK;
    return TINYX_TRANSPORT_ERROR;
}

static void
XtransClose(void *connection)
{
    XtransConnInfo trans = (XtransConnInfo)connection;

    _XSERVTransDisconnect(trans);
    _XSERVTransClose(trans);
}

static const TinyXTransportOps xtransOps = {
    XtransRead,
    XtransWrite,
    XtransClose
};
#endif

TinyXTransportResult
TinyXTransportRead(OsCommPtr oc, void *buffer, size_t size, size_t *count)
{
    if (oc->transportClosed || !oc->transportOps)
        return TINYX_TRANSPORT_CLOSED;
    return oc->transportOps->read(oc->transportData, buffer, size, count);
}

TinyXTransportResult
TinyXTransportWrite(OsCommPtr oc, const void *buffer, size_t size, size_t *count)
{
    if (oc->transportClosed || !oc->transportOps)
        return TINYX_TRANSPORT_CLOSED;
    return oc->transportOps->write(oc->transportData, buffer, size, count);
}

void
TinyXTransportClose(OsCommPtr oc)
{
    if (!oc->transportClosed && oc->transportOps && oc->transportOps->close)
        oc->transportOps->close(oc->transportData);
    oc->transportClosed = TRUE;
    oc->trans_conn = NULL;
}

#ifndef TINYX_MEMORY_ONLY
void
TinyXInitXtransTransport(OsCommPtr oc, XtransConnInfo connection)
{
    oc->transportOps = &xtransOps;
    oc->transportData = connection;
    oc->transportClosed = FALSE;
    /* Retained for native authorization and access-control code. */
    oc->trans_conn = connection;
}
#endif
