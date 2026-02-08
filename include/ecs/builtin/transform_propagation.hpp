#pragma once
#include "../world.hpp"
#include "hierarchy.hpp"
#include "transform.hpp"

#include <queue>

namespace ecs {

// BFS propagation of LocalTransform -> WorldTransform through the hierarchy.
// Roots are entities with LocalTransform + WorldTransform but no Parent.
inline void propagate_transforms(World& world) {
    std::queue<Entity> queue;

    // Find roots and seed the BFS
    world.each<LocalTransform, WorldTransform>([&](Entity e, LocalTransform& local, WorldTransform& wt) {
        if (!world.has<Parent>(e)) {
            wt.matrix = local.matrix;
            auto* children = world.try_get<Children>(e);
            if (children) {
                for (auto child : children->entities)
                    queue.push(child);
            }
        }
    });

    // BFS through children
    while (!queue.empty()) {
        Entity e = queue.front();
        queue.pop();

        auto* parent_comp = world.try_get<Parent>(e);
        if (!parent_comp) continue;

        auto* parent_wt = world.try_get<WorldTransform>(parent_comp->entity);
        auto* local = world.try_get<LocalTransform>(e);
        auto* wt = world.try_get<WorldTransform>(e);
        if (!parent_wt || !local || !wt) continue;

        wt->matrix = Mat4::multiply(parent_wt->matrix, local->matrix);

        auto* children = world.try_get<Children>(e);
        if (children) {
            for (auto child : children->entities)
                queue.push(child);
        }
    }
}

} // namespace ecs
