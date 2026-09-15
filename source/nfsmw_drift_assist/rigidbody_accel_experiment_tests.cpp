#include "asi_host/rigidbody_accel_experiment.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using nfsmw_drift_asi::rigidbody_accel::Inputs;
using nfsmw_drift_asi::rigidbody_accel::DecelerationInputs;
using nfsmw_drift_asi::rigidbody_accel::DecelerationStrengthScale;
using nfsmw_drift_asi::rigidbody_accel::HandbrakeDecelerationEligible;
using nfsmw_drift_asi::rigidbody_accel::ComputeCountersteerAccelerationDirection;
using nfsmw_drift_asi::rigidbody_accel::ComputeCountersteerAccelerationDirectionFromAngle;
using nfsmw_drift_asi::rigidbody_accel::kCountersteerDirectionAngleScale;
using nfsmw_drift_asi::rigidbody_accel::kConfiguredEffectiveTargetAccelerationMps2;
using nfsmw_drift_asi::rigidbody_accel::kAdditionalStrengthReductionScale;
using nfsmw_drift_asi::rigidbody_accel::kConfiguredStrengthScale;
using nfsmw_drift_asi::rigidbody_accel::kConfiguredTargetAccelerationMps2;
using nfsmw_drift_asi::rigidbody_accel::MakePlan;
using nfsmw_drift_asi::rigidbody_accel::MakeDecelerationPlan;
using nfsmw_drift_asi::rigidbody_accel::PlanMode;
using nfsmw_drift_asi::rigidbody_accel::RejectReason;
using nfsmw_drift_asi::rigidbody_accel::SpeedAttenuation;
using nfsmw_drift_asi::rigidbody_accel::SelectPlanMode;
using nfsmw_drift_asi::rigidbody_accel::SustainedDriftEligible;
using nfsmw_drift_asi::rigidbody_accel::VerifyAppliedDelta;

int failures = 0;

void Require(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

Inputs ValidInput() {
    Inputs input{};
    input.enabled = true;
    input.sessionActive = true;
    input.identityStable = true;
    input.physicsStable = true;
    input.grounded = true;
    input.throttle = 1.0f;
    input.dt = 1.0f / 60.0f;
    input.forward = {0.0f, 0.0f, 1.0f};
    // Stay below the 70 km/h full-strength threshold for baseline tests.
    input.linearVelocity = {0.0f, 0.0f, 10.0f};
    input.targetAccelerationMps2 = kConfiguredTargetAccelerationMps2;
    input.ramp = 1.0f;
    return input;
}

Inputs InputAtTotalSpeedKmh(float totalSpeedKmh,
                            float forwardProjectionMps = 20.0f) {
    Inputs input = ValidInput();
    const float totalSpeedMps = totalSpeedKmh / 3.6f;
    const float lateralSquared =
        totalSpeedMps * totalSpeedMps -
        forwardProjectionMps * forwardProjectionMps;
    Require(lateralSquared >= 0.0f,
            "the side-slip fixture must have a realizable lateral component");
    input.linearVelocity = {
        std::sqrt(std::max(0.0f, lateralSquared)), 0.0f,
        forwardProjectionMps};
    return input;
}

void TestDisabledByDefault() {
    Inputs input = ValidInput();
    input.enabled = false;
    const auto plan = MakePlan(input);
    Require(!plan.accepted && plan.reason == RejectReason::Disabled,
            "the experiment must remain disabled unless explicitly enabled");
}

void TestOnlySustainedHoldingDriftIsEligible() {
    using nfsmw_drift::DriftPhase;

    Require(SustainedDriftEligible(true, DriftPhase::Holding, -1) &&
                SustainedDriftEligible(true, DriftPhase::Holding, 1),
            "both committed drift sides must be eligible while holding");
    Require(!SustainedDriftEligible(false, DriftPhase::Holding, 1),
            "an inactive session must never receive forward acceleration");
    Require(!SustainedDriftEligible(true, DriftPhase::Holding, 0) &&
                !SustainedDriftEligible(true, DriftPhase::Holding, 2),
            "holding without a committed drift side must fail closed");
    for (const DriftPhase phase : {
             DriftPhase::Off, DriftPhase::Charging,
             DriftPhase::WaitingDirection, DriftPhase::Entering,
             DriftPhase::SideTransition, DriftPhase::Centering,
             DriftPhase::Exiting}) {
        Require(!SustainedDriftEligible(true, phase, 1),
                "only the sustained Holding phase may receive acceleration");
    }
}

void TestValidPlanUsesDeltaV() {
    Require(std::fabs(kConfiguredTargetAccelerationMps2 - 5.25f) <
                1.0e-5f,
            "the release rigid-body target must remain 5.25 m/s^2");
    Require(std::fabs(kAdditionalStrengthReductionScale - 0.85f) <
                1.0e-5f,
            "the additional rigid-body strength reduction must be 15 percent");
    Require(std::fabs(kConfiguredStrengthScale - 0.612f) < 1.0e-5f,
            "the release rigid-body output must be attenuated to 61.2 percent");
    Require(std::fabs(kConfiguredEffectiveTargetAccelerationMps2 - 3.213f) <
                1.0e-5f,
            "the effective low-speed rigid-body target must be 3.213 m/s^2");
    const auto plan = MakePlan(ValidInput());
    Require(plan.accepted && plan.reason == RejectReason::None,
            "a fully valid forward plan should be accepted");
    Require(std::fabs(plan.deltaVMps -
                      (kConfiguredEffectiveTargetAccelerationMps2 / 60.0f)) <
                1.0e-5f,
            "Accelerate amount must include the 61.2 percent release scale");
    Require(std::fabs(plan.direction.z - 1.0f) < 1.0e-5f,
            "the plan direction must be normalized forward");
    Require(std::fabs(plan.speedMps - 10.0f) < 1.0e-5f,
            "the plan must expose the full linear speed magnitude");
}

void TestUnsafeInputsAreRejected() {
    Inputs input = ValidInput();
    input.throttle = 0.0f;
    Require(MakePlan(input).reason == RejectReason::NoThrottle,
            "zero throttle must not inject velocity");
    input = ValidInput();
    input.brake = 0.5f;
    Require(MakePlan(input).reason == RejectReason::Braking,
            "braking must suppress forward fallback acceleration");
    input = ValidInput();
    input.dt = 0.20f;
    Require(MakePlan(input).reason == RejectReason::InvalidDt,
            "a long physics step must not inject an oversized increment");
    input = ValidInput();
    input.linearVelocity = {0.0f, 0.0f, -2.0f};
    Require(MakePlan(input).reason == RejectReason::ReverseMotion,
            "reverse motion must never receive forward fallback acceleration");
    input = ValidInput();
    input.forward = {0.0f, 0.0f, 0.1f};
    Require(MakePlan(input).reason == RejectReason::InvalidForward,
            "an untrusted forward vector must fail closed");
}

void TestSpeedFadeAndRamp() {
    Inputs input = ValidInput();
    input.linearVelocity = {0.0f, 0.0f, 100.0f / 3.6f};
    const auto plan = MakePlan(input);
    Require(plan.accepted && plan.speedScale < 1.0f &&
                plan.speedScale > 0.5f,
            "mid-range speed must smoothly reduce the planned increment");
    input = ValidInput();
    input.ramp = 0.0f;
    Require(!MakePlan(input).accepted,
            "zero ramp must produce no write");
}

void TestSpeedAttenuationBoundariesAndInterpolation() {
    const float toMps = 1.0f / 3.6f;
    Require(std::fabs(SpeedAttenuation(70.0f * toMps) - 1.0f) < 1.0e-5f,
            "70 km/h must retain full acceleration");
    Require(std::fabs(SpeedAttenuation(125.0f * toMps) - 0.5f) < 1.0e-5f,
            "125 km/h must retain half acceleration");
    Require(std::fabs(SpeedAttenuation(150.0f * toMps) - 0.15f) < 1.0e-5f,
            "150 km/h must retain 15 percent acceleration");
    Require(std::fabs(SpeedAttenuation(170.0f * toMps)) < 1.0e-5f,
            "170 km/h must disable acceleration");
    Require(std::fabs(SpeedAttenuation(100.0f * toMps) -
                      (1.0f - 0.5f * (30.0f / 55.0f))) < 1.0e-5f,
            "70-125 km/h attenuation must be linear");
    Require(std::fabs(SpeedAttenuation(137.5f * toMps) - 0.325f) < 1.0e-5f,
            "125-150 km/h attenuation must be linear");
    Require(std::fabs(SpeedAttenuation(160.0f * toMps) - 0.075f) < 1.0e-5f,
            "150-170 km/h attenuation must be linear");
    Require(std::fabs(SpeedAttenuation(200.0f * toMps)) < 1.0e-5f,
            "speeds above 170 km/h must remain disabled");
    Require(std::fabs(SpeedAttenuation(-5.0f)) < 1.0e-5f,
            "reverse values must fail closed in the standalone curve");
    Require(std::fabs(SpeedAttenuation(
                          std::numeric_limits<float>::quiet_NaN())) <
                1.0e-5f,
            "a non-finite speed must fail closed in the standalone curve");
}

void TestSpeedAttenuationSuppressesPlanAtZero() {
    Inputs input = ValidInput();
    input.linearVelocity = {0.0f, 0.0f, 170.0f / 3.6f};
    const auto plan = MakePlan(input);
    Require(!plan.accepted && plan.reason == RejectReason::ZeroDemand &&
                std::fabs(plan.speedScale) < 1.0e-5f,
            "the 170 km/h zero point must suppress the write");
    input.linearVelocity = {0.0f, 0.0f, 125.0f / 3.6f};
    const auto halfPlan = MakePlan(input);
    Require(halfPlan.accepted && halfPlan.speedScale > 0.49f &&
                halfPlan.speedScale < 0.51f,
            "125 km/h must produce a half-strength plan at full ramp");
    Require(std::fabs(halfPlan.deltaVMps -
                      (kConfiguredEffectiveTargetAccelerationMps2 * 0.5f /
                       60.0f)) <
                1.0e-5f,
            "the 125 km/h plan must apply the half-strength target after release attenuation");
}

void TestStrengthAttenuationAppliesAcrossEverySpeedBand() {
    // The speed curve itself remains unchanged, while the effective target is
    // reduced by the same 61.2% release scale at its endpoints and an interior
    // interpolation.
    const float toMps = 1.0f / 3.6f;
    struct Case {
        float speedKmh;
        float speedMultiplier;
    };
    for (const Case test : {
             Case{70.0f, 1.0f},
             Case{125.0f, 0.5f},
             Case{150.0f, 0.15f},
             Case{160.0f, 0.075f},
         }) {
        Inputs input = ValidInput();
        input.linearVelocity = {0.0f, 0.0f, test.speedKmh * toMps};
        const auto plan = MakePlan(input);
        Require(plan.accepted,
                "every positive speed-band sample below the hard limit should be accepted");
        const float expected =
            kConfiguredEffectiveTargetAccelerationMps2 * test.speedMultiplier;
        Require(std::fabs(plan.targetAccelerationMps2 - expected) < 1.0e-4f,
                "effective target must be nominal target times speed attenuation and 0.612");
        Require(std::fabs(plan.deltaVMps - expected / 60.0f) < 1.0e-4f,
                "delta-v must preserve the 61.2 percent release scale across speed bands");
    }

    Inputs input = ValidInput();
    input.linearVelocity = {0.0f, 0.0f, 170.0f * toMps};
    const auto zeroPlan = MakePlan(input);
    Require(!zeroPlan.accepted && zeroPlan.reason == RejectReason::ZeroDemand,
            "the zero-strength endpoint must remain disabled after global attenuation");
}

void TestSpeedFadeUsesTotalMagnitudeDuringSideSlip() {
    // Keep the forward projection positive so this remains a valid forward
    // drift, while making the total speed materially larger than its forward
    // component. The attenuation must follow the total speed in this case.
    Inputs input = InputAtTotalSpeedKmh(125.0f);
    const auto halfPlan = MakePlan(input);
    Require(halfPlan.accepted,
            "a forward side-slip plan below the hard ceiling should be accepted");
    Require(std::fabs(halfPlan.longitudinalSpeedMps - 20.0f) < 1.0e-4f,
            "side-slip fixture must retain its positive forward projection");
    Require(std::fabs(halfPlan.speedMps - (125.0f / 3.6f)) < 1.0e-4f,
            "plan speed must report the total linear velocity magnitude");
    Require(halfPlan.speedScale > 0.49f && halfPlan.speedScale < 0.51f,
            "125 km/h total speed must receive the half-strength fade under side slip");

    input = InputAtTotalSpeedKmh(150.0f);
    const auto lowPlan = MakePlan(input);
    Require(lowPlan.accepted && lowPlan.speedScale > 0.14f &&
                lowPlan.speedScale < 0.16f,
            "150 km/h total speed must receive the 15 percent fade under side slip");

    input = InputAtTotalSpeedKmh(170.0f);
    const auto zeroPlan = MakePlan(input);
    Require(!zeroPlan.accepted && zeroPlan.reason == RejectReason::ZeroDemand &&
                std::fabs(zeroPlan.speedScale) < 1.0e-5f,
            "170 km/h total speed must disable acceleration even when forward projection is lower");

    // The hard safety ceiling also follows total speed, rather than allowing a
    // very large lateral velocity to pass solely because its forward component
    // is small.
    input = InputAtTotalSpeedKmh(400.0f);
    const auto overLimitPlan = MakePlan(input);
    Require(!overLimitPlan.accepted &&
                overLimitPlan.reason == RejectReason::SpeedLimit,
            "the plan speed ceiling must use total linear velocity magnitude");
}

void TestCountersteerAccelerationDirection() {
    using nfsmw_drift::DegToRad;
    using nfsmw_drift::Dot;

    const nfsmw_drift::Vec3 forward = {0.0f, 0.0f, 1.0f};
    const nfsmw_drift::Vec3 up = {0.0f, 1.0f, 0.0f};
    const float maximumSteer = DegToRad(60.0f);

    nfsmw_drift::Vec3 direction{};
    Require(ComputeCountersteerAccelerationDirection(
                forward, up, 0.0f, 1, maximumSteer, &direction),
            "neutral steering must produce a valid acceleration direction");
    Require(std::fabs(direction.x) < 1.0e-5f &&
                std::fabs(direction.z - 1.0f) < 1.0e-5f,
            "neutral steering must return the body heading exactly");

    // A right-hand drift uses negative steering for countersteer. Narrow the
    // old half-angle deviation by 75%, retaining only 25%: a 60-degree wheel
    // angle therefore produces a 7.5-degree acceleration deviation.
    Require(ComputeCountersteerAccelerationDirection(
                forward, up, -1.0f, 1, maximumSteer, &direction),
            "right-drift countersteer must produce a valid midpoint");
    Require(std::fabs(direction.x + std::sin(DegToRad(7.5f))) < 1.0e-5f &&
                std::fabs(direction.z - std::cos(DegToRad(7.5f))) <
                    1.0e-5f,
            "countersteer acceleration must use the retained quarter of the old midpoint deviation");
    Require(std::fabs(Dot(direction, forward) - std::cos(DegToRad(7.5f))) <
                1.0e-5f,
            "countersteer midpoint must retain 25 percent of the old deviation");

    // Same-direction steering is deliberate player input to tighten the
    // radius; it must never turn the rigid-body fallback sideways.
    Require(ComputeCountersteerAccelerationDirection(
                forward, up, 1.0f, 1, maximumSteer, &direction),
            "same-direction steering must remain a valid plan");
    Require(std::fabs(direction.x) < 1.0e-5f &&
                std::fabs(direction.z - 1.0f) < 1.0e-5f,
            "same-direction steering must retain the body heading");

    // Mirroring the drift side mirrors the lateral component.
    Require(ComputeCountersteerAccelerationDirection(
                forward, up, 1.0f, -1, maximumSteer, &direction),
            "left-drift countersteer must produce a valid midpoint");
    Require(std::fabs(direction.x - std::sin(DegToRad(7.5f))) < 1.0e-5f &&
                std::fabs(direction.z - std::cos(DegToRad(7.5f))) <
                    1.0e-5f,
            "left-drift countersteer must mirror the right-drift direction");
    Require(std::fabs(kCountersteerDirectionAngleScale - 0.25f) < 1.0e-5f,
            "countersteer direction deviation must retain exactly 25 percent");
}

void TestCountersteerDirectionPreservesAttitudeBasis() {
    using nfsmw_drift::DegToRad;
    const nfsmw_drift::Vec3 forward = {0.6f, 0.0f, 0.8f};
    const nfsmw_drift::Vec3 up = {0.0f, 1.0f, 0.0f};
    nfsmw_drift::Vec3 direction{};
    Require(ComputeCountersteerAccelerationDirectionFromAngle(
                forward, up, -DegToRad(30.0f), 1, &direction),
            "a pitched/yawed body basis must produce a valid midpoint");
    const nfsmw_drift::Vec3 expected = {
        0.6f * std::cos(DegToRad(3.75f)) -
            0.8f * std::sin(DegToRad(3.75f)),
        0.0f,
        0.8f * std::cos(DegToRad(3.75f)) +
            0.6f * std::sin(DegToRad(3.75f))};
    Require(std::fabs(direction.x - expected.x) < 1.0e-5f &&
                std::fabs(direction.z - expected.z) < 1.0e-5f,
            "the midpoint must be computed in the body right/forward basis");
    Require(std::fabs(nfsmw_drift::Length(direction) - 1.0f) < 1.0e-5f,
            "the midpoint direction must be normalized");
}

void TestCountersteerDirectionRejectsInvalidInputs() {
    using nfsmw_drift::DegToRad;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const nfsmw_drift::Vec3 forward = {0.0f, 0.0f, 1.0f};
    const nfsmw_drift::Vec3 up = {0.0f, 1.0f, 0.0f};
    nfsmw_drift::Vec3 direction{};
    Require(!ComputeCountersteerAccelerationDirection(
                forward, up, 1.1f, 1, DegToRad(60.0f), &direction),
            "a normalized steering command above one must fail closed");
    Require(!ComputeCountersteerAccelerationDirection(
                forward, up, nan, 1, DegToRad(60.0f), &direction),
            "a non-finite steering command must fail closed");
    Require(!ComputeCountersteerAccelerationDirection(
                forward, up, -1.0f, 0, DegToRad(60.0f), &direction),
            "an unknown drift side must fail closed");
    Require(!ComputeCountersteerAccelerationDirection(
                forward, up, -1.0f, 1, DegToRad(90.0f), &direction),
            "a ninety-degree steering limit must fail closed");
    Require(!ComputeCountersteerAccelerationDirectionFromAngle(
                forward, forward, -DegToRad(30.0f), 1, &direction),
            "an up axis parallel to forward must fail closed");
}

void TestPlanUsesHeadingForSafetyBeforeSteeredDirection() {
    using nfsmw_drift::DegToRad;
    Inputs input{};
    input.enabled = true;
    input.sessionActive = true;
    input.identityStable = true;
    input.physicsStable = true;
    input.grounded = true;
    input.throttle = 1.0f;
    input.dt = 1.0f / 60.0f;
    input.forward = {0.0f, 0.0f, 1.0f};
    input.bodyUp = {0.0f, 1.0f, 0.0f};
    input.appliedSteeringCommand = -1.0f;
    input.driftSide = 1;
    input.maximumSteerAngleRad = DegToRad(60.0f);
    input.useCountersteerDirection = true;
    input.linearVelocity = {0.0f, 0.0f, 10.0f};
    input.targetAccelerationMps2 = kConfiguredTargetAccelerationMps2;
    input.ramp = 1.0f;
    const auto plan = MakePlan(input);
    Require(plan.accepted && plan.reason == RejectReason::None,
            "a valid steered direction must remain plan-eligible");
    Require(std::fabs(plan.longitudinalSpeedMps - 10.0f) < 1.0e-5f,
            "reverse protection must project against the body heading");
    Require(std::fabs(plan.direction.x + std::sin(DegToRad(7.5f))) <
                1.0e-5f &&
                std::fabs(plan.direction.z - std::cos(DegToRad(7.5f))) <
                    1.0e-5f,
            "the accepted plan must carry the narrowed countersteer impulse direction");

    input.bodyUp = {};
    const auto invalidPlan = MakePlan(input);
    Require(!invalidPlan.accepted &&
                invalidPlan.reason == RejectReason::InvalidSteeringDirection,
            "invalid optional steering basis must reject the plan");
}

void TestExactAppliedDeltaIsVerified() {
    const auto verification = VerifyAppliedDelta(
        {3.0f, 0.0f, 20.0f}, {3.0f, 0.0f, 20.08f},
        {0.0f, 0.0f, 1.0f}, 0.08f);
    Require(verification.evaluated && verification.inputsValid &&
                verification.accepted,
            "an exact Accelerate read-back must pass verification");
    Require(std::fabs(verification.projectedDeltaVMps - 0.08f) < 1.0e-5f,
            "the projected read-back delta must be reported");
    Require(verification.orthogonalResidualMps < 1.0e-5f,
            "an exact axial increment must have negligible residual");
}

void TestFloatRoundoffAtHighSpeedIsAccepted() {
    const nfsmw_drift::Vec3 direction = {
        0.70710677f, 0.0f, 0.70710677f};
    const nfsmw_drift::Vec3 before = {77.0f, 0.0f, 77.0f};
    const float plannedDelta = 1.0f / 120.0f;
    const nfsmw_drift::Vec3 after = before + direction * plannedDelta;
    const auto verification =
        VerifyAppliedDelta(before, after, direction, plannedDelta);
    Require(verification.accepted,
            "normal float rounding on a large base velocity must be accepted");
}

void TestSmallReadbackNoiseIsAccepted() {
    const auto verification = VerifyAppliedDelta(
        {4.0f, 1.0f, 30.0f}, {4.0002f, 0.9999f, 30.081f},
        {0.0f, 0.0f, 1.0f}, 0.08f);
    Require(verification.accepted,
            "small projected and perpendicular read-back noise must be tolerated");
    Require(verification.orthogonalResidualMps > 0.0f,
            "perpendicular residual telemetry must preserve small noise");
}

void TestNoOpAndWrongMagnitudeAreRejected() {
    const nfsmw_drift::Vec3 before = {0.0f, 0.0f, 25.0f};
    const auto noOp = VerifyAppliedDelta(
        before, before, {0.0f, 0.0f, 1.0f}, 0.08f);
    Require(!noOp.accepted && !noOp.projectedMatch,
            "a no-op Accelerate call must never report write_ok");

    const auto tooWeak = VerifyAppliedDelta(
        before, {0.0f, 0.0f, 25.04f}, {0.0f, 0.0f, 1.0f}, 0.08f);
    Require(!tooWeak.accepted && !tooWeak.projectedMatch &&
                !tooWeak.magnitudeMatch,
            "a materially weak velocity increment must fail verification");

    const auto tooStrong = VerifyAppliedDelta(
        before, {0.0f, 0.0f, 25.12f}, {0.0f, 0.0f, 1.0f}, 0.08f);
    Require(!tooStrong.accepted && !tooStrong.projectedMatch &&
                !tooStrong.magnitudeMatch,
            "a materially strong velocity increment must fail verification");
}

void TestOrthogonalResidualIsRejected() {
    const auto verification = VerifyAppliedDelta(
        {0.0f, 0.0f, 20.0f}, {0.02f, 0.0f, 20.08f},
        {0.0f, 0.0f, 1.0f}, 0.08f);
    Require(!verification.accepted && verification.projectedMatch &&
                !verification.orthogonalMatch,
            "a sideways velocity injection must fail the residual check");
    Require(std::fabs(verification.orthogonalDelta.x - 0.02f) < 1.0e-5f,
            "orthogonal delta telemetry must identify the sideways change");
}

void TestInvalidVerificationInputsFailClosed() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Require(!VerifyAppliedDelta(
                 {0.0f, 0.0f, 20.0f}, {0.0f, 0.0f, nan},
                 {0.0f, 0.0f, 1.0f}, 0.08f)
                 .accepted,
            "a non-finite read-back must fail closed");
    Require(!VerifyAppliedDelta(
                 {0.0f, 0.0f, 20.0f}, {0.0f, 0.0f, 20.08f},
                 {0.0f, 0.0f, 0.5f}, 0.08f)
                 .accepted,
            "a non-unit planned direction must fail closed");
    Require(!VerifyAppliedDelta(
                 {0.0f, 0.0f, 20.0f}, {0.0f, 0.0f, 20.08f},
                 {0.0f, 0.0f, 1.0f}, nan)
                 .accepted,
            "a non-finite planned delta must fail closed");
}

void TestHandbrakeDecelerationGateAndPriority() {
    Require(!HandbrakeDecelerationEligible(1.0f, 0.5f, 0.199f),
            "handbrake deceleration must not start before 0.2 seconds");
    Require(HandbrakeDecelerationEligible(1.0f, 0.5f, 0.20f),
            "a continuously held handbrake must start deceleration at 0.2 seconds");
    Require(!HandbrakeDecelerationEligible(0.0f, 0.5f, 1.0f),
            "releasing the handbrake must immediately cancel deceleration");
    Require(SelectPlanMode(true, true) == PlanMode::Deceleration,
            "deceleration must take priority when acceleration is also eligible");
    Require(SelectPlanMode(false, true) == PlanMode::Acceleration &&
                SelectPlanMode(false, false) == PlanMode::None,
            "acceleration must remain available only when deceleration is absent");
}

void TestDecelerationStrengthCurve() {
    const float toMps = 1.0f / 3.6f;
    Require(std::fabs(DecelerationStrengthScale(80.0f * toMps) - 0.15f) <
                1.0e-5f,
            "speeds below 100 km/h must retain the weakest 15-percent deceleration");
    Require(std::fabs(DecelerationStrengthScale(100.0f * toMps) - 0.15f) <
                1.0e-5f,
            "100 km/h must be the weakest deceleration knot");
    Require(std::fabs(DecelerationStrengthScale(140.0f * toMps) - 0.575f) <
                1.0e-5f,
            "100-180 km/h deceleration must interpolate linearly");
    Require(std::fabs(DecelerationStrengthScale(180.0f * toMps) - 1.0f) <
                1.0e-5f &&
                std::fabs(DecelerationStrengthScale(220.0f * toMps) - 1.0f) <
                    1.0e-5f,
            "180 km/h and above must use maximum deceleration");
}

void TestDecelerationPlanUsesOppositeVelocity() {
    DecelerationInputs input{};
    input.enabled = true;
    input.sessionActive = true;
    input.identityStable = true;
    input.physicsStable = true;
    input.grounded = true;
    input.handbrake = 1.0f;
    input.handbrakeThreshold = 0.5f;
    input.handbrakeHeldSeconds = 0.20f;
    input.dt = 1.0f / 60.0f;
    input.linearVelocity = {30.0f, 0.0f, 40.0f};
    const auto plan = MakeDecelerationPlan(input);
    Require(plan.accepted && plan.mode == PlanMode::Deceleration,
            "a valid held-handbrake deceleration plan must be accepted");
    Require(std::fabs(plan.direction.x + 0.6f) < 1.0e-5f &&
                std::fabs(plan.direction.z + 0.8f) < 1.0e-5f,
            "deceleration must oppose actual velocity rather than body heading");
    Require(std::fabs(
                nfsmw_drift_asi::rigidbody_accel::
                    kConfiguredMaximumDecelerationMps2 -
                nfsmw_drift_asi::rigidbody_accel::
                    kConfiguredEffectiveTargetAccelerationMps2 * 1.15f) <
                1.0e-5f,
            "maximum deceleration must be 1.15 times maximum effective acceleration");

    input.handbrakeHeldSeconds = 0.19f;
    const auto tooShort = MakeDecelerationPlan(input);
    Require(!tooShort.accepted &&
                tooShort.reason == RejectReason::HandbrakeNotHeld,
            "a short handbrake tap must not create a deceleration write");
}

}  // namespace

int main() {
    TestDisabledByDefault();
    TestOnlySustainedHoldingDriftIsEligible();
    TestValidPlanUsesDeltaV();
    TestUnsafeInputsAreRejected();
    TestSpeedFadeAndRamp();
    TestSpeedAttenuationBoundariesAndInterpolation();
    TestSpeedAttenuationSuppressesPlanAtZero();
    TestStrengthAttenuationAppliesAcrossEverySpeedBand();
    TestSpeedFadeUsesTotalMagnitudeDuringSideSlip();
    TestCountersteerAccelerationDirection();
    TestCountersteerDirectionPreservesAttitudeBasis();
    TestCountersteerDirectionRejectsInvalidInputs();
    TestPlanUsesHeadingForSafetyBeforeSteeredDirection();
    TestExactAppliedDeltaIsVerified();
    TestFloatRoundoffAtHighSpeedIsAccepted();
    TestSmallReadbackNoiseIsAccepted();
    TestNoOpAndWrongMagnitudeAreRejected();
    TestOrthogonalResidualIsRejected();
    TestInvalidVerificationInputsFailClosed();
    TestHandbrakeDecelerationGateAndPriority();
    TestDecelerationStrengthCurve();
    TestDecelerationPlanUsesOppositeVelocity();
    if (failures != 0) {
        std::cerr << failures << " rigid-body acceleration assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All rigid-body acceleration tests passed\n";
    return EXIT_SUCCESS;
}
