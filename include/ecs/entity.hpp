#pragma once
#include <cstdint>
#include <functional>

namespace ecs {

/**
 * @brief Represents a unique object within the ECS world.
 *
 * @details An Entity is a lightweight identifier composed of an index and a generation count.
 * It serves as a key to associate components in the ECS world. The generation count is used
 * to detect and handle invalid (destroyed) entities safely, preventing ABA problems when
 * entity indices are recycled.
 */
struct Entity {
    /**
     * @brief The unique index of the entity in the global entity array.
     */
    uint32_t index = 0;

    /**
     * @brief The generation version of the entity index.
     * @details Incremented whenever an entity index is reused to distinguish between the old and new entity.
     */
    uint32_t generation = 0;

    /**
     * @brief Checks if two Entity handles refer to the exact same entity.
     * @param o The other entity to compare.
     * @return true if both index and generation match.
     */
    bool operator==(const Entity& o) const {
        return index == o.index && generation == o.generation;
    }

    /**
     * @brief Checks if two Entity handles are different.
     * @param o The other entity to compare.
     * @return true if index or generation differ.
     */
    bool operator!=(const Entity& o) const { return !(*this == o); }
};

/**
 * @brief Represents a null or invalid entity handle.
 * @details Can be used as a sentinel value or to initialize an entity handle that points to nothing.
 */
inline constexpr Entity INVALID_ENTITY{0, 0};

/**
 * @brief Hasher for using Entity keys in std::unordered_map or std::unordered_set.
 * @details Packs index and generation into a single 64-bit integer for hashing.
 */
struct EntityHash {
    /**
     * @brief Computes the hash of an Entity.
     * @param e The entity to hash.
     * @return A size_t hash value derived from the combined generation and index.
     */
    size_t operator()(const Entity& e) const {
        return std::hash<uint64_t>{}(static_cast<uint64_t>(e.generation) << 32 | e.index);
    }
};

} // namespace ecs
