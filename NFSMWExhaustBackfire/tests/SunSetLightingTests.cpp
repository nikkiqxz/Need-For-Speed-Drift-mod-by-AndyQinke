#include "SunSetLighting.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace lighting = nfsmw_exhaust::sunset_lighting;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

}  // namespace

int main() {
    const lighting::SpotLight configured{
        {-0.35f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {1.0f, 0.45f, 0.0f},
        0.75f,
        5.0f,
        359.0f,
        360.0f,
        0.0f,
    };
    const float translated[16]{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        10.0f, 20.0f, 30.0f, 1.0f,
    };
    lighting::SpotLight result{};
    require(lighting::transformExhaustLight(
                configured, {1.0f, 2.0f, 3.0f}, translated, 0.5f,
                &result),
            "valid ExhaustLight must transform");
    require(near(result.position.x, 10.65f) &&
                near(result.position.y, 22.0f) &&
                near(result.position.z, 33.0f),
            "position must combine config, emitter, and vehicle matrix");
    require(near(result.direction.x, 0.0f) &&
                near(result.direction.y, 1.0f) &&
                near(result.direction.z, 0.0f),
            "direction must remain normalized");
    require(near(result.color.x, 0.5f) &&
                near(result.color.y, 0.225f) &&
                near(result.color.z, 0.0f),
            "color must use SunSet weather light power");
    require(lighting::samePosition(result, result),
            "duplicate light positions must compare equal");

    auto invalid = configured;
    invalid.range = 0.0f;
    require(!lighting::transformExhaustLight(
                invalid, {0.0f, 0.0f, 0.0f}, translated, 1.0f, &result),
            "invalid ExhaustLight range must be rejected");

    std::cout << "SunSet lighting tests passed\n";
    return 0;
}
