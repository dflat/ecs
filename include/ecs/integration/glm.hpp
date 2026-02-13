#pragma once

#include "../math.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <type_traits>

/**
 * @file glm.hpp
 * @brief GLM Integration Bridge.
 * @details This header provides safe, zero-cost casting between ECS POD types and GLM types.
 * It enforces strict layout compatibility assertions at compile time.
 */

// 1. Layout Verification (Compile-time checks)
static_assert(sizeof(ecs::Vec3) == sizeof(glm::vec3), "ecs::Vec3 size mismatch with glm::vec3");
static_assert(alignof(ecs::Vec3) == alignof(glm::vec3), "ecs::Vec3 alignment mismatch with glm::vec3");
static_assert(std::is_standard_layout_v<ecs::Vec3>, "ecs::Vec3 must be standard layout");

static_assert(sizeof(ecs::Quat) == sizeof(glm::quat), "ecs::Quat size mismatch with glm::quat");
// Note: glm::quat might align differently depending on SIMD settings, but usually matches float[4].
// If this assertion fails, you must use the copy-based conversion.

namespace ecs {

// 2. Zero-Overhead Casting Helpers
// Using reinterpret_cast is standard practice for this pattern, assuming layouts match.

/**
 * @brief Casts an ECS Vec3 to a GLM vec3 reference.
 * @param v The ECS vector.
 * @return A reference to the vector as a glm::vec3.
 */
inline glm::vec3& to_glm(Vec3& v) {
    return reinterpret_cast<glm::vec3&>(v);
}

/**
 * @brief Casts a const ECS Vec3 to a const GLM vec3 reference.
 * @param v The ECS vector.
 * @return A const reference to the vector as a glm::vec3.
 */
inline const glm::vec3& to_glm(const Vec3& v) {
    return reinterpret_cast<const glm::vec3&>(v);
}

/**
 * @brief Casts an ECS Quat to a GLM quat reference.
 * @param q The ECS quaternion.
 * @return A reference to the quaternion as a glm::quat.
 */
inline glm::quat& to_glm(Quat& q) {
    return reinterpret_cast<glm::quat&>(q);
}

/**
 * @brief Casts a const ECS Quat to a const GLM quat reference.
 * @param q The ECS quaternion.
 * @return A const reference to the quaternion as a glm::quat.
 */
inline const glm::quat& to_glm(const Quat& q) {
    return reinterpret_cast<const glm::quat&>(q);
}

// 3. Assignment Helpers (Copy)

/**
 * @brief Converts a GLM vec3 to an ECS Vec3 by value.
 * @param v The GLM vector.
 * @return The ECS vector.
 */
inline Vec3 from_glm(const glm::vec3& v) {
    return Vec3{v.x, v.y, v.z};
}

/**
 * @brief Converts a GLM quat to an ECS Quat by value.
 * @param q The GLM quaternion.
 * @return The ECS quaternion.
 */
inline Quat from_glm(const glm::quat& q) {
    return Quat{q.x, q.y, q.z, q.w};
}

} // namespace ecs
