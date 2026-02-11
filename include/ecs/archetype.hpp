#pragma once
#include "component.hpp"
#include "entity.hpp"

#include <algorithm>
#include <bitset>
#include <map>
#include <unordered_map>
#include <vector>

namespace ecs {

using TypeSet = std::vector<ComponentTypeID>;

struct TypeSetHash {
    size_t operator()(const TypeSet& ts) const {
        size_t h = ts.size();
        for (auto id : ts)
            h ^= std::hash<ComponentTypeID>{}(id) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

inline TypeSet make_typeset(std::initializer_list<ComponentTypeID> ids) {
    TypeSet ts(ids);
    std::sort(ts.begin(), ts.end());
    return ts;
}

struct ArchetypeEdge {
    struct Archetype* add_target = nullptr;
    struct Archetype* remove_target = nullptr;
};

struct Archetype {
    static constexpr size_t CHUNK_ALIGN = 16;

    TypeSet type_set;
    std::bitset<256> component_bits;
    std::map<ComponentTypeID, ComponentColumn> columns;
    std::vector<Entity> entities;

    // Edge cache: component_id -> archetype reached by adding/removing that component
    std::map<ComponentTypeID, ArchetypeEdge> edges;

    Archetype() = default;

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

    size_t count() const { return entities.size(); }

    bool has_component(ComponentTypeID id) const { return columns.find(id) != columns.end(); }

    void assert_parity() const {
        for (auto& [id, col] : columns)
            ECS_ASSERT(col.count == entities.size(), "entity-column parity violated");
    }

    // Append an entity with no component data yet (columns must be pushed separately).
    // Ensures capacity for the new entity across all columns.
    void push_entity(Entity e) {
        ensure_capacity(count() + 1);
        entities.push_back(e);
    }

    // Swap-remove entity at row, returns the entity that was swapped into that row
    // (or INVALID_ENTITY if it was the last row)
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

    // Ensure all columns have capacity for at least `needed` elements.
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
        if (!new_block)
            throw std::bad_alloc();

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
