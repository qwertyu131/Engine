#pragma once

#include <format>
#include <functional>
#include <string_view>

namespace eng::log {

enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

// Messages below this level are dropped. Default: Info.
void setLevel(Level lvl);
Level level();

// Replaces the output sink. Passing an empty function restores the default sink (stderr).
void setSink(std::function<void(Level, std::string_view)> sink);

void write(Level lvl, std::string_view msg);

std::string_view levelName(Level lvl);

} // namespace eng::log

// std::format based; the level check happens before formatting.
#define ENG_LOG_AT(lvl, ...)                                                                       \
    do {                                                                                           \
        if (static_cast<int>(lvl) >= static_cast<int>(::eng::log::level())) {                     \
            ::eng::log::write((lvl), ::std::format(__VA_ARGS__));                                  \
        }                                                                                          \
    } while (0)

#define ENG_LOG_TRACE(...) ENG_LOG_AT(::eng::log::Level::Trace, __VA_ARGS__)
#define ENG_LOG_DEBUG(...) ENG_LOG_AT(::eng::log::Level::Debug, __VA_ARGS__)
#define ENG_LOG_INFO(...) ENG_LOG_AT(::eng::log::Level::Info, __VA_ARGS__)
#define ENG_LOG_WARN(...) ENG_LOG_AT(::eng::log::Level::Warn, __VA_ARGS__)
#define ENG_LOG_ERROR(...) ENG_LOG_AT(::eng::log::Level::Error, __VA_ARGS__)
#define ENG_LOG_FATAL(...) ENG_LOG_AT(::eng::log::Level::Fatal, __VA_ARGS__)
