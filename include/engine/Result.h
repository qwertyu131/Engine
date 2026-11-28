#pragma once
// Engine-wide error / result type (plan F3 Task 2). eng::rhi::Error and eng::rhi::Result are aliases of these, so
// engine/asset (no GPU types) and engine/rhi report errors the same way and the facade (Task 4) passes them on.
//
// exitCode per the F2 Global Constraints: 2 = usage / IO / malformed input, 3 = backend not available,
// 4 = validation error, 5 = device lost / GPU error.
//
// Needs C++23 (<expected>): only the C++23 targets (engine_rhi, engine_asset and their users) include it; the
// `engine` core library stays C++20.
#include <expected>
#include <string>

namespace eng {

struct Error {
    int exitCode = 2;
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

} // namespace eng
