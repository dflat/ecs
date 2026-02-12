#pragma once

/**
 * @file ecs.hpp
 * @brief Main entry point for the ECS library.
 * @details Includes all core ECS functionality and builtin components/systems.
 */

#include "archetype.hpp"
#include "command_buffer.hpp"
#include "component.hpp"
#include "entity.hpp"
#include "prefab.hpp"
#include "serialization.hpp"
#include "system.hpp"
#include "world.hpp"

#include "builtin/hierarchy.hpp"
#include "builtin/hierarchy_ops.hpp"
#include "builtin/transform.hpp"
#include "builtin/transform_propagation.hpp"
