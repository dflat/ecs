#pragma once
#include "../entity.hpp"
#include <vector>

namespace ecs {

struct Parent {
    Entity entity = INVALID_ENTITY;
};

struct Children {
    std::vector<Entity> entities;
};

} // namespace ecs
