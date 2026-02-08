#pragma once
#include <cstring>

namespace ecs {

struct Mat4 {
    float m[16];

    Mat4() { identity(); }

    void identity() {
        std::memset(m, 0, sizeof(m));
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    // Column-major multiply: result = a * b
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

    static Mat4 translation(float x, float y, float z) {
        Mat4 r;
        r.m[12] = x; r.m[13] = y; r.m[14] = z;
        return r;
    }

    bool operator==(const Mat4& o) const { return std::memcmp(m, o.m, sizeof(m)) == 0; }
};

struct LocalTransform {
    Mat4 matrix;
};

struct WorldTransform {
    Mat4 matrix;
};

} // namespace ecs
