#pragma once
#include "component.hpp"
#include "entity.hpp"

#include <algorithm>
#include <bitset>
#include <map>
#include <unordered_map>
#include <vector>

namespace ecs {

/**
 * @brief Represents a unique combination of component types.
 * @details Sorted vector of ComponentTypeIDs. Used as a key to identify Archetypes.
 */
using TypeSet = std::vector<ComponentTypeID>;

/**
 * @brief Hasher for TypeSet to allow usage in unordered maps.
 */
struct TypeSetHash {
    size_t operator()(const TypeSet& ts) const {
        size_t h = ts.size();
        for (auto id : ts)
            h ^= std::hash<ComponentTypeID>{}(id) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

/**
 * @brief Helper to create a sorted TypeSet from an initializer list.
 */
inline TypeSet make_typeset(std::initializer_list<ComponentTypeID> ids) {
    TypeSet ts(ids);
    std::sort(ts.begin(), ts.end());
    return ts;
}

/**
 * @brief Caches transitions between archetypes.
 * @details Used to quickly find the target archetype when adding or removing a component from an entity.
 */
struct ArchetypeEdge {
    struct Archetype* add_target = nullptr;
    struct Archetype* remove_target = nullptr;
};

/**
 * @brief Stores a collection of entities that all possess the exact same set of components.
 *
 * @details The Archetype is the core storage unit of the ECS. It stores components in a
 * Structure-of-Arrays (SoA) layout (via `ComponentColumn`). This ensures high cache locality
 * when iterating over components of a specific type.
 *
 * Memory is managed in a single large block (`block_`) which is subdivided among the columns.
 * When the capacity is exceeded, the entire block is reallocated and data is migrated.
 */
struct Archetype {
    static constexpr size_t CHUNK_ALIGN = 16;

    /** @brief The sorted list of component types this archetype stores. */
    TypeSet type_set;
    /** @brief Bitset for fast component presence checks (optimization for small ID counts). */
    std::bitset<256> component_bits;
    /** @brief Map of component ID to its data column. */
    std::map<ComponentTypeID, ComponentColumn> columns;
    /** @brief List of entities currently stored in this archetype. */
    std::vector<Entity> entities;

    /**
     * @brief Edge cache: component_id -> archetype reached by adding/removing that component.
     * @details Optimization to avoid repeated TypeSet lookups during structural changes.
     */
    std::map<ComponentTypeID, ArchetypeEdge> edges;

    Archetype() = default;

    /**
     * @brief Destructor.
     * @details Destroys all components in all columns and frees the raw memory block.
     */
    ~Archetype() {
        for (auto& [id, col] : columns)
            col.destroy_all();
        std::free(block_);
    }

    Archetype(Archetype&& o) noexcept
        : type_set(std::move(o.type_set)),
          component_bits(o.component_bits),
          columns(std::move(o.columns)),
          entities(std::move(o.entities)),
          edges(std::move(o.edges)),
          block_(o.block_),
          capacity_(o.capacity_) {
        o.block_ = nullptr;
        o.capacity_ = 0;
    }

    Archetype& operator=(Archetype&& o) noexcept {
        if (this != &o) {
            for (auto& [id, col] : columns)
                col.destroy_all();
            std::free(block_);
            type_set = std::move(o.type_set);
            component_bits = o.component_bits;
            columns = std::move(o.columns);
            entities = std::move(o.entities);
            edges = std::move(o.edges);
            block_ = o.block_;
            capacity_ = o.capacity_;
            o.block_ = nullptr;
            o.capacity_ = 0;
        }
        return *this;
    }

    Archetype(const Archetype&) = delete;
    Archetype& operator=(const Archetype&) = delete;

    /** @brief Returns the number of entities in this archetype. */
    size_t count() const { return entities.size(); }

    /** @brief Checks if this archetype contains the specified component type. */
    bool has_component(ComponentTypeID id) const { return columns.find(id) != columns.end(); }

    /** @brief Debug assertion to verify all columns have the same count as the entity list. */
    void assert_parity() const {
        for (auto& [id, col] : columns)
            ECS_ASSERT(col.count == entities.size(), "entity-column parity violated");
    }

    /**
     * @brief Appends an entity to the archetype.
     * @details Reserves space in all columns. The caller is responsible for subsequently
     * pushing the component data into the columns.
     * @param e The entity to add.
     */
    void push_entity(Entity e) {
        ensure_capacity(count() + 1);
        entities.push_back(e);
    }

    /**
     * @brief Removes an entity at a specific row using the "swap and pop" idiom.
     * @details The entity at `row` is replaced by the last entity in the list to maintain compactness.
     * @param row The index of the entity to remove.
     * @return The Entity that was moved into `row` (the one that was previously last), or INVALID_ENTITY if the removed entity was the last one.
     */
    Entity swap_remove(size_t row) {
        Entity swapped = INVALID_ENTITY;
        if (row < entities.size() - 1) {
            swapped = entities.back();
            entities[row] = entities.back();
        }
        entities.pop_back();
        for (auto& [id, col] : columns)
            col.swap_remove(row);
        assert_parity();
        return swapped;
    }

    /**
     * @brief Resizes the underlying memory block to hold at least `needed` elements.
     * @details If reallocation is required, all existing components are moved to the new block.
     * @param needed Minimum capacity required.
     */
    void ensure_capacity(size_t needed) {
        if (capacity_ >= needed)
            return;
        if (columns.empty())
            return;

        size_t new_cap;
        if (capacity_ == 0) {
            static constexpr size_t CHUNK_BYTES = 16384;
            size_t row_size = 0;
            for (auto& [cid, col] : columns)
                row_size += col.elem_size;
            new_cap = (row_size > 0) ? CHUNK_BYTES / row_size : 64;
            if (new_cap < 16)
                new_cap = 16;
        } else {
            new_cap = capacity_ * 2;
        }
        if (new_cap < needed)
            new_cap = needed;

        size_t total = block_size_for(new_cap);
        uint8_t* new_block = static_cast<uint8_t*>(std::malloc(total));

        size_t offset = 0;
        for (auto& [cid, col] : columns) {
            offset = align_up(offset, CHUNK_ALIGN);
            uint8_t* new_data = new_block + offset;
            if (col.data) {
                for (size_t i = 0; i < col.count; ++i)
                    col.move_fn(new_data + i * col.elem_size, col.data + i * col.elem_size);
            }
            col.data = new_data;
            col.capacity = new_cap;
            offset += new_cap * col.elem_size;
        }

        std::free(block_);
        block_ = new_block;
        capacity_ = new_cap;
    }

private:
    uint8_t* block_ = nullptr;
    size_t capacity_ = 0;

    static size_t align_up(size_t offset, size_t align) {
        return (offset + align - 1) & ~(align - 1);
    }

    // Compute total block size for a given capacity
    size_t block_size_for(size_t cap) const {
        size_t offset = 0;
        for (auto& [cid, col] : columns) {
            offset = align_up(offset, CHUNK_ALIGN);
            offset += cap * col.elem_size;
        }
        return offset;
    }
};

} // namespace ecs
