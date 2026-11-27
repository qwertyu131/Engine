#pragma once

#include "core/Log.h"

#include <cstdlib>
#include <format>
#include <string_view>

#ifndef ENG_ENABLE_ASSERTS
#define ENG_ENABLE_ASSERTS 0
#endif

namespace eng::detail {
[[noreturn]] inline void assertFail(const char* expr, std::string_view msg, const char* file, int line) {
    ::eng::log::write(::eng::log::Level::Fatal,
                      ::std::format("assertion failed: {} ({}) at {}:{}", expr, msg, file, line));
    std::abort();
}
} // namespace eng::detail

#if ENG_ENABLE_ASSERTS
#define ENG_ASSERT_MSG(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ::eng::detail::assertFail(#cond, (msg), __FILE__, __LINE__);                           \
        }                                                                                          \
    } while (0)
#else
// Not evaluated, but still type-checked.
#define ENG_ASSERT_MSG(cond, msg)                                                                  \
    do {                                                                                           \
        (void)sizeof(cond);                                                                        \
    } while (0)
#endif

#define ENG_ASSERT(cond) ENG_ASSERT_MSG(cond, "")
