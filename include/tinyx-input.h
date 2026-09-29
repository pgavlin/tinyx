/*
 * Provisional host input interface.
 *
 * Input is injected into the singleton KDrive devices and follows the normal
 * MI and DIX event path. This interface is not thread-safe or reentrant and
 * may change when the public embedding facade is defined.
 */
#ifndef TINYX_INPUT_H
#define TINYX_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The memory DDX uses the conventional US Xorg keycode layout (evdev + 8). */
#define TINYX_INPUT_MIN_KEYCODE 8
#define TINYX_INPUT_MAX_KEYCODE 247

typedef struct {
    /* X keyboard LED mask, as reported by the core keyboard controls. */
    void (*ledsChanged)(void *userdata, uint32_t leds);
    /* X bell parameters: volume percent, pitch in Hz, duration in ms. */
    void (*bell)(void *userdata, int volume, int pitch, int duration);
} TinyXInputHostOps;

/*
 * Configure optional output effects for the memory keyboard. The operation
 * table is copied and userdata is borrowed. Passing NULL disables effects.
 * Callbacks must not reenter the server.
 */
int TinyXInputSetHostOps(const TinyXInputHostOps *ops, void *userdata);
void TinyXInputClearHostOpsAfterFatal(void);

/*
 * Inject root-screen coordinates or relative deltas. Relative motion passes
 * through the configured X pointer acceleration; absolute motion does not.
 */
int TinyXInputPointerMotionAbsolute(int32_t x, int32_t y);
int TinyXInputPointerMotionRelative(int32_t dx, int32_t dy);

/*
 * Inject an X button number or X keycode transition. pressed is boolean.
 * Injection functions return nonzero when the transition is accepted and
 * zero when devices are unavailable, arguments are invalid, or the singleton
 * is poisoned.
 */
int TinyXInputPointerButton(uint32_t button, int pressed);
int TinyXInputKey(uint32_t keycode, int pressed);

/* Release all keys currently held in the KDrive keyboard state. */
int TinyXInputReleaseAllKeys(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYX_INPUT_H */
