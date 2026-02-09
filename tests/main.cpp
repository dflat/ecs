#include <cassert>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <ecs/ecs.hpp>
#include <memory>
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
static jmp_buf jump_buf;
static void abort_handler(int) {
    longjmp(jump_buf, 1);
}

void test_iteration_guard() {
    World w;
    w.create_with(Position{1, 0});

    // Install signal handler to catch SIGABRT from assert
    auto old_handler = signal(SIGABRT, abort_handler);
    bool caught = false;

    if (setjmp(jump_buf) == 0) {
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
    std::printf("All tests passed!\n");
    return 0;
}
