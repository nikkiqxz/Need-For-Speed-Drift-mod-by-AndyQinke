#pragma once

#include <cstdint>

namespace nfsmw_exhaust::sunset_lighting {

struct Vec3 {
    float x;
    float y;
    float z;
};

struct SpotLight {
    Vec3 position;
    Vec3 direction;
    Vec3 color;
    float range;
    float intensity;
    float innerAngle;
    float outerAngle;
    float specular;
};

struct SpotLightModel32 {
    SpotLight light;
    std::int32_t source;
    std::uint32_t flare;
    float flareIntensity;
    float distance;
    std::uint8_t added;
    std::uint8_t padding[3];
};

static_assert(sizeof(SpotLight) == 0x38,
              "SunSet SpotLight layout must remain 56 bytes");
static_assert(sizeof(SpotLightModel32) == 0x4C,
              "SunSet x86 SpotLightModel layout must remain 76 bytes");

bool isValid(const SpotLight& light) noexcept;

bool transformExhaustLight(const SpotLight& configured,
                           const Vec3& emitterPosition,
                           const float vehicleMatrix[16],
                           float weatherLightPower,
                           SpotLight* output) noexcept;

bool samePosition(const SpotLight& left, const SpotLight& right) noexcept;

}  // namespace nfsmw_exhaust::sunset_lighting
