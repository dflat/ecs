#pragma once
#include "../world.hpp"
#include "hierarchy.hpp"

#include <algorithm>
#include <vector>

namespace ecs {

inline void set_parent(World& world, Entity child, Entity parent) {
    ECS_ASSERT(child != parent, "cannot parent entity to itself");
    if (!world.alive(child) || !world.alive(parent))
        return;

    // If child already has a Parent, remove from old parent's Children
    if (world.has<Parent>(child)) {
        Entity old_parent = world.get<Parent>(child).entity;
        if (world.alive(old_parent) && world.has<Children>(old_parent)) {
            auto& kids = world.get<Children>(old_parent).entities;
            kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
        }
    }

    // Set Parent on child (add or overwrite)
    world.add(child, Parent{parent});

    // Add child to new parent's Children (create Children if absent)
    if (!world.has<Children>(parent))
        world.add(parent, Children{{}});
    world.get<Children>(parent).entities.push_back(child);
}

inline void remove_parent(World& world, Entity child) {
    if (!world.alive(child) || !world.has<Parent>(child))
        return;

    Entity parent = world.get<Parent>(child).entity;
    if (world.alive(parent) && world.has<Children>(parent)) {
        auto& kids = world.get<Children>(parent).entities;
        kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
    }

    world.remove<Parent>(child);
}

inline void destroy_recursive(World& world, Entity root) {
    if (!world.alive(root))
        return;

    // BFS to collect all descendants
    std::vector<Entity> to_destroy;
    to_destroy.push_back(root);
    size_t cursor = 0;
    while (cursor < to_destroy.size()) {
        Entity e = to_destroy[cursor++];
        auto* kids = world.try_get<Children>(e);
        if (kids) {
            for (auto child : kids->entities) {
                if (world.alive(child))
                    to_destroy.push_back(child);
            }
        }
    }

    // Destroy in reverse (leaves first)
    for (auto it = to_destroy.rbegin(); it != to_destroy.rend(); ++it)
        world.destroy(*it);
}

} // namespace ecs
