# ECS Library Specification

**Version:** 0.3.0
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
- Serialization. No built-in save/load.
- Reactive/event systems. No observers or change detection.
- Maximum performance at extreme scale (100k+ entities). The current design prioritizes correctness and clarity. Optimization (e.g., bitset archetype matching, chunk allocation) is deferred.
- Singleton resources. World-level unique data not yet supported.

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

Returns a unique `uint32_t` for each type `T`, assigned on first call via a static counter. Deterministic within a single program execution but not stable across builds or translation units that vary call order. (Type IDs are not suitable for serialization keys.)

### 2.3 Archetype

A unique combination of component types. Identified by a **TypeSet**: a sorted `vector<ComponentTypeID>`.

Each archetype owns:
- One **ComponentColumn** per component type (SoA storage).
- A parallel `vector<Entity>` tracking which entity occupies each row.
- An **edge cache** (`map<ComponentTypeID, ArchetypeEdge>`) for O(1) amortized archetype lookup when adding/removing components.

**Invariant:** For every archetype, all columns and the entity vector have identical length (the archetype's entity count).

### 2.4 ComponentColumn (Type-Erased Storage)

A dynamically-sized array for a single component type within an archetype.

| Property | Value |
|---|---|
| Backing memory | `malloc`/`free`, `uint8_t*` buffer |
| Element lifecycle | Placement-new via move constructor; explicit destructor calls |
| Growth policy | 2x capacity, initial capacity 16 |
| Deletion policy | Swap-remove: last element is move-constructed over the deleted slot, maintaining density |

**Function pointers** (`MoveFunc`, `DestroyFunc`) are captured at column creation from the concrete type via `make_column<T>()`. This allows type-erased operations without virtual dispatch.

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

**Matching:** Queries use an internal cache keyed by `(include_types, exclude_types)`. The cache stores a `vector<Archetype*>` of matching archetypes and is invalidated when new archetypes are created (tracked via a generation counter). This makes repeated queries O(1) when the archetype set is stable.

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

Hierarchy is maintained manually: the application is responsible for keeping `Parent` and `Children` consistent. (Automatic consistency enforcement is a potential future feature.)

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
3. **Component type IDs are not stable across builds.** IDs are assigned by call order, which can vary with compiler, link order, or code changes. Cannot be used as serialization keys.
4. **Global column factory registry.** The factory map is a process-wide singleton. Multiple `World` instances share it (harmless in practice, but not isolated).
5. **Migration cost.** Adding/removing a component moves all of an entity's components to a new archetype. Frequent single-component changes on entities with many components are expensive.
6. **Hierarchy consistency is manual.** `Parent` and `Children` can become inconsistent if the application doesn't update both sides.
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

### 8.1 Near-Term

- **Singleton resources.** World-level unique data (e.g., `DeltaTime`, `InputState`) accessible without entity queries.
- **Observers / hooks.** Register callbacks for component add/remove events on specific types. Enables reactive patterns without polling.

### 8.2 Mid-Term

- **Automatic hierarchy consistency.** Adding `Parent` to an entity automatically updates the parent's `Children`, and vice versa. Destruction of a parent cascades or orphans children (configurable).
- **Sorting within archetypes.** Allow sorting an archetype's entities by a component field for spatial or rendering order.

### 8.3 Long-Term

- **Performance foundations.** Bitset archetype matching, chunk allocation for better memory patterns.
- **Parallel iteration.** Split archetype iteration across threads. Requires read/write access declarations per system and a dependency-aware scheduler.
- **Serialization.** Stable type IDs (string-based or hash-based registration) and binary/JSON world snapshots.
- **Scripting bridge.** Type-erased component access for dynamic languages (Python, Lua). Likely via string-keyed component lookup and void* accessors.
- **Prefabs / templates.** Define archetype templates for batch entity creation with shared defaults.

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
│   ├── command_buffer.hpp                      CommandBuffer (standalone deferred command queue)
│   ├── system.hpp                              SystemRegistry (auto-flushes deferred commands)
│   └── builtin/
│       ├── transform.hpp                       Mat4, LocalTransform, WorldTransform
│       ├── hierarchy.hpp                       Parent, Children
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
