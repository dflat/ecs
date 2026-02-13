#pragma once
#include <cstring>
#include <cmath>
#include "../math.hpp"

namespace ecs {

/**
 * @brief A 4x4 floating-point matrix for 3D transformations.
 * @details Stored in column-major order (OpenGL style).
 */
struct Mat4 {
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
        Mat4 r;
        std::memset(r.m, 0, sizeof(r.m));
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                for (int k = 0; k < 4; ++k) {
                    r.m[col * 4 + row] += a.m[k * 4 + row] * b.m[col * 4 + k];
                }
            }
        }
        return r;
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
     * @param pos The position vector.
     * @param rot The rotation quaternion.
     * @param scale The scale vector.
     * @return The composed transformation matrix.
     */
    static Mat4 compose(const Vec3& pos, const Quat& rot, const Vec3& scale) {
        Mat4 m;
        // 1. Rotation (Quat to Mat3)
        // https://en.wikipedia.org/wiki/Quaternions_and_spatial_rotation#Quaternion-derived_rotation_matrix
        float x = rot.x, y = rot.y, z = rot.z, w = rot.w;
        float x2 = x + x, y2 = y + y, z2 = z + z;
        float xx = x * x2, xy = x * y2, xz = x * z2;
        float yy = y * y2, yz = y * z2, zz = z * z2;
        float wx = w * x2, wy = w * y2, wz = w * z2;

        m.m[0] = (1.0f - (yy + zz)) * scale.x;
        m.m[1] = (xy + wz) * scale.x;
        m.m[2] = (xz - wy) * scale.x;
        m.m[3] = 0.0f;

        m.m[4] = (xy - wz) * scale.y;
        m.m[5] = (1.0f - (xx + zz)) * scale.y;
        m.m[6] = (yz + wx) * scale.y;
        m.m[7] = 0.0f;

        m.m[8] = (xz + wy) * scale.z;
        m.m[9] = (yz - wx) * scale.z;
        m.m[10] = (1.0f - (xx + yy)) * scale.z;
        m.m[11] = 0.0f;

        m.m[12] = pos.x;
        m.m[13] = pos.y;
        m.m[14] = pos.z;
        m.m[15] = 1.0f;
        
        return m;
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
