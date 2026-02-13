#pragma once
#include <cstring>
#include <cmath>
#include "../math.hpp"

// We are committing to GLM as our backend implementation
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace ecs {

/**
 * @brief A 4x4 floating-point matrix for 3D transformations.
 * @details Stored in column-major order (OpenGL style).
 * Aligned to 16 bytes to allow for SIMD optimizations in the backing math library.
 */
struct alignas(16) Mat4 {
    /** @brief The 16 floats of the matrix. */
    float m[16];

    /** @brief Default constructor initializes to Identity. */
    Mat4() { identity(); }

    /** @brief Resets the matrix to the Identity matrix. */
    void identity() {
        std::memset(m, 0, sizeof(m));
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    /**
     * @brief Multiplies two matrices.
     * @details Computes `a * b`.
     * @param a The left-hand operand.
     * @param b The right-hand operand.
     * @return The result of the multiplication.
     */
    static Mat4 multiply(const Mat4& a, const Mat4& b) {
        // Bridge to GLM for optimized multiply (potentially SIMD)
        const glm::mat4& ga = reinterpret_cast<const glm::mat4&>(a);
        const glm::mat4& gb = reinterpret_cast<const glm::mat4&>(b);
        glm::mat4 r = ga * gb;
        return reinterpret_cast<Mat4&>(r);
    }

    /**
     * @brief Creates a translation matrix.
     * @param x X translation.
     * @param y Y translation.
     * @param z Z translation.
     * @return A translation matrix.
     */
    static Mat4 translation(float x, float y, float z) {
        Mat4 r;
        r.m[12] = x;
        r.m[13] = y;
        r.m[14] = z;
        return r;
    }

    /**
     * @brief Composes a matrix from position, rotation (quaternion), and scale.
     * @details Computes M = Translation * Rotation * Scale.
     * Optimized using GLM's vector/matrix operations.
     * @param pos The position vector.
     * @param rot The rotation quaternion.
     * @param scale The scale vector.
     * @return The composed transformation matrix.
     */
    static Mat4 compose(const Vec3& pos, const Quat& rot, const Vec3& scale) {
        // Direct cast since layouts are guaranteed by integration/glm.hpp
        const glm::vec3& g_pos = reinterpret_cast<const glm::vec3&>(pos);
        const glm::quat& g_rot = reinterpret_cast<const glm::quat&>(rot);
        const glm::vec3& g_scale = reinterpret_cast<const glm::vec3&>(scale);

        // 1. Rotation (Mat3 -> Mat4)
        glm::mat4 m = glm::mat4_cast(g_rot);

        // 2. Scale (Apply to columns 0, 1, 2)
        m[0] *= g_scale.x;
        m[1] *= g_scale.y;
        m[2] *= g_scale.z;

        // 3. Position (Column 3)
        m[3] = glm::vec4(g_pos, 1.0f);

        return reinterpret_cast<Mat4&>(m);
    }

    /** @brief Checks exact equality via memcmp. */
    bool operator==(const Mat4& o) const { return std::memcmp(m, o.m, sizeof(m)) == 0; }
};

/**
 * @brief Component representing an entity's transform relative to its parent.
 * @details Stores Position, Rotation (Quaternion), and Scale directly (PRS).
 * This structure is designed for ease of use in gameplay logic.
 * Converting to Matrix happens only during propagation to WorldTransform.
 */
struct LocalTransform {
    /** @brief Position relative to parent. */
    Vec3 position = {0.0f, 0.0f, 0.0f};
    /** @brief Rotation relative to parent. Defaults to Identity (0,0,0,1). */
    Quat rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    /** @brief Scale relative to parent. Defaults to (1,1,1). */
    Vec3 scale = {1.0f, 1.0f, 1.0f};
};

/**
 * @brief Component representing an entity's absolute transform in world space.
 * @details Computed by combining the `LocalTransform` with the parent's `WorldTransform`.
 * Often updated automatically by a transform propagation system.
 * This is the transform used for rendering and physics.
 */
struct WorldTransform {
    /** @brief The global transformation matrix. */
    Mat4 matrix;
};

} // namespace ecs
