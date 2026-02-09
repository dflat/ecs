#pragma once
#include "component.hpp"
#include "entity.hpp"

#include <algorithm>
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
    TypeSet type_set;
    std::map<ComponentTypeID, ComponentColumn> columns;
    std::vector<Entity> entities;

    // Edge cache: component_id -> archetype reached by adding/removing that component
    std::map<ComponentTypeID, ArchetypeEdge> edges;

    size_t count() const { return entities.size(); }

    bool has_component(ComponentTypeID id) const { return columns.find(id) != columns.end(); }

    void assert_parity() const {
        for (auto& [id, col] : columns)
            ECS_ASSERT(col.count == entities.size(), "entity-column parity violated");
    }

    // Append an entity with no component data yet (columns must be pushed separately)
    void push_entity(Entity e) { entities.push_back(e); }

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
};

} // namespace ecs
