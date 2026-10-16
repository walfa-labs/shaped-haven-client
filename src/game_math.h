#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace haven {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

constexpr float kTickHz = 60.0f;
constexpr float kSnapshotHz = 20.0f;
constexpr float kTickSeconds = 1.0f / kTickHz;
constexpr float kSnapshotSeconds = 1.0f / kSnapshotHz;

constexpr float kPlayerMoveSpeed = 12.0f;
constexpr float kArenaHalfSize = 1000.0f;

/// The orthographic camera's distance above the XY plane. HUD elements are
/// parented to the camera's node so they follow it, and this constant lets
/// them cancel the camera's own z and sit at the z the combat layers use.
constexpr float kCameraZ = 10.0f;

/// Remote entities are rendered this far behind the newest snapshot, i.e. two
/// 20 Hz snapshot intervals (DESIGN.md section 2).
constexpr float kInterpolationDelaySeconds = 0.1f;

/// Snapshot positions are i16 in 0.05 unit steps (DESIGN.md section 2).
constexpr float kPositionQuantStep = 0.05f;

constexpr float kReconcileSnapThreshold = 2.0f;
constexpr float kReconcileBlendRate = 0.15f;

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
inline Vec2 operator*(float s, Vec2 a) { return a * s; }
inline Vec2& operator+=(Vec2& a, Vec2 b) { a = a + b; return a; }
inline Vec2& operator-=(Vec2& a, Vec2 b) { a = a - b; return a; }

inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float length(Vec2 v) { return std::sqrt(dot(v, v)); }
inline float distance(Vec2 a, Vec2 b) { return length(a - b); }

/// Returns a zero vector for (near) zero-length input rather than dividing by
/// zero, so an idle input frame is a no-op instead of NaN.
inline Vec2 normalized(Vec2 v)
{
    const float len = length(v);
    return len > 1e-6f ? Vec2{v.x / len, v.y / len} : Vec2{0.0f, 0.0f};
}

inline Vec2 lerp(Vec2 a, Vec2 b, float alpha) { return a + (b - a) * alpha; }

/// Hard arena walls at +/-1000 on both axes (DESIGN.md section 1).
inline Vec2 clamped_to_arena(Vec2 v)
{
    return {std::clamp(v.x, -kArenaHalfSize, kArenaHalfSize),
            std::clamp(v.y, -kArenaHalfSize, kArenaHalfSize)};
}

/// One fixed 60 Hz step of the authoritative player integrator.
inline Vec2 integrate(Vec2 pos, Vec2 move, float dt)
{
    return clamped_to_arena(pos + normalized(move) * (kPlayerMoveSpeed * dt));
}

inline int16_t encode_pos(float units)
{
    return static_cast<int16_t>(std::lround(units / kPositionQuantStep));
}

inline float decode_pos(int16_t quantized)
{
    return static_cast<float>(quantized) * kPositionQuantStep;
}

/// Velocity/orientation travel as a u8 angle, 0..255 mapping 0..2pi
/// (DESIGN.md section 2). Stored as [0, 2pi); decoded back to radians.
inline uint8_t encode_angle(float radians)
{
    constexpr float kTwoPi = 6.28318530717958647692f;
    float wrapped = std::fmod(radians, kTwoPi);
    if (wrapped < 0.0f) wrapped += kTwoPi;
    return static_cast<uint8_t>(std::lround(wrapped * (256.0f / kTwoPi)) & 0xFF);
}

inline float decode_angle(uint8_t value)
{
    constexpr float kTwoPi = 6.28318530717958647692f;
    return static_cast<float>(value) * (kTwoPi / 256.0f);
}

}  // namespace haven
