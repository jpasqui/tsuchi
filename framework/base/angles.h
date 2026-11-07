// The little arithmetic the framework needs for angles and rotations. The
// game build has no math library, so these are worked out here. Angles in
// degrees are the engine's: pitch, yaw, roll. A quaternion is x, y, z, w.
#pragma once

namespace tsuchi::math {

constexpr float kPi = 3.14159265358979f;

inline float square_root(float x) {
#if defined(__aarch64__)
    float result;
    asm("fsqrt %s0, %s1" : "=w"(result) : "w"(x)); // the built-in would call a library that is not there
    return result;
#else
    return __builtin_sqrtf(x);
#endif
}

// Sine of an angle in radians, to within 1e-5.
inline float sine(float x) {
    if (x > 1e4f || x < -1e4f) return 0;
    while (x > kPi) x -= 2 * kPi;
    while (x < -kPi) x += 2 * kPi;
    if (x > kPi / 2) x = kPi - x;
    else if (x < -kPi / 2) x = -kPi - x;
    const float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}
inline float cosine(float x) { return sine(x + kPi / 2); }

// Arctangent, to within 1e-6 radians.
inline float arctangent(float x) {
    const bool negative = x < 0;
    if (negative) x = -x;
    const bool inverted = x > 1;
    if (inverted) x = 1 / x;
    // Above tan(pi/12), atan(x) = pi/6 + atan((x - 1/sqrt 3) / (1 + x/sqrt 3)), which keeps the series short.
    const float third = 0.5773502691896258f;
    const bool shifted = x > 0.2679491924311228f;
    if (shifted) x = (x - third) / (1 + third * x);
    const float x2 = x * x;
    float result = x * (1 - x2 * (0.3333333333f - x2 * (0.2f - x2 * (0.1428571429f - x2 * 0.1111111111f))));
    if (shifted) result += kPi / 6;
    if (inverted) result = kPi / 2 - result;
    return negative ? -result : result;
}
inline float arctangent(float y, float x) {
    if (x > 0) return arctangent(y / x);
    if (x < 0) return y >= 0 ? arctangent(y / x) + kPi : arctangent(y / x) - kPi;
    return y > 0 ? kPi / 2 : y < 0 ? -kPi / 2 : 0;
}

inline float within_half_turn(float degrees) {
    while (degrees > 180) degrees -= 360;
    while (degrees < -180) degrees += 360;
    return degrees;
}

// A quaternion as pitch, yaw and roll, the way the engine turns one into the other.
inline void to_angles(const float q[4], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3], degrees = 180 / kPi;
    const float pole = z * x - w * y, limit = 0.4999995f;
    const float yaw = arctangent(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * degrees;
    if (pole < -limit) {
        out[0] = -90, out[1] = yaw, out[2] = within_half_turn(-yaw - 2 * arctangent(x, w) * degrees);
    } else if (pole > limit) {
        out[0] = 90, out[1] = yaw, out[2] = within_half_turn(yaw - 2 * arctangent(x, w) * degrees);
    } else {
        const float s = 2 * pole;
        out[0] = arctangent(s, square_root(1 - s * s)) * degrees;
        out[1] = yaw;
        out[2] = arctangent(-2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * degrees;
    }
}

// Pitch, yaw and roll as a quaternion, the engine's way.
inline void to_quaternion(const float angles[3], float q[4]) {
    const float half = kPi / 360;
    const float sp = sine(angles[0] * half), cp = cosine(angles[0] * half);
    const float sy = sine(angles[1] * half), cy = cosine(angles[1] * half);
    const float sr = sine(angles[2] * half), cr = cosine(angles[2] * half);
    q[0] = cr * sp * sy - sr * cp * cy;
    q[1] = -cr * sp * cy - sr * cp * sy;
    q[2] = cr * cp * sy - sr * sp * cy;
    q[3] = cr * cp * cy + sr * sp * sy;
}

// a times b: b's turn first, then a's.
inline void multiply(const float a[4], const float b[4], float out[4]) {
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x, out[1] = y, out[2] = z, out[3] = w;
}

} // namespace tsuchi::math
