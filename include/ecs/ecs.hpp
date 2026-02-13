#pragma once

/**
 * @file ecs.hpp
 * @brief Main entry point for the ECS library kernel.
 * @details Includes the core ECS functionality (Entity, Component, World, Systems).
 * Builtin modules like Transform and Hierarchy must be included separately.
 */

#include "archetype.hpp"
#include "command_buffer.hpp"
#include "component.hpp"
#include "entity.hpp"
#include "prefab.hpp"
#include "serialization.hpp"
#include "system.hpp"
#include "world.hpp"
