#pragma once
#include "archetype.hpp"
#include "component.hpp"
#include "entity.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <numeric>
#include <unordered_map>
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

    ~World() {
        // resources_ must be destroyed before other members since resource
        // destructors might reference nothing else, but explicit clear keeps
        // destruction order deterministic.
        resources_.clear();
    }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // -- Entity creation --

    Entity create() {
        ECS_ASSERT(!iterating_, "structural change during iteration");
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
        ECS_ASSERT(!iterating_, "structural change during iteration");
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
        arch->assert_parity();

        records_[idx] = {arch, row};

        // Fire on_add hooks after record is set (so get<T>(e) works in hooks)
        (fire_hooks(on_add_hooks_, component_id<std::decay_t<Ts>>(), e,
                    arch->columns.at(component_id<std::decay_t<Ts>>()).get(row)),
         ...);

        return e;
    }

    // -- Entity destruction --

    void destroy(Entity e) {
        ECS_ASSERT(!iterating_, "structural change during iteration");
        if (!alive(e))
            return;
        auto& rec = records_[e.index];
        Archetype* arch = rec.archetype;

        // Fire on_remove hooks before data is destroyed
        for (auto& [cid, col] : arch->columns)
            fire_hooks(on_remove_hooks_, cid, e, col.get(rec.row));

        Entity swapped = arch->swap_remove(rec.row);
        if (swapped != INVALID_ENTITY) {
            records_[swapped.index].row = rec.row;
        }
        rec.archetype = nullptr;
        rec.row = 0;
        generations_[e.index]++;
        free_list_.push_back(e.index);
    }

    bool alive(Entity e) const {
        return e.index < generations_.size() && e.generation == generations_[e.index] &&
               records_[e.index].archetype != nullptr;
    }

    // -- Utility queries --

    // Total live entity count.
    size_t count() const {
        size_t total = 0;
        for (auto& [ts, arch] : archetypes_)
            total += arch->count();
        return total;
    }

    // Count of entities matching all of Ts...
    template <typename... Ts>
    size_t count() const {
        ComponentTypeID ids[] = {component_id<Ts>()...};
        size_t total = 0;
        for (auto& [ts, arch] : archetypes_) {
            bool matches = true;
            for (auto id : ids) {
                if (!arch->has_component(id)) {
                    matches = false;
                    break;
                }
            }
            if (matches)
                total += arch->count();
        }
        return total;
    }

    // Returns (Entity, Ts&...) for a query expected to match exactly one entity.
    // Asserts on 0 or 2+ matches.
    template <typename... Ts, typename Func>
    void single(Func&& fn) {
        size_t found = 0;
        each<Ts...>([&](Entity e, Ts&... comps) {
            ++found;
            ECS_ASSERT(found <= 1, "single<Ts...>() matched more than one entity");
            fn(e, comps...);
        });
        ECS_ASSERT(found == 1, "single<Ts...>() matched zero entities");
    }

    // -- Deferred commands --

    class DeferredProxy {
    public:
        explicit DeferredProxy(std::vector<std::function<void(World&)>>& cmds) : cmds_(cmds) {}

        void destroy(Entity e) {
            cmds_.push_back([e](World& w) { w.destroy(e); });
        }

        template <typename T>
        void add(Entity e, T&& comp) {
            auto ptr = std::make_shared<std::decay_t<T>>(std::forward<T>(comp));
            cmds_.push_back([e, ptr](World& w) { w.add<std::decay_t<T>>(e, std::move(*ptr)); });
        }

        template <typename T>
        void remove(Entity e) {
            cmds_.push_back([e](World& w) { w.remove<std::decay_t<T>>(e); });
        }

        template <typename... Ts>
        void create_with(Ts&&... comps) {
            auto data =
                std::make_shared<std::tuple<std::decay_t<Ts>...>>(std::forward<Ts>(comps)...);
            cmds_.push_back([data](World& w) {
                std::apply([&w](auto&&... args) { w.create_with(std::move(args)...); },
                           std::move(*data));
            });
        }

    private:
        std::vector<std::function<void(World&)>>& cmds_;
    };

    DeferredProxy deferred() { return DeferredProxy{deferred_commands_}; }

    void flush_deferred() {
        ECS_ASSERT(!iterating_, "flush during iteration");
        auto cmds = std::move(deferred_commands_);
        for (auto& cmd : cmds)
            cmd(*this);
    }

    // -- Resources --

    template <typename T>
    void set_resource(T&& value) {
        using U = std::decay_t<T>;
        auto id = component_id<U>();
        auto it = resources_.find(id);
        if (it != resources_.end()) {
            if (it->second.data && it->second.deleter)
                it->second.deleter(it->second.data);
            it->second.data = new U(std::forward<T>(value));
        } else {
            ErasedResource r;
            r.data = new U(std::forward<T>(value));
            r.deleter = [](void* p) {
                delete static_cast<U*>(p);
            };
            resources_.emplace(id, std::move(r));
        }
    }

    template <typename T>
    T& resource() {
        auto it = resources_.find(component_id<T>());
        ECS_ASSERT(it != resources_.end() && it->second.data, "resource<T>() not found");
        return *static_cast<T*>(it->second.data);
    }

    template <typename T>
    T* try_resource() {
        auto it = resources_.find(component_id<T>());
        if (it == resources_.end() || !it->second.data)
            return nullptr;
        return static_cast<T*>(it->second.data);
    }

    template <typename T>
    bool has_resource() const {
        auto it = resources_.find(component_id<T>());
        return it != resources_.end() && it->second.data;
    }

    template <typename T>
    void remove_resource() {
        resources_.erase(component_id<T>());
    }

    // -- Observers --

    template <typename T>
    void on_add(std::function<void(World&, Entity, T&)> fn) {
        auto cid = component_id<std::decay_t<T>>();
        on_add_hooks_[cid].push_back([fn = std::move(fn)](World& w, Entity e, void* ptr) {
            fn(w, e, *static_cast<T*>(ptr));
        });
    }

    template <typename T>
    void on_remove(std::function<void(World&, Entity, T&)> fn) {
        auto cid = component_id<std::decay_t<T>>();
        on_remove_hooks_[cid].push_back([fn = std::move(fn)](World& w, Entity e, void* ptr) {
            fn(w, e, *static_cast<T*>(ptr));
        });
    }

    // -- Component access --

    template <typename T>
    bool has(Entity e) const {
        if (!alive(e))
            return false;
        return records_[e.index].archetype->has_component(component_id<T>());
    }

    template <typename T>
    T& get(Entity e) {
        ECS_ASSERT(alive(e), "get<T> on dead entity");
        ECS_ASSERT(has<T>(e), "get<T> on entity missing component");
        auto& rec = records_[e.index];
        auto& col = rec.archetype->columns.at(component_id<T>());
        return *static_cast<T*>(col.get(rec.row));
    }

    template <typename T>
    T* try_get(Entity e) {
        if (!has<T>(e))
            return nullptr;
        return &get<T>(e);
    }

    // -- Add component (archetype migration) --

    template <typename T>
    void add(Entity e, T&& component) {
        ECS_ASSERT(!iterating_, "structural change during iteration");
        if (!alive(e))
            return;
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

        // Fire on_add after data is in place and record is updated
        fire_hooks(on_add_hooks_, cid, e, new_arch->columns.at(cid).get(records_[e.index].row));
    }

    // -- Remove component (archetype migration) --

    template <typename T>
    void remove(Entity e) {
        ECS_ASSERT(!iterating_, "structural change during iteration");
        if (!alive(e))
            return;
        ComponentTypeID cid = component_id<T>();

        auto& rec = records_[e.index];
        Archetype* old_arch = rec.archetype;
        if (!old_arch->has_component(cid))
            return;

        Archetype* new_arch = find_remove_target(old_arch, cid);
        size_t old_row = rec.row;

        // Fire on_remove before data is destroyed
        fire_hooks(on_remove_hooks_, cid, e, old_arch->columns.at(cid).get(old_row));

        migrate_entity_removing(e, old_arch, new_arch, old_row, cid);
    }

    // -- Query iteration --

    // Exclude filter tag: each<A, B>(Exclude<C, D>{}, fn) matches entities with A,B but not C,D.
    template <typename... Ts>
    struct Exclude {};

    template <typename... Ts, typename Func>
    void each(Func&& fn) {
        iterating_ = true;
        struct Guard {
            bool& flag;
            ~Guard() { flag = false; }
        } guard{iterating_};

        ComponentTypeID ids[] = {component_id<Ts>()...};
        for (auto* arch : cached_query(ids, sizeof...(Ts), nullptr, 0)) {
            size_t n = arch->count();
            if (n == 0)
                continue;
            auto ptrs = std::make_tuple(
                static_cast<Ts*>(static_cast<void*>(arch->columns.at(component_id<Ts>()).data))...);
            for (size_t i = 0; i < n; ++i)
                fn(arch->entities[i], std::get<Ts*>(ptrs)[i]...);
        }
    }

    template <typename... Ts, typename Func>
    void each_no_entity(Func&& fn) {
        iterating_ = true;
        struct Guard {
            bool& flag;
            ~Guard() { flag = false; }
        } guard{iterating_};

        ComponentTypeID ids[] = {component_id<Ts>()...};
        for (auto* arch : cached_query(ids, sizeof...(Ts), nullptr, 0)) {
            size_t n = arch->count();
            if (n == 0)
                continue;
            auto ptrs = std::make_tuple(
                static_cast<Ts*>(static_cast<void*>(arch->columns.at(component_id<Ts>()).data))...);
            for (size_t i = 0; i < n; ++i)
                fn(std::get<Ts*>(ptrs)[i]...);
        }
    }

    // -- Exclude-filter overloads --

    template <typename... Ts, typename... Ex, typename Func>
    void each(Exclude<Ex...>, Func&& fn) {
        iterating_ = true;
        struct Guard {
            bool& flag;
            ~Guard() { flag = false; }
        } guard{iterating_};

        ComponentTypeID include_ids[] = {component_id<Ts>()...};
        ComponentTypeID exclude_ids[] = {component_id<Ex>()...};
        for (auto* arch : cached_query(include_ids, sizeof...(Ts), exclude_ids, sizeof...(Ex))) {
            size_t n = arch->count();
            if (n == 0)
                continue;
            auto ptrs = std::make_tuple(
                static_cast<Ts*>(static_cast<void*>(arch->columns.at(component_id<Ts>()).data))...);
            for (size_t i = 0; i < n; ++i)
                fn(arch->entities[i], std::get<Ts*>(ptrs)[i]...);
        }
    }

    template <typename... Ts, typename... Ex, typename Func>
    void each_no_entity(Exclude<Ex...>, Func&& fn) {
        iterating_ = true;
        struct Guard {
            bool& flag;
            ~Guard() { flag = false; }
        } guard{iterating_};

        ComponentTypeID include_ids[] = {component_id<Ts>()...};
        ComponentTypeID exclude_ids[] = {component_id<Ex>()...};
        for (auto* arch : cached_query(include_ids, sizeof...(Ts), exclude_ids, sizeof...(Ex))) {
            size_t n = arch->count();
            if (n == 0)
                continue;
            auto ptrs = std::make_tuple(
                static_cast<Ts*>(static_cast<void*>(arch->columns.at(component_id<Ts>()).data))...);
            for (size_t i = 0; i < n; ++i)
                fn(std::get<Ts*>(ptrs)[i]...);
        }
    }

    // -- Sorting --

    template <typename T, typename Compare>
    void sort(Compare&& cmp) {
        ECS_ASSERT(!iterating_, "sort during iteration");
        ComponentTypeID cid = component_id<T>();

        for (auto& [ts, arch] : archetypes_) {
            if (!arch->has_component(cid))
                continue;
            size_t n = arch->count();
            if (n <= 1)
                continue;

            // Build index array
            std::vector<size_t> perm(n);
            std::iota(perm.begin(), perm.end(), size_t(0));

            // Sort indices by comparing T column elements
            auto& sort_col = arch->columns.at(cid);
            uint8_t* sort_data = sort_col.data;
            size_t sort_elem = sort_col.elem_size;
            std::sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
                return cmp(*static_cast<T*>(static_cast<void*>(sort_data + a * sort_elem)),
                           *static_cast<T*>(static_cast<void*>(sort_data + b * sort_elem)));
            });

            // Invert the gather permutation to get a scatter permutation
            // perm[i] = "source index for destination i" (gather)
            // inv[i] = "destination index for source i" (scatter)
            std::vector<size_t> inv(n);
            for (size_t i = 0; i < n; ++i)
                inv[perm[i]] = i;

            // Apply scatter permutation via cycle-chasing swaps
            for (size_t i = 0; i < n; ++i) {
                while (inv[i] != i) {
                    size_t j = inv[i];
                    std::swap(arch->entities[i], arch->entities[j]);
                    for (auto& [col_id, col] : arch->columns) {
                        col.swap_fn(col.get(i), col.get(j));
                    }
                    std::swap(inv[i], inv[j]);
                }
            }

            // Update entity records
            for (size_t i = 0; i < n; ++i) {
                records_[arch->entities[i].index].row = i;
            }
        }
    }

private:
    struct ErasedResource {
        void* data = nullptr;
        void (*deleter)(void*) = nullptr;
        ~ErasedResource() {
            if (data && deleter)
                deleter(data);
        }
        ErasedResource() = default;
        ErasedResource(ErasedResource&& o) noexcept : data(o.data), deleter(o.deleter) {
            o.data = nullptr;
        }
        ErasedResource& operator=(ErasedResource&& o) noexcept {
            if (this != &o) {
                if (data && deleter)
                    deleter(data);
                data = o.data;
                deleter = o.deleter;
                o.data = nullptr;
            }
            return *this;
        }
        ErasedResource(const ErasedResource&) = delete;
        ErasedResource& operator=(const ErasedResource&) = delete;
    };

    std::vector<uint32_t> generations_;
    std::vector<EntityRecord> records_;
    std::vector<uint32_t> free_list_;
    std::unordered_map<TypeSet, std::unique_ptr<Archetype>, TypeSetHash> archetypes_;
    std::unordered_map<ComponentTypeID, ErasedResource> resources_;
    bool iterating_ = false;
    std::vector<std::function<void(World&)>> deferred_commands_;

    // -- Observer hooks --
    std::unordered_map<ComponentTypeID, std::vector<std::function<void(World&, Entity, void*)>>>
        on_add_hooks_;
    std::unordered_map<ComponentTypeID, std::vector<std::function<void(World&, Entity, void*)>>>
        on_remove_hooks_;

    void fire_hooks(
        const std::unordered_map<ComponentTypeID,
                                 std::vector<std::function<void(World&, Entity, void*)>>>& hooks,
        ComponentTypeID cid, Entity e, void* data) {
        auto it = hooks.find(cid);
        if (it == hooks.end())
            return;
        for (auto& fn : it->second)
            fn(*this, e, data);
    }

    // -- Query cache --
    struct QueryKey {
        TypeSet include_ids;
        TypeSet exclude_ids;
        bool operator==(const QueryKey& other) const {
            return include_ids == other.include_ids && exclude_ids == other.exclude_ids;
        }
    };
    struct QueryKeyHash {
        size_t operator()(const QueryKey& k) const {
            TypeSetHash h;
            return h(k.include_ids) ^ (h(k.exclude_ids) * 31);
        }
    };
    struct QueryCacheEntry {
        std::vector<Archetype*> archetypes;
        uint64_t generation = 0;
    };
    uint64_t archetype_generation_ = 0;
    mutable std::unordered_map<QueryKey, QueryCacheEntry, QueryKeyHash> query_cache_;

    const std::vector<Archetype*>& cached_query(const ComponentTypeID* include, size_t n_include,
                                                const ComponentTypeID* exclude, size_t n_exclude) {
        QueryKey key{TypeSet(include, include + n_include), TypeSet(exclude, exclude + n_exclude)};
        auto& entry = query_cache_[key];
        if (entry.generation != archetype_generation_) {
            entry.archetypes.clear();
            std::bitset<256> include_mask, exclude_mask;
            for (size_t i = 0; i < n_include; ++i)
                include_mask.set(include[i]);
            for (size_t i = 0; i < n_exclude; ++i)
                exclude_mask.set(exclude[i]);
            for (auto& [ts, arch] : archetypes_) {
                if (archetype_matches(arch->component_bits, include_mask, exclude_mask))
                    entry.archetypes.push_back(arch.get());
            }
            entry.generation = archetype_generation_;
        }
        return entry.archetypes;
    }

    static bool archetype_matches(const std::bitset<256>& arch_bits,
                                  const std::bitset<256>& include_mask,
                                  const std::bitset<256>& exclude_mask) {
        return (arch_bits & include_mask) == include_mask && (arch_bits & exclude_mask).none();
    }

    Archetype* get_or_create_archetype(const TypeSet& ts) {
        auto it = archetypes_.find(ts);
        if (it != archetypes_.end())
            return it->second.get();

        auto arch = std::make_unique<Archetype>();
        arch->type_set = ts;
        auto& factory_reg = column_factory_registry();
        for (auto cid : ts) {
            arch->columns.emplace(cid, factory_reg.at(cid)());
            arch->component_bits.set(cid);
        }
        Archetype* ptr = arch.get();
        archetypes_.emplace(ts, std::move(arch));
        ++archetype_generation_;
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
            if (id != cid)
                new_ts.push_back(id);
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
        new_arch->ensure_capacity(new_arch->count() + 1);
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
    void migrate_entity_removing(Entity e, Archetype* old_arch, Archetype* new_arch, size_t old_row,
                                 ComponentTypeID /*cid_to_remove*/) {
        new_arch->ensure_capacity(new_arch->count() + 1);
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
