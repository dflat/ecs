# RFC-0001: Hot-Path Performance

* **Status:** Active
* **Date:** February 2026

## Summary

Replace internal data structures on the hottest code paths with cache-friendly
alternatives, fix a correctness bug in nested iteration, and add a batch
destroy operation. All changes are internal — no public API changes except the
addition of `destroy_all<T>()`.

## Motivation

Profiling the ECS in a game engine context (Jolt Physics + Raylib) revealed
four issues that will limit scalability as entity counts grow:

1. **`std::map` for archetype columns** — every `get<T>()`, every `each<>()`
   setup, and every migration goes through `columns.at(cid)`, which is O(log N)
   with red-black tree pointer chasing. For a typical archetype with 5-8
   components, this means 2-3 cache-unfriendly indirections per lookup on the
   hottest path in the entire library.

2. **Query cache allocates on every call** — `cached_query()` constructs a
   `QueryKey` containing two `std::vector<ComponentTypeID>` on every `each<>()`
   invocation, even on cache hits. This causes 2 heap allocations per query per
   frame. With ~10 system queries at 60fps, that's ~1200 unnecessary
   allocations/second.

3. **`iterating_` flag doesn't handle nesting** — the flag is a `bool`, so
   nested `each()` calls (e.g., system A iterates X, callback iterates Y)
   leave it in the wrong state. The inner Guard destructor sets
   `iterating_ = false` while the outer loop is still active, allowing
   structural changes to bypass the safety assertion.

4. **No batch destroy** — scene resets require collecting entities into a
   vector then destroying one-by-one:
   ```cpp
   std::vector<Entity> to_destroy;
   world.each<WorldTag>([&](Entity e, WorldTag&) { to_destroy.push_back(e); });
   for (auto e : to_destroy) world.destroy(e);
   ```
   This is a two-pass pattern with a heap allocation. A dedicated
   `destroy_all<T>()` would be simpler and avoid the intermediate vector.

## Design

### 1. Replace `std::map` with flat storage in Archetype

**Current** (`archetype.hpp`):
```cpp
std::map<ComponentTypeID, ComponentColumn> columns;
std::map<ComponentTypeID, ArchetypeEdge> edges;
```

**Proposed:** Replace with sorted `std::vector` + linear scan:
```cpp
std::vector<std::pair<ComponentTypeID, ComponentColumn>> columns;
std::vector<std::pair<ComponentTypeID, ArchetypeEdge>> edges;
```

For the typical archetype width (3-10 components), linear scan on a sorted
contiguous array is faster than tree traversal due to cache locality. The
vectors are sorted by `ComponentTypeID` at construction time and never
reordered afterward (archetype composition is immutable after creation).

**Lookup helper:**
```cpp
ComponentColumn* find_column(ComponentTypeID cid);
const ComponentColumn* find_column(ComponentTypeID cid) const;
```

For archetypes with >16 components (rare), binary search can be used, but
linear scan is expected to be faster up to ~20 elements due to branch
prediction and prefetching on contiguous memory.

**Impact:** Every call to `get<T>()`, `try_get<T>()`, `has<T>()`, `each<>()`,
`add<T>()`, `remove<T>()`, and `destroy()` benefits. This is the single
highest-impact change in this RFC.

### 2. Stack-allocated query cache keys

**Current** (`world.hpp`):
```cpp
QueryKey key{TypeSet(include, include + n_include), TypeSet(exclude, exclude + n_exclude)};
```

`TypeSet` is `std::vector`, so this heap-allocates two vectors per `each<>()`.

**Proposed:** Use a fixed-capacity small key that avoids allocation:
```cpp
struct QueryKey {
    std::array<ComponentTypeID, 16> include_ids;
    std::array<ComponentTypeID, 16> exclude_ids;
    uint8_t n_include;
    uint8_t n_exclude;
};
```

16 component types per query side is generous — real queries rarely exceed 6.
If a query exceeds 16 (which would be pathological), fall back to a heap-
allocated path or static_assert.

The hash function operates directly on the fixed arrays. No allocation on any
`each<>()` call, even on cache miss.

### 3. Fix `iterating_` nesting

**Current:**
```cpp
bool iterating_ = false;
// In each():
iterating_ = true;
struct Guard { bool& flag; ~Guard() { flag = false; } } guard{iterating_};
```

**Proposed:** Replace with an integer counter:
```cpp
int iterating_ = 0;
// In each():
++iterating_;
struct Guard { int& count; ~Guard() { --count; } } guard{iterating_};
// In structural change methods:
ECS_ASSERT(iterating_ == 0, "structural change during iteration");
```

This correctly handles nested queries: the flag only reaches 0 when all
iteration scopes have exited.

### 4. Add `destroy_all<T>()`

**New public API on World:**
```cpp
template <typename T>
size_t destroy_all();
```

Destroys all entities that have component `T`. Returns the number of entities
destroyed. Iterates matching archetypes and destroys all entities in bulk,
avoiding the two-pass collect-then-destroy pattern.

**Implementation:** Query matching archetypes (using the existing cached_query
infrastructure), then for each matched archetype, destroy entities back-to-
front to avoid invalidation issues during swap-remove. Fire `on_remove` hooks
per entity as normal.

**Precondition:** Not called during iteration (asserts `iterating_ == 0`).

## Alternatives Considered

**Flat hash map for columns:** `std::unordered_map` was considered but rejected.
For small N (3-10 entries), the overhead of hashing + bucket lookup exceeds
linear scan on a contiguous array. Additionally, `unordered_map` has poor cache
behavior due to pointer-chasing in bucket chains.

**Component ID indexing (array[256]):** A fixed array indexed by component ID
was considered for column lookup. Rejected because it would waste memory (256
slots per archetype, most empty) and because the sorted vector approach is
simpler with comparable performance for typical archetype widths.

**Template-specialized query keys:** Compile-time query key types (one per
unique `each<Ts...>()` signature) would eliminate runtime key construction
entirely. Rejected as over-engineering for now — the stack-allocated key
eliminates the allocation issue, which is the primary concern.

## Testing

1. **All existing tests must pass unchanged.** These changes are internal
   implementation details — no behavioral changes except the new
   `destroy_all<T>()`.
2. **Nested iteration test:** Add a test that calls `each<A>()` with a
   callback that calls `each<B>()`, verifying the `iterating_` counter works
   correctly.
3. **`destroy_all` test:** Add tests verifying correct destruction count,
   hook invocation, and behavior on empty queries.
4. **Performance smoke test:** Verify that the stress harness doesn't regress
   (ideally improves).

## Risks & Open Questions

- **Sorted vector column lookup:** The `Archetype` destructor, move constructor,
  and `swap_remove` all iterate columns. Switching from `std::map` to
  `std::vector` should simplify these (no tree rebalancing), but all code paths
  that do `columns.find(cid)` or `columns.at(cid)` must be updated to use the
  new lookup helper.
- **Edge cache size:** The edge cache is written to lazily (one entry per
  add/remove transition observed). With a vector, insertion is O(N) but N is
  small and insertions are rare (once per unique transition, then cached).
  This is acceptable.
