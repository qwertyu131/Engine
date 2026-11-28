#pragma once
// Facade settings (spec K-D / §8, plan F3 Task 4): access to the engine's cvar registry (engine/CVar.h). The host sets
// cvars with a source (priority order Default < ... < CommandLine < Code < Console, spec §8): a viewer's console uses
// CVarSource::Console, command-line options CVarSource::CommandLine, the host's code CVarSource::Code.
#include <engine/CVar.h>

#include <functional>
#include <string_view>

namespace eng {

class Settings {
public:
    // Sets a cvar from text. False: unknown name or a value its type / range rejects.
    bool set(std::string_view name, std::string_view value, CVarSource source = CVarSource::Code) {
        return CVarRegistry::get().set(name, value, source);
    }
    CVar* find(std::string_view name) const { return CVarRegistry::get().find(name); }
    void forEach(const std::function<void(const CVar&)>& fn) const { CVarRegistry::get().forEach(fn); }
    // Host cvars (e.g. the viewer's r.VSync): registered once; a second registration returns the existing cvar.
    CVarRegistry& registry() { return CVarRegistry::get(); }
};

} // namespace eng
