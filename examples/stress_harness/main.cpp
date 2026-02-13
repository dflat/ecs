#include <ecs/ecs.hpp>
#include <ecs/builtin/transform.hpp>
#include <ecs/builtin/hierarchy.hpp>
#include <ecs/builtin/transform_propagation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "raylib.h"
#include "rlgl.h"

// ---------------------------------------------------------------------------
// Instancing shaders (embedded — no external files needed)
// ---------------------------------------------------------------------------

static const char* instancing_vs =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec4 vertexColor;\n"
    "in mat4 instanceTransform;\n"
    "uniform mat4 mvp;\n"
    "out vec2 fragTexCoord;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    fragColor = vertexColor;\n"
    "    gl_Position = mvp * instanceTransform * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char* instancing_fs =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec4 texelColor = texture(texture0, fragTexCoord);\n"
    "    finalColor = texelColor * colDiffuse * fragColor;\n"
    "}\n";

static Shader instancing_shader;

// ---------------------------------------------------------------------------
// Harness-local components
// ---------------------------------------------------------------------------

struct Velocity {
    float vx, vy, vz;
};

struct MeshTag {
    enum Type { SPHERE, CUBE, CYLINDER, COUNT };
    Type type;
};

struct PadA {
    float data[4];
};
struct PadB {
    float data[4];
};
struct PadC {
    float data[4];
};
struct PadD {
    float data[4];
};

struct Orbital {
    float speed;
    float orbit_radius;
    float angle;
};

// ---------------------------------------------------------------------------
// Stress modes
// ---------------------------------------------------------------------------

enum class StressMode { FLAT_SWARM, WIDE_SWARM, SHALLOW_TREE, DEEP_CHAIN, COUNT };

static const char* mode_names[] = {"Flat Swarm", "Wide Swarm", "Shallow Tree", "Deep Chain"};

// ---------------------------------------------------------------------------
// Per-system timing with EMA smoothing
// ---------------------------------------------------------------------------

struct Timings {
    double motion_ms = 0.0;
    double propagate_ms = 0.0;
    double collect_ms = 0.0;
    double draw_ms = 0.0;

    void update(double motion, double propagate, double collect, double draw) {
        constexpr double alpha = 0.05;
        motion_ms += alpha * (motion - motion_ms);
        propagate_ms += alpha * (propagate - propagate_ms);
        collect_ms += alpha * (collect - collect_ms);
        draw_ms += alpha * (draw - draw_ms);
    }

    void reset() { motion_ms = propagate_ms = collect_ms = draw_ms = 0.0; }
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static ecs::World world;
static StressMode current_mode = StressMode::FLAT_SWARM;
static Timings timings;

static int target_count = 100;
static int current_count = 0;

static bool paused = false;
static bool show_help = true;
static bool auto_ramp = false;
static int auto_ramp_cliff = 0;

static std::vector<ecs::Entity> entity_tracker; // flat/wide swarm entities
static std::vector<ecs::Entity> root_tracker;   // hierarchy roots

// Meshes and materials (3 types)
static Mesh meshes[MeshTag::COUNT];
static Material materials[MeshTag::COUNT];

// Motion system function pointer — reassigned on mode switch
using SystemFunc = void (*)(ecs::World&);
static SystemFunc motion_system = nullptr;

// Instance buffers
static std::vector<Matrix> instance_buffers[MeshTag::COUNT];

// ---------------------------------------------------------------------------
// Mat4 → Raylib Matrix conversion (column-major → Raylib struct)
// ---------------------------------------------------------------------------

static Matrix mat4_to_raylib(const ecs::Mat4& m) {
    // Our Mat4 is column-major: m[col*4 + row]
    // Raylib Matrix fields: m0-m3 = row 0, m4-m7 = row 1, etc.
    // So we transpose: raylib.row_r_col_c = m[c*4 + r]
    return Matrix{
        m.m[0], m.m[4], m.m[8],  m.m[12],
        m.m[1], m.m[5], m.m[9],  m.m[13],
        m.m[2], m.m[6], m.m[10], m.m[14],
        m.m[3], m.m[7], m.m[11], m.m[15],
    };
}

// ---------------------------------------------------------------------------
// add_child helper (same pattern as visual_harness)
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

// ---------------------------------------------------------------------------
// Random helpers
// ---------------------------------------------------------------------------

static float randf(float lo, float hi) {
    return lo + static_cast<float>(rand()) / static_cast<float>(RAND_MAX) * (hi - lo);
}

static MeshTag::Type rand_mesh() {
    return static_cast<MeshTag::Type>(rand() % MeshTag::COUNT);
}

// ---------------------------------------------------------------------------
// Spawn / despawn functions
// ---------------------------------------------------------------------------

static void spawn_flat(int n) {
    for (int i = 0; i < n; ++i) {
        auto e = world.create_with(
            ecs::LocalTransform{{randf(-50, 50), randf(-50, 50), randf(-50, 50)}},
            ecs::WorldTransform{},
            Velocity{randf(-5, 5), randf(-5, 5), randf(-5, 5)},
            MeshTag{rand_mesh()});
        entity_tracker.push_back(e);
    }
    current_count += n;
}

static void spawn_wide(int n) {
    for (int i = 0; i < n; ++i) {
        auto e = world.create_with(
            ecs::LocalTransform{{randf(-50, 50), randf(-50, 50), randf(-50, 50)}},
            ecs::WorldTransform{},
            Velocity{randf(-5, 5), randf(-5, 5), randf(-5, 5)},
            MeshTag{rand_mesh()},
            PadA{}, PadB{}, PadC{}, PadD{});
        entity_tracker.push_back(e);
    }
    current_count += n;
}

static void spawn_shallow_tree_unit() {
    // 1 root + 8 children + 32 grandchildren = 41
    auto root = world.create_with(
        ecs::LocalTransform{{randf(-30, 30), randf(-10, 10), randf(-30, 30)}},
        ecs::WorldTransform{},
        MeshTag{MeshTag::SPHERE});
    root_tracker.push_back(root);

    for (int c = 0; c < 8; ++c) {
        auto child = world.create_with(
            ecs::LocalTransform{}, // Identity
            ecs::WorldTransform{},
            Orbital{randf(0.5f, 3.0f), randf(1.0f, 3.0f), randf(0, 6.28f)},
            MeshTag{MeshTag::CUBE});
        add_child(root, child);

        for (int g = 0; g < 4; ++g) {
            auto grandchild = world.create_with(
                ecs::LocalTransform{},
                ecs::WorldTransform{},
                Orbital{randf(1.0f, 5.0f), randf(0.3f, 1.0f), randf(0, 6.28f)},
                MeshTag{MeshTag::CYLINDER});
            add_child(child, grandchild);
        }
    }
    current_count += 41;
}

static void destroy_tree_recursive(ecs::Entity e) {
    auto* ch = world.try_get<ecs::Children>(e);
    if (ch) {
        auto children_copy = ch->entities;
        for (auto child : children_copy) {
            if (world.alive(child))
                destroy_tree_recursive(child);
        }
    }
    world.destroy(e);
}

static void rebuild_deep_chain(int n) {
    // Destroy existing chain (must be recursive to get all children)
    for (auto& r : root_tracker) {
        if (world.alive(r))
            destroy_tree_recursive(r);
    }
    root_tracker.clear();
    // Also destroy any remaining entities with Parent (children destroyed via manual walk)
    // Rebuild from scratch
    entity_tracker.clear();

    if (n <= 0) {
        current_count = 0;
        return;
    }

    auto prev = world.create_with(
        ecs::LocalTransform{{0, 0, 0}},
        ecs::WorldTransform{},
        MeshTag{MeshTag::SPHERE});
    root_tracker.push_back(prev);

    for (int i = 1; i < n; ++i) {
        auto e = world.create_with(
            ecs::LocalTransform{{0, 0.5f, 0}},
            ecs::WorldTransform{},
            MeshTag{static_cast<MeshTag::Type>(i % MeshTag::COUNT)});
        add_child(prev, e);
        prev = e;
    }
    current_count = n;
}

static void despawn_flat_or_wide(int n) {
    for (int i = 0; i < n && !entity_tracker.empty(); ++i) {
        auto e = entity_tracker.back();
        entity_tracker.pop_back();
        if (world.alive(e))
            world.destroy(e);
        --current_count;
    }
}

static void despawn_shallow_tree(int n) {
    // Each tree unit is 41 entities; despawn whole trees
    int trees = n / 41;
    if (trees < 1) trees = 1;
    for (int i = 0; i < trees && !root_tracker.empty(); ++i) {
        auto r = root_tracker.back();
        root_tracker.pop_back();
        if (world.alive(r))
            destroy_tree_recursive(r);
        current_count -= 41;
    }
    if (current_count < 0) current_count = 0;
}

// ---------------------------------------------------------------------------
// Motion systems
// ---------------------------------------------------------------------------

static void velocity_update(ecs::World& w) {
    if (paused) return;
    float dt = GetFrameTime();
    w.each<Velocity, ecs::LocalTransform>(
        [&](ecs::Entity, Velocity& vel, ecs::LocalTransform& lt) {
            lt.position.x += vel.vx * dt;
            lt.position.y += vel.vy * dt;
            lt.position.z += vel.vz * dt;

            // Wrap at ±50
            auto wrap = [](float& v, float& pos) {
                if (pos > 50.0f) { pos = -50.0f; }
                else if (pos < -50.0f) { pos = 50.0f; }
                (void)v;
            };
            wrap(vel.vx, lt.position.x);
            wrap(vel.vy, lt.position.y);
            wrap(vel.vz, lt.position.z);
        });
}

static void orbital_motion(ecs::World& w) {
    if (paused) return;
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

static void chain_wiggle(ecs::World& w) {
    if (paused) return;
    double t = GetTime();
    w.each<ecs::LocalTransform, ecs::Parent>(
        [&](ecs::Entity, ecs::LocalTransform& lt, ecs::Parent&) {
            lt.position.x = std::sin(static_cast<float>(t) * 2.0f) * 0.3f;
        });
}

static void noop_motion(ecs::World&) {}

// ---------------------------------------------------------------------------
// Mode switching
// ---------------------------------------------------------------------------

static void destroy_all() {
    for (auto& e : entity_tracker) {
        if (world.alive(e)) world.destroy(e);
    }
    entity_tracker.clear();

    for (auto& r : root_tracker) {
        if (world.alive(r)) destroy_tree_recursive(r);
    }
    root_tracker.clear();

    current_count = 0;
}

static void switch_mode(StressMode mode) {
    destroy_all();
    current_mode = mode;
    target_count = 100;
    timings.reset();
    auto_ramp = false;
    auto_ramp_cliff = 0;

    switch (mode) {
    case StressMode::FLAT_SWARM:  motion_system = velocity_update; break;
    case StressMode::WIDE_SWARM:  motion_system = velocity_update; break;
    case StressMode::SHALLOW_TREE: motion_system = orbital_motion; break;
    case StressMode::DEEP_CHAIN:  motion_system = chain_wiggle; break;
    default: motion_system = noop_motion; break;
    }
}

// ---------------------------------------------------------------------------
// Entity count adjustment (called each frame before systems)
// ---------------------------------------------------------------------------

static void adjust_entity_count() {
    if (current_mode == StressMode::DEEP_CHAIN) {
        if (current_count != target_count) {
            rebuild_deep_chain(target_count);
        }
        return;
    }

    int diff = target_count - current_count;
    if (diff == 0) return;

    if (diff > 0) {
        switch (current_mode) {
        case StressMode::FLAT_SWARM:  spawn_flat(diff); break;
        case StressMode::WIDE_SWARM:  spawn_wide(diff); break;
        case StressMode::SHALLOW_TREE: {
            int trees_needed = diff / 41;
            if (trees_needed < 1) trees_needed = 1;
            for (int i = 0; i < trees_needed; ++i)
                spawn_shallow_tree_unit();
            target_count = current_count; // snap to actual
            break;
        }
        default: break;
        }
    } else {
        int to_remove = -diff;
        switch (current_mode) {
        case StressMode::FLAT_SWARM:
        case StressMode::WIDE_SWARM:
            despawn_flat_or_wide(to_remove);
            break;
        case StressMode::SHALLOW_TREE:
            despawn_shallow_tree(to_remove);
            target_count = current_count; // snap
            break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------------------
// Auto-ramp: increase until frame time > 16ms
// ---------------------------------------------------------------------------

static void auto_ramp_step() {
    if (!auto_ramp) return;

    float frame_ms = GetFrameTime() * 1000.0f;
    if (frame_ms > 16.0f) {
        auto_ramp = false;
        auto_ramp_cliff = current_count;
        return;
    }

    // Increase target
    if (current_mode == StressMode::SHALLOW_TREE) {
        target_count += 41; // one tree unit
    } else {
        if (target_count < 100)       target_count += 10;
        else if (target_count < 1000) target_count += 100;
        else if (target_count < 10000) target_count += 1000;
        else                           target_count += 5000;
    }
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

static void handle_input() {
    // Up/Down: adjust target count (held, not press)
    if (IsKeyDown(KEY_UP)) {
        if (current_mode == StressMode::SHALLOW_TREE) {
            target_count += 41;
        } else {
            if (target_count < 100)       target_count += 10;
            else if (target_count < 1000) target_count += 100;
            else if (target_count < 10000) target_count += 1000;
            else                           target_count += 5000;
        }
    }
    if (IsKeyDown(KEY_DOWN)) {
        if (current_mode == StressMode::SHALLOW_TREE) {
            target_count -= 41;
        } else {
            if (target_count <= 100)      target_count -= 10;
            else if (target_count <= 1000) target_count -= 100;
            else if (target_count <= 10000) target_count -= 1000;
            else                            target_count -= 5000;
        }
        if (target_count < 0) target_count = 0;
    }

    // Left/Right: switch mode
    if (IsKeyPressed(KEY_LEFT)) {
        int m = (static_cast<int>(current_mode) - 1 + static_cast<int>(StressMode::COUNT))
                % static_cast<int>(StressMode::COUNT);
        switch_mode(static_cast<StressMode>(m));
    }
    if (IsKeyPressed(KEY_RIGHT)) {
        int m = (static_cast<int>(current_mode) + 1) % static_cast<int>(StressMode::COUNT);
        switch_mode(static_cast<StressMode>(m));
    }

    // Home: auto-ramp
    if (IsKeyPressed(KEY_HOME)) {
        auto_ramp = !auto_ramp;
        if (auto_ramp) auto_ramp_cliff = 0;
    }

    // P: pause, H: help
    if (IsKeyPressed(KEY_P)) paused = !paused;
    if (IsKeyPressed(KEY_H)) show_help = !show_help;
}

// ---------------------------------------------------------------------------
// Rendering: collect matrices and draw instanced
// ---------------------------------------------------------------------------

static void render_entities() {
    for (int i = 0; i < MeshTag::COUNT; ++i)
        instance_buffers[i].clear();

    world.each<ecs::WorldTransform, MeshTag>(
        [&](ecs::Entity, ecs::WorldTransform& wt, MeshTag& mt) {
            instance_buffers[mt.type].push_back(mat4_to_raylib(wt.matrix));
        });

    for (int i = 0; i < MeshTag::COUNT; ++i) {
        if (!instance_buffers[i].empty()) {
            DrawMeshInstanced(meshes[i], materials[i],
                              instance_buffers[i].data(),
                              static_cast<int>(instance_buffers[i].size()));
        }
    }
}

// ---------------------------------------------------------------------------
// UI drawing
// ---------------------------------------------------------------------------

static void draw_timing_bar(int x, int y, int max_width, double ms, double budget,
                            Color color, const char* label) {
    int bar_width = static_cast<int>((ms / budget) * max_width);
    if (bar_width > max_width) bar_width = max_width;
    if (bar_width < 1) bar_width = 1;

    DrawRectangle(x, y, bar_width, 16, color);
    DrawRectangleLines(x, y, max_width, 16, GRAY);

    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %.2f ms", label, ms);
    DrawText(buf, x + max_width + 10, y, 16, WHITE);
}

static void draw_ui() {
    int y = 10;
    char buf[128];

    // Mode and count
    snprintf(buf, sizeof(buf), "Mode: %s", mode_names[static_cast<int>(current_mode)]);
    DrawText(buf, 10, y, 20, YELLOW);
    y += 24;

    snprintf(buf, sizeof(buf), "Entities: %d (target: %d)", current_count, target_count);
    DrawText(buf, 10, y, 20, WHITE);
    y += 24;

    snprintf(buf, sizeof(buf), "FPS: %d  Frame: %.1f ms", GetFPS(), GetFrameTime() * 1000.0f);
    DrawText(buf, 10, y, 20, WHITE);
    y += 24;

    if (auto_ramp) {
        DrawText("AUTO-RAMP ACTIVE", 10, y, 20, RED);
        y += 24;
    } else if (auto_ramp_cliff > 0) {
        snprintf(buf, sizeof(buf), "60fps cliff: %d entities", auto_ramp_cliff);
        DrawText(buf, 10, y, 20, GREEN);
        y += 24;
    }

    if (paused) {
        DrawText("PAUSED", 10, y, 20, ORANGE);
        y += 24;
    }

    // Timing bars
    y += 8;
    constexpr double budget = 16.0;
    constexpr int bar_w = 200;
    draw_timing_bar(10, y, bar_w, timings.motion_ms, budget, BLUE, "Motion");
    y += 22;
    draw_timing_bar(10, y, bar_w, timings.propagate_ms, budget, ORANGE, "Propagate");
    y += 22;
    draw_timing_bar(10, y, bar_w, timings.collect_ms, budget, GREEN, "Collect");
    y += 22;
    draw_timing_bar(10, y, bar_w, timings.draw_ms, budget, RED, "Draw");
    y += 28;

    double total = timings.motion_ms + timings.propagate_ms + timings.collect_ms + timings.draw_ms;
    snprintf(buf, sizeof(buf), "Total tracked: %.2f ms", total);
    DrawText(buf, 10, y, 16, LIGHTGRAY);
    y += 24;

    // Help
    if (show_help) {
        y += 8;
        DrawText("Controls:", 10, y, 16, LIGHTGRAY); y += 20;
        DrawText("  Up/Down    Adjust entity count", 10, y, 16, LIGHTGRAY); y += 18;
        DrawText("  Left/Right Switch stress mode", 10, y, 16, LIGHTGRAY); y += 18;
        DrawText("  Home       Auto-ramp to 60fps cliff", 10, y, 16, LIGHTGRAY); y += 18;
        DrawText("  P          Pause motion", 10, y, 16, LIGHTGRAY); y += 18;
        DrawText("  H          Toggle help", 10, y, 16, LIGHTGRAY); y += 18;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    srand(42);

    InitWindow(1280, 720, "ECS Stress Harness");
    SetTargetFPS(0); // uncapped

    // Camera
    Camera3D camera = {};
    camera.position = Vector3{40.0f, 30.0f, 40.0f};
    camera.target = Vector3{0.0f, 0.0f, 0.0f};
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 60.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    // Generate meshes (low-poly, uploaded once)
    meshes[MeshTag::SPHERE]   = GenMeshSphere(0.3f, 8, 8);
    meshes[MeshTag::CUBE]     = GenMeshCube(0.5f, 0.5f, 0.5f);
    meshes[MeshTag::CYLINDER] = GenMeshCylinder(0.2f, 0.6f, 8);

    // Load instancing shader and set required locations
    instancing_shader = LoadShaderFromMemory(instancing_vs, instancing_fs);
    instancing_shader.locs[SHADER_LOC_MATRIX_MVP] = GetShaderLocation(instancing_shader, "mvp");
    instancing_shader.locs[SHADER_LOC_COLOR_DIFFUSE] = GetShaderLocation(instancing_shader, "colDiffuse");
    instancing_shader.locs[SHADER_LOC_MAP_DIFFUSE] = GetShaderLocation(instancing_shader, "texture0");
    instancing_shader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(instancing_shader, "instanceTransform");

    // Materials with distinct colors, all using instancing shader
    for (int i = 0; i < MeshTag::COUNT; ++i) {
        materials[i] = LoadMaterialDefault();
        materials[i].shader = instancing_shader;
    }
    materials[MeshTag::SPHERE].maps[MATERIAL_MAP_DIFFUSE].color   = Color{80, 120, 255, 255};
    materials[MeshTag::CUBE].maps[MATERIAL_MAP_DIFFUSE].color     = Color{255, 80, 80, 255};
    materials[MeshTag::CYLINDER].maps[MATERIAL_MAP_DIFFUSE].color = Color{80, 220, 100, 255};

    // Start in flat swarm mode
    switch_mode(StressMode::FLAT_SWARM);

    while (!WindowShouldClose()) {
        UpdateCamera(&camera, CAMERA_ORBITAL);
        handle_input();
        auto_ramp_step();
        adjust_entity_count();

        // --- Motion system ---
        double t0 = GetTime();
        if (motion_system) motion_system(world);
        double t1 = GetTime();

        // --- Transform propagation ---
        ecs::propagate_transforms(world);
        double t2 = GetTime();

        // --- Drawing ---
        BeginDrawing();
        ClearBackground(Color{20, 20, 30, 255});
        BeginMode3D(camera);

        double t3 = GetTime();
        render_entities();
        double t4 = GetTime();

        EndMode3D();
        draw_ui();
        EndDrawing();

        // Update EMA timings
        timings.update(
            (t1 - t0) * 1000.0,
            (t2 - t1) * 1000.0,
            (t4 - t3) * 1000.0,
            0.0 // draw cost is within t3-t4 for instanced; GPU cost not measured separately
        );
    }

    // Cleanup
    UnloadShader(instancing_shader);
    for (int i = 0; i < MeshTag::COUNT; ++i) {
        UnloadMesh(meshes[i]);
        UnloadMaterial(materials[i]);
    }

    CloseWindow();
    return 0;
}
