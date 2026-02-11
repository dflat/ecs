# Pre-Phase 9 Housekeeping

Work to complete before approaching the scripting layer (Phase 9). Tracked
separately from `IMPLEMENTATION.md` to keep the main plan focused on feature
phases.

---

## Progress

> Mark `[x]` when done, `[~]` for in-progress, `[ ]` for not started.

### Workstream A — Performance (High-Impact Fixes)

- [x] A.1 Deferred command arena allocation
- [x] A.2 Observer hook optimization
- [x] A.3 Malloc null check in chunk allocation
- [x] A.4 Document all performance issues (`PERFORMANCE.md`)
- [x] A.5 Benchmarking harness

### Workstream B — Testing Coverage

- [x] B.1 Exception safety tests
- [x] B.2 Reference invalidation tests
- [x] B.3 Circular hierarchy prevention tests
- [x] B.4 Serialization edge case tests
- [x] B.5 Stress / boundary tests

### Workstream C — API Formalization for Scripting

- [x] C.1 Type-erased C API surface design
- [x] C.2 Scripting layer architecture document

---

## Execution Order

1. **A.5** — benchmark harness (establish baseline)
2. **A.1–A.3** — performance fixes (measure against baseline)
3. **A.4** — document remaining issues
4. **B.1–B.5** — testing (independent of perf work)
5. **C.1–C.2** — API design (informs Phase 9 scope)

---

## Workstream A — Performance

### A.1 Deferred command arena allocation

Replace `vector<std::function<void(World&)>>` with a custom type-erased command
buffer. Replace `std::shared_ptr` component storage with inline placement-new in
an arena. Eliminates per-command heap allocation + refcounting overhead.

**Files:** `world.hpp` (DeferredProxy), `command_buffer.hpp`

### A.2 Observer hook optimization

Replace `unordered_map<CID, vector<std::function<...>>>` with a flat vector of
`(CID, function)` pairs, sorted by CID for binary-search lookup.

**Files:** `world.hpp` (on_add_hooks_, on_remove_hooks_)

### A.3 Malloc null check in chunk allocation

Add null check after `std::malloc()` in archetype chunk growth. Throw
`std::bad_alloc` on failure.

**Files:** `archetype.hpp` (ensure_capacity)

### A.4 Document all identified performance issues

Create `PERFORMANCE.md` cataloging every issue found (high, medium, low
priority). Includes: container choice issues (`map` → `unordered_map`), factory
registry, query cache invalidation scope, TypeSet hashing, deferred command
overhead.

**Files:** `PERFORMANCE.md` (new)

### A.5 Benchmarking harness

Add `benchmarks/bench.cpp` measuring:
- Entity creation throughput (empty, 1-component, 5-component)
- Iteration throughput (1M entities, 1–3 components)
- Archetype migration throughput
- Deferred command flush throughput

Uses `std::chrono::high_resolution_clock`, no external deps.

**Files:** `benchmarks/bench.cpp` (new), `CMakeLists.txt`

---

## Workstream B — Testing Coverage

### B.1 Exception safety tests

- World remains consistent if component move-constructor throws during migration
- Partial `create_with` failure doesn't leak entities

### B.2 Reference invalidation tests

- Document and test that component references are invalidated by structural changes
- Get reference → migrate entity → verify reference is stale

### B.3 Circular hierarchy prevention tests

- `set_parent(w, e, e)` (self-parent) triggers assertion
- Indirect cycles (A→B→C→A) are documented as user responsibility

### B.4 Serialization edge case tests

- Corrupted input (truncated stream, invalid counts)
- Component type ID mismatch (register in different order)
- Round-trip with empty world (already exists), world with only destroyed entities

### B.5 Stress / boundary tests

- 256 component types (bitset limit)
- Generation counter near overflow
- Archetype with 0 entities after all destroyed, then repopulate (already exists, extend)

---

## Workstream C — API Formalization for Scripting

### C.1 Type-erased C API surface design

Design (document, not implement) a flat C API over World:

```c
// Lifecycle
ecs_world_t*  ecs_world_create(void);
void          ecs_world_destroy(ecs_world_t* world);

// Entities
ecs_entity_t  ecs_entity_create(ecs_world_t* world);
void          ecs_entity_destroy(ecs_world_t* world, ecs_entity_t entity);
bool          ecs_entity_alive(const ecs_world_t* world, ecs_entity_t entity);

// Components (string-keyed, type-erased)
void          ecs_component_add(ecs_world_t* world, ecs_entity_t entity,
                                const char* type_name, const void* data, size_t size);
void*         ecs_component_get(ecs_world_t* world, ecs_entity_t entity,
                                const char* type_name);
bool          ecs_component_has(const ecs_world_t* world, ecs_entity_t entity,
                                const char* type_name);
void          ecs_component_remove(ecs_world_t* world, ecs_entity_t entity,
                                   const char* type_name);

// Queries
typedef void (*ecs_each_fn)(ecs_entity_t entity, void** components, void* userdata);
void          ecs_query_each(ecs_world_t* world, const char** type_names,
                             size_t count, ecs_each_fn callback, void* userdata);

// Resources
void          ecs_resource_set(ecs_world_t* world, const char* type_name,
                               const void* data, size_t size);
void*         ecs_resource_get(ecs_world_t* world, const char* type_name);
```

This becomes the foundation for both Lua and Python bindings.

**Implementation file:** `include/ecs/c_api.h` (Phase 9)

### C.2 Scripting layer architecture

**Dual-language strategy:**

| Language | Role | Binding Method |
|----------|------|----------------|
| Lua | Gameplay scripting (systems, entity logic) | C API via LuaJIT FFI or sol2 |
| Python | Tooling (editor, asset pipeline, inspection) | pybind11 over C++ API |

**Lua needs:** Entity/component CRUD, queries, systems, deferred commands.
All via the C API (C.1).

**Python needs:** World inspection, serialization, asset pipeline, editor
integration. Richer API with Python objects wrapping C++ types.

**Phase 9.1 prerequisite:** Type-erased component access (`get_raw`,
`has_raw`, `add_raw`, `remove_raw`) keyed by string name. This is the
bridge between the C++ template API and the dynamic C API.

---

## Verification Criteria

- All existing tests pass after each change
- Sanitizer build clean (`cmake -DECS_SANITIZE=ON ..`)
- Zero compiler warnings
- Benchmark numbers recorded before/after perf fixes
- `clang-format -i` on all changed files
