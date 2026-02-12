# ECS Library Specification

**Version:** 0.9
**Status:** Draft
**Language:** C++17, header-only
**Dependencies:** None (standard library only)

---

## 1. Overview

A standalone, archetype-based Entity Component System. Entities with identical component sets share contiguous Structure-of-Arrays (SoA) storage, enabling cache-friendly linear iteration. The library is header-only with no external dependencies; integration with renderers, scripting layers, or application frameworks happens in the consuming application.

### 1.1 Design Goals

- **Cache-friendly iteration.** Query loops index into contiguous typed arrays. No vtable dispatch, no pointer chasing per-entity.
- **Zero external dependencies.** C++17 standard library only. No RTTI, no exceptions in core paths.
- **Minimal API surface.** The `World` class is the single point of contact for all entity/component operations.
- **Composable.** Builtins (transform, hierarchy) are ordinary components with no special status in the core. Applications can ignore them entirely.

### 1.2 Non-Goals (Current Scope)

- Thread safety. All operations assume single-threaded access to a `World`.
- ~~Serialization.~~ Binary world snapshots implemented in Phase 8 — see §3.10.
- ~~Reactive/event systems.~~ Basic observers implemented in Phase 4 — see §3.8.
- Maximum performance at extreme scale (100k+ entities). The current design prioritizes correctness and clarity. ~~Bitset archetype matching~~ is implemented (Phase 7.1). ~~Chunk allocation~~ is implemented (Phase 7.2).
- ~~Singleton resources.~~ Implemented in Phase 3 — see §3.7.

---

## 2. Core Concepts

### 2.1 Entity

A lightweight handle to an entity in the world.

```
struct Entity {
    uint32_t index;
    uint32_t generation;
};
```

- **index**: Slot in the world's entity arrays. Recycled via free-list on destruction.
- **generation**: Monotonically increasing counter per slot. Bumped on destruction. Prevents stale handles from aliasing live entities.
- **INVALID_ENTITY**: `{0, 0}`. Index 0 is reserved at world construction (generation starts at 1) so this value can never refer to a live entity.

**Invariant:** `world.alive(e)` returns true if and only if `e.index` is within bounds, `e.generation` matches the current generation for that slot, and the entity has an archetype assignment.

### 2.2 Component

Any C++ type. No base class, no registration macro, no RTTI.

**Requirements on component types:**
- Move-constructible (used during column growth and archetype migration).
- Destructible.
- No requirement for default-constructibility, copyability, or trivial layout.

**Type identification:**
```
template <typename T>
ComponentTypeID component_id();
```

Returns a unique `uint32_t` for each type `T`, assigned on first call via a static counter. Deterministic within a single program execution but not stable across builds or translation units that vary call order. For serialization, use `register_component<T>(name)` to assign stable string names (see §3.10).

### 2.3 Archetype

A unique combination of component types. Identified by a **TypeSet**: a sorted `vector<ComponentTypeID>`.

Each archetype owns:
- One **ComponentColumn** per component type (SoA storage).
- A parallel `vector<Entity>` tracking which entity occupies each row.
- A **component bitset** (`std::bitset<256>`) with one bit set per component type in the archetype, used for fast query matching.
- An **edge cache** (`map<ComponentTypeID, ArchetypeEdge>`) for O(1) amortized archetype lookup when adding/removing components.

**Invariant:** For every archetype, all columns and the entity vector have identical length (the archetype's entity count).

### 2.4 ComponentColumn (Type-Erased Storage)

A non-owning view into a region of the archetype's chunk-allocated block. Each column stores elements of a single component type contiguously.

| Property | Value |
|---|---|
| Backing memory | Non-owning `uint8_t*` into archetype's block (see §2.3.1) |
| Element lifecycle | Placement-new via move constructor; explicit destructor calls |
| Growth policy | Managed by archetype (see §2.3.1) |
| Deletion policy | Swap-remove: last element is move-constructed over the deleted slot, maintaining density |

**Function pointers** (`MoveFunc`, `DestroyFunc`, `SwapFunc`) are captured at column creation from the concrete type via `make_column<T>()`. This allows type-erased operations without virtual dispatch.

#### 2.3.1 Chunk Allocation

Each archetype owns a single contiguous memory block containing all column data (SoA layout). Column regions are separated by 16-byte alignment padding:

```
block: [Col0: cap * elem0] [pad16] [Col1: cap * elem1] [pad16] ...
```

| Property | Value |
|---|---|
| Initial capacity | `max(16, 16384 / row_size)` where `row_size = sum(elem_sizes)` |
| Growth policy | 2x doubling of the entire block |
| Allocator calls per grow | 1 (vs N per column previously) |

The `entities` vector remains a separate `std::vector`.

**Column Factory Registry:**
A global `map<ComponentTypeID, function<ComponentColumn()>>` is populated by `ensure_column_factory<T>()` on first use of each type. This allows new archetypes to be constructed during migration without compile-time knowledge of the component type at the migration call site.

### 2.5 EntityRecord

```
struct EntityRecord {
    Archetype* archetype;
    size_t row;
};
```

World maintains a parallel array of `EntityRecord` indexed by `Entity::index`. Provides O(1) lookup from any entity handle to its archetype and row, enabling O(1) component access.

---

## 3. World API

`World` is the central registry. It owns all archetypes, entity metadata, and provides the public API.

### 3.1 Entity Lifecycle

| Method | Signature | Description |
|---|---|---|
| `create` | `Entity create()` | Allocate entity in the empty archetype (no components). |
| `create_with` | `Entity create_with<Ts...>(Ts&&...)` | Allocate entity directly into the archetype matching `{Ts...}`. No migration. |
| `destroy` | `void destroy(Entity)` | Remove entity from its archetype via swap-remove. Bump generation. Push index to free-list. |
| `alive` | `bool alive(Entity) const` | Check handle validity (generation match + archetype assigned). |

**create_with** is the preferred creation path. It computes the TypeSet from the template pack, finds or creates the target archetype, and pushes all components in one shot.

**destroy** performs swap-remove: the last entity in the archetype is moved into the destroyed entity's row. The swapped entity's `EntityRecord::row` is updated. This maintains contiguous storage with no gaps.

### 3.2 Component Access

| Method | Signature | Description |
|---|---|---|
| `has<T>` | `bool has<T>(Entity) const` | Test component presence. |
| `get<T>` | `T& get<T>(Entity)` | Direct reference. **Precondition:** entity is alive and has `T`. |
| `try_get<T>` | `T* try_get<T>(Entity)` | Returns pointer or `nullptr`. |

All three are O(1): index into `records_` by entity index, then index into the archetype's column by row.

**Pointer/reference stability:** References returned by `get<T>` and `try_get<T>` are invalidated by any structural change to the same archetype (creation, destruction, or migration of any entity in that archetype). Callers must not hold references across such operations.

### 3.3 Component Mutation (Archetype Migration)

| Method | Signature | Description |
|---|---|---|
| `add<T>` | `void add<T>(Entity, T&&)` | Add component. If already present, overwrites in place (no migration). |
| `remove<T>` | `void remove<T>(Entity)` | Remove component. No-op if not present. |

**Migration procedure (add):**
1. Look up current archetype and row via `EntityRecord`.
2. Compute target TypeSet (current + new component). Check edge cache for target archetype; create if missing.
3. For each column in the target archetype that also exists in the source, move-construct from old row to new archetype (append).
4. Append the new component to its column.
5. Swap-remove the old row from the source archetype. Update the swapped entity's record.
6. Update the migrated entity's record to point to the new archetype and row.

**Migration procedure (remove):** Same as add, but the target TypeSet excludes the removed component. The removed component is destroyed during the swap-remove of the old row.

### 3.4 Utility Queries

| Method | Signature | Description |
|---|---|---|
| `count` | `size_t count() const` | Total live entity count across all archetypes. |
| `count<Ts...>` | `size_t count<Ts...>() const` | Count of entities whose archetype contains all of `{Ts...}`. |
| `single<Ts...>` | `void single<Ts...>(Func&& fn)` | Calls `fn(Entity, Ts&...)` for the one entity matching `{Ts...}`. Asserts if zero or more than one entity matches. |

### 3.5 Query Iteration

```cpp
template <typename... Ts, typename Func>
void each(Func&& fn);
```

Calls `fn(Entity, Ts&...)` for every entity whose archetype contains all of `{Ts...}`.

**Exclude filters:** An overload accepts an `Exclude<Ex...>` tag to skip archetypes containing any of the excluded types:

```cpp
template <typename... Ts, typename... Ex, typename Func>
void each(Exclude<Ex...>, Func&& fn);
```

**Matching:** Queries use an internal cache keyed by `(include_types, exclude_types)`. The cache stores a `vector<Archetype*>` of matching archetypes and is invalidated when new archetypes are created (tracked via a generation counter). This makes repeated queries O(1) when the archetype set is stable. When the cache is invalidated, archetype matching uses bitwise AND+compare on a fixed-size `std::bitset<256>` per archetype (one bit per component type ID), avoiding per-component map lookups. This supports up to 256 distinct component types.

**Iteration:** Within a matched archetype, retrieves typed pointers to each column's raw buffer and indexes linearly. This is the cache-friendly hot path — no indirection per entity.

```cpp
template <typename... Ts, typename Func>
void each_no_entity(Func&& fn);
```

Same as `each` but calls `fn(Ts&...)` without the entity handle. Also supports the `Exclude` overload.

**Constraint:** The callback must not perform structural changes (create, destroy, add, remove) on the world during iteration. Doing so invalidates the column pointers held by the loop. A debug-mode `iterating_` flag asserts on violations. Use `world.deferred()` to queue structural changes for execution after iteration (see §3.6).

### 3.6 Deferred Commands

During `each()` iteration, structural changes are forbidden. The deferred command system lets users queue operations during iteration and apply them afterward.

**World-owned deferred proxy:**

```cpp
World::DeferredProxy deferred();
void flush_deferred();
```

`deferred()` returns a lightweight proxy object with the same structural API as World (`destroy`, `add<T>`, `remove<T>`, `create_with<Ts...>`), but all operations are recorded rather than executed. `flush_deferred()` executes all recorded commands in order and clears the buffer. It asserts `!iterating_`.

Commands are executed in recording order. This means ordering matters — e.g., if `destroy(e)` is recorded before `add(e, T{})`, the entity will be dead when the add runs, and the add becomes a no-op.

`create_with` returns `void` (not `Entity`) since the entity does not exist until flush.

**Standalone CommandBuffer:**

```cpp
class CommandBuffer {
    void destroy(Entity);
    void add<T>(Entity, T&&);
    void remove<T>(Entity);
    void create_with<Ts...>(Ts&&...);
    void flush(World&);
    bool empty() const;
};
```

A standalone command buffer with the same API. Useful when commands need to be accumulated across multiple systems or frames before flushing.

**Implementation:** Both `DeferredProxy` and `CommandBuffer` store commands as `std::function<void(World&)>` lambdas. Component data is kept alive via `std::shared_ptr`. This is simple and correct; performance optimization (custom type-erased storage) is deferred to Phase 7 if profiling warrants it.

### 3.7 Singleton Resources

Typed global data stored on the `World`, independent of entities. Useful for data like delta time, input state, or asset handles that don't belong to any entity.

| Method | Signature | Description |
|---|---|---|
| `set_resource<T>` | `void set_resource(T&& value)` | Insert or overwrite a resource of type `T`. |
| `resource<T>` | `T& resource()` | Get reference. **Precondition:** resource exists (asserts if absent). |
| `try_resource<T>` | `T* try_resource()` | Get pointer, or `nullptr` if absent. |
| `has_resource<T>` | `bool has_resource() const` | Check existence. |
| `remove_resource<T>` | `void remove_resource()` | Erase. No-op if absent. |

Resources use the same `component_id<T>()` namespace as components but occupy separate storage. A type can be used as both a component and a resource simultaneously without conflict.

Resources are destroyed when the `World` is destroyed, or when overwritten/removed. Destruction order of multiple resources is unspecified.

### 3.8 Observers (Component Lifecycle Hooks)

Register callbacks that fire when a component of a given type is added to or removed from any entity:

```cpp
template <typename T>
void on_add(std::function<void(World&, Entity, T&)> fn);

template <typename T>
void on_remove(std::function<void(World&, Entity, T&)> fn);
```

**Semantics:**

- `on_add` fires after the component data is placed in the archetype and the entity's record is updated. This means `get<T>(e)` works inside callbacks.
- `on_remove` fires before the component data is destroyed (before migration or swap-remove).
- `on_add` does **not** fire on overwrite. When `add<T>()` is called on an entity that already has `T`, only the data is updated — no hook fires.
- Multiple hooks per type are supported and fire in registration order.
- Hooks fire for all structural paths: `create_with`, `add` (migration only), `remove`, and `destroy`.

**Re-entry:** Hooks may perform structural changes on **other** entities (e.g., `world.add<X>(other_entity)`), but modifying the observed entity is undefined behavior. This is documented but not enforced at runtime.

**Hook ordering for multi-component operations:**

- `create_with<A, B, C>()`: `on_add` fires per component in template pack order.
- `destroy()`: `on_remove` fires per component in archetype column iteration order (unspecified).

### 3.9 Archetype Sorting

```cpp
template <typename T, typename Compare>
void sort(Compare&& cmp);
```

Sorts all archetypes that contain component `T` by applying `cmp` to pairs of `const T&`. All columns in each matching archetype are rearranged in lockstep, and `EntityRecord::row` is updated for every affected entity.

This is a batch operation — sort order is not maintained incrementally. Call it once per frame where ordering matters (e.g., rendering by depth).

**Precondition:** Not called during iteration (asserts `!iterating_`).

**Algorithm:** For each matching archetype, builds an index array sorted by the comparator, then applies the resulting permutation via in-place cycle-chasing swaps. Column element swaps use a type-erased `SwapFunc` stored on each `ComponentColumn`.

### 3.10 Serialization

**Type Registration:**

```cpp
template <typename T>
void register_component(const char* name,
                        ComponentColumn::SerializeFunc ser = nullptr,
                        ComponentColumn::DeserializeFunc deser = nullptr);
```

Registers a component type with a stable string name for serialization. The name must be unique across all registered types (asserts on conflict). Double-registration with the same name and type is idempotent.

For trivially copyable types, serialize/deserialize functions are auto-generated (memcpy-based) if not provided. For non-trivially-copyable types (e.g., `Children` with `std::vector<Entity>`), explicit serialize/deserialize functions must be provided.

**Lookup helpers:**

| Function | Signature | Description |
|---|---|---|
| `component_id_by_name` | `ComponentTypeID component_id_by_name(const std::string& name)` | Look up ID by name. Asserts if not found. |
| `component_name` | `const std::string& component_name(ComponentTypeID id)` | Look up name by ID. Asserts if not found. |
| `component_registered` | `bool component_registered(ComponentTypeID id)` | Check if a type ID has been registered. |

**World Snapshot:**

```cpp
void serialize(const World& world, std::ostream& out);
void deserialize(World& world, std::istream& in);
```

Binary serialization of the entire world state. All component types present in the world must be registered (asserts otherwise). The target world for `deserialize` must be empty (asserts otherwise).

**Binary format:** Header (magic `"ECS\0"`, version, archetype count, entity slot count) followed by per-archetype blocks (component names, element sizes, serialized column data, entity list) and an entity table (generations, free list).

Round-trip preserves: all entities (alive and destroyed), component data, archetype structure, generation counters, and free list. Resources, observers, and deferred commands are not serialized.

### 3.11 Prefabs

Prefabs are reusable entity templates with default component values. They are data, not entities — they don't appear in queries.

**Creation:**

```cpp
Prefab enemy = Prefab::create(Position{0, 0}, Velocity{0, 0}, Health{100});
```

All component types in a prefab must be **copy-constructible** (enforced via `static_assert`). The prefab stores type-erased defaults in a flat byte buffer.

**Instantiation (free functions):**

```cpp
Entity e = instantiate(world, enemy);                    // copy all defaults
Entity e = instantiate(world, enemy, Position{10, 5});   // override Position
Entity e = instantiate(world, enemy, Velocity{5, 0}, Tag{});  // override + extend
```

- `instantiate(world, prefab)` creates an entity with all prefab defaults copied.
- `instantiate(world, prefab, overrides...)` applies overrides. If an override type exists in the prefab, the override value replaces the default. If an override type is NOT in the prefab, it extends the entity (the entity gets the extra component in addition to all prefab defaults).

**Querying a prefab:**

| Method | Signature | Description |
|---|---|---|
| `has<T>` | `bool has<T>() const` | Check if prefab contains component type `T`. |
| `component_count` | `size_t component_count() const` | Number of component types in the prefab. |

**Observers:** `on_add` hooks fire for all components when an entity is instantiated from a prefab.

**Prefab copying:** Prefabs are copyable and movable. Copying a prefab copy-constructs all stored component defaults.

---

## 4. System Registry

```cpp
class SystemRegistry {
    void add(std::string name, SystemFunc fn);
    void run_all(World& world);
};
```

An ordered list of named `function<void(World&)>`. Systems execute in insertion order. No dependency resolution, no parallelism. This is intentionally minimal — scheduling belongs in the application layer or a future extension.

**Deferred command flush:** `run_all` calls `world.flush_deferred()` after each system returns. This ensures deferred commands from system N are applied before system N+1 runs, giving each system a consistent view of the world.

---

## 5. Builtin Components

These are ordinary components shipped with the library for convenience. They have no special privileges in the core.

### 5.1 Transform

| Type | Fields | Description |
|---|---|---|
| `Mat4` | `float m[16]` | Column-major 4x4 matrix. Provides `identity()`, `multiply()`, `translation()`. |
| `LocalTransform` | `Mat4 matrix` | Transform relative to parent (or world origin if no parent). |
| `WorldTransform` | `Mat4 matrix` | Computed absolute transform. Written by the propagation system. |

### 5.2 Hierarchy

| Type | Fields | Description |
|---|---|---|
| `Parent` | `Entity entity` | Points to parent entity. |
| `Children` | `vector<Entity> entities` | Lists child entities. |

**Managed hierarchy operations** (free functions in `builtin/hierarchy_ops.hpp`):

| Function | Signature | Description |
|---|---|---|
| `set_parent` | `void set_parent(World&, Entity child, Entity parent)` | Set child's parent, keeping both sides in sync. Handles re-parenting (removes from old parent's Children). Creates Children component on parent if absent. No-op if child or parent is dead. Asserts on self-parenting. |
| `remove_parent` | `void remove_parent(World&, Entity child)` | Orphan child: remove from parent's Children, remove Parent component. No-op if child is dead or has no parent. |
| `destroy_recursive` | `void destroy_recursive(World&, Entity root)` | Destroy entity and all descendants via BFS. Destroys leaves first to avoid dangling references. |

These functions are the recommended way to manage hierarchy. Direct manipulation of `Parent` and `Children` is still possible but requires the application to maintain consistency.

### 5.3 Transform Propagation

```cpp
void propagate_transforms(World& world);
```

BFS traversal:
1. Query entities with `LocalTransform` + `WorldTransform` but no `Parent` → roots.
2. For each root: set `WorldTransform = LocalTransform`, enqueue its children.
3. For each child: `WorldTransform = parent.WorldTransform * child.LocalTransform`, enqueue its children.

Uses `try_get<T>()` per entity during BFS (O(1) each). Not maximally cache-friendly for the BFS portion, but adequate for expected entity counts (hundreds, not millions).

---

## 6. Invariants

These must hold at all times outside of an in-progress structural operation:

1. **Entity array ↔ column parity.** Within an archetype, `entities.size() == columns[cid].count` for all columns.
2. **Record consistency.** For every live entity `e`, `records_[e.index].archetype->entities[records_[e.index].row] == e`.
3. **Generation monotonicity.** `generations_[i]` never decreases for any index `i`.
4. **No dangling archetype pointers.** Archetypes are owned by the world (`unique_ptr`) and are never destroyed during the world's lifetime. Edge cache pointers remain valid.
5. **No duplicate types in a TypeSet.** Each ComponentTypeID appears at most once, and the vector is sorted.
6. **Index 0 reservation.** `generations_[0]` is initialized to 1, so `INVALID_ENTITY{0,0}` can never match a live entity.

---

## 7. Known Limitations

These are accepted constraints of the current implementation, not bugs.

1. **Not thread-safe.** No synchronization on any data structure. A single World must be accessed from one thread at a time.
2. **No structural changes during iteration.** `each()` holds raw pointers into column buffers. Direct structural changes during a callback trigger a debug assertion. Use `world.deferred()` to queue changes safely (see §3.6).
3. **Component type IDs are not stable across builds.** IDs are assigned by call order, which can vary with compiler, link order, or code changes. Use `register_component<T>(name)` to assign stable string names for serialization (see §3.10).
4. **Global column factory registry.** The factory map is a process-wide singleton. Multiple `World` instances share it (harmless in practice, but not isolated).
5. **Migration cost.** Adding/removing a component moves all of an entity's components to a new archetype. Frequent single-component changes on entities with many components are expensive.
6. **Hierarchy consistency requires helper functions.** Use `set_parent`, `remove_parent`, and `destroy_recursive` (from `builtin/hierarchy_ops.hpp`) for automatic bidirectional consistency. Direct manipulation of `Parent` and `Children` is possible but the application must keep both sides in sync.
7. **Deferred command overhead.** Each deferred command allocates a `std::function` + `std::shared_ptr` for component data. Adequate for typical use; custom type-erased storage is a potential Phase 7 optimization.

---

## 8. Roadmap

Planned features, roughly ordered by priority. Each item should get its own spec section before implementation. See `IMPLEMENTATION.md` for detailed phased plan and progress tracking.

### 8.0 Completed

- ~~Deferred command buffer~~ — §3.6. Queue structural changes during iteration, flush after.
- ~~Query caching~~ — §3.5. Cached per query signature, invalidated on archetype creation.
- ~~Exclude filters~~ — §3.5. `each<A, B>(Exclude<C>{}, fn)`.
- ~~Utility queries~~ — §3.4. `count()`, `count<Ts...>()`, `single<Ts...>()`.
- ~~Debug-mode invariant checks~~ — `ECS_ASSERT` guards on all structural operations.
- ~~Sanitizer build~~ — `cmake -DECS_SANITIZE=ON`.
- ~~Singleton resources~~ — §3.7. Typed global data on World, independent of entities.
- ~~Observers / hooks~~ — §3.8. Component lifecycle callbacks (`on_add`, `on_remove`).
- ~~Automatic hierarchy consistency~~ — §5.2. Managed operations (`set_parent`, `remove_parent`, `destroy_recursive`).
- ~~Archetype sorting~~ — §3.9. Sort entities within archetypes by a component comparator.
- ~~Bitset archetype matching~~ — §3.5. Fixed-size bitset per archetype for fast query matching.
- ~~Serialization~~ — §3.10. Stable type registration and binary world snapshots.
- ~~Prefabs~~ — §3.11. Reusable entity templates with default component values.

### 8.2 Long-Term

- **Parallel iteration.** Split archetype iteration across threads. Requires read/write access declarations per system and a dependency-aware scheduler.
- **Scripting bridge.** Type-erased component access for dynamic languages (Python, Lua). Likely via string-keyed component lookup and void* accessors.

---

## 9. File Map

```
ecs/
├── CMakeLists.txt                              Build: header-only INTERFACE lib + test exe
├── SPEC.md                                     This document
├── IMPLEMENTATION.md                           Phased build plan and progress tracker
├── CLAUDE.md                                   Claude Code guidelines
├── include/ecs/
│   ├── ecs.hpp                                 Convenience include-all
│   ├── entity.hpp                              Entity, INVALID_ENTITY, EntityHash
│   ├── component.hpp                           ComponentTypeID, component_id<T>(), ComponentColumn, column factory
│   ├── archetype.hpp                           TypeSet, TypeSetHash, Archetype, ArchetypeEdge
│   ├── world.hpp                               World (main API), EntityRecord, DeferredProxy, query cache
│   ├── serialization.hpp                       serialize(), deserialize() (binary world snapshots)
│   ├── command_buffer.hpp                      CommandBuffer (standalone deferred command queue)
│   ├── prefab.hpp                              Prefab, instantiate() (reusable entity templates)
│   ├── system.hpp                              SystemRegistry (auto-flushes deferred commands)
│   └── builtin/
│       ├── transform.hpp                       Mat4, LocalTransform, WorldTransform
│       ├── hierarchy.hpp                       Parent, Children
│       ├── hierarchy_ops.hpp                   set_parent(), remove_parent(), destroy_recursive()
│       └── transform_propagation.hpp           propagate_transforms()
├── tests/
│   └── main.cpp                                Test suite
└── examples/
    ├── visual_harness/                         Raylib visual demo
    └── stress_harness/                         Performance stress test
```

---

## Appendix A: Archetype Migration Diagram

```
Entity e has components [A, B] in Archetype_AB.
User calls: world.add(e, C{...})

  Archetype_AB                          Archetype_ABC
  ┌─────────┬─────────┐                ┌─────────┬─────────┬─────────┐
  │ col_A   │ col_B   │                │ col_A   │ col_B   │ col_C   │
  │─────────│─────────│   migrate →    │─────────│─────────│─────────│
  │ [e's A] │ [e's B] │   ────────→    │ [e's A] │ [e's B] │ [new C] │
  │ ...     │ ...     │                │ ...     │ ...     │ ...     │
  └─────────┴─────────┘                └─────────┴─────────┴─────────┘

  1. Move A, B from old row → append to Archetype_ABC columns
  2. Append new C value
  3. Swap-remove old row in Archetype_AB
  4. Update EntityRecord for migrated entity (and swapped entity if any)
```

## Appendix B: Swap-Remove Diagram

```
Archetype with 4 entities, destroying entity at row 1:

  Before:          After swap-remove:
  row 0: [e0]      row 0: [e0]
  row 1: [e1] ←del row 1: [e3]  (was last, moved here)
  row 2: [e2]      row 2: [e2]
  row 3: [e3]      count: 3

  EntityRecord for e3 updated: row = 1
```
