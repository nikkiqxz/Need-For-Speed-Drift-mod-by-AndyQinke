#include "drift_assist.hpp"

// This file is a host-side integration example. It is intentionally excluded
// from the standalone CMake target because NFSPluginSDK is not vendored here.
// Define NFSMW_DRIFT_NFSPLUGINSDK_MSVC_X86 only in a validated 32-bit MSVC
// plugin project that already supplies the NFSPluginSDK headers.
#if defined(NFSMW_DRIFT_NFSPLUGINSDK_MSVC_X86)

#if !defined(_MSC_VER) || !defined(_M_IX86)
#error "The NFSPluginSDK adapter requires a 32-bit MSVC build and runtime ABI validation."
#endif

#include <NFSPluginSDK/Game.MW05/Types/IInput.h>
#include <NFSPluginSDK/Game.MW05/Types/IRigidBody.h>
#include <NFSPluginSDK/Game.MW05/Types/ISuspension.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace nfsmw_drift::mw05_example {

using NFSPluginSDK::MW05::IInput;
using NFSPluginSDK::MW05::IRigidBody;
using NFSPluginSDK::MW05::ISuspension;

enum class ObjectKind : std::uint8_t {
    RigidBody,
    Input,
    Suspension,
};

struct FrameContext {
    float dt = 0.0f;
    bool inRace = false;
    bool assistRequested = false;
    bool resetState = false;
    bool collisionRecent = false;

    // Host-issued identity for one physics frame.  The token must change for
    // every physics tick and must be non-zero.  A separate generation must be
    // incremented whenever the player vehicle or any of its physics
    // subobjects is replaced.  The generation must be monotonically
    // increasing for the lifetime of this adapter; a new generation may
    // restart frameToken at any positive value.  Both values are compared at
    // every deferred write-back point so an old command cannot reach a reused
    // pointer.
    std::uint64_t frameToken = 0;
    std::uint64_t vehicleGeneration = 0;

    // The host must set this only after resolving the player vehicle through
    // a version-checked path (for example PVehicleEx::GetPlayerInstance(),
    // IsPlayer(), and IsOwnedByPlayer()). The fail-closed default prevents an
    // accidentally supplied AI vehicle from receiving assistance when
    // AssistConfig::requirePlayer is enabled.
    bool playerControlled = false;
};

struct RuntimeGuards {
    // Set this only after checking the PE fingerprint and every hook anchor.
    bool executableAndAnchorsVerified = false;

    // Validate the object address, its vptr, and all methods used below. Method
    // targets must reside in the verified speed.exe .text range.
    bool (*validateObject)(const void* object, ObjectKind kind) = nullptr;

    // A contact-normal provider is required before attitude writes are allowed.
    // A suspension raycast or a collision contact is preferable to world-up.
    bool (*readSurfaceNormal)(Vec3& normalWorld) = nullptr;

    // The normal reader must be tied to the same vehicle and physics frame as
    // the input sample.  The callback should validate that its underlying
    // contact/raycast cache still belongs to these object identities and
    // tokens.  A missing validator disables attitude write-back (input-only
    // operation may continue).
    using SurfaceNormalValidator = bool (*)(const void* rigidBody,
                                            const void* suspension,
                                            std::uint64_t vehicleGeneration,
                                            std::uint64_t frameToken);
    SurfaceNormalValidator validateSurfaceNormal = nullptr;

    // These writers must apply an absolute multiplier relative to the vehicle's
    // original tire/drive parameters (1.0 restores the original value). Bind
    // them to the currently validated player's front lateral-force hook and
    // rear drive-torque hook. They are deliberately called at the physics
    // hook, not from the input-poll phase. The callback must validate its
    // current target on every call and scale the transient force/torque; do not
    // accumulate a modified value in persistent vehicle tuning. The adapter
    // can call each writer more than once for one frame token, so callbacks
    // must be idempotent when given the same absolute multiplier.
    using ModifierWriter = bool (*)(void* userData, float multiplier);
    using ModifierTargetValidator = bool (*)(void* userData,
                                             std::uint64_t vehicleGeneration,
                                             std::uint64_t frameToken);
    void* modifierUserData = nullptr;
    // The modifier target is normally a vehicle/physics context rather than
    // one of the public interface subobjects, so it has its own validator.
    // It must reject a destroyed object, a reused address, or a stale physics
    // frame.  No modifier write is permitted without this callback.
    ModifierTargetValidator validateModifierTarget = nullptr;
    ModifierWriter writeFrontGripMultiplier = nullptr;
    ModifierWriter writeRearDriveMultiplier = nullptr;
};

struct AdapterOptions {
    bool allowInputWrites = true;
    bool allowAttitudeWrites = false;

    // `sdkWheelIndex[canonicalSlot]` maps the controller's FL, FR, RL, RR
    // slots to the SDK suspension indices.  For the validated MW05
    // eTireIdx numbering (FL=0, FR=1, RR=2, RL=3), the rear pair is swapped:
    // canonical RL -> SDK 3 and canonical RR -> SDK 2.  Verify the enum
    // against the target binary and override this map for another
    // SDK/version instead of assuming the same numbering.
    std::array<std::uint32_t, 4> sdkWheelIndex{{0, 1, 3, 2}};
};

struct InputSnapshot {
    float steering = 0.0f;
    float throttle = 0.0f;
    float brake = 0.0f;
    float handbrake = 0.0f;
    bool valid = false;
};

namespace {

constexpr float kMinimumBasisLength = 0.50f;
constexpr float kMaximumBasisLength = 1.50f;
constexpr float kMaximumBasisDot = 0.20f;
constexpr float kMinimumBasisHandedness = 0.25f;

bool IsFinite(float value) {
    return std::isfinite(value) != 0;
}

bool IsFinite(Vec3 value) {
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

Vec3 ToController(const UMath::Vector3& value) {
    // UMath::Vector3 is not laid out as three conventional x/y/z floats.
    // Read named fields explicitly; never reinterpret_cast or memcpy it.
    return {value.x, value.y, value.z};
}

UMath::Vector3 ToGame(Vec3 value) {
    UMath::Vector3 result{};
    result.x = value.x;
    result.y = value.y;
    result.z = value.z;
    return result;
}

float InputOrZero(float value) {
    return IsFinite(value) ? value : 0.0f;
}

bool IsValidFrameIdentity(const FrameContext& frame) {
    return frame.frameToken != 0 && frame.vehicleGeneration != 0;
}

bool IsValidPhysicsFrame(const FrameContext& frame) {
    return IsValidFrameIdentity(frame) && frame.inRace && IsFinite(frame.dt) &&
           frame.dt > 0.0f && frame.dt <= 0.10f;
}

bool IsValidBasis(const Basis& basis) {
    if (!IsFinite(basis.right) || !IsFinite(basis.up) || !IsFinite(basis.forward)) {
        return false;
    }

    const float rightLength = Length(basis.right);
    const float upLength = Length(basis.up);
    const float forwardLength = Length(basis.forward);
    if (!IsFinite(rightLength) || !IsFinite(upLength) || !IsFinite(forwardLength) ||
        rightLength < kMinimumBasisLength || rightLength > kMaximumBasisLength ||
        upLength < kMinimumBasisLength || upLength > kMaximumBasisLength ||
        forwardLength < kMinimumBasisLength || forwardLength > kMaximumBasisLength) {
        return false;
    }

    const Vec3 right = basis.right / rightLength;
    const Vec3 up = basis.up / upLength;
    const Vec3 forward = basis.forward / forwardLength;
    if (std::fabs(Dot(right, up)) > kMaximumBasisDot ||
        std::fabs(Dot(right, forward)) > kMaximumBasisDot ||
        std::fabs(Dot(up, forward)) > kMaximumBasisDot) {
        return false;
    }

    // The controller and the game use right = up x forward.  Reject a
    // reflected basis instead of silently changing the sign of yaw/roll.
    const float handedness = Dot(Cross(up, forward), right);
    return IsFinite(handedness) && handedness >= kMinimumBasisHandedness;
}

bool ReadValidatedBasis(IRigidBody* rigidBody, Basis& basis) {
    if (rigidBody == nullptr) {
        return false;
    }

    UMath::Vector3 right{};
    UMath::Vector3 up{};
    UMath::Vector3 forward{};
    rigidBody->GetRightVector(right);
    rigidBody->GetUpVector(up);
    rigidBody->GetForwardVector(forward);

    const Basis raw{ToController(right), ToController(up), ToController(forward)};
    if (!IsValidBasis(raw)) {
        return false;
    }

    // Normalize the validated vectors for the local-to-world conversion.  The
    // strict checks above ensure this is only a conditioning step, not a way
    // to hide a corrupt or reflected game basis.
    const Vec3 forwardAxis = NormalizeOr(raw.forward, {0.0f, 0.0f, 1.0f});
    Vec3 upAxis = NormalizeOr(raw.up, {0.0f, 1.0f, 0.0f});
    Vec3 rightAxis = NormalizeOr(Cross(upAxis, forwardAxis),
                                 NormalizeOr(raw.right, {1.0f, 0.0f, 0.0f}));
    upAxis = NormalizeOr(Cross(forwardAxis, rightAxis), upAxis);
    rightAxis = NormalizeOr(Cross(upAxis, forwardAxis), rightAxis);
    basis = {rightAxis, upAxis, forwardAxis};
    return IsValidBasis(basis);
}

} // namespace

class DriftAssistAdapter {
public:
    explicit DriftAssistAdapter(
        AssistConfig controllerConfig = {},
        AdapterOptions adapterOptions = {})
        : controller_(controllerConfig), options_(adapterOptions) {}

    // Call this after the engine has refreshed input for the frame. With the
    // s-b-repo SDK, nfsmw::input::on_poll is the suitable input hook point.
    // This phase writes input only; attitude is applied by the second phase.
    // Capture valid interface subobject pointers in a version-verified hook;
    // do not assume GCC/MinGW layout for the game's MSVC MI classes.  The
    // adapter cannot infer player ownership from these three pointers: the
    // host must set FrameContext::playerControlled only after resolving the
    // same PVehicle through a validated GetPlayerInstance()/IsPlayer() /
    // IsOwnedByPlayer() path.  Leave it false when that proof is unavailable.
    bool tickAfterInputPoll(
        const FrameContext& frame,
        const RuntimeGuards& guards,
        IRigidBody* rigidBody,
        IInput* input,
        ISuspension* suspension) {
        const bool frameIdentityValid = IsValidFrameIdentity(frame);
        // Keep a high-water mark outside the active deferred-frame identity.
        // Once a frame has been invalidated and cleared, an old callback must
        // still be rejected instead of becoming a new first frame.
        if (frameIdentityValid) {
            if (highestVehicleGenerationSeen_ != 0 &&
                frame.vehicleGeneration < highestVehicleGenerationSeen_) {
                return false;
            }
            if (frame.vehicleGeneration > highestVehicleGenerationSeen_) {
                highestVehicleGenerationSeen_ = frame.vehicleGeneration;
                highestFrameTokenSeen_ = 0;
            }
            if (highestFrameTokenSeen_ != 0 &&
                frame.frameToken <= highestFrameTokenSeen_) {
                return false;
            }
            highestFrameTokenSeen_ = frame.frameToken;
        }

        // A vehicle generation (or one of its validated interface objects) is
        // part of the controller state identity.  Do this check before stale
        // deferred-frame cleanup: restoring a previous player's steering or
        // axle multiplier through a pointer that now belongs to another car
        // is worse than dropping the old frame.  Hosts are expected to bump
        // vehicleGeneration whenever the player vehicle, its physics objects,
        // or player/AI ownership changes; the pointer check remains a
        // fail-closed fallback for a missed generation bump.
        const bool hasPreviousIdentity = lastFrameToken_ != 0 &&
                                         lastVehicleGeneration_ != 0;
        if (hasPreviousIdentity && !frameIdentityValid) {
            // Do not let an invalid lifecycle sample clear a valid pending
            // frame. The next valid token owns cleanup and resampling.
            return false;
        }
        // Preserve the stale-callback guard before any identity reset.  A late
        // callback can carry a different (already-invalid) object pointer; it
        // must not clear the current identity and then get accepted as a new
        // first frame.
        if (hasPreviousIdentity && frame.vehicleGeneration == lastVehicleGeneration_ &&
            (!IsValidFrameIdentity(frame) || frame.frameToken <= lastFrameToken_)) {
            return false;
        }
        // A delayed callback from an older vehicle generation must never be
        // mistaken for a replacement vehicle.  Reject it before validating
        // any object or touching the current controller identity.  Higher
        // generations are the only valid lifecycle transition and may use a
        // freshly restarted frame token.
        if (hasPreviousIdentity && frame.vehicleGeneration < lastVehicleGeneration_) {
            return false;
        }
        const bool generationChanged =
            hasPreviousIdentity && frame.vehicleGeneration != lastVehicleGeneration_;
        const bool objectIdentityChanged =
            hasPreviousIdentity &&
            (rigidBody != lastRigidBody_ || input != lastInputObject_ ||
             suspension != lastSuspension_);
        const bool ownershipChanged = hasPreviousIdentity &&
                                      frame.playerControlled != lastPlayerControlled_;
        if (generationChanged || objectIdentityChanged || ownershipChanged) {
            controller_.reset();
            lastOutput_ = {};
            clearFrameIdentity();
        }

        const bool frameValid = validate(frame, guards, rigidBody, input, suspension);
        if (!frameValid) {
            // An invalid lifecycle/version/ownership sample is not authority
            // to touch game objects, including a seemingly harmless baseline
            // restore. Modifier writers are required to operate on transient
            // force/torque values, so dropping their pending identity cannot
            // leave persistent tuning behind.
            controller_.reset();
            lastOutput_ = {};
            clearFrameIdentity();
            return false;
        }

        // Capture the new game input before cleaning up a missed prior frame.
        // Restoring the previous command must not erase this frame's player
        // intent.
        const auto& controls = input->GetControls();
        const InputSnapshot sampledInput{InputOrZero(controls.mSteering),
                                         InputOrZero(controls.mGas),
                                         InputOrZero(controls.mBrake),
                                         InputOrZero(controls.mHandBrake),
                                         true};

        // A previous physics hook may have been skipped.  Never carry its
        // command into this frame; restore the baseline when the target is
        // still provably the same vehicle generation, then discard the stale
        // identity.  Cleanup belongs to a later token (or a replacement
        // vehicle generation).  A duplicate/older input callback must not
        // consume the pending state for the current frame.
        if (attitudeArmed_ || modifiersArmed_) {
            const bool resetRequired = cleanupDeferredFrame(frame, guards, input);
            if (resetRequired) {
                controller_.reset();
                lastOutput_ = {};
                clearFrameIdentity();
            }
        }

        VehicleState state{};
        state.dt = frame.dt;
        state.inRace = frame.inRace;
        state.playerControlled = frame.playerControlled;
        state.assistRequested = frame.assistRequested;
        state.resetState = frame.resetState;
        state.collisionRecent = frame.collisionRecent;

        Basis sampledBasis{};
        if (!ReadValidatedBasis(rigidBody, sampledBasis)) {
            cancelPendingFrame(frame, guards, input);
            controller_.reset();
            lastOutput_ = {};
            clearFrameIdentity();
            return false;
        }
        state.body = sampledBasis;
        state.linearVelocityWorld = ToController(rigidBody->GetLinearVelocity());
        state.angularVelocityWorld = ToController(rigidBody->GetAngularVelocity());

        // `sampledInput` was captured before stale-command cleanup.  Reuse all
        // four channels so a cleanup write cannot alter this frame's intent.
        state.steeringInput = sampledInput.steering;
        state.throttleInput = sampledInput.throttle;
        state.brakeInput = sampledInput.brake;
        state.handbrakeInput = sampledInput.handbrake;
        lastInput_ = sampledInput;

        const std::uint32_t wheelCount =
            std::min<std::uint32_t>(suspension->GetNumWheels(), 4U);
        for (std::size_t slot = 0; slot < state.wheelSlip.size(); ++slot) {
            const std::uint32_t sdkIndex = options_.sdkWheelIndex[slot];
            if (sdkIndex >= wheelCount) {
                continue;
            }
            state.wheelSlip[slot] = InputOrZero(suspension->GetWheelSlip(sdkIndex));
            state.wheelLoad[slot] = InputOrZero(suspension->GetWheelLoad(sdkIndex));
            if (suspension->IsWheelOnGround(sdkIndex)) {
                ++state.groundedWheels;
            }
        }

        const bool normalSourceVerified =
            guards.readSurfaceNormal != nullptr &&
            guards.validateSurfaceNormal != nullptr &&
            guards.validateSurfaceNormal(rigidBody,
                                         suspension,
                                         frame.vehicleGeneration,
                                         frame.frameToken);
        if (normalSourceVerified) {
            Vec3 normal{};
            if (guards.readSurfaceNormal(normal) && IsFinite(normal) && Length(normal) > 0.25f) {
                state.surfaceNormal = NormalizeOr(normal, {0.0f, 1.0f, 0.0f});
                state.hasSurfaceNormal = true;
            }
        }

        state.attitudeWriteAvailable = options_.allowAttitudeWrites &&
                                       state.hasSurfaceNormal &&
                                       controller_.config().maximumYawAngularDelta >
                                           1.0e-7f;

        lastOutput_ = controller_.update(state);
        applyInput(lastOutput_, state, input);
        lastRigidBody_ = rigidBody;
        lastInputObject_ = input;
        lastSuspension_ = suspension;
        lastFrameToken_ = frame.frameToken;
        lastVehicleGeneration_ = frame.vehicleGeneration;
        lastPlayerControlled_ = frame.playerControlled;
        lastModifierUserData_ = guards.modifierUserData;
        lastModifierTargetValidator_ = guards.validateModifierTarget;
        lastFrontGripWriter_ = guards.writeFrontGripMultiplier;
        lastRearDriveWriter_ = guards.writeRearDriveMultiplier;
        modifierFrameToken_ = frame.frameToken;
        modifierVehicleGeneration_ = frame.vehicleGeneration;
        modifierWriteObserved_ = false;
        attitudeArmed_ = options_.allowAttitudeWrites && state.hasSurfaceNormal &&
                         Length(lastOutput_.angularVelocityDeltaLocal) > 1.0e-7f;
        pendingFrontGripScale_ = lastOutput_.active ? lastOutput_.frontGripScale : 1.0f;
        pendingRearDriveScale_ = lastOutput_.active ? lastOutput_.rearDriveScale : 1.0f;
        // A partial writer set is unsafe: writing only one axle would leave
        // the other axle with an unknown stale multiplier.  Keep the whole
        // modifier channel disarmed until both writers and the target
        // validator are present and the target has been checked for this
        // exact frame.
        modifiersArmed_ = guards.writeFrontGripMultiplier != nullptr &&
                          guards.writeRearDriveMultiplier != nullptr &&
                          guards.validateModifierTarget != nullptr &&
                          guards.modifierUserData != nullptr &&
                          guards.validateModifierTarget(guards.modifierUserData,
                                                        frame.vehicleGeneration,
                                                        frame.frameToken);
        if (!modifiersArmed_) {
            clearModifierIdentity();
        }
        return true;
    }

    // Call once at ActiveComponents_TickAll exit or immediately before the
    // main-thread physics integrator. It consumes the output calculated during
    // the input phase. Do not call this from a render hook.
    bool applyAttitudeBeforeIntegrator(
        const FrameContext& frame,
        const RuntimeGuards& guards,
        IRigidBody* rigidBody) {
        // A deferred callback can arrive after a newer input tick has already
        // armed a different frame.  Check identity before consuming the
        // pending flag; an old/future callback must be a no-op and leave the
        // newer frame available for its own physics hook.
        if (!attitudeArmed_ || !IsValidFrameIdentity(frame) ||
            frame.frameToken != lastFrameToken_ ||
            frame.vehicleGeneration != lastVehicleGeneration_) {
            return false;
        }
        attitudeArmed_ = false;

        if (rigidBody == nullptr || !guards.executableAndAnchorsVerified ||
            guards.validateObject == nullptr ||
            !IsValidFrameIdentity(frame) ||
            frame.frameToken != lastFrameToken_ ||
            frame.vehicleGeneration != lastVehicleGeneration_ ||
            !frame.inRace || !IsFinite(frame.dt) || frame.dt <= 0.0f || frame.dt > 0.10f ||
            (lastRigidBody_ != nullptr && rigidBody != lastRigidBody_) ||
            !guards.validateObject(rigidBody, ObjectKind::RigidBody) ||
            lastInputObject_ == nullptr || lastSuspension_ == nullptr ||
            !guards.validateObject(lastInputObject_, ObjectKind::Input) ||
            !guards.validateObject(lastSuspension_, ObjectKind::Suspension)) {
            cancelPendingFrame(frame, guards, lastInputObject_);
            controller_.reset();
            lastOutput_ = {};
            clearFrameIdentity();
            return false;
        }

        Vec3 surfaceNormal{};
        if (guards.readSurfaceNormal == nullptr ||
            guards.validateSurfaceNormal == nullptr ||
            !guards.validateSurfaceNormal(rigidBody,
                                          lastSuspension_,
                                          frame.vehicleGeneration,
                                          frame.frameToken) ||
            !guards.readSurfaceNormal(surfaceNormal) ||
            !IsFinite(surfaceNormal) || Length(surfaceNormal) <= 0.25f) {
            cancelPendingFrame(frame, guards, lastInputObject_);
            controller_.reset();
            lastOutput_ = {};
            clearFrameIdentity();
            return false;
        }

        Basis latestBasis{};
        if (!ReadValidatedBasis(rigidBody, latestBasis)) {
            cancelPendingFrame(frame, guards, lastInputObject_);
            controller_.reset();
            lastOutput_ = {};
            clearFrameIdentity();
            return false;
        }

        const bool wrote = applyAttitude(lastOutput_, latestBasis, rigidBody);
        if (!wrote) {
            cancelPendingFrame(frame, guards, lastInputObject_);
            controller_.reset();
            lastOutput_ = {};
            clearFrameIdentity();
        }
        return wrote;
    }

    // Call from a validated aggregate tire-force/drive-torque hook for this
    // vehicle. The hook may run zero, one, or multiple times in a physics
    // frame (substeps and per-axle/per-wheel paths are common). Each same-token
    // call is accepted and invokes both writers as a pair. Writers therefore
    // MUST apply an absolute multiplier against the vehicle's original tuning,
    // never multiply an already-scaled value. A valid inactive frame supplies
    // 1.0 to restore the baseline. If the hook is skipped, the next token's
    // input tick attempts a paired baseline restore before computing output.
    bool applyPhysicsModifiers(const FrameContext& frame,
                               const RuntimeGuards& guards) {
        if (!modifiersArmed_) {
            return false;
        }

        // A late hook from an older frame must not consume the pending command
        // for the current frame.  Leave it armed so the next tick can perform
        // the normal stale-command cleanup.
        if (!IsValidPhysicsFrame(frame) ||
            frame.frameToken != modifierFrameToken_ ||
            frame.vehicleGeneration != modifierVehicleGeneration_) {
            return false;
        }

        if (!hasCurrentModifierTarget(frame, guards) ||
            guards.writeFrontGripMultiplier == nullptr ||
            guards.writeRearDriveMultiplier == nullptr ||
            guards.validateModifierTarget != lastModifierTargetValidator_ ||
            guards.modifierUserData != lastModifierUserData_ ||
            frame.vehicleGeneration != modifierVehicleGeneration_ ||
            guards.writeFrontGripMultiplier != lastFrontGripWriter_ ||
            guards.writeRearDriveMultiplier != lastRearDriveWriter_) {
            pendingFrontGripScale_ = 1.0f;
            pendingRearDriveScale_ = 1.0f;
            restoreInputIfSafe(frame, guards, lastInputObject_);
            controller_.reset();
            lastOutput_ = {};
            clearFrameIdentity();
            return false;
        }

        // Both axle writers are required.  Calling either one in isolation can
        // leave a stale multiplier on the other axle, so this channel fails
        // closed rather than issuing a partial write.
        const bool frontWrote = guards.writeFrontGripMultiplier(
            guards.modifierUserData,
            Clamp(pendingFrontGripScale_, 0.0f, 3.0f));
        const bool rearWrote = guards.writeRearDriveMultiplier(
            guards.modifierUserData,
            Clamp(pendingRearDriveScale_, 0.0f, 3.0f));
        if (!frontWrote || !rearWrote) {
            // A callback can report failure after touching only one axle. Try
            // one paired baseline write, then disable all channels regardless
            // of that result; continuing with a partially modified vehicle is
            // less safe than dropping the frame.
            restorePhysicsModifiersIfSafe(frame, guards);
            restoreInputIfSafe(frame, guards, lastInputObject_);
            controller_.reset();
            lastOutput_ = {};
            clearFrameIdentity();
            return false;
        }
        modifierWriteObserved_ = true;
        // Keep the frame armed until its token advances.  The physics hook may
        // legitimately run more than once (for example, one aggregate call
        // per axle or substep); absolute writers make repeated same-token
        // application idempotent.  The next token triggers baseline cleanup.
        return true;
    }

    // The host may call this at a physics-tick abort/exception path.  It is
    // also called implicitly at the start of the next input tick when either
    // deferred hook was not observed.
    bool cancelPendingWrites(const FrameContext& frame,
                             const RuntimeGuards& guards) {
        if (!attitudeArmed_ && !modifiersArmed_) {
            return false;
        }
        // Cancellation is tied to the exact pending identity.  In particular,
        // a late abort from an older token must not restore inputs/modifiers or
        // clear state belonging to a newer frame.  The next input tick already
        // owns stale-write cleanup through cleanupDeferredFrame().
        if (!IsValidFrameIdentity(frame) ||
            frame.frameToken != lastFrameToken_ ||
            frame.vehicleGeneration != lastVehicleGeneration_) {
            return false;
        }
        cancelPendingFrame(frame, guards, lastInputObject_);
        controller_.reset();
        lastOutput_ = {};
        clearFrameIdentity();
        return true;
    }

    // Clear controller/deferred state at a process or game-session boundary.
    // This also allows a new session to restart its generation/token counters.
    void reset() {
        controller_.reset();
        lastOutput_ = {};
        clearFrameIdentity();
        highestVehicleGenerationSeen_ = 0;
        highestFrameTokenSeen_ = 0;
    }

    const ControlOutput& lastOutput() const { return lastOutput_; }
    DriftAssistController& controller() { return controller_; }

private:
    DriftAssistController controller_{};
    AdapterOptions options_{};
    ControlOutput lastOutput_{};
    bool attitudeArmed_ = false;
    bool modifiersArmed_ = false;
    float pendingFrontGripScale_ = 1.0f;
    float pendingRearDriveScale_ = 1.0f;
    InputSnapshot lastInput_{};
    IRigidBody* lastRigidBody_ = nullptr;
    IInput* lastInputObject_ = nullptr;
    ISuspension* lastSuspension_ = nullptr;
    std::uint64_t lastFrameToken_ = 0;
    std::uint64_t lastVehicleGeneration_ = 0;
    std::uint64_t highestFrameTokenSeen_ = 0;
    std::uint64_t highestVehicleGenerationSeen_ = 0;
    bool lastPlayerControlled_ = false;
    void* lastModifierUserData_ = nullptr;
    RuntimeGuards::ModifierTargetValidator lastModifierTargetValidator_ = nullptr;
    RuntimeGuards::ModifierWriter lastFrontGripWriter_ = nullptr;
    RuntimeGuards::ModifierWriter lastRearDriveWriter_ = nullptr;
    std::uint64_t modifierFrameToken_ = 0;
    std::uint64_t modifierVehicleGeneration_ = 0;
    bool modifierWriteObserved_ = false;

    bool validate(
        const FrameContext& frame,
        const RuntimeGuards& guards,
        const IRigidBody* rigidBody,
        const IInput* input,
        const ISuspension* suspension) const {
        if (rigidBody == nullptr || input == nullptr || suspension == nullptr ||
            !guards.executableAndAnchorsVerified || guards.validateObject == nullptr ||
            !IsValidFrameIdentity(frame)) {
            return false;
        }
        if (!frame.inRace || !IsFinite(frame.dt) || frame.dt <= 0.0f || frame.dt > 0.10f) {
            return false;
        }
        if (controller_.config().requirePlayer && !frame.playerControlled) {
            return false;
        }
        return guards.validateObject(rigidBody, ObjectKind::RigidBody) &&
               guards.validateObject(input, ObjectKind::Input) &&
               guards.validateObject(suspension, ObjectKind::Suspension);
    }

    void applyInput(const ControlOutput& output, const VehicleState& state, IInput* input) const {
        if (!options_.allowInputWrites) {
            return;
        }
        const float steering = output.steeringCommand;
        input->SetControlSteering(Clamp(steering, -1.0f, 1.0f));
        input->SetControlGas(Clamp(state.throttleInput * output.throttleScale, 0.0f, 1.0f));
        input->SetControlBrake(Clamp(state.brakeInput + output.brakeAdd, 0.0f, 1.0f));
    }

    void restoreInputIfSafe(const FrameContext& frame,
                            const RuntimeGuards& guards,
                            IInput* input) const {
        if (!options_.allowInputWrites || input == nullptr || !lastInput_.valid ||
            input != lastInputObject_ ||
            !IsValidFrameIdentity(frame) ||
            frame.frameToken != lastFrameToken_ ||
            frame.vehicleGeneration != lastVehicleGeneration_ ||
            !guards.executableAndAnchorsVerified || guards.validateObject == nullptr ||
            !guards.validateObject(input, ObjectKind::Input)) {
            return;
        }
        input->SetControlSteering(Clamp(lastInput_.steering, -1.0f, 1.0f));
        input->SetControlGas(Clamp(lastInput_.throttle, 0.0f, 1.0f));
        input->SetControlBrake(Clamp(lastInput_.brake, 0.0f, 1.0f));
    }

    // Used when the next physics frame arrives after a deferred hook was
    // skipped.  A new frame token is expected here; only the vehicle
    // generation and validated input object must still match.  The caller
    // captures the new controls before invoking this helper, so restoring the
    // previous command cannot consume current player input.
    void restoreStaleInputIfSafe(const FrameContext& frame,
                                 const RuntimeGuards& guards,
                                 IInput* input) const {
        if (!options_.allowInputWrites || input == nullptr || !lastInput_.valid ||
            input != lastInputObject_ || !IsValidFrameIdentity(frame) ||
            frame.vehicleGeneration != lastVehicleGeneration_ ||
            frame.frameToken <= lastFrameToken_ ||
            !guards.executableAndAnchorsVerified || guards.validateObject == nullptr ||
            !guards.validateObject(input, ObjectKind::Input)) {
            return;
        }
        input->SetControlSteering(Clamp(lastInput_.steering, -1.0f, 1.0f));
        input->SetControlGas(Clamp(lastInput_.throttle, 0.0f, 1.0f));
        input->SetControlBrake(Clamp(lastInput_.brake, 0.0f, 1.0f));
    }

    bool hasCurrentModifierTarget(const FrameContext& frame,
                                  const RuntimeGuards& guards) const {
        if (!guards.executableAndAnchorsVerified || !IsValidFrameIdentity(frame) ||
            guards.modifierUserData == nullptr ||
            guards.validateModifierTarget == nullptr ||
            !guards.validateModifierTarget(guards.modifierUserData,
                                           frame.vehicleGeneration,
                                           frame.frameToken)) {
            return false;
        }
        return true;
    }

    bool restorePhysicsModifiersIfSafe(const FrameContext& frame,
                                       const RuntimeGuards& guards) const {
        if (!hasCurrentModifierTarget(frame, guards) ||
            guards.writeFrontGripMultiplier == nullptr ||
            guards.writeRearDriveMultiplier == nullptr ||
            lastModifierUserData_ == nullptr ||
            guards.modifierUserData != lastModifierUserData_ ||
            guards.validateModifierTarget != lastModifierTargetValidator_ ||
            guards.writeFrontGripMultiplier != lastFrontGripWriter_ ||
            guards.writeRearDriveMultiplier != lastRearDriveWriter_ ||
            frame.vehicleGeneration != modifierVehicleGeneration_ ||
            frame.frameToken < modifierFrameToken_) {
            return false;
        }
        // Restoration is also a paired operation.  Never call one writer if
        // its mate is unavailable or if the callback identity changed.
        const bool frontWrote = guards.writeFrontGripMultiplier(
            guards.modifierUserData, 1.0f);
        const bool rearWrote = guards.writeRearDriveMultiplier(
            guards.modifierUserData, 1.0f);
        return frontWrote && rearWrote;
    }

    void cancelPendingFrame(const FrameContext& frame,
                            const RuntimeGuards& guards,
                            IInput* input) const {
        restoreInputIfSafe(frame, guards, input);
        restorePhysicsModifiersIfSafe(frame, guards);
    }

    // Returns true when the controller must be reset because a deferred
    // channel was missed or could not be restored.  A successfully applied
    // modifier frame is finalized without resetting the controller state;
    // this preserves drift hysteresis across normal frame boundaries while
    // still restoring persistent tuning to its baseline.
    bool cleanupDeferredFrame(const FrameContext& frame,
                              const RuntimeGuards& guards,
                              IInput* input) {
        // This helper is only valid for a later token (or a new vehicle
        // generation).  Keep it defensive in case a future call site bypasses
        // tickAfterInputPoll's ordering check.
        if (!IsValidFrameIdentity(frame) ||
            (frame.vehicleGeneration == lastVehicleGeneration_ &&
             frame.frameToken <= lastFrameToken_)) {
            return false;
        }
        const bool attitudeWasPending = attitudeArmed_;
        bool resetRequired = attitudeWasPending;

        restoreStaleInputIfSafe(frame, guards, input);

        if (modifiersArmed_) {
            if (modifierWriteObserved_) {
                if (!restorePhysicsModifiersIfSafe(frame, guards)) {
                    resetRequired = true;
                }
            } else {
                // No callback ran for the previous frame. Nothing should have
                // been scaled, but the deferred contract was violated; stop
                // assistance before accepting a new sample.
                resetRequired = true;
            }
        }

        attitudeArmed_ = false;
        clearModifierIdentity();
        return resetRequired;
    }

    void clearModifierIdentity() {
        modifiersArmed_ = false;
        modifierWriteObserved_ = false;
        pendingFrontGripScale_ = 1.0f;
        pendingRearDriveScale_ = 1.0f;
        lastModifierUserData_ = nullptr;
        lastModifierTargetValidator_ = nullptr;
        lastFrontGripWriter_ = nullptr;
        lastRearDriveWriter_ = nullptr;
        modifierFrameToken_ = 0;
        modifierVehicleGeneration_ = 0;
    }

    void clearFrameIdentity() {
        attitudeArmed_ = false;
        clearModifierIdentity();
        lastInput_.valid = false;
        lastRigidBody_ = nullptr;
        lastInputObject_ = nullptr;
        lastSuspension_ = nullptr;
        lastFrameToken_ = 0;
        lastVehicleGeneration_ = 0;
        lastPlayerControlled_ = false;
    }

    bool applyAttitude(
        const ControlOutput& output,
        const Basis& body,
        IRigidBody* rigidBody) const {
        const Vec3 deltaWorld =
            body.right * output.angularVelocityDeltaLocal.x +
            body.up * output.angularVelocityDeltaLocal.y +
            body.forward * output.angularVelocityDeltaLocal.z;
        if (!IsFinite(deltaWorld)) {
            return false;
        }

        const Vec3 angularVelocity = ToController(rigidBody->GetAngularVelocity());
        if (!IsFinite(angularVelocity)) {
            return false;
        }
        const Vec3 nextAngularVelocity = angularVelocity + deltaWorld;
        if (!IsFinite(nextAngularVelocity)) {
            return false;
        }
        rigidBody->SetAngularVelocity(ToGame(nextAngularVelocity));
        return true;
    }
};

} // namespace nfsmw_drift::mw05_example

#else

namespace nfsmw_drift::mw05_example {

// Keeps the translation unit valid in standalone builds while documenting that
// the real adapter is opt-in and tied to a validated 32-bit game integration.
constexpr bool kNfsPluginSdkAdapterEnabled = false;

} // namespace nfsmw_drift::mw05_example

#endif
