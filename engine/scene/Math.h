#pragma once
// Small column-major float math for engine/scene and engine/renderer (plan F3 Task 4). Element (row r, column c) of a
// Mat4 is m[c * 4 + r]: the layout of glTF matrices and of an HLSL column_major float4x4 used with mul(M, v).
#include <array>
#include <cmath>
#include <cstddef>

namespace eng::scene {

using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;

struct Mat4 {
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float& at(int r, int c) { return m[static_cast<size_t>(c * 4 + r)]; }
    float at(int r, int c) const { return m[static_cast<size_t>(c * 4 + r)]; }
    static Mat4 identity() { return Mat4{}; }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 x;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a.at(r, k) * b.at(k, c);
            }
            x.at(r, c) = s;
        }
    }
    return x;
}

inline Vec3 transformPoint(const Mat4& a, const Vec3& p) {
    const float w = a.at(3, 0) * p[0] + a.at(3, 1) * p[1] + a.at(3, 2) * p[2] + a.at(3, 3);
    Vec3 o{};
    for (int r = 0; r < 3; ++r) {
        o[r] = a.at(r, 0) * p[0] + a.at(r, 1) * p[1] + a.at(r, 2) * p[2] + a.at(r, 3);
    }
    if (w != 0.0f && w != 1.0f) {
        for (float& v : o) {
            v /= w;
        }
    }
    return o;
}

inline Mat4 transpose(const Mat4& a) {
    Mat4 x;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            x.at(r, c) = a.at(c, r);
        }
    }
    return x;
}

// Determinant of the upper 3 x 3 (the sign decides the winding: glTF 2.0 §3.7.4 instantiation).
inline float determinant3(const Mat4& a) {
    return a.at(0, 0) * (a.at(1, 1) * a.at(2, 2) - a.at(1, 2) * a.at(2, 1)) -
           a.at(0, 1) * (a.at(1, 0) * a.at(2, 2) - a.at(1, 2) * a.at(2, 0)) +
           a.at(0, 2) * (a.at(1, 0) * a.at(2, 1) - a.at(1, 1) * a.at(2, 0));
}

// General 4 x 4 inverse (cofactors, in double); a singular matrix gives the identity.
inline Mat4 inverse(const Mat4& a) {
    double m[16], inv[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = a.m[static_cast<size_t>(i)];
    }
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 x;
    if (det == 0.0 || !std::isfinite(det)) {
        return x;
    }
    for (int i = 0; i < 16; ++i) {
        x.m[static_cast<size_t>(i)] = static_cast<float>(inv[i] / det);
    }
    return x;
}

// glTF node TRS -> matrix (T * R * S, quaternion x y z w).
inline Mat4 trs(const Vec3& t, const Vec4& q, const Vec3& s) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    Mat4 r;
    r.at(0, 0) = (1 - 2 * (y * y + z * z)) * s[0];
    r.at(1, 0) = 2 * (x * y + z * w) * s[0];
    r.at(2, 0) = 2 * (x * z - y * w) * s[0];
    r.at(0, 1) = 2 * (x * y - z * w) * s[1];
    r.at(1, 1) = (1 - 2 * (x * x + z * z)) * s[1];
    r.at(2, 1) = 2 * (y * z + x * w) * s[1];
    r.at(0, 2) = 2 * (x * z + y * w) * s[2];
    r.at(1, 2) = 2 * (y * z - x * w) * s[2];
    r.at(2, 2) = (1 - 2 * (x * x + y * y)) * s[2];
    r.at(0, 3) = t[0];
    r.at(1, 3) = t[1];
    r.at(2, 3) = t[2];
    return r;
}

inline Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline float dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline Vec3 normalize(const Vec3& a) {
    const float l = std::sqrt(dot(a, a));
    return l > 0.0f ? Vec3{a[0] / l, a[1] / l, a[2] / l} : Vec3{0.0f, 0.0f, 0.0f};
}

// Right-handed view matrix (camera looks down -Z), as gl-matrix lookAt.
inline Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 f = normalize(sub(target, eye));
    Vec3 s = normalize(cross(f, up));
    if (dot(s, s) == 0.0f) { // looking along `up`
        s = normalize(cross(f, Vec3{0.0f, 0.0f, 1.0f}));
    }
    const Vec3 u = cross(s, f);
    Mat4 v;
    v.at(0, 0) = s[0], v.at(0, 1) = s[1], v.at(0, 2) = s[2], v.at(0, 3) = -dot(s, eye);
    v.at(1, 0) = u[0], v.at(1, 1) = u[1], v.at(1, 2) = u[2], v.at(1, 3) = -dot(u, eye);
    v.at(2, 0) = -f[0], v.at(2, 1) = -f[1], v.at(2, 2) = -f[2], v.at(2, 3) = dot(f, eye);
    return v;
}

// Right-handed perspective, clip Z in [0, 1] (RHI convention), +Y up.
inline Mat4 perspectiveRhZo(float fovY, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 x;
    x.m.fill(0.0f);
    x.at(0, 0) = f / aspect;
    x.at(1, 1) = f;
    x.at(2, 2) = zFar / (zNear - zFar);
    x.at(2, 3) = zNear * zFar / (zNear - zFar);
    x.at(3, 2) = -1.0f;
    return x;
}

// Right-handed orthographic projection of the view-space box [l, r] x [b, t] x [-zFar, -zNear], clip Z in [0, 1].
inline Mat4 orthoRhZo(float l, float r, float b, float t, float zNear, float zFar) {
    Mat4 x;
    x.at(0, 0) = 2.0f / (r - l);
    x.at(1, 1) = 2.0f / (t - b);
    x.at(2, 2) = 1.0f / (zNear - zFar);
    x.at(0, 3) = -(r + l) / (r - l);
    x.at(1, 3) = -(t + b) / (t - b);
    x.at(2, 3) = zNear / (zNear - zFar);
    return x;
}

} // namespace eng::scene
