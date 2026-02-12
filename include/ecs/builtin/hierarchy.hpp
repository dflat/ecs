#pragma once
#include "../entity.hpp"
#include <vector>

namespace ecs {

/**
 * @brief Component indicating an entity's parent in the scene hierarchy.
 * @details Presence of this component implies the entity is a child.
 * The parent should likely have a corresponding Children component.
 */
struct Parent {
    /** @brief The entity handle of the parent. */
    Entity entity = INVALID_ENTITY;
};

/**
 * @brief Component storing a list of child entities.
 * @details Maintained automatically by hierarchy operations if used correctly.
 */
struct Children {
    /** @brief List of child entity handles. */
    std::vector<Entity> entities;
};

} // namespace ecs
