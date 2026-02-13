#pragma once

#include "../math.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <type_traits>
#include <cstring>

/**
 * @file glm.hpp
 * @brief GLM Integration Bridge.
 * @details This header provides safe, zero-cost casting and high-performance 
 * math operations (inlined) between ECS POD types and GLM.
 */

// 1. Layout Verification (Compile-time checks)
static_assert(sizeof(ecs::Vec3) == sizeof(glm::vec3), "ecs::Vec3 size mismatch");
static_assert(sizeof(ecs::Quat) == sizeof(glm::quat), "ecs::Quat size mismatch");
static_assert(sizeof(ecs::Mat4) == sizeof(glm::mat4), "ecs::Mat4 size mismatch");

namespace ecs {

// --- Zero-Overhead Casting ---

inline const glm::vec3& to_glm(const Vec3& v) { return reinterpret_cast<const glm::vec3&>(v); }
inline const glm::quat& to_glm(const Quat& q) { return reinterpret_cast<const glm::quat&>(q); }
inline const glm::mat4& to_glm(const Mat4& m) { return reinterpret_cast<const glm::mat4&>(m); }
inline glm::mat4& to_glm(Mat4& m) { return reinterpret_cast<glm::mat4&>(m); }

// --- Inlined Math Operations ---

/** @brief Resets matrix to identity. */
inline void mat4_identity(Mat4& m) {
    std::memset(m.m, 0, sizeof(m.m));
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
}

/** @brief Multiplies two matrices (a * b). Inlined GLM implementation. */
inline Mat4 mat4_multiply(const Mat4& a, const Mat4& b) {
    const glm::mat4& ga = to_glm(a);
    const glm::mat4& gb = to_glm(b);
    glm::mat4 r = ga * gb;
    return reinterpret_cast<Mat4&>(r);
}

/** @brief Composes a matrix from PRS. Inlined GLM implementation. */
inline Mat4 mat4_compose(const Vec3& pos, const Quat& rot, const Vec3& scale) {
    const glm::vec3& g_pos = to_glm(pos);
    const glm::quat& g_rot = to_glm(rot);
    const glm::vec3& g_scale = to_glm(scale);

    // Optimized composition
    glm::mat4 m = glm::mat4_cast(g_rot);
    m[0] *= g_scale.x;
    m[1] *= g_scale.y;
    m[2] *= g_scale.z;
    m[3] = glm::vec4(g_pos, 1.0f);

    return reinterpret_cast<Mat4&>(m);
}

} // namespace ecs
