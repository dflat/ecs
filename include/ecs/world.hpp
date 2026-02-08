#pragma once
#include "archetype.hpp"
#include "component.hpp"
#include "entity.hpp"

#include <cassert>
#include <memory>
#include <vector>

namespace ecs {

struct EntityRecord {
    Archetype* archetype = nullptr;
    size_t row = 0;
};

class World {
public:
    World() {
        // Reserve index 0 so INVALID_ENTITY (index=0, gen=0) is never a live entity.
        generations_.push_back(1);
        records_.push_back({});
    }

    ~World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // -- Entity creation --

    Entity create() {
        uint32_t idx;
        if (!free_list_.empty()) {
            idx = free_list_.back();
            free_list_.pop_back();
        } else {
            idx = static_cast<uint32_t>(generations_.size());
            generations_.push_back(0);
            records_.push_back({});
        }
        uint32_t gen = generations_[idx];
        Entity e{idx, gen};

        // Place into the empty archetype
        Archetype* arch = get_or_create_archetype({});
        size_t row = arch->count();
        arch->push_entity(e);
        records_[idx] = {arch, row};
        return e;
    }

    template <typename... Ts>
    Entity create_with(Ts&&... components) {
        // Ensure column factories are registered for all types
        (ensure_column_factory<std::decay_t<Ts>>(), ...);

        TypeSet ts = make_typeset({component_id<std::decay_t<Ts>>()...});
        Archetype* arch = get_or_create_archetype(ts);

        uint32_t idx;
        if (!free_list_.empty()) {
            idx = free_list_.back();
            free_list_.pop_back();
        } else {
            idx = static_cast<uint32_t>(generations_.size());
            generations_.push_back(0);
            records_.push_back({});
        }
        uint32_t gen = generations_[idx];
        Entity e{idx, gen};

        size_t row = arch->count();
        arch->push_entity(e);
        // Push each component into its column
        (push_component_to_archetype<std::decay_t<Ts>>(arch, std::forward<Ts>(components)), ...);

        records_[idx] = {arch, row};
        return e;
    }

    // -- Entity destruction --

    void destroy(Entity e) {
        if (!alive(e)) return;
        auto& rec = records_[e.index];
        Entity swapped = rec.archetype->swap_remove(rec.row);
        if (swapped != INVALID_ENTITY) {
            records_[swapped.index].row = rec.row;
        }
        rec.archetype = nullptr;
        rec.row = 0;
        generations_[e.index]++;
        free_list_.push_back(e.index);
    }

    bool alive(Entity e) const {
        return e.index < generations_.size() &&
               e.generation == generations_[e.index] &&
               records_[e.index].archetype != nullptr;
    }

    // -- Component access --

    template <typename T>
    bool has(Entity e) const {
        if (!alive(e)) return false;
        return records_[e.index].archetype->has_component(component_id<T>());
    }

    template <typename T>
    T& get(Entity e) {
        auto& rec = records_[e.index];
        auto& col = rec.archetype->columns.at(component_id<T>());
        return *static_cast<T*>(col.get(rec.row));
    }

    template <typename T>
    T* try_get(Entity e) {
        if (!has<T>(e)) return nullptr;
        return &get<T>(e);
    }

    // -- Add component (archetype migration) --

    template <typename T>
    void add(Entity e, T&& component) {
        if (!alive(e)) return;
        ensure_column_factory<std::decay_t<T>>();
        ComponentTypeID cid = component_id<std::decay_t<T>>();

        auto& rec = records_[e.index];
        Archetype* old_arch = rec.archetype;
        if (old_arch->has_component(cid)) {
            // Already has it, just overwrite
            auto& col = old_arch->columns.at(cid);
            T* ptr = static_cast<T*>(col.get(rec.row));
            *ptr = std::forward<T>(component);
            return;
        }

        Archetype* new_arch = find_add_target(old_arch, cid);
        migrate_entity(e, old_arch, new_arch, rec.row);

        // Push the new component
        auto& col = new_arch->columns.at(cid);
        std::decay_t<T> tmp = std::forward<T>(component);
        col.push_raw(&tmp);
    }

    // -- Remove component (archetype migration) --

    template <typename T>
    void remove(Entity e) {
        if (!alive(e)) return;
        ComponentTypeID cid = component_id<T>();

        auto& rec = records_[e.index];
        Archetype* old_arch = rec.archetype;
        if (!old_arch->has_component(cid)) return;

        Archetype* new_arch = find_remove_target(old_arch, cid);
        size_t old_row = rec.row;

        // Before migration, destroy the removed component
        // Migration will move everything except the removed component
        migrate_entity_removing(e, old_arch, new_arch, old_row, cid);
    }

    // -- Query iteration --

    template <typename... Ts, typename Func>
    void each(Func&& fn) {
        ComponentTypeID ids[] = {component_id<Ts>()...};
        for (auto& [ts, arch] : archetypes_) {
            bool matches = true;
            for (auto id : ids) {
                if (!arch->has_component(id)) { matches = false; break; }
            }
            if (!matches) continue;

            size_t n = arch->count();
            if (n == 0) continue;

            // Get typed pointers into each column
            auto ptrs = std::make_tuple(
                static_cast<Ts*>(static_cast<void*>(arch->columns.at(component_id<Ts>()).data))...
            );

            for (size_t i = 0; i < n; ++i) {
                fn(arch->entities[i], std::get<Ts*>(ptrs)[i]...);
            }
        }
    }

    // Overload without entity parameter
    template <typename... Ts, typename Func>
    void each_no_entity(Func&& fn) {
        ComponentTypeID ids[] = {component_id<Ts>()...};
        for (auto& [ts, arch] : archetypes_) {
            bool matches = true;
            for (auto id : ids) {
                if (!arch->has_component(id)) { matches = false; break; }
            }
            if (!matches) continue;

            size_t n = arch->count();
            if (n == 0) continue;

            auto ptrs = std::make_tuple(
                static_cast<Ts*>(static_cast<void*>(arch->columns.at(component_id<Ts>()).data))...
            );

            for (size_t i = 0; i < n; ++i) {
                fn(std::get<Ts*>(ptrs)[i]...);
            }
        }
    }

private:
    std::vector<uint32_t> generations_;
    std::vector<EntityRecord> records_;
    std::vector<uint32_t> free_list_;
    std::unordered_map<TypeSet, std::unique_ptr<Archetype>, TypeSetHash> archetypes_;

    Archetype* get_or_create_archetype(const TypeSet& ts) {
        auto it = archetypes_.find(ts);
        if (it != archetypes_.end()) return it->second.get();

        auto arch = std::make_unique<Archetype>();
        arch->type_set = ts;
        auto& factory_reg = column_factory_registry();
        for (auto cid : ts) {
            arch->columns.emplace(cid, factory_reg.at(cid)());
        }
        Archetype* ptr = arch.get();
        archetypes_.emplace(ts, std::move(arch));
        return ptr;
    }

    Archetype* find_add_target(Archetype* src, ComponentTypeID cid) {
        auto edge_it = src->edges.find(cid);
        if (edge_it != src->edges.end() && edge_it->second.add_target)
            return edge_it->second.add_target;

        TypeSet new_ts = src->type_set;
        new_ts.push_back(cid);
        std::sort(new_ts.begin(), new_ts.end());
        Archetype* target = get_or_create_archetype(new_ts);
        src->edges[cid].add_target = target;
        return target;
    }

    Archetype* find_remove_target(Archetype* src, ComponentTypeID cid) {
        auto edge_it = src->edges.find(cid);
        if (edge_it != src->edges.end() && edge_it->second.remove_target)
            return edge_it->second.remove_target;

        TypeSet new_ts;
        for (auto id : src->type_set) {
            if (id != cid) new_ts.push_back(id);
        }
        Archetype* target = get_or_create_archetype(new_ts);
        src->edges[cid].remove_target = target;
        return target;
    }

    template <typename T>
    void push_component_to_archetype(Archetype* arch, T&& comp) {
        auto& col = arch->columns.at(component_id<std::decay_t<T>>());
        std::decay_t<T> tmp = std::forward<T>(comp);
        col.push_raw(&tmp);
    }

    // Migrate entity from old_arch[old_row] to new_arch, moving all shared columns.
    // Does NOT handle the added component — caller pushes it after.
    void migrate_entity(Entity e, Archetype* old_arch, Archetype* new_arch, size_t old_row) {
        // Move shared column data to new archetype
        for (auto& [cid, new_col] : new_arch->columns) {
            auto it = old_arch->columns.find(cid);
            if (it != old_arch->columns.end()) {
                void* src = it->second.get(old_row);
                new_col.push_raw(src);
            }
        }

        new_arch->push_entity(e);
        size_t new_row = new_arch->count() - 1;

        // Swap-remove from old archetype (entity + all columns)
        Entity swapped = old_arch->swap_remove(old_row);
        if (swapped != INVALID_ENTITY) {
            records_[swapped.index].row = old_row;
        }

        records_[e.index] = {new_arch, new_row};
    }

    // Migrate removing a component: moves all columns except cid_to_remove.
    void migrate_entity_removing(Entity e, Archetype* old_arch, Archetype* new_arch,
                                  size_t old_row, ComponentTypeID /*cid_to_remove*/) {
        // Move shared column data (all except the removed one)
        for (auto& [cid, new_col] : new_arch->columns) {
            auto it = old_arch->columns.find(cid);
            if (it != old_arch->columns.end()) {
                void* src = it->second.get(old_row);
                new_col.push_raw(src);
            }
        }

        new_arch->push_entity(e);
        size_t new_row = new_arch->count() - 1;

        // Destroy the removed component before swap-remove
        // (swap_remove will destroy whatever is at old_row, but after the move
        //  the shared columns at old_row are already moved-from shells.
        //  The removed column still has live data at old_row.)
        // Actually swap_remove destroys all columns at old_row, which is correct:
        // moved-from objects get destroyed (no-op for most types), and the removed
        // component gets properly destroyed.
        Entity swapped = old_arch->swap_remove(old_row);
        if (swapped != INVALID_ENTITY) {
            records_[swapped.index].row = old_row;
        }

        records_[e.index] = {new_arch, new_row};
    }
};

} // namespace ecs
