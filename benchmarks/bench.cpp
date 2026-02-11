#include <chrono>
#include <cstdio>
#include <ecs/ecs.hpp>
#include <vector>

using namespace ecs;

// Simple benchmark components
struct Pos {
    float x, y, z;
};
struct Vel {
    float dx, dy, dz;
};
struct Acc {
    float ax, ay, az;
};
struct Mass {
    float value;
};
struct Tag1 {};
struct Tag2 {};
struct Tag3 {};

// Timer utility
struct Timer {
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point start;

    Timer() : start(Clock::now()) {}

    double elapsed_ms() const {
        auto end = Clock::now();
        return std::chrono::duration<double, std::milli>(end - start).count();
    }
};

static void bench_entity_creation_empty(size_t n) {
    World w;
    Timer t;
    for (size_t i = 0; i < n; ++i)
        w.create();
    double ms = t.elapsed_ms();
    std::printf("  create (empty)          %zu entities: %.2f ms (%.0f ent/ms)\n", n, ms, n / ms);
}

static void bench_entity_creation_1comp(size_t n) {
    World w;
    Timer t;
    for (size_t i = 0; i < n; ++i)
        w.create_with(Pos{1.0f, 2.0f, 3.0f});
    double ms = t.elapsed_ms();
    std::printf("  create (1 component)    %zu entities: %.2f ms (%.0f ent/ms)\n", n, ms, n / ms);
}

static void bench_entity_creation_5comp(size_t n) {
    World w;
    Timer t;
    for (size_t i = 0; i < n; ++i)
        w.create_with(Pos{1, 2, 3}, Vel{4, 5, 6}, Acc{7, 8, 9}, Mass{10}, Tag1{});
    double ms = t.elapsed_ms();
    std::printf("  create (5 components)   %zu entities: %.2f ms (%.0f ent/ms)\n", n, ms, n / ms);
}

static void bench_iteration_1comp(size_t n) {
    World w;
    for (size_t i = 0; i < n; ++i)
        w.create_with(Pos{1, 2, 3});

    Timer t;
    w.each<Pos>([](Entity, Pos& p) {
        p.x += 1.0f;
        p.y += 1.0f;
        p.z += 1.0f;
    });
    double ms = t.elapsed_ms();
    std::printf("  iterate (1 comp)        %zu entities: %.2f ms (%.0f ent/ms)\n", n, ms, n / ms);
}

static void bench_iteration_2comp(size_t n) {
    World w;
    for (size_t i = 0; i < n; ++i)
        w.create_with(Pos{0, 0, 0}, Vel{1, 1, 1});

    Timer t;
    w.each<Pos, Vel>([](Entity, Pos& p, Vel& v) {
        p.x += v.dx;
        p.y += v.dy;
        p.z += v.dz;
    });
    double ms = t.elapsed_ms();
    std::printf("  iterate (2 comp)        %zu entities: %.2f ms (%.0f ent/ms)\n", n, ms, n / ms);
}

static void bench_iteration_3comp(size_t n) {
    World w;
    for (size_t i = 0; i < n; ++i)
        w.create_with(Pos{0, 0, 0}, Vel{1, 1, 1}, Acc{0.1f, 0.1f, 0.1f});

    Timer t;
    w.each<Pos, Vel, Acc>([](Entity, Pos& p, Vel& v, Acc& a) {
        v.dx += a.ax;
        v.dy += a.ay;
        v.dz += a.az;
        p.x += v.dx;
        p.y += v.dy;
        p.z += v.dz;
    });
    double ms = t.elapsed_ms();
    std::printf("  iterate (3 comp)        %zu entities: %.2f ms (%.0f ent/ms)\n", n, ms, n / ms);
}

static void bench_migration(size_t n) {
    World w;
    std::vector<Entity> entities;
    entities.reserve(n);
    for (size_t i = 0; i < n; ++i)
        entities.push_back(w.create_with(Pos{1, 2, 3}));

    Timer t;
    for (auto e : entities)
        w.add(e, Vel{0, 0, 0});
    double ms = t.elapsed_ms();
    std::printf("  migration (add 1 comp)  %zu entities: %.2f ms (%.0f ent/ms)\n", n, ms, n / ms);
}

static void bench_deferred_flush(size_t n) {
    World w;
    for (size_t i = 0; i < n; ++i)
        w.create_with(Pos{0, 0, 0});

    // Queue deferred destroys
    std::vector<Entity> to_destroy;
    to_destroy.reserve(n);
    w.each<Pos>([&](Entity e, Pos&) { to_destroy.push_back(e); });

    Timer t;
    for (auto e : to_destroy)
        w.deferred().destroy(e);
    w.flush_deferred();
    double ms = t.elapsed_ms();
    std::printf("  deferred flush (destroy) %zu cmds: %.2f ms (%.0f cmd/ms)\n", n, ms, n / ms);
}

static void bench_destroy(size_t n) {
    World w;
    std::vector<Entity> entities;
    entities.reserve(n);
    for (size_t i = 0; i < n; ++i)
        entities.push_back(w.create_with(Pos{1, 2, 3}, Vel{4, 5, 6}));

    Timer t;
    for (auto e : entities)
        w.destroy(e);
    double ms = t.elapsed_ms();
    std::printf("  destroy (2 comp)        %zu entities: %.2f ms (%.0f ent/ms)\n", n, ms, n / ms);
}

int main() {
    constexpr size_t N_SMALL = 100'000;
    constexpr size_t N_LARGE = 1'000'000;

    std::printf("=== ECS Benchmarks ===\n\n");

    std::printf("Entity Creation:\n");
    bench_entity_creation_empty(N_SMALL);
    bench_entity_creation_1comp(N_SMALL);
    bench_entity_creation_5comp(N_SMALL);

    std::printf("\nIteration:\n");
    bench_iteration_1comp(N_LARGE);
    bench_iteration_2comp(N_LARGE);
    bench_iteration_3comp(N_LARGE);

    std::printf("\nArchetype Migration:\n");
    bench_migration(N_SMALL);

    std::printf("\nDestruction:\n");
    bench_destroy(N_SMALL);

    std::printf("\nDeferred Commands:\n");
    bench_deferred_flush(N_SMALL);

    std::printf("\nDone.\n");
    return 0;
}
