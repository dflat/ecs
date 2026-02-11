#include <cassert>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <ecs/ecs.hpp>
#include <memory>
#include <sstream>
#include <string>

using namespace ecs;

struct Position {
    float x, y;
};
struct Velocity {
    float dx, dy;
};
struct Health {
    int hp;
};
struct Tag {};
struct A {};
struct B {};
struct C {};
struct D {};
struct E {};

void test_create_destroy() {
    World w;
    Entity e = w.create_with(Position{1, 2}, Velocity{3, 4});
    assert(w.alive(e));
    assert(w.has<Position>(e));
    assert(w.has<Velocity>(e));
    assert(!w.has<Health>(e));
    assert(w.get<Position>(e).x == 1.0f);
    assert(w.get<Velocity>(e).dy == 4.0f);

    w.destroy(e);
    assert(!w.alive(e));
    std::printf("  create/destroy: OK\n");
}

void test_query_iteration() {
    World w;
    Entity e1 = w.create_with(Position{1, 0}, Velocity{10, 0});
    Entity e2 = w.create_with(Position{2, 0}, Velocity{20, 0});
    Entity e3 = w.create_with(Position{3, 0}); // no velocity

    int count = 0;
    w.each<Position, Velocity>([&](Entity, Position& p, Velocity& v) {
        p.x += v.dx;
        ++count;
    });
    assert(count == 2);
    assert(w.get<Position>(e1).x == 11.0f);
    assert(w.get<Position>(e2).x == 22.0f);
    assert(w.get<Position>(e3).x == 3.0f); // untouched
    std::printf("  query iteration: OK\n");
}

void test_add_remove_component() {
    World w;
    Entity e = w.create_with(Position{5, 5});
    assert(!w.has<Health>(e));

    w.add(e, Health{100});
    assert(w.has<Health>(e));
    assert(w.get<Health>(e).hp == 100);
    assert(w.get<Position>(e).x == 5.0f); // preserved after migration

    w.remove<Health>(e);
    assert(!w.has<Health>(e));
    assert(w.get<Position>(e).x == 5.0f); // still preserved
    std::printf("  add/remove component: OK\n");
}

void test_swap_remove_correctness() {
    World w;
    Entity e1 = w.create_with(Position{1, 0});
    Entity e2 = w.create_with(Position{2, 0});
    Entity e3 = w.create_with(Position{3, 0});

    // Destroy e1 (row 0) — e3 should be swapped into row 0
    w.destroy(e1);
    assert(!w.alive(e1));
    assert(w.alive(e2));
    assert(w.alive(e3));
    assert(w.get<Position>(e2).x == 2.0f);
    assert(w.get<Position>(e3).x == 3.0f);

    // Iterate to confirm density
    int count = 0;
    w.each<Position>([&](Entity, Position&) { ++count; });
    assert(count == 2);
    std::printf("  swap-remove correctness: OK\n");
}

void test_generation_reuse() {
    World w;
    Entity e1 = w.create_with(Position{1, 0});
    uint32_t idx = e1.index;
    w.destroy(e1);

    Entity e2 = w.create();
    assert(e2.index == idx);                    // reused index
    assert(e2.generation == e1.generation + 1); // bumped generation
    assert(!w.alive(e1));                       // old handle is dead
    assert(w.alive(e2));
    std::printf("  generation reuse: OK\n");
}

void test_hierarchy_propagation() {
    World w;

    // Root at (10, 0, 0)
    Entity root =
        w.create_with(LocalTransform{Mat4::translation(10, 0, 0)}, WorldTransform{}, Children{{}});

    // Child at (0, 5, 0) relative to root
    Entity child = w.create_with(LocalTransform{Mat4::translation(0, 5, 0)}, WorldTransform{},
                                 Parent{root}, Children{{}});

    // Grandchild at (0, 0, 3) relative to child
    Entity grandchild =
        w.create_with(LocalTransform{Mat4::translation(0, 0, 3)}, WorldTransform{}, Parent{child});

    // Wire up children
    w.get<Children>(root).entities.push_back(child);
    w.get<Children>(child).entities.push_back(grandchild);

    propagate_transforms(w);

    // Root world = local = (10, 0, 0)
    auto& root_wt = w.get<WorldTransform>(root).matrix;
    assert(root_wt.m[12] == 10.0f && root_wt.m[13] == 0.0f && root_wt.m[14] == 0.0f);

    // Child world = root * child_local = (10, 5, 0)
    auto& child_wt = w.get<WorldTransform>(child).matrix;
    assert(child_wt.m[12] == 10.0f && child_wt.m[13] == 5.0f && child_wt.m[14] == 0.0f);

    // Grandchild world = child_world * grandchild_local = (10, 5, 3)
    auto& gc_wt = w.get<WorldTransform>(grandchild).matrix;
    assert(gc_wt.m[12] == 10.0f && gc_wt.m[13] == 5.0f && gc_wt.m[14] == 3.0f);

    std::printf("  hierarchy propagation: OK\n");
}

void test_system_registry() {
    World w;
    w.create_with(Position{0, 0}, Velocity{1, 1});

    SystemRegistry systems;
    systems.add("movement", [](World& world) {
        world.each<Position, Velocity>([](Entity, Position& p, Velocity& v) {
            p.x += v.dx;
            p.y += v.dy;
        });
    });

    systems.run_all(w);
    systems.run_all(w);

    int count = 0;
    w.each<Position>([&](Entity, Position& p) {
        assert(p.x == 2.0f && p.y == 2.0f);
        ++count;
    });
    assert(count == 1);
    std::printf("  system registry: OK\n");
}

// -- Phase 0.2: expanded test coverage --

void test_generation_wraps() {
    World w;
    // Create and destroy at the same index multiple times
    Entity first = w.create_with(Position{0, 0});
    uint32_t idx = first.index;

    for (int i = 0; i < 10; ++i) {
        Entity e = (i == 0) ? first : w.create_with(Position{float(i), 0});
        assert(e.index == idx);
        assert(e.generation == first.generation + uint32_t(i));
        w.destroy(e);
    }
    // Old handles are all dead
    assert(!w.alive(first));
    std::printf("  generation wraps: OK\n");
}

void test_multi_migration() {
    World w;
    // A -> AB -> ABC -> AB -> A
    Entity e = w.create_with(A{});
    assert(w.has<A>(e));
    assert(!w.has<B>(e));

    w.add(e, B{});
    assert(w.has<A>(e) && w.has<B>(e));

    w.add(e, C{});
    assert(w.has<A>(e) && w.has<B>(e) && w.has<C>(e));

    w.remove<C>(e);
    assert(w.has<A>(e) && w.has<B>(e) && !w.has<C>(e));

    w.remove<B>(e);
    assert(w.has<A>(e) && !w.has<B>(e) && !w.has<C>(e));

    assert(w.alive(e));
    std::printf("  multi migration: OK\n");
}

void test_empty_archetype_reuse() {
    World w;
    Entity e1 = w.create_with(Position{1, 0});
    Entity e2 = w.create_with(Position{2, 0});
    w.destroy(e1);
    w.destroy(e2);

    // Archetype for [Position] exists but is empty. Create new entities in it.
    Entity e3 = w.create_with(Position{3, 0});
    Entity e4 = w.create_with(Position{4, 0});
    assert(w.alive(e3) && w.alive(e4));
    assert(w.get<Position>(e3).x == 3.0f);
    assert(w.get<Position>(e4).x == 4.0f);

    int count = 0;
    w.each<Position>([&](Entity, Position&) { ++count; });
    assert(count == 2);
    std::printf("  empty archetype reuse: OK\n");
}

void test_nontrivial_components() {
    World w;

    // std::string (non-trivial move, heap-allocating)
    Entity e1 = w.create_with(std::string("hello world, this is a long string to avoid SSO"));
    assert(w.get<std::string>(e1) == "hello world, this is a long string to avoid SSO");

    // std::unique_ptr (move-only)
    Entity e2 = w.create_with(std::make_unique<int>(42));
    assert(*w.get<std::unique_ptr<int>>(e2) == 42);

    // Destroy and verify no leaks (sanitizer catches these)
    w.destroy(e1);
    w.destroy(e2);
    assert(!w.alive(e1));
    assert(!w.alive(e2));
    std::printf("  non-trivial components: OK\n");
}

void test_empty_query() {
    World w;
    // No entities at all
    int count = 0;
    w.each<Position>([&](Entity, Position&) { ++count; });
    assert(count == 0);

    // Entities exist, but none match
    w.create_with(Health{100});
    w.each<Position>([&](Entity, Position&) { ++count; });
    assert(count == 0);
    std::printf("  empty query: OK\n");
}

void test_create_with_single() {
    World w;
    Entity e = w.create_with(Position{7, 8});
    assert(w.has<Position>(e));
    assert(w.get<Position>(e).x == 7.0f);
    std::printf("  create_with single: OK\n");
}

void test_create_with_many() {
    World w;
    Entity e = w.create_with(A{}, B{}, C{}, D{}, E{});
    assert(w.has<A>(e));
    assert(w.has<B>(e));
    assert(w.has<C>(e));
    assert(w.has<D>(e));
    assert(w.has<E>(e));
    std::printf("  create_with many: OK\n");
}

void test_add_overwrite() {
    World w;
    Entity e = w.create_with(Health{50});
    w.add(e, Health{100});
    assert(w.get<Health>(e).hp == 100); // overwritten, no migration
    std::printf("  add overwrite: OK\n");
}

void test_try_get_dead() {
    World w;
    Entity e = w.create_with(Position{1, 2});
    w.destroy(e);
    assert(w.try_get<Position>(e) == nullptr);
    std::printf("  try_get dead: OK\n");
}

// Test that the iteration guard catches structural changes.
// We override ECS_ASSERT behavior using signal handling to catch the abort.
static sigjmp_buf jump_buf;
static void abort_handler(int) {
    siglongjmp(jump_buf, 1);
}

void test_iteration_guard() {
    World w;
    w.create_with(Position{1, 0});

    // Install signal handler to catch SIGABRT from assert
    auto old_handler = signal(SIGABRT, abort_handler);
    bool caught = false;

    if (sigsetjmp(jump_buf, 1) == 0) {
        w.each<Position>([&](Entity, Position&) {
            w.create(); // structural change during iteration — should assert
        });
    } else {
        caught = true;
    }

    signal(SIGABRT, old_handler);
    assert(caught);
    std::printf("  iteration guard: OK\n");
}

// -- Phase 1.1: Exclude filters --

void test_exclude_filter() {
    World w;
    Entity e1 = w.create_with(A{}, B{});
    w.create_with(A{}, C{});
    w.create_with(A{}, B{}, C{});

    // each<A>(Exclude<C>) should match e1 (has A, no C) but not e2 or e3
    std::vector<Entity> matched;
    w.each<A>(World::Exclude<C>{}, [&](Entity e, A&) { matched.push_back(e); });
    assert(matched.size() == 1);
    assert(matched[0] == e1);

    // each<A>(Exclude<B, C>) should match nothing (all entities with A also have B or C)
    matched.clear();
    w.each<A>(World::Exclude<B, C>{}, [&](Entity e, A&) { matched.push_back(e); });
    assert(matched.empty());

    // each<A, B>(Exclude<C>) should match e1 only
    matched.clear();
    w.each<A, B>(World::Exclude<C>{}, [&](Entity e, A&, B&) { matched.push_back(e); });
    assert(matched.size() == 1);
    assert(matched[0] == e1);

    std::printf("  exclude filter: OK\n");
}

void test_exclude_no_entity() {
    World w;
    w.create_with(Position{1, 0}, Health{10});
    w.create_with(Position{2, 0});
    w.create_with(Position{3, 0}, Velocity{0, 0});

    // Should only match the entity with Position but no Health and no Velocity
    float sum = 0;
    w.each_no_entity<Position>(World::Exclude<Health, Velocity>{},
                               [&](Position& p) { sum += p.x; });
    assert(sum == 2.0f);
    std::printf("  exclude no_entity: OK\n");
}

// -- Phase 1.2: Utility queries --

void test_count() {
    World w;
    assert(w.count() == 0);

    w.create_with(Position{1, 0});
    w.create_with(Position{2, 0}, Velocity{1, 0});
    w.create_with(Health{50});
    assert(w.count() == 3);
    assert(w.count<Position>() == 2);
    assert((w.count<Position, Velocity>() == 1));
    assert(w.count<Health>() == 1);
    assert(w.count<Tag>() == 0);
    std::printf("  count: OK\n");
}

void test_single() {
    World w;
    w.create_with(Position{42, 99});
    w.create_with(Health{100});

    // Exactly one entity with Position
    bool called = false;
    w.single<Position>([&](Entity, Position& p) {
        assert(p.x == 42.0f && p.y == 99.0f);
        called = true;
    });
    assert(called);
    std::printf("  single: OK\n");
}

void test_single_assert_zero() {
    World w;
    // No entities with Position — single should assert
    auto old_handler = signal(SIGABRT, abort_handler);
    bool caught = false;
    if (sigsetjmp(jump_buf, 1) == 0) {
        w.single<Position>([](Entity, Position&) {});
    } else {
        caught = true;
    }
    signal(SIGABRT, old_handler);
    assert(caught);
    std::printf("  single assert zero: OK\n");
}

void test_single_assert_multi() {
    World w;
    w.create_with(Position{1, 0});
    w.create_with(Position{2, 0});
    // Two entities with Position — single should assert
    auto old_handler = signal(SIGABRT, abort_handler);
    bool caught = false;
    if (sigsetjmp(jump_buf, 1) == 0) {
        w.single<Position>([](Entity, Position&) {});
    } else {
        caught = true;
    }
    signal(SIGABRT, old_handler);
    assert(caught);
    std::printf("  single assert multi: OK\n");
}

// -- Phase 1.3: Query caching --

void test_query_cache_invalidation() {
    World w;
    w.create_with(Position{1, 0});
    w.create_with(Position{2, 0});

    // First query — cache miss, should find 2
    int count = 0;
    w.each<Position>([&](Entity, Position&) { ++count; });
    assert(count == 2);

    // Create entity in a NEW archetype that also has Position
    w.create_with(Position{3, 0}, Velocity{0, 0});

    // Second query — cache should be invalidated, should find 3
    count = 0;
    w.each<Position>([&](Entity, Position&) { ++count; });
    assert(count == 3);

    std::printf("  query cache invalidation: OK\n");
}

// -- Phase 2.1: CommandBuffer standalone --

void test_command_buffer_basic() {
    World w;
    CommandBuffer cb;

    // Queue a create
    cb.create_with(Position{10, 20}, Velocity{1, 2});
    // Queue a destroy
    Entity to_kill = w.create_with(Health{100});
    cb.destroy(to_kill);
    // Queue an add
    Entity target = w.create_with(Position{0, 0});
    cb.add(target, Health{50});
    // Queue a remove
    Entity to_strip = w.create_with(Position{5, 5}, Velocity{1, 1});
    cb.remove<Velocity>(to_strip);

    assert(!cb.empty());
    cb.flush(w);
    assert(cb.empty());

    // Verify: created entity exists
    assert((w.count<Position, Velocity>() == 1)); // the created one (to_strip lost Velocity)
    // to_kill is dead
    assert(!w.alive(to_kill));
    // target has Health
    assert(w.has<Health>(target));
    assert(w.get<Health>(target).hp == 50);
    // to_strip lost Velocity
    assert(!w.has<Velocity>(to_strip));
    assert(w.has<Position>(to_strip));
    std::printf("  command buffer basic: OK\n");
}

void test_command_buffer_empty_flush() {
    World w;
    CommandBuffer cb;
    assert(cb.empty());
    cb.flush(w); // no-op
    assert(w.count() == 0);
    std::printf("  command buffer empty flush: OK\n");
}

void test_command_buffer_nontrivial_types() {
    World w;
    CommandBuffer cb;
    std::string long_str = "this is a long string to avoid small string optimization entirely";
    cb.create_with(std::string(long_str));
    cb.flush(w);
    assert(w.count<std::string>() == 1);
    w.each<std::string>([&](Entity, std::string& s) { assert(s == long_str); });
    std::printf("  command buffer nontrivial types: OK\n");
}

void test_command_buffer_destroy_then_add() {
    World w;
    Entity e = w.create_with(Position{1, 2});
    CommandBuffer cb;
    cb.destroy(e);
    cb.add(e, Health{999}); // entity will be dead when this runs
    cb.flush(w);
    assert(!w.alive(e));
    // Health add was a no-op since entity is dead
    assert(w.count<Health>() == 0);
    std::printf("  command buffer destroy then add: OK\n");
}

// -- Phase 2.2: Integration with iteration --

void test_deferred_destroy_during_iteration() {
    World w;
    w.create_with(Position{1, 0});
    w.create_with(Position{2, 0});
    w.create_with(Position{3, 0});

    SystemRegistry systems;
    systems.add("destroyer", [](World& world) {
        world.each<Position>([&](Entity e, Position& p) {
            if (p.x == 2.0f)
                world.deferred().destroy(e);
        });
    });

    systems.run_all(w);
    assert(w.count<Position>() == 2);
    // Verify the right entity was destroyed
    float sum = 0;
    w.each<Position>([&](Entity, Position& p) { sum += p.x; });
    assert(sum == 4.0f); // 1 + 3
    std::printf("  deferred destroy during iteration: OK\n");
}

void test_deferred_add_during_iteration() {
    World w;
    Entity e = w.create_with(Position{5, 5});

    SystemRegistry systems;
    systems.add("adder", [e](World& world) {
        world.each<Position>([&](Entity, Position&) { world.deferred().add(e, Health{42}); });
    });

    systems.run_all(w);
    assert(w.has<Health>(e));
    assert(w.get<Health>(e).hp == 42);
    std::printf("  deferred add during iteration: OK\n");
}

void test_deferred_manual_flush() {
    World w;
    Entity e = w.create_with(Position{1, 0});

    w.each<Position>([&](Entity, Position&) { w.deferred().add(e, Velocity{9, 9}); });

    assert(!w.has<Velocity>(e)); // not flushed yet
    w.flush_deferred();
    assert(w.has<Velocity>(e));
    assert(w.get<Velocity>(e).dx == 9.0f);
    std::printf("  deferred manual flush: OK\n");
}

// --- Phase 3: Resources ---

struct DeltaTime {
    float dt;
};

void test_resource_set_get() {
    World w;
    w.set_resource(DeltaTime{0.016f});
    assert(w.has_resource<DeltaTime>());
    assert(w.resource<DeltaTime>().dt == 0.016f);
    std::printf("  resource set/get: OK\n");
}

void test_resource_overwrite() {
    World w;
    w.set_resource(DeltaTime{0.016f});
    w.set_resource(DeltaTime{0.032f});
    assert(w.resource<DeltaTime>().dt == 0.032f);
    std::printf("  resource overwrite: OK\n");
}

void test_resource_try_nullptr() {
    World w;
    assert(w.try_resource<DeltaTime>() == nullptr);
    w.set_resource(DeltaTime{0.016f});
    assert(w.try_resource<DeltaTime>() != nullptr);
    assert(w.try_resource<DeltaTime>()->dt == 0.016f);
    std::printf("  resource try nullptr: OK\n");
}

void test_resource_has_remove() {
    World w;
    assert(!w.has_resource<DeltaTime>());
    w.set_resource(DeltaTime{0.016f});
    assert(w.has_resource<DeltaTime>());
    w.remove_resource<DeltaTime>();
    assert(!w.has_resource<DeltaTime>());
    assert(w.try_resource<DeltaTime>() == nullptr);
    // remove again is a no-op
    w.remove_resource<DeltaTime>();
    std::printf("  resource has/remove: OK\n");
}

void test_resource_destructor() {
    // std::string resource — sanitizer will catch leaks
    {
        World w;
        w.set_resource(std::string("a long string to avoid small string optimization entirely"));
        assert(w.resource<std::string>() ==
               "a long string to avoid small string optimization entirely");
        // world destruction cleans up the resource
    }
    // Overwrite also cleans up the old value
    {
        World w;
        w.set_resource(std::string("first"));
        w.set_resource(std::string("second"));
        assert(w.resource<std::string>() == "second");
    }
    std::printf("  resource destructor: OK\n");
}

// --- Phase 4: Observers ---

void test_on_add_fires() {
    World w;
    int called = 0;
    Entity captured{};
    w.on_add<Health>(std::function<void(World&, Entity, Health&)>([&](World&, Entity e, Health& h) {
        ++called;
        captured = e;
        assert(h.hp == 42);
    }));

    Entity e = w.create();
    w.add(e, Health{42});
    assert(called == 1);
    assert(captured == e);
    std::printf("  on_add fires: OK\n");
}

void test_on_add_create_with() {
    World w;
    int called = 0;
    w.on_add<Health>(std::function<void(World&, Entity, Health&)>([&](World&, Entity, Health& h) {
        ++called;
        assert(h.hp == 99);
    }));

    w.create_with(Health{99});
    assert(called == 1);
    std::printf("  on_add create_with: OK\n");
}

void test_on_remove_fires() {
    World w;
    int called = 0;
    w.on_remove<Health>(
        std::function<void(World&, Entity, Health&)>([&](World&, Entity, Health& h) {
            ++called;
            assert(h.hp == 77);
        }));

    Entity e = w.create_with(Health{77}, Position{0, 0});
    w.remove<Health>(e);
    assert(called == 1);
    std::printf("  on_remove fires: OK\n");
}

void test_on_remove_destroy() {
    World w;
    int called = 0;
    w.on_remove<Health>(
        std::function<void(World&, Entity, Health&)>([&](World&, Entity, Health& h) {
            ++called;
            assert(h.hp == 55);
        }));

    Entity e = w.create_with(Health{55});
    w.destroy(e);
    assert(called == 1);
    std::printf("  on_remove destroy: OK\n");
}

void test_on_add_not_on_overwrite() {
    World w;
    int called = 0;
    w.on_add<Health>(
        std::function<void(World&, Entity, Health&)>([&](World&, Entity, Health&) { ++called; }));

    Entity e = w.create_with(Health{10});
    assert(called == 1);  // fired on create_with
    w.add(e, Health{20}); // overwrite — should NOT fire
    assert(called == 1);
    std::printf("  on_add not on overwrite: OK\n");
}

void test_multiple_hooks() {
    World w;
    std::vector<int> order;
    w.on_add<Health>(std::function<void(World&, Entity, Health&)>(
        [&](World&, Entity, Health&) { order.push_back(1); }));
    w.on_add<Health>(std::function<void(World&, Entity, Health&)>(
        [&](World&, Entity, Health&) { order.push_back(2); }));

    w.create_with(Health{10});
    assert(order.size() == 2);
    assert(order[0] == 1);
    assert(order[1] == 2);
    std::printf("  multiple hooks: OK\n");
}

void test_hook_receives_correct_data() {
    World w;
    w.on_add<Health>(
        std::function<void(World&, Entity, Health&)>([](World& world, Entity e, Health& h) {
            // Can read the value
            assert(h.hp == 100);
            // Can also read via get<T>
            assert(world.get<Health>(e).hp == 100);
            // Can mutate
            h.hp = 200;
        }));

    Entity e = w.create_with(Health{100});
    assert(w.get<Health>(e).hp == 200); // mutated by hook
    std::printf("  hook receives correct data: OK\n");
}

void test_hook_structural_change_other_entity() {
    World w;
    Entity other = w.create();
    w.on_add<Health>(std::function<void(World&, Entity, Health&)>(
        [other](World& world, Entity, Health&) { world.add(other, Tag{}); }));

    w.create_with(Health{10});
    assert(w.has<Tag>(other));
    std::printf("  hook structural change other entity: OK\n");
}

// --- Phase 5: Hierarchy Consistency ---

void test_set_parent() {
    World w;
    Entity parent = w.create();
    Entity child = w.create();

    set_parent(w, child, parent);

    assert(w.has<Parent>(child));
    assert(w.get<Parent>(child).entity == parent);
    assert(w.has<Children>(parent));
    assert(w.get<Children>(parent).entities.size() == 1);
    assert(w.get<Children>(parent).entities[0] == child);
    std::printf("  set_parent: OK\n");
}

void test_set_parent_reparent() {
    World w;
    Entity a = w.create();
    Entity b = w.create();
    Entity child = w.create();

    set_parent(w, child, a);
    set_parent(w, child, b);

    // a should no longer have child
    assert(w.has<Children>(a));
    assert(w.get<Children>(a).entities.empty());

    // b should have child
    assert(w.has<Children>(b));
    assert(w.get<Children>(b).entities.size() == 1);
    assert(w.get<Children>(b).entities[0] == child);

    // child's Parent should point to b
    assert(w.get<Parent>(child).entity == b);
    std::printf("  set_parent reparent: OK\n");
}

void test_remove_parent() {
    World w;
    Entity parent = w.create();
    Entity child = w.create();

    set_parent(w, child, parent);
    remove_parent(w, child);

    assert(!w.has<Parent>(child));
    assert(w.get<Children>(parent).entities.empty());
    std::printf("  remove_parent: OK\n");
}

void test_destroy_recursive() {
    World w;
    Entity root = w.create();
    Entity child = w.create();
    Entity grandchild = w.create();

    set_parent(w, child, root);
    set_parent(w, grandchild, child);

    destroy_recursive(w, root);

    assert(!w.alive(root));
    assert(!w.alive(child));
    assert(!w.alive(grandchild));
    std::printf("  destroy_recursive: OK\n");
}

void test_destroy_recursive_leaf() {
    World w;
    Entity root = w.create();
    Entity child_a = w.create();
    Entity child_b = w.create();

    set_parent(w, child_a, root);
    set_parent(w, child_b, root);

    destroy_recursive(w, child_a);

    assert(!w.alive(child_a));
    assert(w.alive(root));
    assert(w.alive(child_b));
    // root's Children should still contain child_b (child_a not cleaned up from
    // parent's list since we destroyed it directly, but that's fine — dead
    // entities in Children is a known consequence of raw destroy)
    std::printf("  destroy_recursive leaf: OK\n");
}

void test_set_parent_creates_children() {
    World w;
    Entity parent = w.create();
    Entity child = w.create();

    // parent has no Children component yet
    assert(!w.has<Children>(parent));

    set_parent(w, child, parent);

    assert(w.has<Children>(parent));
    assert(w.get<Children>(parent).entities.size() == 1);
    std::printf("  set_parent creates Children: OK\n");
}

void test_hierarchy_propagation_with_set_parent() {
    World w;

    Entity root = w.create_with(LocalTransform{Mat4::translation(10, 0, 0)}, WorldTransform{});
    Entity child = w.create_with(LocalTransform{Mat4::translation(0, 5, 0)}, WorldTransform{});
    Entity grandchild = w.create_with(LocalTransform{Mat4::translation(0, 0, 3)}, WorldTransform{});

    set_parent(w, child, root);
    set_parent(w, grandchild, child);

    propagate_transforms(w);

    auto& root_wt = w.get<WorldTransform>(root).matrix;
    assert(root_wt.m[12] == 10.0f && root_wt.m[13] == 0.0f && root_wt.m[14] == 0.0f);

    auto& child_wt = w.get<WorldTransform>(child).matrix;
    assert(child_wt.m[12] == 10.0f && child_wt.m[13] == 5.0f && child_wt.m[14] == 0.0f);

    auto& gc_wt = w.get<WorldTransform>(grandchild).matrix;
    assert(gc_wt.m[12] == 10.0f && gc_wt.m[13] == 5.0f && gc_wt.m[14] == 3.0f);

    std::printf("  hierarchy propagation with set_parent: OK\n");
}

// --- Phase 6: Sorting ---

struct Depth {
    float z;
};

void test_sort_basic_order() {
    World w;
    Entity e1 = w.create_with(Depth{3.0f});
    Entity e2 = w.create_with(Depth{1.0f});
    Entity e3 = w.create_with(Depth{2.0f});

    w.sort<Depth>([](const Depth& a, const Depth& b) { return a.z < b.z; });

    // Verify iteration order is sorted
    std::vector<float> order;
    w.each<Depth>([&](Entity, Depth& d) { order.push_back(d.z); });
    assert(order.size() == 3);
    assert(order[0] == 1.0f);
    assert(order[1] == 2.0f);
    assert(order[2] == 3.0f);

    // Verify get still works via entity handles
    assert(w.get<Depth>(e1).z == 3.0f);
    assert(w.get<Depth>(e2).z == 1.0f);
    assert(w.get<Depth>(e3).z == 2.0f);
    std::printf("  sort basic order: OK\n");
}

void test_sort_multi_column() {
    World w;
    Entity e1 = w.create_with(Depth{3.0f}, Position{30, 0});
    Entity e2 = w.create_with(Depth{1.0f}, Position{10, 0});
    Entity e3 = w.create_with(Depth{2.0f}, Position{20, 0});

    w.sort<Depth>([](const Depth& a, const Depth& b) { return a.z < b.z; });

    // Both columns should be rearranged in lockstep
    std::vector<float> depths;
    std::vector<float> positions;
    w.each<Depth, Position>([&](Entity, Depth& d, Position& p) {
        depths.push_back(d.z);
        positions.push_back(p.x);
    });
    assert(depths[0] == 1.0f && positions[0] == 10.0f);
    assert(depths[1] == 2.0f && positions[1] == 20.0f);
    assert(depths[2] == 3.0f && positions[2] == 30.0f);

    // Entity handles still work
    assert(w.get<Position>(e1).x == 30.0f);
    assert(w.get<Position>(e2).x == 10.0f);
    assert(w.get<Position>(e3).x == 20.0f);
    std::printf("  sort multi column: OK\n");
}

void test_sort_single_entity() {
    World w;
    Entity e = w.create_with(Depth{5.0f});
    w.sort<Depth>([](const Depth& a, const Depth& b) { return a.z < b.z; });
    assert(w.get<Depth>(e).z == 5.0f);
    std::printf("  sort single entity: OK\n");
}

void test_sort_empty_archetype() {
    World w;
    // Create and destroy to leave an empty archetype with Depth
    Entity e = w.create_with(Depth{1.0f});
    w.destroy(e);
    w.sort<Depth>([](const Depth& a, const Depth& b) { return a.z < b.z; });
    // No crash
    std::printf("  sort empty archetype: OK\n");
}

void test_sort_equal_keys() {
    World w;
    w.create_with(Depth{2.0f}, Position{1, 0});
    w.create_with(Depth{2.0f}, Position{2, 0});
    w.create_with(Depth{2.0f}, Position{3, 0});

    w.sort<Depth>([](const Depth& a, const Depth& b) { return a.z < b.z; });

    // All depths should still be 2.0, no corruption
    int count = 0;
    w.each<Depth, Position>([&](Entity, Depth& d, Position&) {
        assert(d.z == 2.0f);
        ++count;
    });
    assert(count == 3);
    std::printf("  sort equal keys: OK\n");
}

void test_sort_multiple_archetypes() {
    World w;
    // Depth-only archetype
    Entity e1 = w.create_with(Depth{3.0f});
    Entity e2 = w.create_with(Depth{1.0f});
    // Depth+Position archetype
    Entity e3 = w.create_with(Depth{4.0f}, Position{40, 0});
    Entity e4 = w.create_with(Depth{2.0f}, Position{20, 0});

    w.sort<Depth>([](const Depth& a, const Depth& b) { return a.z < b.z; });

    // Depth-only archetype should be sorted
    assert(w.get<Depth>(e1).z == 3.0f);
    assert(w.get<Depth>(e2).z == 1.0f);
    // Depth+Position archetype should be sorted
    assert(w.get<Depth>(e3).z == 4.0f);
    assert(w.get<Depth>(e4).z == 2.0f);

    // Verify iteration order within each archetype
    std::vector<float> depth_only;
    w.each<Depth>(World::Exclude<Position>{}, [&](Entity, Depth& d) { depth_only.push_back(d.z); });
    assert(depth_only[0] == 1.0f);
    assert(depth_only[1] == 3.0f);

    std::vector<float> depth_pos;
    w.each<Depth, Position>([&](Entity, Depth& d, Position&) { depth_pos.push_back(d.z); });
    assert(depth_pos[0] == 2.0f);
    assert(depth_pos[1] == 4.0f);
    std::printf("  sort multiple archetypes: OK\n");
}

void test_sort_assert_during_iteration() {
    World w;
    w.create_with(Depth{1.0f});

    auto old_handler = signal(SIGABRT, abort_handler);
    bool caught = false;
    if (sigsetjmp(jump_buf, 1) == 0) {
        w.each<Depth>([&](Entity, Depth&) {
            w.sort<Depth>([](const Depth& a, const Depth& b) { return a.z < b.z; });
        });
    } else {
        caught = true;
    }
    signal(SIGABRT, old_handler);
    assert(caught);
    std::printf("  sort assert during iteration: OK\n");
}

// --- Phase 7.1: Bitset archetype matching ---

struct F {};
struct G {};
struct H {};

void test_bitset_many_archetypes() {
    World w;
    // Create entities across 6+ distinct archetypes with overlapping components
    w.create_with(A{});
    w.create_with(A{}, B{});
    w.create_with(A{}, B{}, C{});
    w.create_with(A{}, B{}, C{}, D{});
    w.create_with(A{}, E{});
    w.create_with(F{}, G{});
    w.create_with(A{}, F{}, G{}, H{});
    w.create_with(B{}, F{});

    // Query for A — should match 6 entities across 6 archetypes
    assert(w.count<A>() == 6);

    // Query for A, B — should match 3 (A+B, A+B+C, A+B+C+D)
    assert((w.count<A, B>() == 3));

    // Query with exclude: A but not B — should match 3 (A alone, A+E, A+F+G+H)
    int count = 0;
    w.each<A>(World::Exclude<B>{}, [&](Entity, A&) { ++count; });
    assert(count == 3);

    // Query with exclude: A but not E and not F — should match 3 (A, A+B, A+B+C, A+B+C+D) minus
    // none with E or F
    count = 0;
    w.each<A>(World::Exclude<E, F>{}, [&](Entity, A&) { ++count; });
    assert(count == 4); // A, A+B, A+B+C, A+B+C+D

    // Query for F, G — should match 2 (F+G, A+F+G+H)
    assert((w.count<F, G>() == 2));

    std::printf("  bitset many archetypes: OK\n");
}

// --- Phase 8.1: Stable Type Registration ---

void test_register_component_lookup() {
    register_component<Position>("Position");
    register_component<Velocity>("Velocity");

    assert(component_id_by_name("Position") == component_id<Position>());
    assert(component_id_by_name("Velocity") == component_id<Velocity>());
    assert(component_name(component_id<Position>()) == "Position");
    assert(component_name(component_id<Velocity>()) == "Velocity");
    assert(component_registered(component_id<Position>()));
    assert(component_registered(component_id<Velocity>()));
    std::printf("  register component lookup: OK\n");
}

void test_register_component_idempotent() {
    register_component<Position>("Position");
    register_component<Position>("Position"); // same name, same type — no-op
    assert(component_id_by_name("Position") == component_id<Position>());
    std::printf("  register component idempotent: OK\n");
}

void test_register_component_conflict() {
    auto old_handler = signal(SIGABRT, abort_handler);
    bool caught = false;
    if (sigsetjmp(jump_buf, 1) == 0) {
        register_component<Health>("Position"); // different type, same name as Position
    } else {
        caught = true;
    }
    signal(SIGABRT, old_handler);
    assert(caught);
    std::printf("  register component conflict: OK\n");
}

// --- Phase 8.2: Serialization ---

void test_serialize_round_trip() {
    register_component<Position>("Position");
    register_component<Velocity>("Velocity");
    register_component<Health>("Health");

    World w1;
    Entity e1 = w1.create_with(Position{1.0f, 2.0f}, Velocity{3.0f, 4.0f});
    Entity e2 = w1.create_with(Position{5.0f, 6.0f}, Health{100});
    Entity e3 = w1.create_with(Health{50});

    std::stringstream ss;
    serialize(w1, ss);

    World w2;
    deserialize(w2, ss);

    assert(w2.alive(e1));
    assert(w2.alive(e2));
    assert(w2.alive(e3));
    assert(w2.get<Position>(e1).x == 1.0f && w2.get<Position>(e1).y == 2.0f);
    assert(w2.get<Velocity>(e1).dx == 3.0f && w2.get<Velocity>(e1).dy == 4.0f);
    assert(w2.get<Position>(e2).x == 5.0f && w2.get<Position>(e2).y == 6.0f);
    assert(w2.get<Health>(e2).hp == 100);
    assert(w2.get<Health>(e3).hp == 50);
    assert(!w2.has<Velocity>(e2));
    assert(!w2.has<Position>(e3));
    assert(w2.count() == 3);
    std::printf("  serialize round trip: OK\n");
}

void test_serialize_destroyed_entities() {
    register_component<Position>("Position");

    World w1;
    Entity e1 = w1.create_with(Position{1.0f, 0.0f});
    Entity e2 = w1.create_with(Position{2.0f, 0.0f});
    Entity e3 = w1.create_with(Position{3.0f, 0.0f});
    w1.destroy(e2); // free list should contain e2's index

    std::stringstream ss;
    serialize(w1, ss);

    World w2;
    deserialize(w2, ss);

    assert(w2.alive(e1));
    assert(!w2.alive(e2)); // destroyed
    assert(w2.alive(e3));
    assert(w2.get<Position>(e1).x == 1.0f);
    assert(w2.get<Position>(e3).x == 3.0f);
    assert(w2.count() == 2);

    // New entity should reuse the free slot
    Entity e4 = w2.create();
    assert(e4.index == e2.index);
    assert(e4.generation == e2.generation + 1);
    std::printf("  serialize destroyed entities: OK\n");
}

void test_serialize_empty_world() {
    World w1;

    std::stringstream ss;
    serialize(w1, ss);

    World w2;
    deserialize(w2, ss);

    assert(w2.count() == 0);
    std::printf("  serialize empty world: OK\n");
}

void test_serialize_with_hierarchy() {
    register_component<Parent>("Parent");
    register_component<Children>(
        "Children",
        // Custom serialize for Children (non-trivially-copyable)
        [](const void* elem, std::ostream& out) {
            auto& children = *static_cast<const Children*>(elem);
            uint32_t count = static_cast<uint32_t>(children.entities.size());
            out.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (auto& e : children.entities) {
                out.write(reinterpret_cast<const char*>(&e.index), sizeof(uint32_t));
                out.write(reinterpret_cast<const char*>(&e.generation), sizeof(uint32_t));
            }
        },
        [](void* elem, std::istream& in) {
            auto& children = *static_cast<Children*>(elem);
            uint32_t count;
            in.read(reinterpret_cast<char*>(&count), sizeof(count));
            children.entities.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                in.read(reinterpret_cast<char*>(&children.entities[i].index), sizeof(uint32_t));
                in.read(reinterpret_cast<char*>(&children.entities[i].generation),
                        sizeof(uint32_t));
            }
        });

    World w1;
    Entity parent = w1.create_with(Position{10.0f, 0.0f});
    Entity child = w1.create_with(Position{0.0f, 5.0f});
    set_parent(w1, child, parent);

    std::stringstream ss;
    serialize(w1, ss);

    World w2;
    deserialize(w2, ss);

    assert(w2.alive(parent));
    assert(w2.alive(child));
    assert(w2.has<Parent>(child));
    assert(w2.get<Parent>(child).entity == parent);
    assert(w2.has<Children>(parent));
    assert(w2.get<Children>(parent).entities.size() == 1);
    assert(w2.get<Children>(parent).entities[0] == child);
    std::printf("  serialize with hierarchy: OK\n");
}

void test_serialize_unregistered_type_asserts() {
    // Use a type that is NOT registered
    struct Unregistered {
        int val;
    };

    World w;
    w.create_with(Unregistered{42});

    auto old_handler = signal(SIGABRT, abort_handler);
    bool caught = false;
    if (sigsetjmp(jump_buf, 1) == 0) {
        std::stringstream ss;
        serialize(w, ss);
    } else {
        caught = true;
    }
    signal(SIGABRT, old_handler);
    assert(caught);
    std::printf("  serialize unregistered type asserts: OK\n");
}

int main() {
    std::printf("Running ECS tests...\n");
    test_create_destroy();
    test_query_iteration();
    test_add_remove_component();
    test_swap_remove_correctness();
    test_generation_reuse();
    test_hierarchy_propagation();
    test_system_registry();
    std::printf("  -- Phase 0.2 --\n");
    test_generation_wraps();
    test_multi_migration();
    test_empty_archetype_reuse();
    test_nontrivial_components();
    test_empty_query();
    test_create_with_single();
    test_create_with_many();
    test_add_overwrite();
    test_try_get_dead();
    test_iteration_guard();
    std::printf("  -- Phase 1.1 --\n");
    test_exclude_filter();
    test_exclude_no_entity();
    std::printf("  -- Phase 1.2 --\n");
    test_count();
    test_single();
    test_single_assert_zero();
    test_single_assert_multi();
    std::printf("  -- Phase 1.3 --\n");
    test_query_cache_invalidation();
    std::printf("  -- Phase 2.1 --\n");
    test_command_buffer_basic();
    test_command_buffer_empty_flush();
    test_command_buffer_nontrivial_types();
    test_command_buffer_destroy_then_add();
    std::printf("  -- Phase 2.2 --\n");
    test_deferred_destroy_during_iteration();
    test_deferred_add_during_iteration();
    test_deferred_manual_flush();
    std::printf("  -- Phase 3 --\n");
    test_resource_set_get();
    test_resource_overwrite();
    test_resource_try_nullptr();
    test_resource_has_remove();
    test_resource_destructor();
    std::printf("  -- Phase 4 --\n");
    test_on_add_fires();
    test_on_add_create_with();
    test_on_remove_fires();
    test_on_remove_destroy();
    test_on_add_not_on_overwrite();
    test_multiple_hooks();
    test_hook_receives_correct_data();
    test_hook_structural_change_other_entity();
    std::printf("  -- Phase 5 --\n");
    test_set_parent();
    test_set_parent_reparent();
    test_remove_parent();
    test_destroy_recursive();
    test_destroy_recursive_leaf();
    test_set_parent_creates_children();
    test_hierarchy_propagation_with_set_parent();
    std::printf("  -- Phase 6 --\n");
    test_sort_basic_order();
    test_sort_multi_column();
    test_sort_single_entity();
    test_sort_empty_archetype();
    test_sort_equal_keys();
    test_sort_multiple_archetypes();
    test_sort_assert_during_iteration();
    std::printf("  -- Phase 7.1 --\n");
    test_bitset_many_archetypes();
    std::printf("  -- Phase 8.1 --\n");
    test_register_component_lookup();
    test_register_component_idempotent();
    test_register_component_conflict();
    std::printf("  -- Phase 8.2 --\n");
    test_serialize_round_trip();
    test_serialize_destroyed_entities();
    test_serialize_empty_world();
    test_serialize_with_hierarchy();
    test_serialize_unregistered_type_asserts();
    std::printf("All tests passed!\n");
    return 0;
}
