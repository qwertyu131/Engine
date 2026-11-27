#include <doctest/doctest.h>

#include "core/CVar.h"
#include "core/Log.h"

#include <clocale>
#include <cmath>
#include <ostream> // doctest stringifies std::string_view via operator<<
#include <string>
#include <utility>
#include <vector>

using eng::CVar;
using eng::CVarFlags;
using eng::CVarRegistry;
using eng::CVarSource;

namespace {

// Captures log output for the lifetime of the object, then restores the default sink and level.
struct LogCapture {
    std::vector<std::pair<eng::log::Level, std::string>> lines;
    eng::log::Level previous = eng::log::level();

    LogCapture() {
        eng::log::setLevel(eng::log::Level::Trace);
        eng::log::setSink([this](eng::log::Level lvl, std::string_view msg) {
            lines.emplace_back(lvl, std::string(msg));
        });
    }
    ~LogCapture() {
        eng::log::setSink(nullptr);
        eng::log::setLevel(previous);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    [[nodiscard]] size_t warnings() const {
        size_t n = 0;
        for (const auto& [lvl, text] : lines) {
            if (lvl == eng::log::Level::Warn) {
                ++n;
            }
        }
        return n;
    }
};

} // namespace

TEST_SUITE("cvar") {
    TEST_CASE("register and read default") {
        CVarRegistry reg;
        CVar* a = reg.registerInt("r.Test.A", 3, "test int");
        REQUIRE(a != nullptr);
        CHECK(a->getInt() == 3);
        CHECK(a->source() == CVarSource::Default);
        CHECK(a->name() == "r.Test.A");
        CHECK(a->help() == "test int");
        CHECK(a->flags() == CVarFlags::None);
        CHECK(std::holds_alternative<int32_t>(a->value()));
    }

    TEST_CASE("lower priority source cannot override higher") {
        LogCapture log;
        CVarRegistry reg;
        CVar* a = reg.registerInt("r.Test.Prio", 3, "");
        CHECK(reg.set("r.Test.Prio", "5", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.Prio", "7", CVarSource::ProjectIni));
        CHECK(a->getInt() == 5);
        CHECK(a->source() == CVarSource::Console);
        CHECK(log.warnings() >= 1);
    }

    TEST_CASE("equal or higher priority overrides") {
        CVarRegistry reg;
        CVar* a = reg.registerInt("r.Test.Higher", 3, "");
        CHECK(reg.set("r.Test.Higher", "5", CVarSource::Code));
        CHECK(reg.set("r.Test.Higher", "6", CVarSource::Code));
        CHECK(a->getInt() == 6);
        CHECK(reg.set("r.Test.Higher", "9", CVarSource::Console));
        CHECK(a->getInt() == 9);
        CHECK(a->source() == CVarSource::Console);
    }

    TEST_CASE("console beats code (Unreal order)") {
        CVarRegistry reg;
        CVar* a = reg.registerInt("r.Test.ConsoleWins", 3, "");
        CHECK(reg.set("r.Test.ConsoleWins", "5", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.ConsoleWins", "9", CVarSource::Code));
        CHECK(a->getInt() == 5);
        CHECK(a->source() == CVarSource::Console);
    }

    TEST_CASE("int clamp to min/max") {
        LogCapture log;
        CVarRegistry reg;
        CVar* a = reg.registerInt("r.Test.Clamp", 1, "", CVarFlags::None, 0, 4);
        CHECK(reg.set("r.Test.Clamp", "10", CVarSource::Console));
        CHECK(a->getInt() == 4);
        CHECK(log.warnings() == 1);
        CHECK(reg.set("r.Test.Clamp", "-3", CVarSource::Console));
        CHECK(a->getInt() == 0);
    }

    TEST_CASE("float clamp to min/max") {
        CVarRegistry reg;
        CVar* f = reg.registerFloat("r.Test.FClamp", 0.5f, "", CVarFlags::None, 0.0f, 1.0f);
        CHECK(reg.set("r.Test.FClamp", "2.5", CVarSource::Console));
        CHECK(f->getFloat() == 1.0f);
    }

    TEST_CASE("default outside range is clamped at registration") {
        LogCapture log;
        CVarRegistry reg;
        CVar* a = reg.registerInt("r.Test.BadDefault", 99, "", CVarFlags::None, 0, 4);
        CHECK(a->getInt() == 4);
        CHECK(log.warnings() == 1);
    }

    TEST_CASE("parse failure rejected") {
        LogCapture log;
        CVarRegistry reg;
        CVar* a = reg.registerInt("r.Test.Parse", 3, "");
        CHECK_FALSE(reg.set("r.Test.Parse", "abc", CVarSource::Console));
        CHECK(a->getInt() == 3);
        CHECK(a->source() == CVarSource::Default);
        CHECK_FALSE(reg.set("r.Test.Parse", "5x", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.Parse", "", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.Parse", "1.5", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.Parse", "99999999999", CVarSource::Console)); // > int32
        CHECK(a->getInt() == 3);
        CHECK(log.warnings() == 5);
        CHECK(reg.set("r.Test.Parse", " -12 ", CVarSource::Console)); // surrounding blanks trimmed
        CHECK(a->getInt() == -12);
        CHECK(reg.set("r.Test.Parse", "+7", CVarSource::Console));
        CHECK(a->getInt() == 7);
    }

    TEST_CASE("float parse is locale independent and rejects non-finite") {
        CVarRegistry reg;
        CVar* f = reg.registerFloat("r.Test.Float", 1.0f, "");

        // Turkish (and many other) locales use ',' as the decimal separator.
        const char* oldLocale = std::setlocale(LC_ALL, nullptr);
        const std::string saved = oldLocale ? oldLocale : "C";
        const bool switched = std::setlocale(LC_ALL, "Turkish") != nullptr ||
                              std::setlocale(LC_ALL, "tr_TR.UTF-8") != nullptr ||
                              std::setlocale(LC_ALL, "de_DE.UTF-8") != nullptr;
        CHECK(reg.set("r.Test.Float", "0.25", CVarSource::Console));
        CHECK(f->getFloat() == 0.25f);
        CHECK_FALSE(reg.set("r.Test.Float", "0,5", CVarSource::Console));
        std::setlocale(LC_ALL, saved.c_str());
        const std::string localeNote = std::string("non-C locale active during parse: ") + (switched ? "yes" : "no (none installed)");
        MESSAGE(localeNote);

        CHECK(reg.set("r.Test.Float", "1e-3", CVarSource::Console));
        CHECK(f->getFloat() == doctest::Approx(0.001f));
        CHECK(reg.set("r.Test.Float", "3", CVarSource::Console));
        CHECK(f->getFloat() == 3.0f);

        CHECK_FALSE(reg.set("r.Test.Float", "nan", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.Float", "inf", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.Float", "1e999", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.Float", "0.5f", CVarSource::Console));
        CHECK(f->getFloat() == 3.0f);
    }

    TEST_CASE("bool parse accepts 0/1/true/false/on/off case-insensitively") {
        CVarRegistry reg;
        CVar* b = reg.registerBool("r.Test.Bool", false, "");
        const std::pair<const char*, bool> ok[] = {
            {"1", true}, {"0", false}, {"true", true}, {"FALSE", false},
            {"On", true}, {"off", false}, {"TRUE", true}, {"oFf", false},
        };
        for (const auto& [text, expected] : ok) {
            CAPTURE(text);
            CHECK(reg.set("r.Test.Bool", text, CVarSource::Console));
            CHECK(b->getBool() == expected);
        }
        CHECK_FALSE(reg.set("r.Test.Bool", "yes", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.Bool", "2", CVarSource::Console));
        CHECK(b->getBool() == false);
    }

    TEST_CASE("string cvar stores raw value") {
        CVarRegistry reg;
        CVar* s = reg.registerString("r.Test.Str", "abc", "");
        CHECK(s->getString() == "abc");
        CHECK(reg.set("r.Test.Str", "C:\\Users\\Heros\\ç ğ ü", CVarSource::CommandLine));
        CHECK(s->getString() == "C:\\Users\\Heros\\ç ğ ü");
    }

    TEST_CASE("ReadOnly rejects non-Default/Code") {
        LogCapture log;
        CVarRegistry reg;
        CVar* r = reg.registerInt("r.Test.RO", 1, "", CVarFlags::ReadOnly);
        CHECK_FALSE(reg.set("r.Test.RO", "2", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.RO", "2", CVarSource::CommandLine));
        CHECK_FALSE(reg.set("r.Test.RO", "2", CVarSource::UserIni));
        CHECK(r->getInt() == 1);
        CHECK(log.warnings() == 3);
        CHECK(reg.set("r.Test.RO", "3", CVarSource::Code));
        CHECK(r->getInt() == 3);
    }

    TEST_CASE("case-insensitive lookup") {
        CVarRegistry reg;
        CVar* q = reg.registerInt("r.Shadow.Quality", 2, "");
        CHECK(reg.find("r.Shadow.Quality") == q);
        CHECK(reg.find("R.SHADOW.QUALITY") == q); // 'I' must fold to 'i' regardless of locale
        CHECK(reg.find("r.shadow.quality") == q);
        CHECK(reg.find("r.Shadow.Qualit") == nullptr);
        CHECK(q->name() == "r.Shadow.Quality");
        CHECK(reg.set("R.SHADOW.QUALITY", "3", CVarSource::Console));
        CHECK(q->getInt() == 3);
        CHECK_FALSE(reg.set("r.Does.Not.Exist", "1", CVarSource::Console));
    }

    TEST_CASE("onChanged fires once per effective change, not on no-op") {
        CVarRegistry reg;
        CVar* a = reg.registerInt("r.Test.Changed", 3, "");
        std::vector<std::pair<std::string, int32_t>> events;
        const uint64_t id = reg.onChanged([&](const CVar& c) {
            events.emplace_back(std::string(c.name()), c.getInt());
        });

        CHECK(reg.set("r.Test.Changed", "4", CVarSource::Code));
        REQUIRE(events.size() == 1);
        CHECK(events[0].first == "r.Test.Changed");
        CHECK(events[0].second == 4);

        // Same value from an equal or higher source: accepted, source upgraded, no event.
        CHECK(reg.set("r.Test.Changed", "4", CVarSource::Code));
        CHECK(reg.set("r.Test.Changed", "4", CVarSource::Console));
        CHECK(a->source() == CVarSource::Console);
        CHECK(events.size() == 1);

        // Rejected sets produce no event.
        CHECK_FALSE(reg.set("r.Test.Changed", "abc", CVarSource::Console));
        CHECK_FALSE(reg.set("r.Test.Changed", "8", CVarSource::Code));
        CHECK(events.size() == 1);

        reg.removeListener(id);
        CHECK(reg.set("r.Test.Changed", "5", CVarSource::Console));
        CHECK(events.size() == 1);
    }

    TEST_CASE("duplicate register returns existing and warns") {
        LogCapture log;
        CVarRegistry reg;
        CVar* first = reg.registerInt("r.Test.Dup", 1, "first");
        CVar* second = reg.registerInt("R.TEST.DUP", 2, "second");
        CHECK(second == first);
        CHECK(first->getInt() == 1);
        CHECK(first->help() == "first");
        CHECK(log.warnings() == 1);

        // Same name, different type: cannot return a usable cvar.
        CHECK(reg.registerFloat("r.Test.Dup", 1.0f, "") == nullptr);
    }

    TEST_CASE("typed CVar::set rejects wrong value type") {
        CVarRegistry reg;
        CVar* a = reg.registerInt("r.Test.Typed", 1, "");
        CHECK_FALSE(a->set(eng::CVarValue{1.5f}, CVarSource::Code));
        CHECK_FALSE(a->set(eng::CVarValue{std::string("2")}, CVarSource::Code));
        CHECK(a->getInt() == 1);
        CHECK(a->set(eng::CVarValue{int32_t{2}}, CVarSource::Code));
        CHECK(a->getInt() == 2);
    }

    TEST_CASE("typed CVar::set notifies registry listeners") {
        CVarRegistry reg;
        CVar* a = reg.registerBool("r.Test.TypedNotify", false, "");
        int calls = 0;
        reg.onChanged([&](const CVar&) { ++calls; });
        CHECK(a->set(eng::CVarValue{true}, CVarSource::Code));
        CHECK(a->set(eng::CVarValue{true}, CVarSource::Code));
        CHECK(calls == 1);
    }

    TEST_CASE("forEach visits every cvar in name order") {
        CVarRegistry reg;
        reg.registerInt("r.B", 0, "");
        reg.registerInt("r.a", 0, "");
        reg.registerInt("r.C", 0, "");
        std::vector<std::string> names;
        reg.forEach([&](const CVar& c) { names.emplace_back(c.name()); });
        CHECK(names == std::vector<std::string>{"r.a", "r.B", "r.C"});
    }

    TEST_CASE("flags bitmask operators") {
        constexpr CVarFlags f = CVarFlags::ReadOnly | CVarFlags::Cheat;
        CHECK(eng::hasFlag(f, CVarFlags::ReadOnly));
        CHECK(eng::hasFlag(f, CVarFlags::Cheat));
        CHECK_FALSE(eng::hasFlag(f, CVarFlags::RequiresRestart));
        CHECK((f & CVarFlags::Cheat) == CVarFlags::Cheat);
        CVarFlags g = CVarFlags::None;
        g |= CVarFlags::RequiresPipelineRebuild;
        CHECK(static_cast<uint32_t>(g) == 4u);
    }

    TEST_CASE("global registry is a singleton") {
        CHECK(&CVarRegistry::get() == &CVarRegistry::get());
        CVar* g = CVarRegistry::get().registerInt("test.Global.Singleton", 7, "");
        CHECK(CVarRegistry::get().find("TEST.GLOBAL.SINGLETON") == g);
    }
}
