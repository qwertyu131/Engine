#include "core/CVar.h"

#include "core/Assert.h"
#include "core/Log.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>

#if !defined(__cpp_lib_to_chars) || __cpp_lib_to_chars < 201611L
#include <locale>
#include <sstream>
#define ENG_CVAR_FLOAT_FROM_STREAM 1
#endif

namespace eng {
namespace {

// ASCII-only folding: std::tolower is locale dependent (Turkish 'I' -> dotless i).
constexpr char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string foldKey(std::string_view name) {
    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(), asciiLower);
    return key;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

constexpr bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && isBlank(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isBlank(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<bool> parseBool(std::string_view s) {
    s = trim(s);
    if (s == "1" || equalsIgnoreCase(s, "true") || equalsIgnoreCase(s, "on")) {
        return true;
    }
    if (s == "0" || equalsIgnoreCase(s, "false") || equalsIgnoreCase(s, "off")) {
        return false;
    }
    return std::nullopt;
}

std::optional<int32_t> parseInt(std::string_view s) {
    s = trim(s);
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
        if (!s.empty() && s.front() == '-') {
            return std::nullopt;
        }
    }
    if (s.empty()) {
        return std::nullopt;
    }
    int32_t v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 10);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

std::optional<float> parseFloat(std::string_view s) {
    s = trim(s);
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
        if (!s.empty() && s.front() == '-') {
            return std::nullopt;
        }
    }
    if (s.empty()) {
        return std::nullopt;
    }
    float v = 0.0f;
#if ENG_CVAR_FLOAT_FROM_STREAM
    // Fallback for standard libraries without floating-point from_chars. The classic locale keeps
    // the parse independent of the global locale.
    std::istringstream in{std::string(s)};
    in.imbue(std::locale::classic());
    in >> std::noskipws >> v;
    if (in.fail() || in.peek() != std::char_traits<char>::eof()) {
        return std::nullopt;
    }
#else
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, std::chars_format::general);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
#endif
    if (!std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

std::string valueToString(const CVarValue& v) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, bool>) {
                return x ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::string>) {
                return x;
            } else {
                return std::format("{}", x);
            }
        },
        v);
}

std::string_view typeName(const CVarValue& v) {
    switch (v.index()) {
    case 0: return "bool";
    case 1: return "int";
    case 2: return "float";
    case 3: return "string";
    default: return "?";
    }
}

// Clamps numeric values into [min, max]. Returns true if the value was changed.
bool clampValue(CVarValue& v, const std::optional<CVarValue>& min, const std::optional<CVarValue>& max) {
    bool clamped = false;
    auto apply = [&](auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, float>) {
            if (min) {
                if (const T* lo = std::get_if<T>(&*min); lo && x < *lo) {
                    x = *lo;
                    clamped = true;
                }
            }
            if (max) {
                if (const T* hi = std::get_if<T>(&*max); hi && x > *hi) {
                    x = *hi;
                    clamped = true;
                }
            }
        }
    };
    std::visit(apply, v);
    return clamped;
}

} // namespace

std::string_view cvarSourceName(CVarSource source) {
    switch (source) {
    case CVarSource::Default: return "Default";
    case CVarSource::Scalability: return "Scalability";
    case CVarSource::PlatformIni: return "PlatformIni";
    case CVarSource::ProjectIni: return "ProjectIni";
    case CVarSource::UserIni: return "UserIni";
    case CVarSource::CommandLine: return "CommandLine";
    case CVarSource::Console: return "Console";
    case CVarSource::Code: return "Code";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------- CVar

CVar::CVar(CVarRegistry& owner, std::string name, std::string help, CVarFlags flags, CVarValue def,
           std::optional<CVarValue> min, std::optional<CVarValue> max)
    : m_owner(owner),
      m_name(std::move(name)),
      m_help(std::move(help)),
      m_flags(flags),
      m_value(def),
      m_default(std::move(def)),
      m_min(std::move(min)),
      m_max(std::move(max)) {}

bool CVar::getBool() const {
    const bool* v = std::get_if<bool>(&m_value);
    return v ? *v : false;
}

int32_t CVar::getInt() const {
    const int32_t* v = std::get_if<int32_t>(&m_value);
    return v ? *v : 0;
}

float CVar::getFloat() const {
    const float* v = std::get_if<float>(&m_value);
    return v ? *v : 0.0f;
}

const std::string& CVar::getString() const {
    static const std::string empty;
    const std::string* v = std::get_if<std::string>(&m_value);
    return v ? *v : empty;
}

bool CVar::set(CVarValue v, CVarSource source) {
    if (v.index() != m_value.index()) {
        ENG_LOG_WARN("cvar '{}': rejected {} value from {} (cvar type is {})", m_name, typeName(v),
                     cvarSourceName(source), typeName(m_value));
        return false;
    }
    if (static_cast<uint8_t>(source) < static_cast<uint8_t>(m_source)) {
        ENG_LOG_WARN("cvar '{}': rejected '{}' from {} (lower priority than current source {})", m_name,
                     valueToString(v), cvarSourceName(source), cvarSourceName(m_source));
        return false;
    }
    if (hasFlag(m_flags, CVarFlags::ReadOnly) && source != CVarSource::Default && source != CVarSource::Code) {
        ENG_LOG_WARN("cvar '{}': rejected '{}' from {} (read-only)", m_name, valueToString(v),
                     cvarSourceName(source));
        return false;
    }
    if (const float* f = std::get_if<float>(&v); f && !std::isfinite(*f)) {
        ENG_LOG_WARN("cvar '{}': rejected non-finite value from {}", m_name, cvarSourceName(source));
        return false;
    }
    if (clampValue(v, m_min, m_max)) {
        ENG_LOG_WARN("cvar '{}': value from {} clamped to {}", m_name, cvarSourceName(source), valueToString(v));
    }

    m_source = source;
    if (v == m_value) {
        return true;
    }
    m_value = std::move(v);
    m_owner.notifyChanged(*this);
    return true;
}

// -------------------------------------------------------------------------------------- CVarRegistry

CVarRegistry& CVarRegistry::get() {
    static CVarRegistry instance;
    return instance;
}

CVarRegistry::CVarRegistry() = default;
CVarRegistry::~CVarRegistry() = default;

CVar* CVarRegistry::registerImpl(std::string_view name, CVarValue def, std::string_view help, CVarFlags flags,
                                 std::optional<CVarValue> min, std::optional<CVarValue> max) {
    std::string key = foldKey(name);
    std::lock_guard lock(m_mutex);
    if (const auto it = m_cvars.find(key); it != m_cvars.end()) {
        CVar* existing = it->second.get();
        if (existing->value().index() != def.index()) {
            ENG_LOG_ERROR("cvar '{}': already registered as {} (as '{}'), cannot register as {}", name,
                          typeName(existing->value()), existing->name(), typeName(def));
            return nullptr;
        }
        ENG_LOG_WARN("cvar '{}': already registered (as '{}'); returning existing", name, existing->name());
        return existing;
    }
    if (clampValue(def, min, max)) {
        ENG_LOG_WARN("cvar '{}': default clamped to {}", name, valueToString(def));
    }
    auto cvar = std::unique_ptr<CVar>(
        new CVar(*this, std::string(name), std::string(help), flags, std::move(def), std::move(min), std::move(max)));
    CVar* raw = cvar.get();
    m_cvars.emplace(std::move(key), std::move(cvar));
    return raw;
}

CVar* CVarRegistry::registerBool(std::string_view name, bool def, std::string_view help, CVarFlags flags) {
    return registerImpl(name, CVarValue{def}, help, flags, std::nullopt, std::nullopt);
}

CVar* CVarRegistry::registerInt(std::string_view name, int32_t def, std::string_view help, CVarFlags flags,
                                std::optional<int32_t> min, std::optional<int32_t> max) {
    std::optional<CVarValue> lo;
    std::optional<CVarValue> hi;
    if (min) {
        lo = CVarValue{*min};
    }
    if (max) {
        hi = CVarValue{*max};
    }
    return registerImpl(name, CVarValue{def}, help, flags, std::move(lo), std::move(hi));
}

CVar* CVarRegistry::registerFloat(std::string_view name, float def, std::string_view help, CVarFlags flags,
                                  std::optional<float> min, std::optional<float> max) {
    std::optional<CVarValue> lo;
    std::optional<CVarValue> hi;
    if (min) {
        lo = CVarValue{*min};
    }
    if (max) {
        hi = CVarValue{*max};
    }
    return registerImpl(name, CVarValue{def}, help, flags, std::move(lo), std::move(hi));
}

CVar* CVarRegistry::registerString(std::string_view name, std::string_view def, std::string_view help,
                                   CVarFlags flags) {
    return registerImpl(name, CVarValue{std::string(def)}, help, flags, std::nullopt, std::nullopt);
}

CVar* CVarRegistry::find(std::string_view name) const {
    const std::string key = foldKey(name);
    std::lock_guard lock(m_mutex);
    const auto it = m_cvars.find(key);
    return it == m_cvars.end() ? nullptr : it->second.get();
}

bool CVarRegistry::set(std::string_view name, std::string_view value, CVarSource source) {
    CVar* cvar = find(name);
    if (!cvar) {
        ENG_LOG_WARN("cvar '{}': not registered (set '{}' from {} ignored)", name, value, cvarSourceName(source));
        return false;
    }
    std::optional<CVarValue> parsed;
    switch (cvar->value().index()) {
    case 0:
        if (auto b = parseBool(value)) {
            parsed = CVarValue{*b};
        }
        break;
    case 1:
        if (auto i = parseInt(value)) {
            parsed = CVarValue{*i};
        }
        break;
    case 2:
        if (auto f = parseFloat(value)) {
            parsed = CVarValue{*f};
        }
        break;
    case 3: parsed = CVarValue{std::string(value)}; break;
    default: break;
    }
    if (!parsed) {
        ENG_LOG_WARN("cvar '{}': cannot parse '{}' from {} as {}", cvar->name(), value, cvarSourceName(source),
                     typeName(cvar->value()));
        return false;
    }
    return cvar->set(std::move(*parsed), source);
}

void CVarRegistry::forEach(const std::function<void(const CVar&)>& fn) const {
    std::vector<const CVar*> snapshot;
    {
        std::lock_guard lock(m_mutex);
        snapshot.reserve(m_cvars.size());
        for (const auto& [key, cvar] : m_cvars) {
            snapshot.push_back(cvar.get());
        }
    }
    for (const CVar* c : snapshot) {
        fn(*c);
    }
}

uint64_t CVarRegistry::onChanged(Listener listener) {
    ENG_ASSERT_MSG(static_cast<bool>(listener), "onChanged: empty listener");
    if (!listener) {
        return 0; // never a valid id; removeListener(0) is a no-op
    }
    std::lock_guard lock(m_mutex);
    const uint64_t id = m_nextListenerId++;
    m_listeners.emplace_back(id, std::make_shared<Listener>(std::move(listener)));
    return id;
}

void CVarRegistry::removeListener(uint64_t id) {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_listeners, [id](const auto& entry) { return entry.first == id; });
}

void CVarRegistry::notifyChanged(const CVar& cvar) {
    // Call listeners outside the lock so they may use the registry (find, set, removeListener).
    std::vector<std::shared_ptr<Listener>> listeners;
    {
        std::lock_guard lock(m_mutex);
        listeners.reserve(m_listeners.size());
        for (const auto& [id, l] : m_listeners) {
            listeners.push_back(l);
        }
    }
    for (const auto& l : listeners) {
        (*l)(cvar);
    }
}

} // namespace eng
