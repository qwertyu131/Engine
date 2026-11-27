#pragma once

// Console variables (spec §8). A cvar has a typed value, a default, optional min/max, help text,
// flags and the source that last set it. A set from a source with lower priority than the current
// one is rejected, so e.g. an ini file loaded after a console command cannot undo it.
//
// Threading: registration, lookup and listener management are thread-safe. Setting values and
// reading them concurrently from several threads is not; mutate cvars from one thread.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace eng {

// Priority order: a higher value wins. Equal priority overrides too (last writer wins).
enum class CVarSource : uint8_t {
    Default = 0,
    Scalability = 1,
    PlatformIni = 2,
    ProjectIni = 3,
    UserIni = 4,
    CommandLine = 5,
    Console = 6,
    Code = 7,
};

enum class CVarFlags : uint32_t {
    None = 0,
    ReadOnly = 1,                // only Default and Code may set it
    RequiresRestart = 2,         // value takes effect on next start
    RequiresPipelineRebuild = 4, // capability resolver must rebuild pipelines
    Cheat = 8,
};

constexpr CVarFlags operator|(CVarFlags a, CVarFlags b) {
    return static_cast<CVarFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr CVarFlags operator&(CVarFlags a, CVarFlags b) {
    return static_cast<CVarFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr CVarFlags operator~(CVarFlags a) { return static_cast<CVarFlags>(~static_cast<uint32_t>(a)); }
constexpr CVarFlags& operator|=(CVarFlags& a, CVarFlags b) { return a = a | b; }
constexpr CVarFlags& operator&=(CVarFlags& a, CVarFlags b) { return a = a & b; }
constexpr bool hasFlag(CVarFlags set, CVarFlags flag) { return (set & flag) == flag && flag != CVarFlags::None; }

using CVarValue = std::variant<bool, int32_t, float, std::string>;

std::string_view cvarSourceName(CVarSource source);

class CVarRegistry;

class CVar {
public:
    CVar(const CVar&) = delete;
    CVar& operator=(const CVar&) = delete;

    [[nodiscard]] std::string_view name() const { return m_name; }
    [[nodiscard]] std::string_view help() const { return m_help; }
    [[nodiscard]] CVarFlags flags() const { return m_flags; }
    [[nodiscard]] CVarSource source() const { return m_source; }
    [[nodiscard]] const CVarValue& value() const { return m_value; }
    [[nodiscard]] const CVarValue& defaultValue() const { return m_default; }

    // Typed getters return the value if the cvar has that type, otherwise false / 0 / 0.0f / "".
    [[nodiscard]] bool getBool() const;
    [[nodiscard]] int32_t getInt() const;
    [[nodiscard]] float getFloat() const;
    [[nodiscard]] const std::string& getString() const;

    // Returns true if the value was accepted (possibly clamped to min/max, possibly unchanged).
    // Returns false (value and source unchanged, warning logged) if the variant type does not match,
    // the source has lower priority than the current one, the cvar is ReadOnly and the source is
    // neither Default nor Code, or a float is not finite. Listeners fire only on an actual change.
    bool set(CVarValue v, CVarSource source);

private:
    friend class CVarRegistry;
    CVar(CVarRegistry& owner, std::string name, std::string help, CVarFlags flags, CVarValue def,
         std::optional<CVarValue> min, std::optional<CVarValue> max);

    CVarRegistry& m_owner;
    std::string m_name;
    std::string m_help;
    CVarFlags m_flags;
    CVarValue m_value;
    CVarValue m_default;
    std::optional<CVarValue> m_min;
    std::optional<CVarValue> m_max;
    CVarSource m_source = CVarSource::Default;
};

class CVarRegistry {
public:
    using Listener = std::function<void(const CVar&)>;

    // Process-wide registry. Separate instances may be constructed (e.g. for tests).
    static CVarRegistry& get();

    CVarRegistry();
    ~CVarRegistry();
    CVarRegistry(const CVarRegistry&) = delete;
    CVarRegistry& operator=(const CVarRegistry&) = delete;

    // Names are case-insensitive (ASCII folding only, locale independent); original case is kept.
    // Registering an existing name with the same type returns the existing cvar and logs a warning.
    // With a different type it logs an error and returns nullptr.
    // A default outside [min, max] is clamped (warning).
    CVar* registerBool(std::string_view name, bool def, std::string_view help,
                       CVarFlags flags = CVarFlags::None);
    CVar* registerInt(std::string_view name, int32_t def, std::string_view help,
                      CVarFlags flags = CVarFlags::None, std::optional<int32_t> min = std::nullopt,
                      std::optional<int32_t> max = std::nullopt);
    CVar* registerFloat(std::string_view name, float def, std::string_view help,
                        CVarFlags flags = CVarFlags::None, std::optional<float> min = std::nullopt,
                        std::optional<float> max = std::nullopt);
    CVar* registerString(std::string_view name, std::string_view def, std::string_view help,
                         CVarFlags flags = CVarFlags::None);

    [[nodiscard]] CVar* find(std::string_view name) const;

    // Parses `value` for the cvar's type, then applies CVar::set rules.
    // Parsing (surrounding ASCII blanks trimmed, the whole remaining text must be consumed):
    //   bool:   0/1/true/false/on/off, case-insensitive
    //   int:    base-10 int32, optional leading '+'; out of int32 range is rejected
    //   float:  locale-independent ('.' decimal); nan/inf/overflow rejected
    //   string: taken verbatim (no trimming)
    // Returns false if the cvar does not exist or the value is rejected.
    bool set(std::string_view name, std::string_view value, CVarSource source);

    // Visits every cvar in case-insensitive name order.
    void forEach(const std::function<void(const CVar&)>& fn) const;

    // Returns a listener id (> 0). An empty listener asserts (Debug) and returns 0.
    uint64_t onChanged(Listener listener);
    void removeListener(uint64_t id);

private:
    friend class CVar;
    CVar* registerImpl(std::string_view name, CVarValue def, std::string_view help, CVarFlags flags,
                       std::optional<CVarValue> min, std::optional<CVarValue> max);
    void notifyChanged(const CVar& cvar);

    mutable std::mutex m_mutex;
    std::map<std::string, std::unique_ptr<CVar>> m_cvars; // key: ASCII-lowercased name
    std::vector<std::pair<uint64_t, std::shared_ptr<Listener>>> m_listeners;
    uint64_t m_nextListenerId = 1;
};

} // namespace eng
