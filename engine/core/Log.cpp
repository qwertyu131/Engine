#include "core/Log.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

namespace eng::log {
namespace {

std::atomic<Level> g_level{Level::Info};

std::mutex& sinkMutex() {
    static std::mutex m;
    return m;
}

std::function<void(Level, std::string_view)>& sinkRef() {
    static std::function<void(Level, std::string_view)> sink;
    return sink;
}

void defaultSink(Level lvl, std::string_view msg) {
    const std::string_view name = levelName(lvl);
    std::fprintf(stderr, "[%.*s] %.*s\n", static_cast<int>(name.size()), name.data(),
                 static_cast<int>(msg.size()), msg.data());
    std::fflush(stderr);
}

} // namespace

void setLevel(Level lvl) { g_level.store(lvl, std::memory_order_relaxed); }

Level level() { return g_level.load(std::memory_order_relaxed); }

void setSink(std::function<void(Level, std::string_view)> sink) {
    std::lock_guard lock(sinkMutex());
    sinkRef() = std::move(sink);
}

void write(Level lvl, std::string_view msg) {
    if (static_cast<int>(lvl) < static_cast<int>(level())) {
        return;
    }
    std::lock_guard lock(sinkMutex());
    if (sinkRef()) {
        sinkRef()(lvl, msg);
    } else {
        defaultSink(lvl, msg);
    }
}

std::string_view levelName(Level lvl) {
    switch (lvl) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Fatal: return "fatal";
    }
    return "?";
}

} // namespace eng::log
