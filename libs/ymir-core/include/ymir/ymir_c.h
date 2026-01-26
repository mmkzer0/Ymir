#ifndef YMIR_YMIR_C_H
#define YMIR_YMIR_C_H

#include <ymir/export.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ymir_handle ymir_handle_t;

typedef enum ymir_result {
    YMIR_RESULT_OK = 0,
    YMIR_RESULT_INVALID_ARGUMENT = 1,
    YMIR_RESULT_IO_ERROR = 2,
    YMIR_RESULT_SIZE_MISMATCH = 3,
    YMIR_RESULT_INTERNAL_ERROR = 4,
    YMIR_RESULT_NOT_READY = 5
} ymir_result_t;

typedef enum ymir_log_level {
    YMIR_LOG_LEVEL_TRACE = 1,
    YMIR_LOG_LEVEL_DEBUG = 2,
    YMIR_LOG_LEVEL_INFO = 3,
    YMIR_LOG_LEVEL_WARN = 4,
    YMIR_LOG_LEVEL_ERROR = 5
} ymir_log_level_t;

typedef void (*ymir_log_callback_t)(void *user_data, ymir_log_level_t level, const char *message);

typedef struct ymir_config {
    uint32_t struct_size;
    uint32_t flags;
} ymir_config_t;

#define YMIR_CONFIG_INIT                                                                                               \
    { sizeof(ymir_config_t), 0u }

YMIR_CORE_EXPORT ymir_handle_t *ymir_create(const ymir_config_t *config);
YMIR_CORE_EXPORT void ymir_destroy(ymir_handle_t *handle);

// Note: devlog output is global; the most recently set callback receives it.
YMIR_CORE_EXPORT void ymir_set_log_callback(ymir_handle_t *handle, ymir_log_callback_t callback, void *user_data);

typedef enum ymir_control_pad_button {
    YMIR_CONTROL_PAD_BUTTON_RIGHT = 1u << 15u,
    YMIR_CONTROL_PAD_BUTTON_LEFT = 1u << 14u,
    YMIR_CONTROL_PAD_BUTTON_DOWN = 1u << 13u,
    YMIR_CONTROL_PAD_BUTTON_UP = 1u << 12u,
    YMIR_CONTROL_PAD_BUTTON_START = 1u << 11u,
    YMIR_CONTROL_PAD_BUTTON_A = 1u << 10u,
    YMIR_CONTROL_PAD_BUTTON_C = 1u << 9u,
    YMIR_CONTROL_PAD_BUTTON_B = 1u << 8u,
    YMIR_CONTROL_PAD_BUTTON_R = 1u << 7u,
    YMIR_CONTROL_PAD_BUTTON_X = 1u << 6u,
    YMIR_CONTROL_PAD_BUTTON_Y = 1u << 5u,
    YMIR_CONTROL_PAD_BUTTON_Z = 1u << 4u,
    YMIR_CONTROL_PAD_BUTTON_L = 1u << 3u,
    YMIR_CONTROL_PAD_BUTTON_ALL = (1u << 15u) | (1u << 14u) | (1u << 13u) | (1u << 12u) | (1u << 11u) |
                                  (1u << 10u) | (1u << 9u) | (1u << 8u) | (1u << 7u) | (1u << 6u) | (1u << 5u) |
                                  (1u << 4u) | (1u << 3u)
} ymir_control_pad_button_t;

typedef enum ymir_framebuffer_format {
    YMIR_FRAMEBUFFER_FORMAT_XBGR8888 = 1
} ymir_framebuffer_format_t;

typedef enum ymir_audio_format {
    YMIR_AUDIO_FORMAT_S16 = 1
} ymir_audio_format_t;

typedef struct ymir_audio_info {
    uint32_t sample_rate;
    uint32_t channels;
    ymir_audio_format_t format;
} ymir_audio_info_t;

typedef struct ymir_framebuffer_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    ymir_framebuffer_format_t format;
} ymir_framebuffer_info_t;

YMIR_CORE_EXPORT ymir_result_t ymir_set_ipl_path(ymir_handle_t *handle, const char *path);
YMIR_CORE_EXPORT ymir_result_t ymir_reset(ymir_handle_t *handle, bool hard_reset);

// pressed_buttons is a bitmask with 1=pressed. Use ymir_control_pad_button_t values.
YMIR_CORE_EXPORT ymir_result_t ymir_set_control_pad_buttons(ymir_handle_t *handle, uint32_t port,
                                                            uint16_t pressed_buttons);

YMIR_CORE_EXPORT void ymir_get_audio_info(ymir_handle_t *handle, ymir_audio_info_t *out_info);
// Returns the number of frames read. Output is interleaved, stereo, S16.
YMIR_CORE_EXPORT size_t ymir_read_audio_samples(ymir_handle_t *handle, int16_t *out_samples, size_t max_frames);

YMIR_CORE_EXPORT uint64_t ymir_step_master_sh2(ymir_handle_t *handle);
YMIR_CORE_EXPORT void ymir_run_frame(ymir_handle_t *handle);

YMIR_CORE_EXPORT ymir_result_t ymir_copy_framebuffer(ymir_handle_t *handle, void *out_buffer, size_t buffer_size,
                                                     ymir_framebuffer_info_t *out_info, uint64_t *out_frame_id);

YMIR_CORE_EXPORT const char *ymir_get_last_error(ymir_handle_t *handle);
YMIR_CORE_EXPORT const char *ymir_get_version_string(void);

#ifdef __cplusplus
}
#endif

#endif
