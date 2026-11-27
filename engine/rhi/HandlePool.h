#pragma once
// Generational handle pool shared by the backends (plan F2 Task 2).
// id = (generation << 20) | (slot + 1); 0 is the null handle. get() of a stale or null handle asserts
// (ENG_ASSERT: Debug builds) and returns nullptr otherwise.
#include "core/Assert.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace eng::rhi {

template <class T>
class HandlePool {
public:
    static constexpr uint32_t kSlotBits = 20;
    static constexpr uint32_t kSlotMask = (1u << kSlotBits) - 1u;
    static constexpr uint32_t kGenerationMask = (1u << (32u - kSlotBits)) - 1u;

    uint32_t add(T value) {
        uint32_t slot;
        if (!free_.empty()) {
            slot = free_.back();
            free_.pop_back();
        } else {
            slot = static_cast<uint32_t>(slots_.size());
            ENG_ASSERT_MSG(slot < kSlotMask, "handle pool full");
            slots_.push_back({});
        }
        Slot& s = slots_[slot];
        s.value.emplace(std::move(value));
        return (s.generation << kSlotBits) | (slot + 1u);
    }

    T* get(uint32_t id) {
        Slot* s = slotOf(id);
        ENG_ASSERT_MSG(s != nullptr, "stale or null RHI handle");
        return s ? &*s->value : nullptr;
    }
    const T* get(uint32_t id) const { return const_cast<HandlePool*>(this)->get(id); }

    // Like get(), without asserting: for checks that report an error instead.
    T* tryGet(uint32_t id) {
        Slot* s = slotOf(id);
        return s ? &*s->value : nullptr;
    }

    // Moves the value out and frees the slot (the generation advances, so old ids become stale).
    std::optional<T> remove(uint32_t id) {
        Slot* s = slotOf(id);
        ENG_ASSERT_MSG(s != nullptr, "destroy of a stale or null RHI handle");
        if (!s) {
            return std::nullopt;
        }
        std::optional<T> out = std::move(s->value);
        s->value.reset();
        s->generation = (s->generation + 1u) & kGenerationMask;
        free_.push_back((id & kSlotMask) - 1u);
        return out;
    }

    template <class F>
    void forEach(F&& f) {
        for (Slot& s : slots_) {
            if (s.value) {
                f(*s.value);
            }
        }
    }

    // Removes every value (device shutdown), calling f on each.
    template <class F>
    void clear(F&& f) {
        for (Slot& s : slots_) {
            if (s.value) {
                f(*s.value);
                s.value.reset();
            }
        }
        slots_.clear();
        free_.clear();
    }

private:
    struct Slot {
        std::optional<T> value;
        uint32_t generation = 0;
    };

    Slot* slotOf(uint32_t id) {
        const uint32_t slot = (id & kSlotMask);
        if (slot == 0 || slot > slots_.size()) {
            return nullptr;
        }
        Slot& s = slots_[slot - 1u];
        if (!s.value || s.generation != (id >> kSlotBits)) {
            return nullptr;
        }
        return &s;
    }

    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
};

} // namespace eng::rhi
