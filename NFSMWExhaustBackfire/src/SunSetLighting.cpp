#include "SunSetLighting.hpp"

#include <cstddef>
#include <cmath>

namespace nfsmw_exhaust::sunset_lighting {
namespace {

bool finiteVec3(const Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

}  // namespace

bool isValid(const SpotLight& light) noexcept {
    return finiteVec3(light.position) && finiteVec3(light.direction) &&
           finiteVec3(light.color) && std::isfinite(light.range) &&
           std::isfinite(light.intensity) &&
           std::isfinite(light.innerAngle) &&
           std::isfinite(light.outerAngle) &&
           std::isfinite(light.specular) && light.range > 0.0f &&
           light.intensity >= 0.0f;
}

bool transformExhaustLight(const SpotLight& configured,
                           const Vec3& emitterPosition,
                           const float vehicleMatrix[16],
                           float weatherLightPower,
                           SpotLight* output) noexcept {
    if (vehicleMatrix == nullptr || output == nullptr ||
        !isValid(configured) || !finiteVec3(emitterPosition) ||
        !std::isfinite(weatherLightPower) || weatherLightPower < 0.0f) {
        return false;
    }
    for (std::size_t index = 0; index < 16; ++index) {
        if (!std::isfinite(vehicleMatrix[index])) return false;
    }

    SpotLight transformed = configured;
    const Vec3 local{
        configured.position.x + emitterPosition.x,
        configured.position.y + emitterPosition.y,
        configured.position.z + emitterPosition.z,
    };
    const float w = local.x * vehicleMatrix[3] +
                    local.y * vehicleMatrix[7] +
                    local.z * vehicleMatrix[11] + vehicleMatrix[15];
    if (!std::isfinite(w) || std::fabs(w) < 0.000001f) return false;

    transformed.position.x =
        (local.x * vehicleMatrix[0] + local.y * vehicleMatrix[4] +
         local.z * vehicleMatrix[8] + vehicleMatrix[12]) /
        w;
    transformed.position.y =
        (local.x * vehicleMatrix[1] + local.y * vehicleMatrix[5] +
         local.z * vehicleMatrix[9] + vehicleMatrix[13]) /
        w;
    transformed.position.z =
        (local.x * vehicleMatrix[2] + local.y * vehicleMatrix[6] +
         local.z * vehicleMatrix[10] + vehicleMatrix[14]) /
        w;

    Vec3 direction{
        configured.direction.x * vehicleMatrix[0] +
            configured.direction.y * vehicleMatrix[4] +
            configured.direction.z * vehicleMatrix[8],
        configured.direction.x * vehicleMatrix[1] +
            configured.direction.y * vehicleMatrix[5] +
            configured.direction.z * vehicleMatrix[9],
        configured.direction.x * vehicleMatrix[2] +
            configured.direction.y * vehicleMatrix[6] +
            configured.direction.z * vehicleMatrix[10],
    };
    const float directionLength = std::sqrt(
        direction.x * direction.x + direction.y * direction.y +
        direction.z * direction.z);
    if (!std::isfinite(directionLength) || directionLength < 0.000001f) {
        return false;
    }
    transformed.direction = {
        direction.x / directionLength,
        direction.y / directionLength,
        direction.z / directionLength,
    };
    transformed.color.x *= weatherLightPower;
    transformed.color.y *= weatherLightPower;
    transformed.color.z *= weatherLightPower;
    if (!isValid(transformed)) return false;

    *output = transformed;
    return true;
}

bool samePosition(const SpotLight& left, const SpotLight& right) noexcept {
    return left.position.x == right.position.x &&
           left.position.y == right.position.y &&
           left.position.z == right.position.z;
}

}  // namespace nfsmw_exhaust::sunset_lighting
