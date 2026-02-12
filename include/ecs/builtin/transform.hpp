#pragma once
#include <cstring>

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

    /** @brief Checks exact equality via memcmp. */
    bool operator==(const Mat4& o) const { return std::memcmp(m, o.m, sizeof(m)) == 0; }
};

/**
 * @brief Component representing an entity's transform relative to its parent.
 * @details If the entity has no parent, this is equivalent to the world transform.
 */
struct LocalTransform {
    /** @brief The local transformation matrix. */
    Mat4 matrix;
};

/**
 * @brief Component representing an entity's absolute transform in world space.
 * @details Computed by combining the `LocalTransform` with the parent's `WorldTransform`.
 * Often updated automatically by a transform propagation system.
 */
struct WorldTransform {
    /** @brief The global transformation matrix. */
    Mat4 matrix;
};

} // namespace ecs
