#pragma once
#include "archetype.hpp"
#include "command_buffer.hpp"
#include "component.hpp"
#include "entity.hpp"
#include "prefab.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <iosfwd>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace ecs {

/**
 * @brief Internal tracking structure for an entity's location.
 * @details Maps an Entity index to the specific Archetype and row where its data resides.
 */
struct EntityRecord {
    Archetype* archetype = nullptr;
    size_t row = 0;
};

/**
 * @brief The central manager for the Entity Component System.
 *
 * @details The World class is responsible for:
 * - Creating and destroying entities.
 * - Managing the storage of components via Archetypes.
 * - Executing queries over entities with specific component signatures.
 * - Managing global "singleton" Resources.
 * - Handling structural changes (adding/removing components) by migrating entities between
 * archetypes.
 * - Registering and invoking lifecycle hooks (observers).
 *
 * It is not thread-safe for write operations.
 */
class World {
public:
    /**
     * @brief Constructs a new World.
     * @details Initializes the entity index generation array. Index 0 is reserved for
     * INVALID_ENTITY.
     */
    World() {
        // Reserve index 0 so INVALID_ENTITY (index=0, gen=0) is never a live entity.
        generations_.push_back(1);
        records_.push_back({});
    }

    /**
     * @brief Destructor.
     * @details Clears all resources and destroys the world.
     */
    ~World() {
        // resources_ must be destroyed before other members since resource
        // destructors might reference nothing else, but explicit clear keeps
        // destruction order deterministic.
        resources_.clear();
    }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // -- Entity creation --

    /**
     * @brief Creates a new empty Entity.
     * @return The handle to the newly created entity.
     * @warning Asserts if called during query iteration (`each`).
     */
    Entity create() {
        ECS_ASSERT(iterating_ == 0, "structural change during iteration");
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

    /**
     * @brief Creates a new Entity initialized with a set of components.
     * @tparam Ts Types of the components to initialize.
     * @param components The component values to move/copy into the entity.
     * @return The handle to the newly created entity.
     * @warning Asserts if called during query iteration.
     */
    template <typename... Ts>
    Entity create_with(Ts&&... components) {
        ECS_ASSERT(iterating_ == 0, "structural change during iteration");
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
                    arch->find_column(component_id<std::decay_t<Ts>>())->get(row)),
         ...);

        return e;
    }

    // -- Entity destruction --

    /**
     * @brief Destroys an entity and releases its components.
     * @param e The entity to destroy.
     * @details If the entity is already dead, this does nothing.
     * @warning Asserts if called during query iteration.
     */
    void destroy(Entity e) {
        ECS_ASSERT(iterating_ == 0, "structural change during iteration");
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

    /**
     * @brief Destroys all entities that have component T.
     * @tparam T The component type to match.
     * @return The number of entities destroyed.
     * @warning Asserts if called during query iteration.
     */
    template <typename T>
    size_t destroy_all() {
        ECS_ASSERT(iterating_ == 0, "structural change during iteration");
        ComponentTypeID cid = component_id<T>();
        size_t destroyed = 0;

        // Collect matching archetypes (can't iterate archetypes_ while modifying entities)
        std::vector<Archetype*> matches;
        for (auto& [ts, arch] : archetypes_) {
            if (arch->has_component(cid))
                matches.push_back(arch.get());
        }

        for (auto* arch : matches) {
            // Destroy back-to-front to avoid swap-remove invalidation
            while (arch->count() > 0) {
                size_t row = arch->count() - 1;
                Entity e = arch->entities[row];

                for (auto& [col_cid, col] : arch->columns)
                    fire_hooks(on_remove_hooks_, col_cid, e, col.get(row));

                arch->swap_remove(row);
                records_[e.index].archetype = nullptr;
                records_[e.index].row = 0;
                generations_[e.index]++;
                free_list_.push_back(e.index);
                ++destroyed;
            }
        }

        return destroyed;
    }

    /**
     * @brief Checks if an entity handle refers to a valid, living entity.
     * @param e The entity handle.
     * @return true if valid and alive, false otherwise.
     */
    bool alive(Entity e) const {
        return e.index < generations_.size() && e.generation == generations_[e.index] &&
               records_[e.index].archetype != nullptr;
    }

    // -- Utility queries --

    /**
     * @brief Returns the total number of living entities in the world.
     */
    size_t count() const {
        size_t total = 0;
        for (auto& [ts, arch] : archetypes_)
            total += arch->count();
        return total;
    }

    /**
     * @brief Returns the number of entities that possess all specified components.
     * @tparam Ts Component types to query for.
     */
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

    /**
     * @brief Finds the single entity matching the components Ts... and invokes fn.
     * @tparam Ts Component types identifying the entity.
     * @tparam Func Callback function signature `void(Entity, Ts&...)`.
     * @warning Asserts if 0 or more than 1 entity matches.
     */
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

    /**
     * @brief Gets the command buffer for deferred operations.
     * @details Use this to queue structural changes (add/remove/destroy) during iteration.
     */
    CommandBuffer& deferred() { return deferred_commands_; }

    /**
     * @brief Executes all queued commands in the deferred buffer.
     * @warning Asserts if called during query iteration.
     */
    void flush_deferred() {
        ECS_ASSERT(iterating_ == 0, "flush during iteration");
        deferred_commands_.flush(*this);
    }

    // -- Resources --

    /**
     * @brief Creates or updates a global resource.
     * @tparam T The resource type.
     * @param value The value to store.
     * @details Resources are unique per type. Setting an existing resource type overwrites it.
     */
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

    /**
     * @brief Retrieves a reference to a global resource.
     * @tparam T The resource type.
     * @return Reference to the resource.
     * @warning Asserts if the resource does not exist.
     */
    template <typename T>
    T& resource() {
        auto it = resources_.find(component_id<T>());
        ECS_ASSERT(it != resources_.end() && it->second.data, "resource<T>() not found");
        return *static_cast<T*>(it->second.data);
    }

    /**
     * @brief Tries to retrieve a pointer to a global resource.
     * @tparam T The resource type.
     * @return Pointer to the resource, or nullptr if not found.
     */
    template <typename T>
    T* try_resource() {
        auto it = resources_.find(component_id<T>());
        if (it == resources_.end() || !it->second.data)
            return nullptr;
        return static_cast<T*>(it->second.data);
    }

    /**
     * @brief Checks if a global resource exists.
     * @tparam T The resource type.
     */
    template <typename T>
    bool has_resource() const {
        auto it = resources_.find(component_id<T>());
        return it != resources_.end() && it->second.data;
    }

    /**
     * @brief Removes a global resource.
     * @tparam T The resource type.
     */
    template <typename T>
    void remove_resource() {
        resources_.erase(component_id<T>());
    }

    // -- Observers --

    /**
     * @brief Registers a callback invoked whenever a component of type T is added to an entity.
     * @tparam T The component type.
     * @param fn Callback signature `void(World&, Entity, T&)`.
     */
    template <typename T>
    void on_add(std::function<void(World&, Entity, T&)> fn) {
        auto cid = component_id<std::decay_t<T>>();
        on_add_hooks_[cid].push_back([fn = std::move(fn)](World& w, Entity e, void* ptr) {
            fn(w, e, *static_cast<T*>(ptr));
        });
    }

    /**
     * @brief Registers a callback invoked whenever a component of type T is removed from an entity.
     * @tparam T The component type.
     * @param fn Callback signature `void(World&, Entity, T&)`.
     * @details The component data is valid during the callback but is destroyed immediately after.
     */
    template <typename T>
    void on_remove(std::function<void(World&, Entity, T&)> fn) {
        auto cid = component_id<std::decay_t<T>>();
        on_remove_hooks_[cid].push_back([fn = std::move(fn)](World& w, Entity e, void* ptr) {
            fn(w, e, *static_cast<T*>(ptr));
        });
    }

    // -- Component access --

    /**
     * @brief Checks if an entity has a specific component.
     * @tparam T The component type.
     * @param e The entity.
     * @return true if the entity exists and has the component.
     */
    template <typename T>
    bool has(Entity e) const {
        if (!alive(e))
            return false;
        return records_[e.index].archetype->has_component(component_id<T>());
    }

    /**
     * @brief Retrieves a reference to an entity's component.
     * @tparam T The component type.
     * @param e The entity.
     * @return Reference to the component.
     * @warning Asserts if the entity is dead or missing the component.
     */
    template <typename T>
    T& get(Entity e) {
        ECS_ASSERT(alive(e), "get<T> on dead entity");
        ECS_ASSERT(has<T>(e), "get<T> on entity missing component");
        auto& rec = records_[e.index];
        auto* col = rec.archetype->find_column(component_id<T>());
        return *static_cast<T*>(col->get(rec.row));
    }

    /**
     * @brief Tries to retrieve a pointer to an entity's component.
     * @tparam T The component type.
     * @param e The entity.
     * @return Pointer to the component, or nullptr if missing/dead.
     */
    template <typename T>
    T* try_get(Entity e) {
        if (!has<T>(e))
            return nullptr;
        return &get<T>(e);
    }

    // -- Add component (archetype migration) --

    /**
     * @brief Adds or replaces a component on an entity.
     * @tparam T The component type.
     * @param e The entity.
     * @param component The component data.
     * @details If the entity already has T, the data is overwritten (assignment).
     * If not, the entity is migrated to a new archetype containing T.
     * @warning Asserts if called during query iteration.
     */
    template <typename T>
    void add(Entity e, T&& component) {
        ECS_ASSERT(iterating_ == 0, "structural change during iteration");
        if (!alive(e))
            return;
        ensure_column_factory<std::decay_t<T>>();
        ComponentTypeID cid = component_id<std::decay_t<T>>();

        auto& rec = records_[e.index];
        Archetype* old_arch = rec.archetype;
        if (old_arch->has_component(cid)) {
            // Already has it, just overwrite
            auto* col = old_arch->find_column(cid);
            T* ptr = static_cast<T*>(col->get(rec.row));
            *ptr = std::forward<T>(component);
            return;
        }

        Archetype* new_arch = find_add_target(old_arch, cid);
        migrate_entity(e, old_arch, new_arch, rec.row);

        // Push the new component
        auto* col = new_arch->find_column(cid);
        std::decay_t<T> tmp = std::forward<T>(component);
        col->push_raw(&tmp);

        // Fire on_add after data is in place and record is updated
        fire_hooks(on_add_hooks_, cid, e, new_arch->find_column(cid)->get(records_[e.index].row));
    }

    // -- Remove component (archetype migration) --

    /**
     * @brief Removes a component from an entity.
     * @tparam T The component type.
     * @param e The entity.
     * @details Migrates the entity to an archetype without T. If the entity doesn't have T, does
     * nothing.
     * @warning Asserts if called during query iteration.
     */
    template <typename T>
    void remove(Entity e) {
        ECS_ASSERT(iterating_ == 0, "structural change during iteration");
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
        fire_hooks(on_remove_hooks_, cid, e, old_arch->find_column(cid)->get(old_row));

        migrate_entity_removing(e, old_arch, new_arch, old_row, cid);
    }

    // -- Query iteration --

    // Exclude filter tag: each<A, B>(Exclude<C, D>{}, fn) matches entities with A,B but not C,D.
    template <typename... Ts>
    struct Exclude {};

    /**
     * @brief Iterates over all entities possessing components Ts...
     * @tparam Ts Component types to match.
     * @tparam Func Callback signature `void(Entity, Ts&...)`.
     * @param fn The callback to invoke for each matching entity.
     */
    template <typename... Ts, typename Func>
    void each(Func&& fn) {
        ++iterating_;
        struct Guard {
            int& count;
            ~Guard() { --count; }
        } guard{iterating_};

        ComponentTypeID ids[] = {component_id<Ts>()...};
        for (auto* arch : cached_query(ids, sizeof...(Ts), nullptr, 0)) {
            size_t n = arch->count();
            if (n == 0)
                continue;
            auto ptrs = std::make_tuple(static_cast<Ts*>(
                static_cast<void*>(arch->find_column(component_id<Ts>())->data))...);
            for (size_t i = 0; i < n; ++i)
                fn(arch->entities[i], std::get<Ts*>(ptrs)[i]...);
        }
    }

    /**
     * @brief Iterates over components Ts... ignoring the Entity ID.
     * @tparam Ts Component types to match.
     * @tparam Func Callback signature `void(Ts&...)`.
     * @param fn The callback to invoke.
     */
    template <typename... Ts, typename Func>
    void each_no_entity(Func&& fn) {
        ++iterating_;
        struct Guard {
            int& count;
            ~Guard() { --count; }
        } guard{iterating_};

        ComponentTypeID ids[] = {component_id<Ts>()...};
        for (auto* arch : cached_query(ids, sizeof...(Ts), nullptr, 0)) {
            size_t n = arch->count();
            if (n == 0)
                continue;
            auto ptrs = std::make_tuple(static_cast<Ts*>(
                static_cast<void*>(arch->find_column(component_id<Ts>())->data))...);
            for (size_t i = 0; i < n; ++i)
                fn(std::get<Ts*>(ptrs)[i]...);
        }
    }

    // -- Exclude-filter overloads --

    /**
     * @brief Iterates entities with Ts... but WITHOUT Ex...
     * @tparam Ts Included component types.
     * @tparam Ex Excluded component types.
     * @tparam Func Callback signature `void(Entity, Ts&...)`.
     * @param fn The callback.
     */
    template <typename... Ts, typename... Ex, typename Func>
    void each(Exclude<Ex...>, Func&& fn) {
        ++iterating_;
        struct Guard {
            int& count;
            ~Guard() { --count; }
        } guard{iterating_};

        ComponentTypeID include_ids[] = {component_id<Ts>()...};
        ComponentTypeID exclude_ids[] = {component_id<Ex>()...};
        for (auto* arch : cached_query(include_ids, sizeof...(Ts), exclude_ids, sizeof...(Ex))) {
            size_t n = arch->count();
            if (n == 0)
                continue;
            auto ptrs = std::make_tuple(static_cast<Ts*>(
                static_cast<void*>(arch->find_column(component_id<Ts>())->data))...);
            for (size_t i = 0; i < n; ++i)
                fn(arch->entities[i], std::get<Ts*>(ptrs)[i]...);
        }
    }

    /**
     * @brief Iterates components Ts... (excluding Ex...) ignoring Entity ID.
     * @tparam Ts Included component types.
     * @tparam Ex Excluded component types.
     * @tparam Func Callback signature `void(Ts&...)`.
     * @param fn The callback.
     */
    template <typename... Ts, typename... Ex, typename Func>
    void each_no_entity(Exclude<Ex...>, Func&& fn) {
        ++iterating_;
        struct Guard {
            int& count;
            ~Guard() { --count; }
        } guard{iterating_};

        ComponentTypeID include_ids[] = {component_id<Ts>()...};
        ComponentTypeID exclude_ids[] = {component_id<Ex>()...};
        for (auto* arch : cached_query(include_ids, sizeof...(Ts), exclude_ids, sizeof...(Ex))) {
            size_t n = arch->count();
            if (n == 0)
                continue;
            auto ptrs = std::make_tuple(static_cast<Ts*>(
                static_cast<void*>(arch->find_column(component_id<Ts>())->data))...);
            for (size_t i = 0; i < n; ++i)
                fn(std::get<Ts*>(ptrs)[i]...);
        }
    }

    // -- Sorting --

    /**
     * @brief Sorts entities in their archetypes based on a component comparison.
     * @tparam T The component type to sort by.
     * @tparam Compare Comparator type (e.g., lambda `bool(const T&, const T&)`).
     * @param cmp The comparator function.
     * @details This performs an in-place sort within each archetype containing T.
     * This improves data locality for subsequent iterations.
     * @warning Asserts if called during query iteration.
     */
    template <typename T, typename Compare>
    void sort(Compare&& cmp) {
        ECS_ASSERT(iterating_ == 0, "sort during iteration");
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
            auto& sort_col = *arch->find_column(cid);
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

    friend void serialize(const World& world, std::ostream& out);
    friend void deserialize(World& world, std::istream& in);
    friend class CommandBuffer;
    friend Entity instantiate(World& world, const Prefab& prefab);
    template <typename... Overrides>
    friend Entity instantiate(World& world, const Prefab& prefab, Overrides&&... overrides);

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
    int iterating_ = 0;
    CommandBuffer deferred_commands_;

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
    static constexpr size_t MAX_QUERY_TERMS = 16;

    struct QueryKey {
        std::array<ComponentTypeID, MAX_QUERY_TERMS> include_ids{};
        std::array<ComponentTypeID, MAX_QUERY_TERMS> exclude_ids{};
        uint8_t n_include = 0;
        uint8_t n_exclude = 0;

        QueryKey() = default;
        QueryKey(const ComponentTypeID* inc, size_t ni, const ComponentTypeID* exc, size_t ne)
            : n_include(static_cast<uint8_t>(ni)), n_exclude(static_cast<uint8_t>(ne)) {
            ECS_ASSERT(ni <= MAX_QUERY_TERMS, "query exceeds max include terms");
            ECS_ASSERT(ne <= MAX_QUERY_TERMS, "query exceeds max exclude terms");
            for (size_t i = 0; i < ni; ++i)
                include_ids[i] = inc[i];
            for (size_t i = 0; i < ne; ++i)
                exclude_ids[i] = exc[i];
        }

        bool operator==(const QueryKey& other) const {
            if (n_include != other.n_include || n_exclude != other.n_exclude)
                return false;
            for (uint8_t i = 0; i < n_include; ++i)
                if (include_ids[i] != other.include_ids[i])
                    return false;
            for (uint8_t i = 0; i < n_exclude; ++i)
                if (exclude_ids[i] != other.exclude_ids[i])
                    return false;
            return true;
        }
    };

    struct QueryKeyHash {
        size_t operator()(const QueryKey& k) const {
            size_t h = k.n_include;
            for (uint8_t i = 0; i < k.n_include; ++i)
                h ^= std::hash<ComponentTypeID>{}(k.include_ids[i]) + 0x9e3779b9 + (h << 6) +
                     (h >> 2);
            h ^= k.n_exclude * 31;
            for (uint8_t i = 0; i < k.n_exclude; ++i)
                h ^= std::hash<ComponentTypeID>{}(k.exclude_ids[i]) + 0x9e3779b9 + (h << 6) +
                     (h >> 2);
            return h;
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
        QueryKey key(include, n_include, exclude, n_exclude);
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
        arch->columns.reserve(ts.size());
        for (auto cid : ts) {
            arch->columns.emplace_back(cid, factory_reg.at(cid)());
            arch->component_bits.set(cid);
        }
        // ts is already sorted, so columns are in sorted order
        Archetype* ptr = arch.get();
        archetypes_.emplace(ts, std::move(arch));
        ++archetype_generation_;
        return ptr;
    }

    Archetype* find_add_target(Archetype* src, ComponentTypeID cid) {
        auto* edge = src->find_edge(cid);
        if (edge && edge->add_target)
            return edge->add_target;

        TypeSet new_ts = src->type_set;
        new_ts.push_back(cid);
        std::sort(new_ts.begin(), new_ts.end());
        Archetype* target = get_or_create_archetype(new_ts);
        src->edge_for(cid).add_target = target;
        return target;
    }

    Archetype* find_remove_target(Archetype* src, ComponentTypeID cid) {
        auto* edge = src->find_edge(cid);
        if (edge && edge->remove_target)
            return edge->remove_target;

        TypeSet new_ts;
        for (auto id : src->type_set) {
            if (id != cid)
                new_ts.push_back(id);
        }
        Archetype* target = get_or_create_archetype(new_ts);
        src->edge_for(cid).remove_target = target;
        return target;
    }

    template <typename T>
    void push_component_to_archetype(Archetype* arch, T&& comp) {
        auto* col = arch->find_column(component_id<std::decay_t<T>>());
        std::decay_t<T> tmp = std::forward<T>(comp);
        col->push_raw(&tmp);
    }

    // Type-erased add: migrates entity and moves raw component data into the new archetype.
    void add_raw(Entity e, ComponentTypeID cid, void* data, ComponentColumn::MoveFunc move_fn) {
        ECS_ASSERT(iterating_ == 0, "structural change during iteration");
        if (!alive(e))
            return;
        auto& rec = records_[e.index];
        Archetype* old_arch = rec.archetype;
        if (old_arch->has_component(cid)) {
            // Already has it — overwrite via move
            auto* col = old_arch->find_column(cid);
            col->destroy_fn(col->get(rec.row));
            move_fn(col->get(rec.row), data);
            return;
        }

        Archetype* new_arch = find_add_target(old_arch, cid);
        migrate_entity(e, old_arch, new_arch, rec.row);

        auto* col = new_arch->find_column(cid);
        col->push_raw(data);

        fire_hooks(on_add_hooks_, cid, e, new_arch->find_column(cid)->get(records_[e.index].row));
    }

    // Type-erased remove component.
    void remove_raw(Entity e, ComponentTypeID cid) {
        ECS_ASSERT(iterating_ == 0, "structural change during iteration");
        if (!alive(e))
            return;
        auto& rec = records_[e.index];
        Archetype* old_arch = rec.archetype;
        if (!old_arch->has_component(cid))
            return;

        Archetype* new_arch = find_remove_target(old_arch, cid);
        size_t old_row = rec.row;

        fire_hooks(on_remove_hooks_, cid, e, old_arch->find_column(cid)->get(old_row));
        migrate_entity_removing(e, old_arch, new_arch, old_row, cid);
    }

    // Type-erased create_with: creates entity with N components given as parallel arrays.
    Entity create_with_raw(ComponentTypeID* ids, void** data, ComponentColumn::MoveFunc* /*moves*/,
                           size_t count) {
        ECS_ASSERT(iterating_ == 0, "structural change during iteration");
        TypeSet ts(ids, ids + count);
        std::sort(ts.begin(), ts.end());
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
        for (size_t i = 0; i < count; ++i) {
            auto* col = arch->find_column(ids[i]);
            col->push_raw(data[i]);
        }
        arch->assert_parity();

        records_[idx] = {arch, row};

        for (size_t i = 0; i < count; ++i) {
            fire_hooks(on_add_hooks_, ids[i], e, arch->find_column(ids[i])->get(row));
        }

        return e;
    }

    // Migrate entity from old_arch[old_row] to new_arch, moving all shared columns.
    // Does NOT handle the added component — caller pushes it after.
    void migrate_entity(Entity e, Archetype* old_arch, Archetype* new_arch, size_t old_row) {
        new_arch->ensure_capacity(new_arch->count() + 1);
        // Move shared column data to new archetype
        for (auto& [cid, new_col] : new_arch->columns) {
            auto* old_col = old_arch->find_column(cid);
            if (old_col) {
                void* src = old_col->get(old_row);
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
            auto* old_col = old_arch->find_column(cid);
            if (old_col) {
                void* src = old_col->get(old_row);
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

// -- CommandBuffer::flush() definition (needs complete World type) --

/**
 * @brief Flushes all queued commands in the buffer to the World.
 * @details Commands are executed in FIFO order. Structural changes here are safe.
 * @param w The World to apply commands to.
 */
inline void CommandBuffer::flush(World& w) {
    // Take ownership of buffer to allow re-entrant commands during flush
    std::vector<uint8_t> local_buf = std::move(buf_);
    buf_.clear();

    size_t pos = 0;
    while (pos < local_buf.size()) {
        pos = align_up(pos, alignof(CmdHeader));
        if (pos + sizeof(CmdHeader) > local_buf.size())
            break;
        auto* hdr = reinterpret_cast<CmdHeader*>(local_buf.data() + pos);
        pos += sizeof(CmdHeader);

        switch (hdr->tag) {
        case CmdTag::Destroy:
            w.destroy(hdr->entity);
            break;
        case CmdTag::Add: {
            pos = align_up(pos, alignof(std::max_align_t));
            void* data = local_buf.data() + pos;
            w.add_raw(hdr->entity, hdr->cid, data, hdr->move_fn);
            // If add_raw consumed the data via move, the source is now moved-from.
            // If the entity was dead and add_raw was a no-op, we must destroy the data.
            // add_raw calls push_raw which move-constructs and destroys source, OR
            // it overwrites via destroy+move. Either way the source is consumed.
            // If entity was dead, add_raw returned early — destroy the unconsumed data.
            if (!w.alive(hdr->entity))
                hdr->destroy_fn(data);
            pos += hdr->payload;
            break;
        }
        case CmdTag::Remove:
            w.remove_raw(hdr->entity, hdr->cid);
            break;
        case CmdTag::CreateWith: {
            size_t count = hdr->payload;
            // Collect sub-entries
            struct Gathered {
                ComponentTypeID cid;
                void* data;
                ComponentColumn::MoveFunc move_fn;
                ComponentColumn::DestroyFunc destroy_fn;
            };
            std::vector<Gathered> entries;
            entries.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                pos = align_up(pos, alignof(SubEntry));
                auto* sub = reinterpret_cast<SubEntry*>(local_buf.data() + pos);
                pos += sizeof(SubEntry);
                pos = align_up(pos, alignof(std::max_align_t));
                void* data = local_buf.data() + pos;
                entries.push_back({sub->cid, data, sub->move_fn, sub->destroy_fn});
                pos += sub->elem_size;
            }
            // Build parallel arrays for create_with_raw
            std::vector<ComponentTypeID> ids(count);
            std::vector<void*> data_ptrs(count);
            std::vector<ComponentColumn::MoveFunc> moves(count);
            for (size_t i = 0; i < count; ++i) {
                ids[i] = entries[i].cid;
                data_ptrs[i] = entries[i].data;
                moves[i] = entries[i].move_fn;
            }
            w.create_with_raw(ids.data(), data_ptrs.data(), moves.data(), count);
            break;
        }
        }
    }
}

// -- Prefab instantiation (needs complete World) --

/**
 * @brief Instantiates an entity from a Prefab.
 * @param world The target world.
 * @param prefab The Prefab template.
 * @return The new entity.
 */
inline Entity instantiate(World& world, const Prefab& prefab) {
    ECS_ASSERT(world.iterating_ == 0, "structural change during iteration");
    ECS_ASSERT(prefab.component_count() > 0, "instantiate: empty prefab");

    // Build TypeSet from prefab entries
    TypeSet ts;
    ts.reserve(prefab.component_count());
    for (auto& entry : prefab.entries())
        ts.push_back(entry.cid);
    std::sort(ts.begin(), ts.end());

    Archetype* arch = world.get_or_create_archetype(ts);

    // Allocate entity
    uint32_t idx;
    if (!world.free_list_.empty()) {
        idx = world.free_list_.back();
        world.free_list_.pop_back();
    } else {
        idx = static_cast<uint32_t>(world.generations_.size());
        world.generations_.push_back(0);
        world.records_.push_back({});
    }
    uint32_t gen = world.generations_[idx];
    Entity e{idx, gen};

    size_t row = arch->count();
    arch->push_entity(e);

    // Copy-construct each component from prefab defaults
    for (auto& entry : prefab.entries()) {
        auto* col = arch->find_column(entry.cid);
        // copy_fn placement-new constructs into the column slot
        entry.copy_fn(col->data + col->count * col->elem_size, prefab.data() + entry.buf_offset);
        ++col->count;
    }
    arch->assert_parity();

    world.records_[idx] = {arch, row};

    // Fire on_add hooks
    for (auto& entry : prefab.entries()) {
        world.fire_hooks(world.on_add_hooks_, entry.cid, e, arch->find_column(entry.cid)->get(row));
    }

    return e;
}

/**
 * @brief Instantiates a Prefab with some overridden components.
 * @tparam Overrides Component types to override or add.
 * @param world The target world.
 * @param prefab The Prefab template.
 * @param overrides Component values to use instead of the prefab defaults (or in addition to them).
 * @return The new entity.
 */
template <typename... Overrides>
Entity instantiate(World& world, const Prefab& prefab, Overrides&&... overrides) {
    ECS_ASSERT(world.iterating_ == 0, "structural change during iteration");
    ECS_ASSERT(prefab.component_count() > 0, "instantiate: empty prefab");

    // Ensure column factories for override types
    (ensure_column_factory<std::decay_t<Overrides>>(), ...);

    // Build TypeSet: prefab entries + any extra override types
    TypeSet ts;
    ts.reserve(prefab.component_count() + sizeof...(Overrides));
    for (auto& entry : prefab.entries())
        ts.push_back(entry.cid);

    // Add override type IDs not already in prefab
    ComponentTypeID override_ids[] = {component_id<std::decay_t<Overrides>>()...};
    for (auto oid : override_ids) {
        bool found = false;
        for (auto cid : ts) {
            if (cid == oid) {
                found = true;
                break;
            }
        }
        if (!found)
            ts.push_back(oid);
    }
    std::sort(ts.begin(), ts.end());

    Archetype* arch = world.get_or_create_archetype(ts);

    // Allocate entity
    uint32_t idx;
    if (!world.free_list_.empty()) {
        idx = world.free_list_.back();
        world.free_list_.pop_back();
    } else {
        idx = static_cast<uint32_t>(world.generations_.size());
        world.generations_.push_back(0);
        world.records_.push_back({});
    }
    uint32_t gen = world.generations_[idx];
    Entity e{idx, gen};

    size_t row = arch->count();
    arch->push_entity(e);

    // Build a set of overridden type IDs for quick lookup
    // (small array, linear scan is fine for typical override counts)

    // Copy non-overridden prefab defaults
    for (auto& entry : prefab.entries()) {
        bool overridden = false;
        for (auto oid : override_ids) {
            if (entry.cid == oid) {
                overridden = true;
                break;
            }
        }
        if (!overridden) {
            auto* col = arch->find_column(entry.cid);
            entry.copy_fn(col->data + col->count * col->elem_size,
                          prefab.data() + entry.buf_offset);
            ++col->count;
        }
    }

    // Push overrides (move-construct)
    auto push_override = [&](auto&& comp) {
        using U = std::decay_t<decltype(comp)>;
        auto* col = arch->find_column(component_id<U>());
        U tmp = std::forward<decltype(comp)>(comp);
        col->push_raw(&tmp);
    };
    (push_override(std::forward<Overrides>(overrides)), ...);

    arch->assert_parity();
    world.records_[idx] = {arch, row};

    // Fire on_add hooks for all components
    for (auto cid : ts) {
        world.fire_hooks(world.on_add_hooks_, cid, e, arch->find_column(cid)->get(row));
    }

    return e;
}

} // namespace ecs
