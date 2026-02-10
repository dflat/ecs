# Implementation Plan

Phased plan for evolving the ECS library from its current v0.1 foundation toward
the full roadmap described in SPEC.md. Each phase is a self-contained unit of work
that compiles, passes all tests, and can be committed independently.

Within each phase, items are ordered by dependency — later items may depend on
earlier ones. Items across phases are independent unless noted.

---

## Progress

> Update this table as items are completed. Mark `[x]` when done, `[~]` for
> in-progress, `[ ]` for not started. Each item should be committed and verified
> before marking complete.

### Phase 0 — Hardening & Test Infrastructure
- [x] 0.1 Debug-mode invariant checks
- [x] 0.2 Expand test coverage
- [x] 0.3 Sanitizer build configuration

### Phase 1 — Query Improvements
- [x] 1.1 Exclude filters
- [x] 1.2 Utility queries (`count`, `single`)
- [x] 1.3 Query caching

### Phase 2 — Deferred Commands
- [x] 2.1 CommandBuffer
- [x] 2.2 Integration with iteration

### Phase 3 — Singleton Resources
- [ ] 3.1 Resource storage

### Phase 4 — Observers
- [ ] 4.1 Component lifecycle hooks

### Phase 5 — Automatic Hierarchy Consistency
- [ ] 5.1 Managed hierarchy operations
- [ ] 5.2 Refactor transform propagation

### Phase 6 — Archetype Sorting
- [ ] 6.1 Sort entities within an archetype

### Phase 7 — Performance Foundations
- [ ] 7.1 Bitset archetype matching
- [ ] 7.2 Chunk allocation

### Phase 8 — Serialization
- [ ] 8.1 Stable type registration
- [ ] 8.2 World snapshot (binary)

### Phase 9 — Scripting Bridge
- [ ] 9.1 Type-erased component access
- [ ] 9.2 Python bindings (pybind11)

### Phase 10 — Parallel Iteration
- [ ] 10.1 System access declarations
- [ ] 10.2 Parallel dispatch

### Phase 11 — Prefabs
- [ ] 11.1 Prefab templates

---

## Phase 0 — Hardening & Test Infrastructure

Solidify the foundation before building on it. Everything here addresses gaps in
the v0.1 implementation that would become harder to fix later.

### 0.1 Debug-mode invariant checks

Add an `ECS_ASSERT` macro (defaults to `assert`, overridable). Insert checks at
structural boundaries:

- After every `push_entity` / `swap_remove`: verify entity-column parity
  (SPEC §6.1).
- In `get<T>`: verify entity is alive and has `T`.
- In `each()`: set a `world.iterating_` flag on entry, clear on exit. Assert
  it is false in `create`, `destroy`, `add`, `remove` — catches the
  "no structural changes during iteration" violation at dev time (SPEC §7.2).

**Files:** `component.hpp`, `archetype.hpp`, `world.hpp`
**Verify:** Existing tests still pass. New test that intentionally violates the
iteration guard triggers an assertion failure (via death-test or try/catch on a
custom assert handler).

### 0.2 Expand test coverage

Add targeted tests for edge cases not covered by the v0.1 suite:

- Create and destroy the same entity index multiple times (generation wraps).
- Migrate an entity through 3+ archetypes in sequence (A → AB → ABC → AB → A).
- Destroy all entities in an archetype, then create new ones in it.
- Component type with non-trivial move semantics (`std::string`,
  `std::unique_ptr`).
- Empty `each<>()` over a world with no matching archetypes (zero iterations,
  no crash).
- `create_with` with a single component. `create_with` with 5+ components.
- `add<T>` when entity already has `T` (overwrite path).
- `try_get` on dead entity returns `nullptr`.

**Files:** `tests/main.cpp`
**Verify:** All new tests pass. No memory errors under sanitizer (`-fsanitize=address,undefined`).

### 0.3 Sanitizer CI build configuration

Add a CMake preset or option for sanitizer builds:

```cmake
option(ECS_SANITIZE "Enable ASan + UBSan" OFF)
```

When enabled, add `-fsanitize=address,undefined -fno-omit-frame-pointer` to
compile and link flags.

**Files:** `CMakeLists.txt`
**Verify:** `cmake -DECS_SANITIZE=ON .. && make && ./ecs_test` runs clean.

---

## Phase 1 — Query Improvements

### 1.1 Exclude filters

Add an `Exclude<Ts...>` tag type and an overload of `each`:

```cpp
template <typename... Include, typename... Ex, typename Func>
void each(Exclude<Ex...>, Func&& fn);
```

Archetype matching: must contain all `Include` types and none of the `Ex` types.

**Files:** `world.hpp`
**Verify:** Test: entities with `[A, B]` match `each<A>(Exclude<C>{}, fn)`,
entities with `[A, C]` do not. Refactor `propagate_transforms` to use
`Exclude<Parent>` instead of the per-entity `has<Parent>` check.

### 1.2 Utility queries

Add convenience methods to `World`:

| Method | Description |
|---|---|
| `count()` | Total live entity count. |
| `count<Ts...>()` | Count of entities matching `Ts...`. |
| `single<Ts...>()` | Returns `(Entity, Ts&...)` for a query expected to match exactly one entity. Asserts on 0 or 2+. |

**Files:** `world.hpp`
**Verify:** Tests for each method, including `single` assertion on empty and
multi-match.

### 1.3 Query caching

Introduce a `QueryCache` that stores, per TypeSet key, a `vector<Archetype*>` of
matching archetypes. Invalidated when a new archetype is created (bump a
generation counter on World; cache checks its snapshot).

`each()` and `each_no_entity()` use the cache internally. External API unchanged.

**Files:** `world.hpp` (or new `query.hpp` if it grows large)
**Verify:** Functional: all existing tests pass. Behavioral: add a test that
creates entities, runs a query, creates entities in a new archetype, runs the
query again — second run picks up the new archetype.

---

## Phase 2 — Deferred Commands

### 2.1 CommandBuffer

New type that records structural operations without executing them:

```cpp
class CommandBuffer {
    void create_with(...);  // type-erased variant
    void destroy(Entity);
    void add<T>(Entity, T&&);
    void remove<T>(Entity);
    void flush(World&);     // apply all recorded commands
};
```

Commands are stored as a `vector<Command>` where `Command` is a variant/tagged
union of the four operation types. Component data is stored inline via
type-erased blobs (reuse `ComponentColumn`'s move/destroy pattern).

**Files:** new `include/ecs/command_buffer.hpp`, update `ecs.hpp`
**Verify:** Test: record several creates, destroys, adds, and removes into a
buffer. Flush. Verify world state matches expectations. Test: flush an empty
buffer is a no-op.

### 2.2 Integration with iteration

Add `World::deferred()` which returns a `CommandBuffer&` owned by the world.
Auto-flushed at a defined point (e.g., after `SystemRegistry::run_all`, or
manually). This lets system callbacks safely call `world.deferred().destroy(e)`
during `each()`.

**Files:** `world.hpp`, `system.hpp`, `command_buffer.hpp`
**Verify:** Test: system that destroys entities via deferred commands during
`each()`. After `run_all`, destroyed entities are gone. Test: system that adds
a component via deferred — component is present after flush.

---

## Phase 3 — Singleton Resources

### 3.1 Resource storage

Add typed resource storage to `World`:

```cpp
template <typename T> void set_resource(T&& resource);
template <typename T> T& resource();
template <typename T> T* try_resource();
template <typename T> bool has_resource() const;
template <typename T> void remove_resource();
```

Stored in a `map<ComponentTypeID, unique_ptr<void, deleter>>` (or a small
type-erased wrapper). Uses the same `component_id<T>()` namespace as
components — no conflict since resources and components occupy separate storage.

**Files:** `world.hpp`
**Verify:** Test: set, get, overwrite, remove a resource. Test: `try_resource`
returns `nullptr` when absent. Test: resource with non-trivial destructor is
cleaned up on world destruction.

---

## Phase 4 — Observers

### 4.1 Component lifecycle hooks

Register callbacks that fire when a component of a given type is added to or
removed from any entity:

```cpp
template <typename T>
void on_add(function<void(World&, Entity, T&)> fn);

template <typename T>
void on_remove(function<void(World&, Entity, T&)> fn);
```

Hooks fire synchronously at the point of structural change — inside `add<T>`
(after data is placed in the new archetype) and inside `remove<T>` / `destroy`
(before data is destroyed). Multiple hooks per type are supported and fire in
registration order.

**Files:** `world.hpp`
**Verify:** Test: `on_add<Health>` fires when `Health` is added via `add()` and
via `create_with()`. Test: `on_remove<Health>` fires on `remove<Health>()` and
on `destroy()` of an entity that has `Health`. Test: hook receives correct entity
and component reference.

---

## Phase 5 — Automatic Hierarchy Consistency

### 5.1 Managed hierarchy operations

Add helper methods that keep `Parent` and `Children` in sync:

```cpp
void set_parent(Entity child, Entity parent);
void remove_parent(Entity child);
void destroy_recursive(Entity e);  // destroy entity and all descendants
```

`set_parent`:
1. If child already has a `Parent`, remove child from old parent's `Children`.
2. Add/set `Parent{new_parent}` on child.
3. Add child to new parent's `Children` (create `Children` component if absent).

`destroy_recursive`:
1. Collect all descendants via BFS.
2. Destroy in reverse order (leaves first).

**Files:** `world.hpp` or new `include/ecs/builtin/hierarchy_ops.hpp`
**Verify:** Test: `set_parent` correctly updates both sides. Test: re-parenting
cleans up old parent's children list. Test: `destroy_recursive` removes entire
subtree. Test: destroying a leaf doesn't affect siblings.

### 5.2 Refactor transform propagation

Update `propagate_transforms` to use `Exclude<Parent>` (from Phase 1.1) for
root detection. If already done in 1.1, this is just verification.

**Files:** `builtin/transform_propagation.hpp`
**Verify:** Hierarchy propagation tests still pass.

---

## Phase 6 — Archetype Sorting

### 6.1 Sort entities within an archetype

Add a method to sort all entities in an archetype by a user-provided comparator
applied to one of their component types:

```cpp
template <typename T, typename Compare>
void sort(Compare&& cmp);
```

Sorting rearranges all columns in lockstep (permutation sort). Updates
`EntityRecord::row` for every affected entity.

This is a batch operation, not maintained incrementally. Call it once per frame
where ordering matters (e.g., rendering by depth).

**Files:** `world.hpp`
**Verify:** Test: create entities with a `Depth{float}` component in random
order. Call `sort<Depth>(...)`. Verify iteration order matches expected sort.
Verify `get<T>` still works for all entities after sort.

---

## Phase 7 — Performance Foundations

### 7.1 Bitset archetype matching

Replace the linear `has_component` check in `each()` with a fixed-size bitset
per archetype. Set bit `component_id<T>()` for each component in the archetype.
Query matching becomes a bitwise AND + compare.

Requires a max component type count (e.g., 256) or a dynamic bitset. Start with
a fixed `std::bitset<256>`.

**Files:** `archetype.hpp`, `world.hpp`
**Verify:** All tests pass. Benchmark (manual or scripted) shows reduced query
matching time with many archetypes.

### 7.2 Chunk allocation

Replace per-column `malloc`/`free` with a chunk-based allocator. Each archetype
allocates fixed-size chunks (e.g., 16KB). Columns within a chunk are laid out
contiguously. Benefits:

- Fewer allocator calls.
- Better TLB behavior.
- Natural alignment boundary for future SIMD.

This is a significant internal refactor with no API change.

**Files:** `component.hpp`, `archetype.hpp`
**Verify:** All tests pass. Sanitizer clean. Memory usage profile is comparable
or better.

---

## Phase 8 — Serialization

### 8.1 Stable type registration

Add a string-based type registry alongside the numeric IDs:

```cpp
template <typename T>
void register_component(const char* name);
```

Populates a bidirectional map: `name ↔ ComponentTypeID`. Required for
serialization — numeric IDs are not stable across builds.

**Files:** `component.hpp`
**Verify:** Test: register two types, look up by name, get correct IDs. Test:
double-registration with same name is idempotent. Test: conflicting names assert.

### 8.2 World snapshot (binary)

```cpp
void serialize(const World& world, std::ostream& out);
void deserialize(World& world, std::istream& in);
```

Format: header (entity count, archetype count) → per-archetype (type names,
entity count, raw column data) → entity table (index, generation, archetype
index, row).

Only registered component types can be serialized. Unregistered types in a world
cause a runtime error on serialize.

**Files:** new `include/ecs/serialization.hpp`
**Verify:** Test: round-trip a world with multiple archetypes. Verify all
entities, components, and hierarchy survive. Test: serialize world with
unregistered type fails cleanly.

---

## Phase 9 — Scripting Bridge

### 9.1 Type-erased component access

Add runtime (string-keyed) access to components for dynamic languages:

```cpp
void* get_raw(Entity e, const char* component_name);
bool  has_raw(Entity e, const char* component_name);
```

Requires type registration from Phase 8.1.

**Files:** `world.hpp`
**Verify:** Test: access a registered component by string name, mutate it via
`void*`, verify change visible through typed `get<T>`.

### 9.2 Python bindings (pybind11)

Expose `World`, `Entity`, and the raw access API to Python. This is an optional
build target, not part of the header-only core.

**Files:** new `bindings/python/` directory, separate `CMakeLists.txt`
**Verify:** Python test script creates entities, adds components, queries, and
destroys.

---

## Phase 10 — Parallel Iteration

### 10.1 System access declarations

Extend `SystemRegistry::add` to accept read/write access metadata:

```cpp
systems.add("movement", access<Position>(ReadWrite), access<Velocity>(ReadOnly), fn);
```

Build a dependency graph: systems that write to a component conflict with
any system that reads or writes the same component. Non-conflicting systems
can run in parallel.

**Files:** `system.hpp`
**Verify:** Test: declare systems with overlapping and non-overlapping access.
Verify dependency graph is correct (unit test the graph, not actual threading).

### 10.2 Parallel dispatch

`SystemRegistry::run_all_parallel(World&, thread_pool&)` executes independent
systems concurrently. Systems with dependencies run in topological order.

Archetype iteration within a single system is not parallelized in this phase —
that requires per-archetype chunk splitting (Phase 7.2).

**Files:** `system.hpp`
**Verify:** Test: two independent systems produce correct results when dispatched
in parallel. Stress test under thread sanitizer (`-fsanitize=thread`).

---

## Phase 11 — Prefabs

### 11.1 Prefab templates

A `Prefab` is a stored archetype template with default component values:

```cpp
Prefab prefab = world.create_prefab<Position, Velocity, Health>(
    Position{0, 0}, Velocity{0, 0}, Health{100}
);

Entity e = world.instantiate(prefab);           // copy defaults
Entity e = world.instantiate(prefab, overrides); // override specific components
```

Prefabs are data, not entities. They don't appear in queries.

**Files:** new `include/ecs/prefab.hpp`, update `world.hpp`
**Verify:** Test: instantiate a prefab, verify defaults. Test: instantiate with
override, verify override applied and non-overridden defaults preserved. Test:
instantiate 1000 entities from same prefab, verify all independent.

---

## Summary

| Phase | Focus | Depends On |
|-------|-------|------------|
| 0 | Hardening & test infra | — |
| 1 | Query improvements (exclude, count, caching) | 0 |
| 2 | Deferred command buffer | 0 |
| 3 | Singleton resources | 0 |
| 4 | Observers | 2 |
| 5 | Hierarchy consistency | 1.1, 4 |
| 6 | Archetype sorting | 0 |
| 7 | Performance (bitset, chunks) | 1 |
| 8 | Serialization | 0 |
| 9 | Scripting bridge | 8.1 |
| 10 | Parallel iteration | 7 |
| 11 | Prefabs | 0 |

Phases 1, 2, 3, 6, 8, and 11 are independent of each other (all depend only on
Phase 0). They can be implemented in any order or in parallel. Phases 4, 5, 7, 9,
and 10 have explicit upstream dependencies as shown.
