#pragma once
#include <cstdint>
#include <functional>

namespace ecs {

struct Entity {
    uint32_t index = 0;
    uint32_t generation = 0;

    bool operator==(const Entity& o) const {
        return index == o.index && generation == o.generation;
    }
    bool operator!=(const Entity& o) const { return !(*this == o); }
};

inline constexpr Entity INVALID_ENTITY{0, 0};

struct EntityHash {
    size_t operator()(const Entity& e) const {
        return std::hash<uint64_t>{}(static_cast<uint64_t>(e.generation) << 32 | e.index);
    }
};

} // namespace ecs
