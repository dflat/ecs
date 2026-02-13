#pragma once
#include "../math.hpp"

namespace ecs {

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
