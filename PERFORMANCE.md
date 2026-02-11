# Performance Issues Catalog

Living document of known performance issues. Items marked **[FIXED]** were
addressed during the housekeeping pass. Remaining items are candidates for
future optimization.

---

## High Priority

### Deferred command heap allocation — [FIXED]
- **Before:** Each deferred command allocated a `std::function` + `std::shared_ptr` for
  captured component data. Two heap allocations per command.
- **After:** Arena-based `CommandArena` with type-erased inline storage. Zero heap
  allocations per command (amortized).
- **Files:** `world.hpp` (DeferredProxy, CommandArena), `command_buffer.hpp`

### Observer hook map overhead — [FIXED]
- **Before:** `unordered_map<CID, vector<function>>` — hash lookup + pointer indirection
  per hook dispatch.
- **After:** Flat `vector<HookEntry>` sorted by CID with binary-search lookup.
  Better cache locality, no hash overhead.
- **Files:** `world.hpp`

### Malloc null check — [FIXED]
- **Before:** `std::malloc()` return value unchecked in `Archetype::ensure_capacity`.
  OOM would cause null pointer dereference.
- **After:** Throws `std::bad_alloc` on allocation failure.
- **Files:** `archetype.hpp`

---

## Medium Priority

### Column factory registry uses `std::map`
- `column_factory_registry()` is a `std::map<ComponentTypeID, std::function<ComponentColumn()>>`.
- `std::map` has tree overhead; lookups occur during every archetype creation.
- **Fix:** Switch to `std::unordered_map` or a flat vector indexed by CID (since
  CIDs are dense sequential integers starting at 0).
- **Files:** `component.hpp`

### Archetype edge cache uses `std::map`
- `Archetype::edges` is `std::map<ComponentTypeID, ArchetypeEdge>`.
- Used during add/remove migration. Tree overhead on every migration.
- **Fix:** Switch to `std::unordered_map` or flat vector.
- **Files:** `archetype.hpp`

### Archetype column map uses `std::map`
- `Archetype::columns` is `std::map<ComponentTypeID, ComponentColumn>`.
- Accessed on every `get<T>`, `push_raw`, migration. Red-black tree overhead.
- **Fix:** Switch to `std::unordered_map` or, better, a flat vector indexed by
  column index with a CID→index lookup table in the archetype.
- **Files:** `archetype.hpp`, `world.hpp`

### Query cache invalidation is global
- Any new archetype bumps `archetype_generation_`, invalidating all cached
  queries even if they can't possibly match the new archetype.
- **Fix:** Per-query selective invalidation, or check only new archetypes against
  each cached query.
- **Files:** `world.hpp`

### TypeSet hashing
- `TypeSetHash` uses a simple XOR-shift hash. Collision rate may be high for
  similar TypeSets (e.g., {1,2,3} vs {1,2,4}).
- **Fix:** Use a stronger hash (FNV-1a, or order-dependent mixing).
- **Files:** `archetype.hpp`

---

## Low Priority

### Name registry uses `std::map<string, CID>`
- `name_to_id_registry()` and `id_to_name_registry()` are `std::map`s.
- Only used during registration and serialization (not hot paths).
- **Fix:** Switch to `unordered_map` if serialization becomes a hot path.
- **Files:** `component.hpp`

### `count()` iterates all archetypes
- `World::count()` sums `arch->count()` across all archetypes.
- Could maintain a running total instead.
- **Fix:** Track `live_entity_count_` in World, increment on create, decrement
  on destroy.
- **Files:** `world.hpp`

### `count<Ts...>()` does not use query cache
- Iterates all archetypes with a manual component check instead of using the
  bitset-based cached query infrastructure.
- **Fix:** Use `cached_query` internally.
- **Files:** `world.hpp`

### `each()` constructs query key on every call
- The `QueryKey` is built from raw arrays each time, allocating a `TypeSet`
  (which is `vector<CID>`).
- **Fix:** Use a stack-allocated key for small type counts, or cache the query
  handle externally.
- **Files:** `world.hpp`

### Entity vector in archetype is separate from chunk
- `Archetype::entities` is a `std::vector<Entity>` separate from the
  chunk-allocated column block.
- **Fix:** Include the entity array in the chunk block layout for better locality.
- **Files:** `archetype.hpp`

---

## Benchmark Baseline (Pre-Housekeeping)

```
Entity Creation:
  create (empty)          100000 entities: 6.52 ms (15339 ent/ms)
  create (1 component)    100000 entities: 15.44 ms (6475 ent/ms)
  create (5 components)   100000 entities: 49.10 ms (2037 ent/ms)

Iteration:
  iterate (1 comp)        1000000 entities: 1.66 ms (601415 ent/ms)
  iterate (2 comp)        1000000 entities: 2.38 ms (419388 ent/ms)
  iterate (3 comp)        1000000 entities: 4.92 ms (203358 ent/ms)

Archetype Migration:
  migration (add 1 comp)  100000 entities: 8.43 ms (11860 ent/ms)

Destruction:
  destroy (2 comp)        100000 entities: 5.47 ms (18297 ent/ms)

Deferred Commands:
  deferred flush (destroy) 100000 cmds: 5.81 ms (17221 cmd/ms)
```

## Benchmark Post-Housekeeping (A.1-A.3)

```
Entity Creation:
  create (empty)          100000 entities: 5.98 ms (16721 ent/ms)
  create (1 component)    100000 entities: 11.87 ms (8427 ent/ms)
  create (5 components)   100000 entities: 37.01 ms (2702 ent/ms)

Iteration:
  iterate (1 comp)        1000000 entities: 1.61 ms (622194 ent/ms)
  iterate (2 comp)        1000000 entities: 2.90 ms (345337 ent/ms)
  iterate (3 comp)        1000000 entities: 4.29 ms (232981 ent/ms)

Archetype Migration:
  migration (add 1 comp)  100000 entities: 9.68 ms (10336 ent/ms)

Destruction:
  destroy (2 comp)        100000 entities: 5.29 ms (18910 ent/ms)

Deferred Commands:
  deferred flush (destroy) 100000 cmds: 5.15 ms (19416 cmd/ms)
```
