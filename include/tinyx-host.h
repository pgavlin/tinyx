/*
 * Provisional host runtime services used by the embeddable server core.
 *
 * The initial implementation is a process-global singleton and is not
 * thread-safe or reentrant.  This is an internal boundary; the public
 * embedding API will wrap it in a later phase.
 */
#ifndef TINYX_HOST_H
#define TINYX_HOST_H

#include <stdarg.h>
#include <stdint.h>

typedef enum {
    TINYX_HOST_LOG_ERROR,
    TINYX_HOST_LOG_AUDIT,
    TINYX_HOST_LOG_FATAL
} TinyXHostLogLevel;

typedef struct {
    uint32_t (*monotonicTimeMillis)(void *userdata);
    void (*log)(void *userdata, TinyXHostLogLevel level, const char *message);
    void (*wakeup)(void *userdata);
} TinyXHostOps;

typedef void (*TinyXHostOperation)(void *closure);

/*
 * Select host-provided services. A missing monotonicTimeMillis callback uses
 * the native clock. Passing NULL restores the native process implementation.
 * The table is copied; the
 * userdata remains borrowed. Configuration must be performed while no
 * protected operation is active and cannot be changed after a fatal error.
 */
int TinyXHostSetOps(const TinyXHostOps *ops, void *userdata);

/* Internal service entry points used by the core. */
uint32_t TinyXHostMonotonicTimeMillis(void);
void TinyXHostWakeup(void);
void TinyXHostVLog(TinyXHostLogLevel level, const char *format, va_list args);
void TinyXHostLog(TinyXHostLogLevel level, const char *format, ...);

/*
 * Establish a fatal-error boundary around one core operation.  With custom
 * host operations, FatalError poisons the singleton and unwinds to this
 * boundary instead of terminating the process.  Returns zero after a fatal
 * error and one otherwise.  A poisoned server must not be entered again.
 */
int TinyXHostRunProtected(TinyXHostOperation operation, void *closure);
int TinyXHostIsPoisoned(void);
int TinyXHostHasCustomOps(void);
const char *TinyXHostLastError(void);
/* Drop borrowed callback data after poisoning without clearing the poison. */
void TinyXHostClearCallbacksAfterFatal(void);

/* Called only by FatalError; it does not return. */
void TinyXHostFatal(const char *message);

#endif /* TINYX_HOST_H */
