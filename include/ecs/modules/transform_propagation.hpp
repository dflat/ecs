#pragma once
#include "../world.hpp"
#include "hierarchy.hpp"
#include "transform.hpp"

#include <queue>

namespace ecs {

/**
 * @brief Propagates LocalTransforms to WorldTransforms through the hierarchy.
 * @details Performs a Breadth-First Search (BFS) starting from root entities (entities with
 * LocalTransform but no Parent). For each entity, it computes the WorldTransform by multiplying
 * the parent's WorldTransform with the entity's LocalTransform matrix (composed from PRS).
 * 
 * Roots: WorldTransform = Compose(LocalTransform)
 * Children: WorldTransform = Parent.WorldTransform * Compose(LocalTransform)
 * 
 * @param world The ECS world to process.
 */
inline void propagate_transforms(World& world) {
    std::queue<Entity> queue;

    // Find roots (entities with transforms but no Parent) and seed the BFS
    world.each<LocalTransform, WorldTransform>(
        World::Exclude<Parent>{}, [&](Entity e, LocalTransform& local, WorldTransform& wt) {
            // Root entity: World = Local (composed)
            wt.matrix = Mat4::compose(local.position, local.rotation, local.scale);
            
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

        // Child World = Parent World * Local Matrix
        Mat4 local_mat = Mat4::compose(local->position, local->rotation, local->scale);
        wt->matrix = Mat4::multiply(parent_wt->matrix, local_mat);

        auto* children = world.try_get<Children>(e);
        if (children) {
            for (auto child : children->entities)
                queue.push(child);
        }
    }
}

} // namespace ecs
