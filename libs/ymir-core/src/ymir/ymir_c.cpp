#include <ymir/ymir_c.h>

#include <ymir/sys/saturn.hpp>
#include <ymir/util/dev_log.hpp>
#include <ymir/version.hpp>

#include <fmt/format.h>

#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

struct ymir_handle {
    ymir::Saturn saturn;
    ymir_log_callback_t log_callback = nullptr;
    void *log_user_data = nullptr;
    std::string last_error;
};

namespace {

ymir_log_level_t ToLogLevel(devlog::Level level) {
    switch (level) {
        case devlog::level::trace:
            return YMIR_LOG_LEVEL_TRACE;
        case devlog::level::debug:
            return YMIR_LOG_LEVEL_DEBUG;
        case devlog::level::info:
            return YMIR_LOG_LEVEL_INFO;
        case devlog::level::warn:
            return YMIR_LOG_LEVEL_WARN;
        case devlog::level::error:
            return YMIR_LOG_LEVEL_ERROR;
        default:
            return YMIR_LOG_LEVEL_INFO;
    }
}

void DevLogSink(devlog::Level level, const char *message, void *user_data) {
    auto *handle = static_cast<ymir_handle *>(user_data);
    if (handle == nullptr) {
        return;
    }
    if (handle->log_callback == nullptr) {
        return;
    }
    handle->log_callback(handle->log_user_data, ToLogLevel(level), message);
}

void EmitLog(ymir_handle *handle, ymir_log_level_t level, const char *message) {
    if (handle == nullptr) {
        return;
    }
    if (handle->log_callback == nullptr) {
        return;
    }
    handle->log_callback(handle->log_user_data, level, message);
}

void SetLastError(ymir_handle *handle, ymir_log_level_t level, std::string message) {
    if (handle == nullptr) {
        return;
    }
    handle->last_error = std::move(message);
    if (handle->log_callback != nullptr) {
        handle->log_callback(handle->log_user_data, level, handle->last_error.c_str());
    }
}

void ClearLastError(ymir_handle *handle) {
    if (handle == nullptr) {
        return;
    }
    handle->last_error.clear();
}

} // namespace

extern "C" {

ymir_handle_t *ymir_create(const ymir_config_t *config) {
    static_cast<void>(config);

    try {
        auto *handle = new ymir_handle();
        return handle;
    } catch (const std::exception &) {
        return nullptr;
    }
}

void ymir_destroy(ymir_handle_t *handle) {
    if (handle != nullptr) {
        const auto sink_state = devlog::GetLogSink();
        if (sink_state.sink == &DevLogSink && sink_state.user_data == handle) {
            devlog::SetLogSink(nullptr, nullptr);
        }
    }
    delete handle;
}

void ymir_set_log_callback(ymir_handle_t *handle, ymir_log_callback_t callback, void *user_data) {
    if (handle == nullptr) {
        return;
    }
    handle->log_callback = callback;
    handle->log_user_data = user_data;
    if (callback != nullptr) {
        devlog::SetLogSink(&DevLogSink, handle);
    } else {
        const auto sink_state = devlog::GetLogSink();
        if (sink_state.sink == &DevLogSink && sink_state.user_data == handle) {
            devlog::SetLogSink(nullptr, nullptr);
        }
    }
}

ymir_result_t ymir_set_ipl_path(ymir_handle_t *handle, const char *path) {
    if (handle == nullptr || path == nullptr || path[0] == '\0') {
        if (handle != nullptr) {
            SetLastError(handle, YMIR_LOG_LEVEL_ERROR, "No IPL ROM path provided");
        }
        return YMIR_RESULT_INVALID_ARGUMENT;
    }

    try {
        std::filesystem::path rom_path(path);
        std::error_code error;
        const auto file_size = std::filesystem::file_size(rom_path, error);
        if (error) {
            SetLastError(handle, YMIR_LOG_LEVEL_ERROR,
                         fmt::format("Failed to read IPL ROM size: {}", error.message()));
            return YMIR_RESULT_IO_ERROR;
        }
        if (file_size != ymir::sys::kIPLSize) {
            SetLastError(
                handle, YMIR_LOG_LEVEL_ERROR,
                fmt::format("IPL ROM size mismatch: expected {} bytes, got {} bytes", ymir::sys::kIPLSize,
                            file_size));
            return YMIR_RESULT_SIZE_MISMATCH;
        }

        std::array<uint8, ymir::sys::kIPLSize> buffer{};
        std::ifstream input(rom_path, std::ios::binary);
        if (!input) {
            SetLastError(handle, YMIR_LOG_LEVEL_ERROR, "Failed to open IPL ROM file");
            return YMIR_RESULT_IO_ERROR;
        }

        input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        if (!input || input.gcount() != static_cast<std::streamsize>(buffer.size())) {
            SetLastError(handle, YMIR_LOG_LEVEL_ERROR, "Failed to read IPL ROM file");
            return YMIR_RESULT_IO_ERROR;
        }

        handle->saturn.LoadIPL(std::span<uint8, ymir::sys::kIPLSize>(buffer));
        ClearLastError(handle);
        EmitLog(handle, YMIR_LOG_LEVEL_INFO, "IPL ROM loaded");
        return YMIR_RESULT_OK;
    } catch (const std::exception &ex) {
        SetLastError(handle, YMIR_LOG_LEVEL_ERROR, fmt::format("Exception while loading IPL ROM: {}", ex.what()));
        return YMIR_RESULT_INTERNAL_ERROR;
    }
}

ymir_result_t ymir_reset(ymir_handle_t *handle, bool hard_reset) {
    if (handle == nullptr) {
        return YMIR_RESULT_INVALID_ARGUMENT;
    }

    try {
        handle->saturn.Reset(hard_reset);
        ClearLastError(handle);
        return YMIR_RESULT_OK;
    } catch (const std::exception &ex) {
        SetLastError(handle, YMIR_LOG_LEVEL_ERROR, fmt::format("Exception while resetting: {}", ex.what()));
        return YMIR_RESULT_INTERNAL_ERROR;
    }
}

uint64_t ymir_step_master_sh2(ymir_handle_t *handle) {
    if (handle == nullptr) {
        return 0;
    }

    try {
        return handle->saturn.StepMasterSH2();
    } catch (const std::exception &ex) {
        SetLastError(handle, YMIR_LOG_LEVEL_ERROR, fmt::format("Exception while stepping: {}", ex.what()));
        return 0;
    }
}

void ymir_run_frame(ymir_handle_t *handle) {
    if (handle == nullptr) {
        return;
    }

    try {
        handle->saturn.RunFrame();
    } catch (const std::exception &ex) {
        SetLastError(handle, YMIR_LOG_LEVEL_ERROR, fmt::format("Exception while running frame: {}", ex.what()));
    }
}

const char *ymir_get_last_error(ymir_handle_t *handle) {
    if (handle == nullptr) {
        return nullptr;
    }
    if (handle->last_error.empty()) {
        return nullptr;
    }
    return handle->last_error.c_str();
}

const char *ymir_get_version_string(void) {
    return ymir::version::string;
}

} // extern "C"
