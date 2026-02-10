# ECS Stress Harness

Benchmarks ECS throughput by measuring how many entities can be created, iterated, and transformed per frame. Isolates ECS cost from rendering cost using `DrawMeshInstanced` (one draw call per mesh type).

## Build

```bash
cd build
cmake -DECS_BUILD_EXAMPLES=ON ..
make stress_harness
./examples/stress_harness/stress_harness
```

## Controls

| Key | Action |
|-----|--------|
| Up/Down | Increase/decrease entity count (held) |
| Left/Right | Switch stress mode (resets entities to 100) |
| Home | Auto-ramp: increase entities until frame time >16ms |
| P | Pause motion systems |
| H | Toggle help overlay |

Entity count scaling: +10 below 100, +100 below 1k, +1000 below 10k, +5000 above.

## Stress Modes

| Mode | Tests | Entity Shape |
|------|-------|-------------|
| **Flat Swarm** | Raw `each<>` iteration throughput | N entities, 4-component archetype |
| **Wide Swarm** | Cache/column effects of wider archetypes | N entities, 8-component archetype (4 padding) |
| **Shallow Tree** | `propagate_transforms` BFS breadth | Trees of 41 (1 root + 8 children + 32 grandchildren) |
| **Deep Chain** | `propagate_transforms` BFS depth | Single chain of N entities |

## Interpreting Results

The timing bars show EMA-smoothed millisecond costs for each phase against a 16ms budget:

- **Motion** (blue): Cost of the mode-specific motion system (`each<>` iteration)
- **Propagate** (orange): Cost of `propagate_transforms` (BFS hierarchy walk)
- **Collect** (green): Cost of collecting world transforms into instance buffers
- **Draw** (red): Reserved for future GPU timing

Use **auto-ramp** (Home key) to find the entity count where frame time exceeds 16ms. Compare across modes to identify bottlenecks:

- Flat vs Wide Swarm: measures cache pressure from wider archetypes
- Shallow Tree: propagation cost should grow with tree count
- Deep Chain: propagation cost should grow with chain length; worst case for BFS depth
