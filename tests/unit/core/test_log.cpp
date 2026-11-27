#include <doctest/doctest.h>

#include "core/Log.h"

#include <string>
#include <utility>
#include <vector>

TEST_SUITE("log") {
    TEST_CASE("log sink receives message at or above level") {
        std::vector<std::pair<eng::log::Level, std::string>> captured;
        const eng::log::Level previousLevel = eng::log::level();
        eng::log::setSink([&](eng::log::Level lvl, std::string_view msg) {
            captured.emplace_back(lvl, std::string(msg));
        });
        eng::log::setLevel(eng::log::Level::Warn);

        eng::log::write(eng::log::Level::Info, "ignored");
        eng::log::write(eng::log::Level::Error, "boom");

        eng::log::setSink(nullptr);
        eng::log::setLevel(previousLevel);

        REQUIRE(captured.size() == 1);
        CHECK(captured[0].first == eng::log::Level::Error);
        CHECK(captured[0].second == "boom");
    }

    TEST_CASE("log macros format arguments") {
        std::vector<std::string> captured;
        const eng::log::Level previousLevel = eng::log::level();
        eng::log::setSink([&](eng::log::Level, std::string_view msg) { captured.emplace_back(msg); });
        eng::log::setLevel(eng::log::Level::Trace);

        ENG_LOG_WARN("value={} name={}", 42, "x");

        eng::log::setSink(nullptr);
        eng::log::setLevel(previousLevel);

        REQUIRE(captured.size() == 1);
        CHECK(captured[0] == "value=42 name=x");
    }
}
