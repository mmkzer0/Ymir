#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace ymir::debug {

class LineFramer {
public:
    static constexpr size_t kMaxLineLength = 1024 * 1024; // 1 MiB

    using LineCallback = std::function<void(std::string_view)>;
    using ErrorCallback = std::function<void(std::string_view)>;

    LineFramer(LineCallback onLine, ErrorCallback onError)
        : m_onLine(std::move(onLine))
        , m_onError(std::move(onError)) {}

    void Push(const char *data, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            const char c = data[i];
            if (c == '\n') {
                if (m_droppingOversizedLine) {
                    m_droppingOversizedLine = false;
                } else if (!m_buffer.empty()) {
                    m_onLine(m_buffer);
                }
                m_buffer.clear();
            } else if (c == '\r' || m_droppingOversizedLine) {
                continue;
            } else if (m_buffer.length() >= kMaxLineLength) {
                m_onError("Line length limit exceeded");
                m_buffer.clear();
                m_droppingOversizedLine = true;
            } else {
                m_buffer += c;
            }
        }
    }

private:
    std::string m_buffer;
    bool m_droppingOversizedLine{};
    LineCallback m_onLine;
    ErrorCallback m_onError;
};

} // namespace ymir::debug
