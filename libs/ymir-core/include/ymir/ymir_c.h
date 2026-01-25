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
    YMIR_RESULT_INTERNAL_ERROR = 4
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

YMIR_CORE_EXPORT ymir_result_t ymir_set_ipl_path(ymir_handle_t *handle, const char *path);
YMIR_CORE_EXPORT ymir_result_t ymir_reset(ymir_handle_t *handle, bool hard_reset);

YMIR_CORE_EXPORT uint64_t ymir_step_master_sh2(ymir_handle_t *handle);
YMIR_CORE_EXPORT void ymir_run_frame(ymir_handle_t *handle);

YMIR_CORE_EXPORT const char *ymir_get_last_error(ymir_handle_t *handle);
YMIR_CORE_EXPORT const char *ymir_get_version_string(void);

#ifdef __cplusplus
}
#endif

#endif
