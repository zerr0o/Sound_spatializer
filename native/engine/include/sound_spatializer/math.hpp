#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace sound_spatializer {

constexpr float kPi = std::numbers::pi_v<float>;

struct Vec3f {
    float x{};
    float y{};
    float z{};

    constexpr Vec3f operator+(const Vec3f& rhs) const noexcept { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
    constexpr Vec3f operator-(const Vec3f& rhs) const noexcept { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
    constexpr Vec3f operator*(float scalar) const noexcept { return {x * scalar, y * scalar, z * scalar}; }
    constexpr Vec3f operator/(float scalar) const noexcept { return {x / scalar, y / scalar, z / scalar}; }
};

[[nodiscard]] inline float dot(const Vec3f& a, const Vec3f& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] inline float length(const Vec3f& value) noexcept {
    return std::sqrt(dot(value, value));
}

[[nodiscard]] inline Vec3f normalized(const Vec3f& value) noexcept {
    const float magnitude = length(value);
    return magnitude > 1.0e-8F ? value / magnitude : Vec3f{};
}

struct Quaternionf {
    float w{1.0F};
    float x{};
    float y{};
    float z{};

    [[nodiscard]] Quaternionf normalized_value() const noexcept {
        const float magnitude = std::sqrt(w * w + x * x + y * y + z * z);
        if (magnitude <= 1.0e-8F) {
            return {};
        }
        return {w / magnitude, x / magnitude, y / magnitude, z / magnitude};
    }

    [[nodiscard]] Quaternionf conjugate() const noexcept { return {w, -x, -y, -z}; }
};

[[nodiscard]] inline Quaternionf operator*(const Quaternionf& a, const Quaternionf& b) noexcept {
    return {
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

[[nodiscard]] inline Vec3f rotate(const Quaternionf& rotation, const Vec3f& vector) noexcept {
    const Quaternionf q = rotation.normalized_value();
    const Quaternionf result = q * Quaternionf{0.0F, vector.x, vector.y, vector.z} * q.conjugate();
    return {result.x, result.y, result.z};
}

[[nodiscard]] inline Quaternionf quaternion_from_rotation_vector(const Vec3f& value) noexcept {
    const float angle = length(value);
    if (angle <= 1.0e-8F) {
        return {};
    }
    const float half = angle * 0.5F;
    const float scale = std::sin(half) / angle;
    return Quaternionf{std::cos(half), value.x * scale, value.y * scale, value.z * scale}.normalized_value();
}

[[nodiscard]] inline Vec3f rotation_vector_from_quaternion(Quaternionf value) noexcept {
    value = value.normalized_value();
    if (value.w < 0.0F) {
        value = {-value.w, -value.x, -value.y, -value.z};
    }
    const float vector_length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (vector_length <= 1.0e-8F) {
        return {};
    }
    const float angle = 2.0F * std::atan2(vector_length, std::clamp(value.w, -1.0F, 1.0F));
    return {value.x * angle / vector_length, value.y * angle / vector_length, value.z * angle / vector_length};
}

[[nodiscard]] inline Quaternionf slerp(Quaternionf a, Quaternionf b, float alpha) noexcept {
    a = a.normalized_value();
    b = b.normalized_value();
    float cosine = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (cosine < 0.0F) {
        b = {-b.w, -b.x, -b.y, -b.z};
        cosine = -cosine;
    }
    alpha = std::clamp(alpha, 0.0F, 1.0F);
    if (cosine > 0.9995F) {
        return Quaternionf{
            a.w + alpha * (b.w - a.w),
            a.x + alpha * (b.x - a.x),
            a.y + alpha * (b.y - a.y),
            a.z + alpha * (b.z - a.z),
        }.normalized_value();
    }
    const float angle = std::acos(std::clamp(cosine, -1.0F, 1.0F));
    const float denominator = std::sin(angle);
    const float left = std::sin((1.0F - alpha) * angle) / denominator;
    const float right = std::sin(alpha * angle) / denominator;
    return {a.w * left + b.w * right, a.x * left + b.x * right, a.y * left + b.y * right, a.z * left + b.z * right};
}

[[nodiscard]] inline std::array<float, 3> yaw_pitch_roll(const Quaternionf& input) noexcept {
    const Quaternionf q = input.normalized_value();
    const float yaw = std::atan2(2.0F * (q.w * q.y + q.x * q.z), 1.0F - 2.0F * (q.y * q.y + q.x * q.x));
    const float pitch = std::asin(std::clamp(2.0F * (q.w * q.x - q.z * q.y), -1.0F, 1.0F));
    const float roll = std::atan2(2.0F * (q.w * q.z + q.x * q.y), 1.0F - 2.0F * (q.z * q.z + q.x * q.x));
    return {yaw, pitch, roll};
}

} // namespace sound_spatializer

