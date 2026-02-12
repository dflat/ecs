#pragma once
#include "../world.hpp"
#include "hierarchy.hpp"
#include "transform.hpp"

#include <queue>

namespace ecs {

/**
 * @brief Updates `WorldTransform` components based on `LocalTransform` and hierarchy.
 *
 * @details Performs a Breadth-First Search (BFS) traversal starting from root entities (those with
 * no `Parent`). Computes the world transform by multiplying the parent's world transform
 * with the child's local transform.
 *
 * - Roots: WorldTransform = LocalTransform
 * - Children: WorldTransform = Parent.WorldTransform * LocalTransform
 *
 * @param world The world to update.
 * @note Entities missing either LocalTransform or WorldTransform are skipped.
 */
inline void propagate_transforms(World& world) {
    std::queue<Entity> queue;

    // Find roots (entities with transforms but no Parent) and seed the BFS
    world.each<LocalTransform, WorldTransform>(
        World::Exclude<Parent>{}, [&](Entity e, LocalTransform& local, WorldTransform& wt) {
            wt.matrix = local.matrix;
            auto* children = world.try_get<Children>(e);
            if (children) {
                for (auto child : children->entities)
                    queue.push(child);
            }
        });

    // BFS through children
    while (!queue.empty()) {
        Entity e = queue.front();
        queue.pop();

        auto* parent_comp = world.try_get<Parent>(e);
        if (!parent_comp)
            continue;

        auto* parent_wt = world.try_get<WorldTransform>(parent_comp->entity);
        auto* local = world.try_get<LocalTransform>(e);
        auto* wt = world.try_get<WorldTransform>(e);
        if (!parent_wt || !local || !wt)
            continue;

        wt->matrix = Mat4::multiply(parent_wt->matrix, local->matrix);

        auto* children = world.try_get<Children>(e);
        if (children) {
            for (auto child : children->entities)
                queue.push(child);
        }
    }
}

} // namespace ecs
