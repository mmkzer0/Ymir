#include <ymir/ymir_c.h>

#include <ymir/hw/smpc/peripheral/peripheral_report.hpp>
#include <ymir/hw/scsp/scsp_defs.hpp>
#include <ymir/sys/saturn.hpp>
#include <ymir/util/dev_log.hpp>
#include <ymir/version.hpp>

#include <fmt/format.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

struct ymir_handle;

struct InputCallbackContext {
    ymir_handle *handle = nullptr;
    uint8 port_index = 0;
};

constexpr uint32 kAudioBufferFrames = 16384u;
constexpr uint32 kAudioBufferMask = kAudioBufferFrames - 1u;
static_assert((kAudioBufferFrames & kAudioBufferMask) == 0u, "Audio buffer size must be a power of two.");

struct AudioRingBuffer {
    std::array<sint16, kAudioBufferFrames * 2u> samples{};
    std::atomic<uint32> write_index{0};
    std::atomic<uint32> read_index{0};

    void Clear() {
        write_index.store(0, std::memory_order_relaxed);
        read_index.store(0, std::memory_order_relaxed);
    }
};

// handle struct 
// basically a saturn instance + related data (for now)
struct ymir_handle {
    ymir::Saturn saturn;
    ymir_log_callback_t log_callback = nullptr;
    void *log_user_data = nullptr;
    std::string last_error;
    std::array<std::atomic<uint16>, 2> control_pad_release_mask;
    std::array<InputCallbackContext, 2> input_callback_contexts;
    AudioRingBuffer audio_buffer;
    std::mutex framebuffer_mutex;
    std::vector<uint32> framebuffer;
    uint32 framebuffer_width = 0;
    uint32 framebuffer_height = 0;
    uint64 framebuffer_frame_id = 0;
    bool framebuffer_ready = false;
};

namespace {

constexpr uint16 kControlPadButtonMask = static_cast<uint16>(ymir::peripheral::Button::All);

// switch devlog level to enum
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

void AudioSampleCallback(sint16 left, sint16 right, void *ctx) {
    auto *handle = static_cast<ymir_handle *>(ctx);
    if (handle == nullptr) {
        return;
    }
    auto &buffer = handle->audio_buffer;
    const uint32 write = buffer.write_index.load(std::memory_order_relaxed);
    const uint32 next = (write + 1u) & kAudioBufferMask;
    uint32 read = buffer.read_index.load(std::memory_order_acquire);
    if (next == read) {
        read = (read + 1u) & kAudioBufferMask;
        buffer.read_index.store(read, std::memory_order_release);
    }
    const size_t base = static_cast<size_t>(write) * 2u;
    buffer.samples[base] = left;
    buffer.samples[base + 1u] = right;
    buffer.write_index.store(next, std::memory_order_release);
}

void PeripheralReportCallback(ymir::peripheral::PeripheralReport &report, void *ctx) {
    auto *context = static_cast<InputCallbackContext *>(ctx);
    if (context == nullptr) {
        return;
    }
    if (context->handle == nullptr) {
        return;
    }
    const size_t port_index = context->port_index;
    if (port_index >= context->handle->control_pad_release_mask.size()) {
        return;
    }
    switch (report.type) {
    case ymir::peripheral::PeripheralType::ControlPad: {
        const uint16 release_mask = context->handle->control_pad_release_mask[port_index].load(
            std::memory_order_relaxed);
        report.report.controlPad.buttons = static_cast<ymir::peripheral::Button>(release_mask);
        break;
    }
    default:
        break;
    }
}

// invoke callback to log level /w message
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

// render callback
// take a frame buffer, w/h and the ctx
void FrameCompleteCallback(uint32 *fb, uint32 width, uint32 height, void *ctx) {
    auto *handle = static_cast<ymir_handle *>(ctx);
    if (handle == nullptr) {
        return;
    }
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixels == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(handle->framebuffer_mutex);
    handle->framebuffer.resize(pixels);
    std::copy_n(fb, pixels, handle->framebuffer.data());
    handle->framebuffer_width = width;
    handle->framebuffer_height = height;
    handle->framebuffer_frame_id += 1;
    handle->framebuffer_ready = true;
}

// emits log via callback
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

// creates and returns new emu handle based on config
ymir_handle_t *ymir_create(const ymir_config_t *config) {
    static_cast<void>(config);

    try {
        auto *handle = new ymir_handle();
        handle->control_pad_release_mask[0].store(kControlPadButtonMask, std::memory_order_relaxed);
        handle->control_pad_release_mask[1].store(kControlPadButtonMask, std::memory_order_relaxed);
        handle->input_callback_contexts[0] = {handle, 0};
        handle->input_callback_contexts[1] = {handle, 1};
        auto &port1 = handle->saturn.SMPC.GetPeripheralPort1();
        port1.ConnectControlPad();
        port1.SetPeripheralReportCallback({&handle->input_callback_contexts[0], &PeripheralReportCallback});
        auto &port2 = handle->saturn.SMPC.GetPeripheralPort2();
        port2.ConnectControlPad();
        port2.SetPeripheralReportCallback({&handle->input_callback_contexts[1], &PeripheralReportCallback});
        handle->audio_buffer.Clear();
        handle->saturn.SCSP.SetSampleCallback({handle, &AudioSampleCallback});
        handle->saturn.VDP.SetRenderCallback({handle, &FrameCompleteCallback});
        return handle;
    } catch (const std::exception &) {
        return nullptr;
    }
}

// destroys emulation instance, resets log sink, then destroys handle
void ymir_destroy(ymir_handle_t *handle) {
    if (handle != nullptr) {
        handle->saturn.VDP.SetRenderCallback({});
        handle->saturn.SCSP.SetSampleCallback({});
        handle->saturn.SMPC.GetPeripheralPort1().SetPeripheralReportCallback({});
        handle->saturn.SMPC.GetPeripheralPort2().SetPeripheralReportCallback({});
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

// set ipl path of instance from path
ymir_result_t ymir_set_ipl_path(ymir_handle_t *handle, const char *path) {
    // early return with error on invalid parameters
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
        // error out on faulty file size read
        if (error) {
            SetLastError(handle, YMIR_LOG_LEVEL_ERROR,
                         fmt::format("Failed to read IPL ROM size: {}", error.message()));
            return YMIR_RESULT_IO_ERROR;
        }
        // error out on invalid IPL
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

// reset function, takes handle and bool for hard reset
ymir_result_t ymir_reset(ymir_handle_t *handle, bool hard_reset) {
    if (handle == nullptr) {
        return YMIR_RESULT_INVALID_ARGUMENT;
    }

    try {
        handle->saturn.Reset(hard_reset);
        {
            std::lock_guard<std::mutex> lock(handle->framebuffer_mutex);
            handle->framebuffer_ready = false;
            handle->framebuffer_frame_id = 0;
        }
        handle->audio_buffer.Clear();
        ClearLastError(handle);
        return YMIR_RESULT_OK;
    } catch (const std::exception &ex) {
        SetLastError(handle, YMIR_LOG_LEVEL_ERROR, fmt::format("Exception while resetting: {}", ex.what()));
        return YMIR_RESULT_INTERNAL_ERROR;
    }
}

ymir_result_t ymir_set_control_pad_buttons(ymir_handle_t *handle, uint32_t port, uint16_t pressed_buttons) {
    if (handle == nullptr) {
        return YMIR_RESULT_INVALID_ARGUMENT;
    }
    if (port < 1 || port > 2) {
        return YMIR_RESULT_INVALID_ARGUMENT;
    }

    const size_t port_index = port - 1;
    const uint16 pressed_mask = static_cast<uint16>(pressed_buttons) & kControlPadButtonMask;
    const uint16 release_mask = static_cast<uint16>(kControlPadButtonMask &
                                                    static_cast<uint16>(~pressed_mask));
    handle->control_pad_release_mask[port_index].store(release_mask, std::memory_order_relaxed);
    return YMIR_RESULT_OK;
}

void ymir_get_audio_info(ymir_handle_t *handle, ymir_audio_info_t *out_info) {
    // API returns audio stream description (format defined by core)
    // Caller uses info to configure audio output pipeline 

    // not used for now -> possible future per handle audio config
    // cast to void silencing 'unused param' warnings
    static_cast<void>(handle);

    // early return on no audio info provided
    if (out_info == nullptr) {
        return;
    }

    // init to known safe state first
    // avoids partially filled dater later should we early-out
    *out_info = ymir_audio_info_t{};

    // set core defaults for 
    out_info->sample_rate = static_cast<uint32_t>(ymir::scsp::kAudioFreq);
    out_info->channels = 2u;                    // Stereo out
    out_info->format = YMIR_AUDIO_FORMAT_S16;   // Signed 16-bit PCM
}

void ymir_get_audio_buffer_state(ymir_handle_t *handle, ymir_audio_buffer_state_t *out_state) {
    if (out_state == nullptr) {
        return;
    }

    *out_state = ymir_audio_buffer_state_t{};
    out_state->capacity_frames = kAudioBufferFrames - 1u;

    if (handle == nullptr) {
        return;
    }

    auto &buffer = handle->audio_buffer;
    const uint32 write = buffer.write_index.load(std::memory_order_acquire);
    const uint32 read = buffer.read_index.load(std::memory_order_relaxed);
    out_state->queued_frames = (write - read) & kAudioBufferMask;
}

size_t ymir_read_audio_samples(ymir_handle_t *handle, int16_t *out_samples, size_t max_frames) {
    if (handle == nullptr || out_samples == nullptr || max_frames == 0) {
        return 0;
    }
    auto &buffer = handle->audio_buffer;
    const uint32 write = buffer.write_index.load(std::memory_order_acquire);
    uint32 read = buffer.read_index.load(std::memory_order_relaxed);
    const uint32 available = (write - read) & kAudioBufferMask;
    const uint32 frames_to_read = std::min<uint32>(static_cast<uint32>(max_frames), available);
    for (uint32 i = 0; i < frames_to_read; ++i) {
        const size_t base = static_cast<size_t>(read) * 2u;
        out_samples[i * 2u] = buffer.samples[base];
        out_samples[i * 2u + 1u] = buffer.samples[base + 1u];
        read = (read + 1u) & kAudioBufferMask;
    }
    buffer.read_index.store(read, std::memory_order_release);
    return frames_to_read;
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

ymir_result_t ymir_copy_framebuffer(ymir_handle_t *handle, void *out_buffer, size_t buffer_size,
                                    ymir_framebuffer_info_t *out_info, uint64_t *out_frame_id) {
    if (handle == nullptr || out_buffer == nullptr) {
        return YMIR_RESULT_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(handle->framebuffer_mutex);
    if (!handle->framebuffer_ready) {
        return YMIR_RESULT_NOT_READY;
    }

    const size_t pixels = static_cast<size_t>(handle->framebuffer_width) *
                          static_cast<size_t>(handle->framebuffer_height);
    const size_t required_bytes = pixels * sizeof(uint32);
    if (buffer_size < required_bytes) {
        return YMIR_RESULT_SIZE_MISMATCH;
    }

    std::memcpy(out_buffer, handle->framebuffer.data(), required_bytes);
    if (out_info != nullptr) {
        out_info->width = handle->framebuffer_width;
        out_info->height = handle->framebuffer_height;
        out_info->stride_bytes = handle->framebuffer_width * sizeof(uint32);
        out_info->format = YMIR_FRAMEBUFFER_FORMAT_XBGR8888;
    }
    if (out_frame_id != nullptr) {
        *out_frame_id = handle->framebuffer_frame_id;
    }
    return YMIR_RESULT_OK;
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
