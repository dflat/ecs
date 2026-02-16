# RFC-0000: Library Baseline

* **Status:** Baseline/Implemented
* **Date:** February 2026

## Summary

This RFC documents the baseline state of the ECS library as of Phases 0-8
completion. It captures the current architecture, capabilities, and known
technical debt to serve as a reference point for future RFCs.

## Tech Stack

- **Language:** C++17 (header-only)
- **Dependencies:** None (stdlib only)
- **Build:** CMake 3.14+, tested with GCC/Clang/MSVC
- **Testing:** Custom test harness with `sigjmp_buf`/`SIGABRT` for assertion tests
- **CI:** Sanitizer builds (`-fsanitize=address,undefined`)

## Core Architecture

### Data Model
- **Archetype-based ECS:** Entities with identical component sets share an
  `Archetype`. Components stored in Structure-of-Arrays (SoA) layout for
  cache-friendly iteration.
- **Entity handles:** 64-bit (32-bit index + 32-bit generation) with free-list
  recycling. Stale handles detected via generation mismatch.
- **Component columns:** Type-erased (`ComponentColumn`) with function pointers
  for move/destroy/swap/serialize. Memory owned by a single `malloc` block per
  archetype.

### Query System
- Bitset-based matching (`std::bitset<256>`) for fast archetype filtering.
- Query cache keyed by `(include_ids, exclude_ids)`, invalidated by archetype
  generation counter.
- Supports `each<Ts...>()`, `each_no_entity<Ts...>()`, `Exclude<Ex...>` filters,
  and `single<Ts...>()`.

### Structural Changes
- Adding/removing components migrates entities between archetypes.
- Edge cache memoizes archetype transitions (e.g., `[A,B] + C -> [A,B,C]`).
- `CommandBuffer` provides deferred structural changes safe for use during
  iteration. Commands stored in a linear byte buffer (zero per-command allocation).

### Modules (Phases 5-6)
- **Hierarchy:** `Parent`/`Children` components, `set_parent()`,
  `remove_parent()`, `destroy_recursive()`.
- **Transforms:** `LocalTransform`/`WorldTransform` with `propagate_transforms()`
  BFS propagation.

### Observers (Phase 4)
- `on_add<T>()` and `on_remove<T>()` lifecycle hooks.
- Fire synchronously during structural changes.

### Serialization (Phase 8)
- `register_component<T>(name)` for stable cross-build type identity.
- Binary serialize/deserialize for trivially-copyable types; custom
  serializers for complex types.

### Prefabs (Phase 8)
- Type-erased templates that copy-construct entities from stored defaults.
- `instantiate(world, prefab, overrides...)` for override-on-spawn.

## Completed Phases

| Phase | Description |
|-------|-------------|
| 0 | Core types: Entity, ComponentColumn, Archetype |
| 1 | World: create, destroy, add, remove, get, has |
| 2 | Query iteration: each, each_no_entity, Exclude |
| 3 | CommandBuffer for deferred structural changes |
| 4 | Observers: on_add, on_remove hooks |
| 5 | Hierarchy module: Parent/Children, destroy_recursive |
| 6 | Transform module: LocalTransform, WorldTransform, propagation |
| 7 | Performance: bitset matching, edge cache, query cache, sorting |
| 8 | Serialization, type registration, prefabs |

## Known Technical Debt

### Performance

1. **`std::map` for archetype columns** (`archetype.hpp`): Column lookup is
   O(log N) with pointer chasing. Should be a flat array or small sorted vector
   for the typical 3-10 components per archetype.

2. **Query cache allocates on every call** (`world.hpp`): `cached_query()`
   constructs two `std::vector`s (the `QueryKey`) per `each<>()` invocation,
   even on cache hits. Should use stack-allocated keys.

3. **`std::map` for edge cache** (`archetype.hpp`): Same O(log N) issue for
   archetype transition lookups during migration.

4. **`std::function` for hooks**: Heap allocation and virtual dispatch overhead
   on every hook invocation during structural changes.

### Correctness

5. **`iterating_` flag doesn't handle nesting**: A single `bool` means nested
   `each()` calls leave the flag in the wrong state. The inner query's Guard
   destructor sets `iterating_ = false` while the outer loop is still active.
   Should be an integer counter.

### Missing Features

6. **No batch operations**: No `destroy_all<T>()` or `clear()`. Scene resets
   require collecting entities into a vector then destroying one-by-one.

7. **No event/signal system**: Systems can only communicate by reading/writing
   shared components. No publish/subscribe for discrete events.

8. **No parallel iteration** (Phase 10 planned): All iteration is
   single-threaded.

9. **No scripting bridge** (Phase 9 planned): No type-erased component access
   for dynamic languages.

10. **256 component type limit**: Fixed `bitset<256>` caps the number of
    distinct component types. Sufficient for now but may need expansion.
