#include <cassert>
#include <cstdio>
#include <ecs/ecs.hpp>

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

int main() {
    std::printf("Running ECS tests...\n");
    test_create_destroy();
    test_query_iteration();
    test_add_remove_component();
    test_swap_remove_correctness();
    test_generation_reuse();
    test_hierarchy_propagation();
    test_system_registry();
    std::printf("All tests passed!\n");
    return 0;
}
