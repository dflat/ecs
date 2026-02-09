# ECS Visual Test Harness

A visual harness that validates the ECS library's transform propagation,
hierarchy traversal, and query patterns using a 3D solar system scene.

## Building

From the ECS project root:

```bash
mkdir -p build && cd build
cmake -DECS_BUILD_EXAMPLES=ON ..
make visual_harness
./examples/visual_harness/visual_harness
```

Raylib 5.0 is fetched automatically via CMake FetchContent on first build.

For sanitizer builds:

```bash
cmake -DECS_BUILD_EXAMPLES=ON -DECS_SANITIZE=ON ..
```

## Controls

| Key | Action |
|-----|--------|
| Mouse drag | Rotate camera |
| Scroll | Zoom |
| `1` | Add random planet orbiting Sun |
| `2` | Remove last added planet |
| `D` | Destroy random entity (with hierarchy cleanup) |
| `P` | Pause / unpause orbital motion |
| `R` | Reset scene to initial state |
| `Space` | Toggle wireframe / solid rendering |
| `H` | Toggle help overlay |

## What It Tests

**Hierarchy** — Multi-level parent-child relationships (Sun > Earth > Moon,
Sun > Mars > Phobos/Deimos). Multiple children per parent.

**Transform propagation** — `propagate_transforms` BFS produces correct
world-space positions. Moons visibly orbit their planets, which orbit the Sun.
Wrong matrix multiplication order or incorrect propagation depth would be
immediately apparent.

**Query patterns** — `each<Ts...>`, `Exclude<Parent>` for root detection,
`try_get` for safe access during hierarchy traversal.

**Dynamic mutation** — `create_with`, `add`, `destroy` at runtime. Adding and
removing planets stress-tests archetype migration and entity lifecycle.

**SystemRegistry** — Systems registered and executed in order each frame.

## Scene

```
Sun (root, yellow, radius 2.0)
+-- Earth (blue, orbit 5u, radius 0.8)
|   +-- Moon (gray, orbit 1.5u, radius 0.3)
+-- Mars (red, orbit 8u, radius 0.6)
    +-- Phobos (gray, orbit 1.2u, radius 0.2)
    +-- Deimos (gray, orbit 1.8u, radius 0.25)
```
