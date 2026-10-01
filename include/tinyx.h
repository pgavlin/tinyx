/* Public embedding API for TinyX. */
#ifndef TINYX_H
#define TINYX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINYX_API_VERSION_MAJOR 1
#define TINYX_API_VERSION_MINOR 3
#define TINYX_NO_TIMEOUT UINT32_MAX

#define TINYX_MIN_KEYCODE 8
#define TINYX_MAX_KEYCODE 247
#define TINYX_POINTER_BUTTON_COUNT 5

#if defined(_WIN32) && defined(TINYX_BUILD_DLL)
#define TINYX_API __declspec(dllexport)
#elif defined(_WIN32) && defined(TINYX_USE_DLL)
#define TINYX_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define TINYX_API __attribute__((visibility("default")))
#else
#define TINYX_API
#endif

typedef struct tinyx_server tinyx_server;
typedef struct tinyx_client tinyx_client;

typedef enum tinyx_status {
    TINYX_OK = 0,
    TINYX_ERROR_INVALID_ARGUMENT = 1,
    TINYX_ERROR_INVALID_STATE = 2,
    TINYX_ERROR_ALREADY_EXISTS = 3,
    TINYX_ERROR_OUT_OF_MEMORY = 4,
    TINYX_ERROR_WOULD_BLOCK = 5,
    TINYX_ERROR_CLOSED = 6,
    TINYX_ERROR_UNSUPPORTED = 7,
    TINYX_ERROR_POISONED = 8,
    TINYX_ERROR_FATAL = 9
} tinyx_status;

typedef enum tinyx_log_level {
    TINYX_LOG_ERROR = 0,
    TINYX_LOG_AUDIT = 1,
    TINYX_LOG_FATAL = 2
} tinyx_log_level;

typedef struct tinyx_error {
    tinyx_status status;
    char message[1024];
} tinyx_error;

typedef struct tinyx_host_ops {
    uint32_t struct_size;
    uint32_t (*monotonic_time_ms)(void *userdata);
    void (*log)(void *userdata, tinyx_log_level level, const char *message);
    void (*wakeup)(void *userdata);
    void (*leds_changed)(void *userdata, uint32_t leds);
    void (*bell)(void *userdata, int volume_percent, int pitch_hz,
                 int duration_ms);
} tinyx_host_ops;

typedef enum tinyx_visual_class {
    TINYX_VISUAL_STATIC_GRAY = 0,
    TINYX_VISUAL_GRAY_SCALE = 1,
    TINYX_VISUAL_STATIC_COLOR = 2,
    TINYX_VISUAL_PSEUDO_COLOR = 3,
    TINYX_VISUAL_TRUE_COLOR = 4,
    TINYX_VISUAL_DIRECT_COLOR = 5
} tinyx_visual_class;

typedef struct tinyx_visual_config {
    uint32_t visual_class;
    uint32_t bits_per_rgb;
    uint32_t colormap_entries;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
} tinyx_visual_config;

typedef struct tinyx_depth_config {
    uint32_t depth;
    uint32_t bits_per_pixel;
    const tinyx_visual_config *visuals;
    size_t visual_count;
} tinyx_depth_config;

typedef struct tinyx_framebuffer_config {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    /*
     * Optional physical screen dimensions. Zero selects the default 75 DPI
     * during creation and preserves the current DPI during resize.
     */
    uint32_t width_mm;
    uint32_t height_mm;
    size_t stride_bytes;
    void *pixels;
    size_t pixels_size;
} tinyx_framebuffer_config;

typedef struct tinyx_screen_config {
    uint32_t struct_size;
    tinyx_framebuffer_config framebuffer;
    /*
     * Optional complete, ordered depth/visual topology. A null pointer and
     * zero count select the default depth-24 TrueColor screen and legacy
     * pixmap formats. A custom topology must include depth 1 in 1 bpp. The
     * selected root must remain depth-24 TrueColor in 32 bpp with the masks
     * reported by tinyx_framebuffer_info. Arrays are copied during creation.
     */
    const tinyx_depth_config *depths;
    size_t depth_count;
    size_t root_depth_index;
    size_t root_visual_index;
} tinyx_screen_config;

typedef struct tinyx_config {
    uint32_t struct_size;
    uint32_t api_version_major;
    uint32_t api_version_minor;
    tinyx_host_ops host;
    void *host_userdata;
    const tinyx_screen_config *initial_screen;
} tinyx_config;

typedef struct tinyx_step_result {
    uint32_t requests_processed;
    int immediate_work;
    int generation_finished;
    uint32_t next_timeout_ms;
} tinyx_step_result;

typedef struct tinyx_client_config {
    uint32_t struct_size;
    size_t input_buffer_limit;
    size_t output_buffer_limit;
} tinyx_client_config;

typedef enum tinyx_byte_order {
    TINYX_BYTE_ORDER_LSB_FIRST = 0,
    TINYX_BYTE_ORDER_MSB_FIRST = 1
} tinyx_byte_order;

typedef struct tinyx_framebuffer_info {
    const void *pixels;
    size_t size;
    uint32_t width;
    uint32_t height;
    size_t stride_bytes;
    uint32_t depth;
    uint32_t bits_per_pixel;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    tinyx_byte_order byte_order;
} tinyx_framebuffer_info;

typedef struct tinyx_damage_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} tinyx_damage_rect;

TINYX_API void tinyx_framebuffer_config_init(
    tinyx_framebuffer_config *config);
TINYX_API void tinyx_screen_config_init(tinyx_screen_config *config);
TINYX_API void tinyx_config_init(tinyx_config *config);
TINYX_API void tinyx_client_config_init(tinyx_client_config *config);

TINYX_API tinyx_status tinyx_server_create(const tinyx_config *config,
                                            tinyx_server **out_server,
                                            tinyx_error *error);
TINYX_API tinyx_status tinyx_server_destroy(tinyx_server *server);
TINYX_API const char *tinyx_server_last_error(const tinyx_server *server);

TINYX_API tinyx_status tinyx_server_step(tinyx_server *server,
                                         uint32_t request_budget,
                                         tinyx_step_result *result);

TINYX_API tinyx_status tinyx_client_open(tinyx_server *server,
                                         const tinyx_client_config *config,
                                         tinyx_client **out_client);
TINYX_API tinyx_status tinyx_client_send(tinyx_client *client,
                                         const void *bytes, size_t length,
                                         size_t *out_length);
TINYX_API tinyx_status tinyx_client_receive(tinyx_client *client,
                                            void *bytes, size_t capacity,
                                            size_t *out_length);
TINYX_API size_t tinyx_client_receive_pending(const tinyx_client *client);
TINYX_API tinyx_status tinyx_client_shutdown_send(tinyx_client *client);
TINYX_API int tinyx_client_is_closed(const tinyx_client *client);
TINYX_API void tinyx_client_destroy(tinyx_client *client);

/*
 * Replace the active framebuffer geometry and storage without changing the
 * screen's depth/visual topology. On success, the previous framebuffer
 * pointer is invalid, old borrowed storage is no longer used, and the new
 * framebuffer is fully damaged. A failure leaves the active screen unchanged.
 */
TINYX_API tinyx_status tinyx_server_resize(
    tinyx_server *server, const tinyx_framebuffer_config *framebuffer);

TINYX_API tinyx_status tinyx_server_get_framebuffer(
    tinyx_server *server, tinyx_framebuffer_info *info);
/*
 * Return and clear accumulated framebuffer damage. Passing zero capacity and
 * a null rectangle pointer reports the number of rectangles without clearing
 * them, so the caller can size a subsequent request.
 */
TINYX_API tinyx_status tinyx_server_take_damage(tinyx_server *server,
                                                tinyx_damage_rect *rects,
                                                size_t capacity,
                                                size_t *out_count);

TINYX_API tinyx_status tinyx_pointer_motion_absolute(tinyx_server *server,
                                                     int32_t x, int32_t y);
TINYX_API tinyx_status tinyx_pointer_motion_relative(tinyx_server *server,
                                                     int32_t dx, int32_t dy);
TINYX_API tinyx_status tinyx_pointer_button(tinyx_server *server,
                                            uint32_t button, int pressed);
TINYX_API tinyx_status tinyx_key(tinyx_server *server, uint32_t keycode,
                                 int pressed);
TINYX_API tinyx_status tinyx_release_all_keys(tinyx_server *server);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TINYX_H */
