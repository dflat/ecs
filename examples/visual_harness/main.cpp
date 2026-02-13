#include <ecs/ecs.hpp>
#include <ecs/builtin/transform.hpp>
#include <ecs/builtin/hierarchy.hpp>
#include <ecs/builtin/transform_propagation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "raylib.h"

// ---------------------------------------------------------------------------
// Harness-local components (not part of the ECS library)
// ---------------------------------------------------------------------------

struct Renderable {
    float radius;
    uint8_t r, g, b;
};

struct Orbital {
    float speed;
    float orbit_radius;
    float angle;
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static ecs::World world;
static ecs::SystemRegistry systems;

static bool paused = false;
static bool wireframe = false;
static bool show_help = true;

static ecs::Entity sun = ecs::INVALID_ENTITY;
static std::vector<ecs::Entity> dynamic_planets;

// ---------------------------------------------------------------------------
// Hierarchy helpers
// ---------------------------------------------------------------------------

static void add_child(ecs::Entity parent, ecs::Entity child) {
    world.add(child, ecs::Parent{parent});
    auto* ch = world.try_get<ecs::Children>(parent);
    if (ch) {
        ch->entities.push_back(child);
    } else {
        world.add(parent, ecs::Children{{child}});
    }
}

static void destroy_entity_recursive(ecs::Entity e) {
    // Remove from parent's children list
    auto* p = world.try_get<ecs::Parent>(e);
    if (p && world.alive(p->entity)) {
        auto* siblings = world.try_get<ecs::Children>(p->entity);
        if (siblings) {
            auto& v = siblings->entities;
            v.erase(std::remove(v.begin(), v.end(), e), v.end());
        }
    }

    // Recursively destroy children first
    auto* ch = world.try_get<ecs::Children>(e);
    if (ch) {
        // Copy because destruction modifies the vector
        auto children_copy = ch->entities;
        for (auto child : children_copy) {
            if (world.alive(child))
                destroy_entity_recursive(child);
        }
    }

    world.destroy(e);
}

// ---------------------------------------------------------------------------
// Scene setup
// ---------------------------------------------------------------------------

static ecs::Entity make_body(float orbit_r, float speed, float radius,
                             uint8_t r, uint8_t g, uint8_t b) {
    return world.create_with(
        ecs::LocalTransform{{orbit_r, 0.0f, 0.0f}},
        ecs::WorldTransform{}, Renderable{radius, r, g, b},
        Orbital{speed, orbit_r, 0.0f});
}

static void build_scene() {
    // Sun (root — no Parent)
    sun = world.create_with(ecs::LocalTransform{}, ecs::WorldTransform{},
                            Renderable{2.0f, 255, 220, 50});

    // Earth
    auto earth = make_body(5.0f, 1.0f, 0.8f, 50, 100, 255);
    add_child(sun, earth);

    // Moon
    auto moon = make_body(1.5f, 2.5f, 0.3f, 180, 180, 180);
    add_child(earth, moon);

    // Mars
    auto mars = make_body(8.0f, 0.6f, 0.6f, 220, 80, 50);
    add_child(sun, mars);

    // Phobos
    auto phobos = make_body(1.2f, 3.0f, 0.2f, 160, 160, 160);
    add_child(mars, phobos);

    // Deimos
    auto deimos = make_body(1.8f, 2.0f, 0.25f, 140, 140, 140);
    add_child(mars, deimos);
}

static void reset_scene() {
    // Destroy everything
    std::vector<ecs::Entity> all;
    world.each<ecs::WorldTransform>(
        [&](ecs::Entity e, ecs::WorldTransform&) { all.push_back(e); });
    for (auto e : all) {
        if (world.alive(e))
            world.destroy(e);
    }
    dynamic_planets.clear();

    build_scene();
}

// ---------------------------------------------------------------------------
// Systems
// ---------------------------------------------------------------------------

static void orbital_motion(ecs::World& w) {
    if (paused)
        return;
    float dt = GetFrameTime();
    w.each<Orbital, ecs::LocalTransform>(
        [&](ecs::Entity, Orbital& orb, ecs::LocalTransform& lt) {
            orb.angle += orb.speed * dt;
            float x = std::cos(orb.angle) * orb.orbit_radius;
            float z = std::sin(orb.angle) * orb.orbit_radius;
            lt.position.x = x;
            lt.position.y = 0.0f;
            lt.position.z = z;
        });
}

static void transform_propagation(ecs::World& w) {
    ecs::propagate_transforms(w);
}

// ---------------------------------------------------------------------------
// Dynamic entity creation / destruction
// ---------------------------------------------------------------------------

static void add_random_planet() {
    float orbit_r = 3.0f + static_cast<float>(rand() % 80) / 10.0f;
    float speed = 0.3f + static_cast<float>(rand() % 20) / 10.0f;
    float radius = 0.3f + static_cast<float>(rand() % 5) / 10.0f;
    auto r = static_cast<uint8_t>(80 + rand() % 176);
    auto g = static_cast<uint8_t>(80 + rand() % 176);
    auto b = static_cast<uint8_t>(80 + rand() % 176);

    auto planet = make_body(orbit_r, speed, radius, r, g, b);
    add_child(sun, planet);
    dynamic_planets.push_back(planet);
}

static void remove_last_planet() {
    while (!dynamic_planets.empty()) {
        auto e = dynamic_planets.back();
        dynamic_planets.pop_back();
        if (world.alive(e)) {
            destroy_entity_recursive(e);
            return;
        }
    }
}

static void destroy_random_entity() {
    // Collect all non-sun entities
    std::vector<ecs::Entity> candidates;
    world.each<ecs::WorldTransform>([&](ecs::Entity e, ecs::WorldTransform&) {
        if (e != sun)
            candidates.push_back(e);
    });
    if (candidates.empty())
        return;

    auto target = candidates[static_cast<size_t>(rand()) % candidates.size()];
    // Remove from dynamic_planets tracking if present
    dynamic_planets.erase(
        std::remove(dynamic_planets.begin(), dynamic_planets.end(), target),
        dynamic_planets.end());
    destroy_entity_recursive(target);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void draw_bodies() {
    world.each<ecs::WorldTransform, Renderable>(
        [&](ecs::Entity, ecs::WorldTransform& wt, Renderable& vis) {
            Vector3 pos = {wt.matrix.m[12], wt.matrix.m[13], wt.matrix.m[14]};
            Color col = {vis.r, vis.g, vis.b, 255};

            if (wireframe) {
                DrawSphereWires(pos, vis.radius, 12, 12, col);
            } else {
                DrawSphere(pos, vis.radius, col);
                DrawSphereWires(pos, vis.radius, 12, 12,
                                Color{static_cast<uint8_t>(vis.r / 2),
                                      static_cast<uint8_t>(vis.g / 2),
                                      static_cast<uint8_t>(vis.b / 2), 255});
            }
        });
}

static void draw_orbit_rings() {
    world.each<Orbital, ecs::WorldTransform, ecs::Parent>(
        [&](ecs::Entity, Orbital& orb, ecs::WorldTransform&,
            ecs::Parent& par) {
            auto* pwt = world.try_get<ecs::WorldTransform>(par.entity);
            if (!pwt)
                return;
            Vector3 center = {pwt->matrix.m[12], pwt->matrix.m[13],
                              pwt->matrix.m[14]};
            // Draw orbit circle as line segments on XZ plane
            int segments = 64;
            for (int i = 0; i < segments; ++i) {
                float a0 = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * PI;
                float a1 = (static_cast<float>(i + 1) / static_cast<float>(segments)) * 2.0f * PI;
                Vector3 p0 = {center.x + std::cos(a0) * orb.orbit_radius,
                              center.y,
                              center.z + std::sin(a0) * orb.orbit_radius};
                Vector3 p1 = {center.x + std::cos(a1) * orb.orbit_radius,
                              center.y,
                              center.z + std::sin(a1) * orb.orbit_radius};
                DrawLine3D(p0, p1, Color{80, 80, 80, 255});
            }
        });
}

static void draw_ui() {
    DrawFPS(10, 10);

    size_t total = world.count();
    size_t with_orbital = world.count<Orbital>();
    size_t with_children = world.count<ecs::Children>();

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Entities: %zu  Orbital: %zu  Parents: %zu",
                  total, with_orbital, with_children);
    DrawText(buf, 10, 35, 18, LIGHTGRAY);

    if (paused)
        DrawText("PAUSED", GetScreenWidth() / 2 - 40, 10, 24, RED);

    if (show_help) {
        int y = 70;
        int sz = 16;
        Color c = LIGHTGRAY;
        DrawText("--- Controls ---", 10, y, sz, c);
        y += 20;
        DrawText("Mouse: rotate camera  Scroll: zoom", 10, y, sz, c);
        y += 18;
        DrawText("1: Add planet   2: Remove last planet", 10, y, sz, c);
        y += 18;
        DrawText("D: Destroy random entity", 10, y, sz, c);
        y += 18;
        DrawText("P: Pause/unpause   Space: Toggle wireframe", 10, y, sz, c);
        y += 18;
        DrawText("R: Reset scene   H: Toggle help", 10, y, sz, c);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    srand(42);

    InitWindow(1280, 720, "ECS Visual Harness — Solar System");
    SetTargetFPS(60);

    Camera3D camera = {};
    camera.position = Vector3{15.0f, 12.0f, 15.0f};
    camera.target = Vector3{0.0f, 0.0f, 0.0f};
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    // Register systems
    systems.add("orbital_motion", orbital_motion);
    systems.add("transform_propagation", transform_propagation);

    // Build initial scene
    build_scene();

    while (!WindowShouldClose()) {
        // Input
        UpdateCamera(&camera, CAMERA_ORBITAL);

        if (IsKeyPressed(KEY_ONE))
            add_random_planet();
        if (IsKeyPressed(KEY_TWO))
            remove_last_planet();
        if (IsKeyPressed(KEY_D))
            destroy_random_entity();
        if (IsKeyPressed(KEY_P))
            paused = !paused;
        if (IsKeyPressed(KEY_R))
            reset_scene();
        if (IsKeyPressed(KEY_SPACE))
            wireframe = !wireframe;
        if (IsKeyPressed(KEY_H))
            show_help = !show_help;

        // Update
        systems.run_all(world);

        // Draw
        BeginDrawing();
        ClearBackground(Color{20, 20, 30, 255});

        BeginMode3D(camera);
        DrawGrid(20, 1.0f);
        draw_orbit_rings();
        draw_bodies();
        EndMode3D();

        draw_ui();
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
