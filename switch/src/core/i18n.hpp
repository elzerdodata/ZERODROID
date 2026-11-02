#pragma once

#include <atomic>

namespace gnx::i18n {

inline std::atomic<bool>& english_flag() {
    static std::atomic<bool> flag{false};
    return flag;
}

inline void set_english(bool english) {
    english_flag().store(english, std::memory_order_relaxed);
}

inline bool english() {
    return english_flag().load(std::memory_order_relaxed);
}

// Prefer this over hard-coded UI strings in shared/core code.
inline const char* tr(const char* spanish, const char* english_text) {
    return english() ? english_text : spanish;
}

}  // namespace gnx::i18n
