#pragma once

namespace ecs {

/**
 * @brief Standard packed float3 layout.
 * @details Matches GLM, Raylib, Unity, Unreal, etc.
 * Designed to be binary-compatible with common math libraries.
 */
struct Vec3 {
    /** @brief X component. */
    float x;
    /** @brief Y component. */
    float y;
    /** @brief Z component. */
    float z;
};

/**
 * @brief Standard packed float4 quaternion layout (x,y,z,w).
 * @details Matches GLM internal storage.
 * Designed to be binary-compatible with common math libraries.
 */
struct Quat {
    /** @brief X component of the vector part. */
    float x;
    /** @brief Y component of the vector part. */
    float y;
    /** @brief Z component of the vector part. */
    float z;
    /** @brief W component (scalar part). */
    float w;
};

/**
 * @brief Standard packed 4x4 matrix (16 floats).
 * @details Stored in column-major order.
 * Designed to be binary-compatible with GLM, Raylib, and OpenGL.
 */
struct alignas(16) Mat4 {
    float m[16];
};

} // namespace ecs
