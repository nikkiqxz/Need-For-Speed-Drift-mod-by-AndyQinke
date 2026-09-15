#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef NFSMW_ENABLE_ALPHA_BRIDGE
#define NFSMW_ENABLE_ALPHA_BRIDGE 0
#endif

#ifndef NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT
#define NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT 0
#endif

#ifndef NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE
#define NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE 0
#endif

#ifndef NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
#define NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE 0
#endif
#ifndef NFSMW_ENABLE_REAR_WHEEL_STEERING
#define NFSMW_ENABLE_REAR_WHEEL_STEERING 0
#endif

#include "alpha_bridge.hpp"

#include "driver_assist.hpp"
#include "driver_assist_hud.hpp"
#include "drift_drive_torque.hpp"
#include "drift_camera.hpp"
#include "drift_assist.hpp"
#include "handling_probe.hpp"
#include "handling_probe_state.hpp"
#include "nfsmw_drift_target_profile.hpp"
#include "player_vehicle_selection.hpp"
#include "rear_wheel_steering.hpp"
#include "rigidbody_accel_experiment.hpp"
#include "runtime_logging.hpp"
#include "steering_response.hpp"
#include "vehicle_countersteer.hpp"

#include <nfsmw_sdk/nfsmw_sdk.h>
#if NFSMW_ENABLE_ALPHA_BRIDGE
#include <MinHook.h>
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#endif

namespace nfsmw_drift_asi::alpha_bridge {
namespace {

#if NFSMW_ENABLE_ALPHA_BRIDGE

using InputPollFn = void (NFSMW_FASTCALL*)(void* self, void* unusedEdx);
using ActiveComponentsFn = void (NFSMW_CDECL*)(float dt);
using CameraLookAtFn = int (NFSMW_CDECL*)(
    void* outMatrix, nfsmw_drift::Vec3* from, nfsmw_drift::Vec3* to,
    nfsmw_drift::Vec3* up);
using SetAngularVelocityFn = void (NFSMW_THISCALL*)(
    void* self, const nfsmw_drift::Vec3* angularVelocity);
using SetLinearVelocityFn = void (NFSMW_THISCALL*)(
    void* self, const nfsmw_drift::Vec3* linearVelocity);
using AccelerateFn = void (NFSMW_THISCALL*)(
    void* self, const nfsmw_drift::Vec3* distribution, float amount);
using GetForwardVectorFn = void (NFSMW_THISCALL*)(
    void* self, nfsmw_drift::Vec3* out);
using SimableGetPlayerFn = void* (NFSMW_THISCALL*)(void* self);
using SimablePredicateFn = bool (NFSMW_THISCALL*)(void* self);
using PlayerGetSimableFn = void* (NFSMW_THISCALL*)(void* self);
using SuspensionGetCountFn = std::uint32_t (NFSMW_THISCALL*)(void* self);
using SuspensionGetWheelFloatFn = float (NFSMW_THISCALL*)(
    void* self, std::uint32_t index);
using SuspensionSetWheelFloatFn = void (NFSMW_THISCALL*)(
    void* self, std::int32_t index, float value);
using SuspensionGetWheelSteerFn = float (NFSMW_THISCALL*)(
    void* self, std::uint32_t index);
using TransmissionGetDriveTorqueFn = float (NFSMW_THISCALL*)(void* self);

constexpr std::uintptr_t kInputPollRva = 0x002349B0u;
constexpr std::uintptr_t kActiveComponentsRva = 0x000BA940u;
constexpr std::uintptr_t kPVehicleInstancesRva = 0x005352B0u;
constexpr std::uintptr_t kRawInputVtableRva = 0x004A7B9Cu;
constexpr std::uintptr_t kPVehiclePrimaryVtableRva = 0x004AA9D8u;
constexpr std::uintptr_t kPVehicleSimableVtableRva = 0x004AA940u;
constexpr std::uintptr_t kRigidBodyVtableRva = 0x004AC880u;
constexpr std::uintptr_t kSetAngularVelocityRva = 0x00296FF0u;
constexpr std::uintptr_t kRigidBodyAccelerateRva = 0x00299E10u;
constexpr std::uintptr_t kSimableGetPlayerRva = 0x00286A20u;
constexpr std::uintptr_t kSimableIsPlayerRva = 0x00286A10u;
constexpr std::uintptr_t kSimableIsOwnedByPlayerRva = 0x00276FD0u;
constexpr std::uintptr_t kPlayerVtableRva = 0x004B0B10u;
constexpr std::uintptr_t kPlayerGetSimableRva = 0x002F8FB0u;
constexpr std::uintptr_t kCameraLookAtCallRva = 0x0007DCBCu;
constexpr std::uintptr_t kCreateLookAtMatrixRva = 0x002CF0A0u;

constexpr std::uint8_t kSimableGetPlayerSignature[] = {
    0x8B, 0x41, 0x58, 0xC3};
constexpr std::uint8_t kSimableIsPlayerSignature[] = {
    0x8B, 0x51, 0x58, 0x33, 0xC0, 0x85, 0xD2, 0x0F,
    0x95, 0xC0, 0xC3};
constexpr std::uint8_t kSimableIsOwnedByPlayerSignature[] = {
    0x8B, 0x41, 0x34, 0x85, 0xC0, 0x56, 0x57,
    0x0F, 0x84, 0x81, 0x00, 0x00, 0x00};
constexpr std::uint8_t kPlayerGetSimableSignature[] = {
    0x83, 0xE9, 0x20, 0xE9, 0x78, 0xFF, 0xFF, 0xFF};
constexpr std::uint8_t kCreateLookAtMatrixSignature[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC,
    0x8C, 0x00, 0x00, 0x00, 0x8B, 0x45, 0x10, 0xD9};

constexpr std::size_t kInputPollVtableSlot = 4;
constexpr std::size_t kSetAngularVelocityVtableSlot = 25;
constexpr std::size_t kSetLinearVelocityVtableSlot = 24;
constexpr std::size_t kRigidBodyForwardVtableSlot = 13;
constexpr std::size_t kRigidBodyAccelerateVtableSlot = 39;
constexpr std::size_t kPVehicleInstanceLimit = 64;
constexpr std::size_t kPVehicleInstanceStride = 8;
constexpr std::size_t kPVehicleInstanceEnabledOffset = 4;
constexpr std::size_t kPVehicleReadableSize = 0x160;
constexpr std::size_t kPVehicleSimableOffset = 0x2C;
constexpr std::size_t kPVehicleDirtyOffset = 0x48;
constexpr std::size_t kPVehicleObjectTypeOffset = 0x60;
constexpr std::size_t kRigidBodyHolderOffset = 0x30;
constexpr std::size_t kLinearVelocityOffset = 0x20;
constexpr std::size_t kAngularVelocityOffset = 0x30;
constexpr std::size_t kRigidBodyModeOffset = 0x5D;
constexpr std::size_t kRightVectorOffset = 0x70;
constexpr std::size_t kUpVectorOffset = 0x80;
constexpr std::size_t kForwardVectorOffset = 0x90;
constexpr std::size_t kPVehicleRigidBodyOffset = 0x78;
constexpr std::size_t kPVehiclePlayerOffset = 0x84;
constexpr std::size_t kPVehicleSuspensionOffset = 0xF0;
constexpr std::size_t kPVehicleRenderableOffset = 0x108;
constexpr std::size_t kPVehicleSpeedOffset = 0x11C;
constexpr std::size_t kPVehicleGroundedOffset = 0x130;
constexpr std::uintptr_t kHudVehicleGlobalRva = 0x0052FD98u;
// CustomHud's MW GetCarName source follows this gameplay database chain:
// [base+0x52C510] -> +0x10 -> +0x1C -> actual selected vehicle name.
constexpr std::uintptr_t kCurrentCarDatabaseRva = 0x0052C510u;
constexpr std::size_t kCurrentCarRecordOffset = 0x10;
constexpr std::size_t kCurrentCarNameOffset = 0x1C;
constexpr std::size_t kHudVehicleDrivetrainOffset = 0x2C4;
constexpr std::size_t kDrivetrainRawGearOffset = 0x60;
// PVehicle embeds Attrib::Instance at +0xD0. Its Collection pointer is the
// second field, and Attrib::Collection::mKey is at +0x20.
constexpr std::size_t kPVehicleAttributeCollectionOffset = 0xD4;
constexpr std::size_t kPVehicleTransmissionOffset = 0xFC;
constexpr std::size_t kAttributeCollectionParentOffset = 0x10;
constexpr std::size_t kAttributeCollectionKeyOffset = 0x20;
constexpr std::uint32_t kMaximumAttributeParentDepth = 16;
// Verified in the target's 0x006B1BB0 constructor: the common vehicle
// component embeds its suspension at +0x4C, transmission interface at +0x90,
// and transmission Attrib::Instance collection pointer at +0xD8.
constexpr std::size_t kComponentSuspensionOffset = 0x4C;
constexpr std::size_t kComponentTransmissionInterfaceOffset = 0x90;
constexpr std::size_t kComponentTransmissionCollectionOffset = 0xD8;
constexpr std::uintptr_t kTransmissionInterfaceVtableRva = 0x004ABAC4u;
// The live PVehicle transmission uses the alternate 0x006B37B0 component.
// Its ITransmission subobject is +0x4C and its transmission Instance is +0x158.
constexpr std::uintptr_t kLiveTransmissionVtableRva = 0x004AB720u;
constexpr std::size_t kLiveTransmissionInterfaceOffset = 0x4C;
constexpr std::size_t kLiveTransmissionCollectionOffset = 0x15C;
constexpr std::size_t kTransmissionDriveTorqueVtableSlot = 8;
constexpr std::uintptr_t kTransmissionDriveTorqueRva = 0x002A0580u;
constexpr std::size_t kTransmissionCurrentGearOffset = 0x38;
constexpr std::size_t kSimableGetPlayerVtableSlot = 8;
constexpr std::size_t kSimableIsPlayerVtableSlot = 9;
constexpr std::size_t kSimableIsOwnedByPlayerVtableSlot = 10;
constexpr std::size_t kPlayerGetSimableVtableSlot = 1;
constexpr std::size_t kSuspensionGetNumWheelsVtableSlot = 2;
constexpr std::size_t kSuspensionGetWheelSlipVtableSlot = 12;
constexpr std::size_t kSuspensionGetWheelSlipAngleVtableSlot = 15;
constexpr std::size_t kSuspensionGetWheelAngularVelocityVtableSlot = 20;
constexpr std::size_t kSuspensionSetWheelAngularVelocityVtableSlot = 21;
constexpr std::size_t kSuspensionGetWheelSteerVtableSlot = 22;

constexpr std::uint32_t kProbationSamples = 60;
constexpr DWORD kPhysicsFreshnessMs = 250;
constexpr DWORD kFailureLogIntervalMs = 1000;
constexpr DWORD kTelemetryLogIntervalMs = 500;
constexpr DWORD kRigidBodyTelemetryLogIntervalMs = 500;
constexpr DWORD kRearWheelSteerDiagnosticIntervalMs = 1000;
constexpr DWORD kPendingYawMaximumAgeMs = 150;
constexpr float kMaximumCadenceDt = 0.25f;
constexpr float kMaximumControlDt = 0.05f;
constexpr float kFullSteeringResponseCalibrationPerSecond = 4.0f;
constexpr float kMaximumVehicleSpeedMps = 110.0f;
constexpr float kMaximumVectorMagnitude = 1000.0f;
constexpr float kMaximumAngularMagnitude = 20.0f;
constexpr float kMaximumManualYawDeltaRadS = 0.15f;
constexpr float kYawReadBackToleranceRadS = 0.001f;
constexpr float kMinimumOffsetChangeRad = 1.0e-6f;
constexpr float kFourWheelSlipRatioThreshold = 0.15f;
constexpr float kFourWheelSlipAngleThresholdRad =
    5.0f * nfsmw_drift::kPi / 180.0f;
constexpr float kMaximumWheelSlipMagnitude = 100.0f;
constexpr float kMaximumWheelAngularVelocity = 5000.0f;
constexpr float kLaunchControlVelocityToleranceMps = 0.01f;
constexpr float kLaunchControlWheelToleranceRadS = 0.05f;
// The rigid-body fallback is deliberately a modest forward velocity assist,
// not a replacement for the game's tire/drive model.  Keep the strength in
// the portable planner so the bridge and its tests use one release value.
constexpr float kForwardAccelTargetMps2 =
    rigidbody_accel::kConfiguredTargetAccelerationMps2;
constexpr float kForwardAccelRiseSeconds = 1.0f;
constexpr float kForwardAccelFallSeconds = 0.25f;
constexpr DWORD kHandbrakeDecelerationHoldMilliseconds = 200;
constexpr std::uint8_t kRelativeCallOpcode = 0xE8u;
constexpr std::size_t kRelativeCallSize = 5;

constexpr std::uint8_t kAccelerateSignature[] = {
    0x8B, 0x41, 0x30, 0x8B, 0x00, 0x8A, 0x48, 0x5D,
    0x84, 0xC9, 0x75, 0x2A};

struct Identity {
    std::uintptr_t pvehicle = 0;
    std::uintptr_t player = 0;
    std::uintptr_t rigidBody = 0;
    std::uintptr_t holder = 0;
    std::uintptr_t inner = 0;
};

struct VehicleSample {
    Identity identity{};
    nfsmw_drift::Basis body{};
    nfsmw_drift::Vec3 linearVelocity{};
    nfsmw_drift::Vec3 angularVelocity{};
    float speedMps = 0.0f;
    std::uint32_t groundedWheels = 0;
    std::uint32_t directVehicleCollectionKey = 0;
    std::uint32_t vehicleCollectionKey = 0;
    std::uint32_t countersteerCollectionKey = 0;
    std::uint32_t vehicleKey = 0;
    std::array<char,
               vehicle_countersteer::kMaximumVehicleNameLength + 1>
        vehicleName{};
    const char* vehicleNameStatus = "not-read";
    std::uint32_t rearSteeringVisualCollectionKey = 0;
    std::uint32_t countersteerCollectionDepth = 0;
    std::uint32_t transmissionCollectionKey = 0;
    std::uintptr_t transmission = 0;
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
    std::uint32_t collectionChainKeys[kMaximumAttributeParentDepth + 1]{};
    std::uint32_t collectionChainLength = 0;
    const char* collectionChainStop = "unreadable-root";
    std::uint32_t transmissionChainKeys[kMaximumAttributeParentDepth + 1]{};
    std::uint32_t transmissionChainLength = 0;
    const char* transmissionChainStop = "unreadable-root";
    std::uintptr_t transmissionInterface = 0;
    std::uintptr_t transmissionVtable = 0;
    const char* transmissionStatus = "not-checked";
#endif
    std::uintptr_t suspension = 0;
};

enum class VehicleReadFailure : std::uint8_t {
    None,
    InvalidDestination,
    InstanceTableAddress,
    InstanceTableRange,
    InstanceSlotAddress,
    InstanceSlotRead,
    InstanceEnabledAddress,
    InstanceEnabledRead,
    InstanceEnabledValue,
    InstanceEnabledWithoutVehicle,
    PVehicleRange,
    PVehicleVtableContract,
    PVehicleVtableRead,
    PVehiclePlayerAddress,
    PVehiclePlayerRead,
    PVehicleStateAddress,
    PVehicleStateRead,
    SimableAddress,
    SimableVtableRead,
    SimableVtableContract,
    SimableVtableMismatch,
    SimableMethodAddress,
    SimableMethodRead,
    SimableMethodContract,
    SimableMethodMismatch,
    SimablePlayerCall,
    PlayerVtableRead,
    PlayerVtableContract,
    PlayerVtableMismatch,
    PlayerMethodAddress,
    PlayerMethodRead,
    PlayerMethodContract,
    PlayerMethodMismatch,
    PlayerSimableCall,
    PlayerCandidateCount,
    RigidBodyAddress,
    RigidBodyRead,
    RigidBodyNull,
    RigidBodyRange,
    RigidBodyVtableContract,
    RigidBodyVtableRead,
    RigidBodyVtableMismatch,
    HolderAddress,
    HolderRead,
    HolderNull,
    InnerRead,
    InnerNull,
    VelocityAddress,
    LinearVelocityRead,
    AngularVelocityRead,
    LinearVelocityBounds,
    AngularVelocityBounds,
    BasisAddress,
    BasisRead,
    BasisInvalid,
    SpeedAddress,
    SpeedRead,
    SpeedBounds,
    GroundedAddress,
    GroundedRead,
    GroundedBounds,
    MeasuredSpeedBounds,
    SpeedMismatch,
};

struct VehicleReadDiagnostics {
    VehicleReadFailure failure = VehicleReadFailure::None;
    std::uint32_t slot = static_cast<std::uint32_t>(kPVehicleInstanceLimit);
    std::uint32_t populatedSlots = 0;
    std::uint32_t enabledSlots = 0;
    std::uint32_t disabledSlots = 0;
    std::uint32_t nonNullPlayers = 0;
    std::uint32_t qualifiedPlayers = 0;
    std::uint32_t duplicateQualifiedPlayers = 0;
    std::uint32_t provenRetiredSlots = 0;
    std::uint32_t scanErrors = 0;
    std::uintptr_t candidatePvehicle = 0;
    std::uintptr_t candidatePlayer = 0;
    std::uintptr_t candidateSimable = 0;
    std::uintptr_t alternatePvehicle = 0;
    std::uintptr_t alternatePlayer = 0;
    std::uintptr_t alternateSimable = 0;
};

struct PendingManualYaw {
    bool valid = false;
    std::uint64_t sourcePhysicsSerial = 0;
    std::uint64_t inputSequence = 0;
    DWORD publishTick = 0;
    DWORD sourceThread = 0;
    Identity identity{};
    float rawSteering = 0.0f;
    int controlSide = 0;
    float manualYawStrength = 0.0f;
    nfsmw_drift::YawResponseMode expectedMode =
        nfsmw_drift::YawResponseMode::None;
    float sourceBodyOffsetRad = 0.0f;
    float sourcePathYawRateRadS = 0.0f;
};

std::atomic<InputPollFn> g_inputOriginal{nullptr};
std::atomic<ActiveComponentsFn> g_activeOriginal{nullptr};
std::atomic<CameraLookAtFn> g_cameraLookAtOriginal{nullptr};
std::atomic<std::uintptr_t> g_cameraAllowedReturnAddress{0};
std::atomic<SetAngularVelocityFn> g_setAngularVelocity{nullptr};
std::atomic<AccelerateFn> g_rigidbodyAccelerate{nullptr};
std::atomic<std::uintptr_t> g_imageBase{0};
std::atomic<std::size_t> g_imageSize{0};
std::atomic<std::uintptr_t> g_inputPollTarget{0};
std::atomic<bool> g_hooksArmed{false};
std::atomic<bool> g_permanentFault{false};
std::atomic<bool> g_yawPermanentFault{false};
std::atomic<DWORD> g_physicsThread{0};
std::atomic<DWORD> g_lastPhysicsTick{0};
std::atomic<std::uint32_t> g_lastPhysicsDtBits{0};
std::atomic<std::uint64_t> g_physicsSerial{0};
std::atomic<DWORD> g_lastFailureLogTick{0};
std::atomic<DWORD> g_lastCadenceFailureLogTick{0};
std::atomic<DWORD> g_lastTelemetryLogTick{0};
std::atomic<DWORD> g_lastYawTelemetryLogTick{0};
std::atomic<DWORD> g_lastDriverAssistTelemetryLogTick{0};
std::atomic<DWORD> g_lastRigidbodyTelemetryLogTick{0};
std::atomic<std::uint32_t> g_latestThrottleBits{0};
std::atomic<std::uint32_t> g_latestBrakeBits{0};
std::atomic<std::uint32_t> g_latestHandbrakeBits{0};
std::atomic<DWORD> g_latestHandbrakeHeldMilliseconds{0};
std::atomic<DWORD> g_handbrakePressedSinceTick{0};
std::atomic<std::uint64_t> g_latestInputPhysicsSerial{0};
std::atomic<std::uintptr_t> g_latestInputPvehicle{0};
std::atomic<DWORD> g_latestInputTick{0};
// The rigid-body path must follow the steering command that actually reached
// the game's input rows (automatic countersteer, manual slew, or raw
// pass-through), rather than a stale controller target.  The serial is the
// commit token and is always published last; readers validate it twice before
// accepting the payload.
std::atomic<std::uint32_t> g_latestAppliedSteeringBits{0};
std::atomic<std::uint64_t> g_latestSteeringPhysicsSerial{0};
std::atomic<std::uintptr_t> g_latestSteeringPvehicle{0};
std::atomic<std::uint32_t> g_rigidbodyAccelRampBits{0};
std::atomic<bool> g_rigidbodyAccelWriterDisabled{false};
std::atomic<bool> g_driverEscWriterDisabled{false};
std::atomic<bool> g_rigidbodyAccelStaticValid{false};
std::atomic<int> g_cameraTargetSide{0};
std::atomic<bool> g_cameraHookAttempted{false};
std::atomic<bool> g_cameraHookInstalled{false};
std::atomic<SuspensionGetWheelSteerFn> g_rearWheelSteerOriginal{nullptr};
std::atomic<std::uintptr_t> g_rearWheelSteerHookTarget{0};
std::atomic<bool> g_rearWheelSteerHookAttempted{false};
std::atomic<bool> g_rearWheelSteerHookInstalled{false};
std::atomic<bool> g_rearWheelSteerPermanentFault{false};
std::atomic<bool> g_rearWheelSteerOverrideEnabled{false};
std::atomic<std::uintptr_t> g_rearWheelSteerSuspension{0};
std::atomic<std::uint32_t> g_rearWheelSteerAngleBits{0};
std::atomic<DWORD> g_rearWheelSteerPublishTick{0};
std::array<std::atomic<std::uint64_t>, 4> g_rearWheelSteerCalls{};
std::atomic<std::uint64_t> g_rearWheelSteerOtherCalls{0};
std::atomic<std::uint64_t> g_rearWheelSteerOverrides{0};
std::atomic<DWORD> g_rearWheelSteerDiagnosticTick{0};
float g_rearWheelSteerSmoothedAngleRad = 0.0f;
DWORD g_rearWheelSteerSlewTick = 0;
std::uintptr_t g_rearWheelSteerSlewPvehicle = 0;
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
std::atomic<std::uintptr_t> g_wheelSteerProbeSuspension{0};
std::array<std::atomic<std::uint32_t>, 4> g_wheelSteerOriginalBits{};
std::array<std::atomic<std::uint32_t>, 2> g_rearWheelSteerReturnBits{};
std::array<std::atomic<std::uintptr_t>, 8> g_rearWheelSteerCallsites{};
std::array<std::atomic<std::uint64_t>, 8> g_rearWheelSteerCallsiteCounts{};
#endif
std::atomic<TransmissionGetDriveTorqueFn> g_driveTorqueOriginal{nullptr};
std::atomic<bool> g_driveTorqueHookAttempted{false};
std::atomic<bool> g_driveTorqueHookInstalled{false};
std::atomic<bool> g_driveTorqueOverrideEnabled{false};
std::atomic<std::uintptr_t> g_driveTorqueTransmission{0};
std::atomic<DWORD> g_driveTorquePublishTick{0};
std::atomic<std::uint64_t> g_driveTorqueCalls{0};
std::atomic<std::uint64_t> g_driveTorquePlayerCalls{0};
std::atomic<std::uint64_t> g_driveTorqueActiveCalls{0};
std::atomic<std::uint64_t> g_driveTorquePositiveCalls{0};
std::atomic<std::uint64_t> g_driveTorqueForwardCalls{0};
std::atomic<std::uint64_t> g_driveTorqueBoosts{0};
std::atomic<std::uintptr_t> g_driveTorqueLastSelf{0};
std::atomic<std::uint32_t> g_driveTorqueLastGear{0};
std::atomic<std::uint32_t> g_driveTorqueLastBaseBits{0};
SRWLOCK g_stateLock = SRWLOCK_INIT;
SRWLOCK g_steeringSnapshotLock = SRWLOCK_INIT;
SRWLOCK g_cameraAngleLock = SRWLOCK_INIT;
Identity g_stableIdentity{};
std::uint32_t g_probationCount = 0;
std::uint64_t g_lastProbationSerial = 0;
std::uint64_t g_lastControllerSerial = 0;
std::uint64_t g_latestInputSequence = 0;
bool g_runtimeReady = false;
bool g_hasCompletedInitialProbation = false;
drift_camera::CameraTransitionState g_cameraTransitionState{};
DWORD g_lastCameraTick = 0;
nfsmw_drift::ControlOutput g_cachedOutput{};
std::uint64_t g_cachedOutputSerial = 0;
driver_assist::Controller g_driverAssistController{};
driver_assist::Output g_cachedDriverAssistOutput{};
std::uint64_t g_cachedDriverAssistSerial = 0;
bool g_manualInputObservedSinceUpdate = false;
std::uint8_t g_manualDirectionMaskSinceUpdate = 0;
PendingManualYaw g_pendingManualYaw{};
// Keep the live bridge on the limiter call order used by the accepted 0.9.5
// build: an active session always advances either the automatic rate path or
// the manual fixed-rate path from the last command actually written.  The
// newer SessionLimiter added a per-serial veto/pass-through state; with the
// game's multiple input polls that state could turn an automatic frame into a
// synthetic neutral command and produce the visible zero-crossing regression.
steering_response::Limiter g_steeringResponseLimiter{};
steering_response::PreviousCommandTracker g_steeringInputHistory{};
// Tracks ownership across input polls.  A re-engagement frame must re-anchor
// the limiter to the automatic target before the normal automatic rate path
// resumes; otherwise the old manual trajectory crosses zero first.
bool g_steeringAutomaticOwnership = false;
nfsmw_drift::DriftPhase g_lastLoggedPhase = nfsmw_drift::DriftPhase::Off;
nfsmw_drift::SteeringAssistMode g_lastLoggedSteeringMode =
    nfsmw_drift::SteeringAssistMode::None;
nfsmw_drift::DriftAssistController* g_controller = nullptr;
nfsmw_drift::AssistConfig g_baseControllerConfig{};
vehicle_countersteer::Registry g_vehicleCountersteerMultipliers{};
rear_wheel_steering::Registry g_rearWheelSteeringVehicles{};
std::uint32_t g_currentVehicleCollectionKey = 0;

float BitsToFloat(std::uint32_t bits);
float NFSMW_FASTCALL DriftDriveTorqueDetour(void* self, void* unusedEdx);

void Log(const char* message) {
    if (message == nullptr) {
        return;
    }
    char line[1200]{};
    std::snprintf(line, sizeof(line), "[nfsmw_drift_assist] %s\n", message);
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(line);
}

#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
void RearWheelSteerDiagnosticWrite(const char* message) {
    if (message == nullptr) {
        return;
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&RearWheelSteerDiagnosticWrite),
            &module) == FALSE) {
        return;
    }
    char path[1024]{};
    const DWORD length = GetModuleFileNameA(
        module, path, static_cast<DWORD>(sizeof(path)));
    if (length == 0 || length >= sizeof(path)) {
        return;
    }
    char* separator = std::strrchr(path, '\\');
    if (separator == nullptr) {
        separator = std::strrchr(path, '/');
    }
    if (separator == nullptr) {
        return;
    }
    const char fileName[] =
        "\\Slippery_Drifting_FlashFish_by_AndyQinke.rws.log";
    const std::size_t prefixLength =
        static_cast<std::size_t>(separator - path);
    if (prefixLength + sizeof(fileName) > sizeof(path)) {
        return;
    }
    std::memcpy(path + prefixLength, fileName, sizeof(fileName));

    const HANDLE file = CreateFileA(
        path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char line[1600]{};
    const int written = std::snprintf(
        line, sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n",
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds), message);
    if (written > 0) {
        DWORD bytesWritten = 0;
        const DWORD byteCount = static_cast<DWORD>(
            std::min<int>(written, static_cast<int>(sizeof(line) - 1)));
        (void)WriteFile(file, line, byteCount, &bytesWritten, nullptr);
    }
    CloseHandle(file);
}
#endif

void LogDriverAssistTelemetry(const driver_assist::Output& output,
                              int gear,
                              float brake,
                              float throttle,
                              float bodyOffsetRad,
                              float yawRateRadS);

void LogFailureRateLimited(const char* reason) {
    const DWORD now = GetTickCount();
    DWORD previous = g_lastFailureLogTick.load(std::memory_order_relaxed);
    if (previous != 0 &&
        static_cast<DWORD>(now - previous) < kFailureLogIntervalMs) {
        return;
    }
    if (!g_lastFailureLogTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }
    char message[1120]{};
    std::snprintf(message, sizeof(message),
                  "alpha write path paused: %s", reason);
    Log(message);
}

void LogCadenceFailureRateLimited(DWORD inputThread,
                                  DWORD physicsThread,
                                  std::uint64_t physicsSerial,
                                  DWORD physicsTick,
                                  DWORD now,
                                  float dt) {
    DWORD previous =
        g_lastCadenceFailureLogTick.load(std::memory_order_relaxed);
    if (previous != 0 &&
        static_cast<DWORD>(now - previous) < kFailureLogIntervalMs) {
        return;
    }
    if (!g_lastCadenceFailureLogTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }
    const DWORD age = physicsTick == 0
                          ? static_cast<DWORD>(-1)
                          : static_cast<DWORD>(now - physicsTick);
    const bool threadValid =
        physicsThread != 0 && inputThread == physicsThread;
    const bool serialValid = physicsSerial != 0;
    const bool freshnessValid =
        physicsTick != 0 && age <= kPhysicsFreshnessMs;
    const bool dtValid =
        std::isfinite(dt) && dt > 0.0f && dt <= kMaximumCadenceDt;
    char message[520]{};
    std::snprintf(
        message, sizeof(message),
        "alpha write path paused: cadence input_thread=%lu "
        "physics_thread=%lu serial=%llu age_ms=%lu dt=%.6f "
        "thread_ok=%d serial_ok=%d freshness_ok=%d dt_ok=%d",
        static_cast<unsigned long>(inputThread),
        static_cast<unsigned long>(physicsThread),
        static_cast<unsigned long long>(physicsSerial),
        static_cast<unsigned long>(age), static_cast<double>(dt),
        threadValid ? 1 : 0, serialValid ? 1 : 0,
        freshnessValid ? 1 : 0, dtValid ? 1 : 0);
    Log(message);
}

const char* VehicleReadFailureText(VehicleReadFailure failure) {
    switch (failure) {
        case VehicleReadFailure::None:
            return "stage=none detail=unspecified";
        case VehicleReadFailure::InvalidDestination:
            return "stage=sample detail=invalid-destination";
        case VehicleReadFailure::InstanceTableAddress:
            return "stage=instance-table detail=address-overflow";
        case VehicleReadFailure::InstanceTableRange:
            return "stage=instance-table detail=range-unreadable";
        case VehicleReadFailure::InstanceSlotAddress:
            return "stage=instance-table detail=slot-address-overflow";
        case VehicleReadFailure::InstanceSlotRead:
            return "stage=instance-table detail=slot-pointer-unreadable";
        case VehicleReadFailure::InstanceEnabledAddress:
            return "stage=instance-table detail=enabled-address-overflow";
        case VehicleReadFailure::InstanceEnabledRead:
            return "stage=instance-table detail=enabled-byte-unreadable";
        case VehicleReadFailure::InstanceEnabledValue:
            return "stage=instance-table detail=enabled-byte-invalid";
        case VehicleReadFailure::InstanceEnabledWithoutVehicle:
            return "stage=instance-table detail=enabled-slot-has-null-vehicle";
        case VehicleReadFailure::PVehicleRange:
            return "stage=pvehicle detail=object-range-unreadable";
        case VehicleReadFailure::PVehicleVtableContract:
            return "stage=pvehicle-vtable detail=expected-contract-unavailable";
        case VehicleReadFailure::PVehicleVtableRead:
            return "stage=pvehicle-vtable detail=pointer-unreadable";
        case VehicleReadFailure::PVehiclePlayerAddress:
            return "stage=pvehicle detail=player-field-address-overflow";
        case VehicleReadFailure::PVehiclePlayerRead:
            return "stage=pvehicle detail=player-field-unreadable";
        case VehicleReadFailure::PVehicleStateAddress:
            return "stage=pvehicle detail=state-field-address-overflow";
        case VehicleReadFailure::PVehicleStateRead:
            return "stage=pvehicle detail=state-field-unreadable";
        case VehicleReadFailure::SimableAddress:
            return "stage=simable detail=subobject-address-overflow";
        case VehicleReadFailure::SimableVtableRead:
            return "stage=simable-vtable detail=pointer-unreadable";
        case VehicleReadFailure::SimableVtableContract:
            return "stage=simable-vtable detail=not-read-only-image-data";
        case VehicleReadFailure::SimableVtableMismatch:
            return "stage=simable-vtable detail=mismatch";
        case VehicleReadFailure::SimableMethodAddress:
            return "stage=simable-vtable detail=slot-address-overflow";
        case VehicleReadFailure::SimableMethodRead:
            return "stage=simable-vtable detail=slot-unreadable";
        case VehicleReadFailure::SimableMethodContract:
            return "stage=simable-vtable detail=method-not-executable-image-code";
        case VehicleReadFailure::SimableMethodMismatch:
            return "stage=simable-vtable detail=method-target-mismatch";
        case VehicleReadFailure::SimablePlayerCall:
            return "stage=simable detail=player-contract-call-fault";
        case VehicleReadFailure::PlayerVtableRead:
            return "stage=player-vtable detail=pointer-unreadable";
        case VehicleReadFailure::PlayerVtableContract:
            return "stage=player-vtable detail=expected-contract-unavailable";
        case VehicleReadFailure::PlayerVtableMismatch:
            return "stage=player-vtable detail=mismatch";
        case VehicleReadFailure::PlayerMethodAddress:
            return "stage=player-vtable detail=slot-address-overflow";
        case VehicleReadFailure::PlayerMethodRead:
            return "stage=player-vtable detail=slot-unreadable";
        case VehicleReadFailure::PlayerMethodContract:
            return "stage=player-vtable detail=method-not-executable-image-code";
        case VehicleReadFailure::PlayerMethodMismatch:
            return "stage=player-vtable detail=method-target-mismatch";
        case VehicleReadFailure::PlayerSimableCall:
            return "stage=player detail=get-simable-call-fault";
        case VehicleReadFailure::PlayerCandidateCount:
            return "stage=candidate-scan detail=qualified-player-count-not-one";
        case VehicleReadFailure::RigidBodyAddress:
            return "stage=rigid-body detail=field-address-overflow";
        case VehicleReadFailure::RigidBodyRead:
            return "stage=rigid-body detail=field-unreadable";
        case VehicleReadFailure::RigidBodyNull:
            return "stage=rigid-body detail=null";
        case VehicleReadFailure::RigidBodyRange:
            return "stage=rigid-body detail=object-range-unreadable";
        case VehicleReadFailure::RigidBodyVtableContract:
            return "stage=rigid-body-vtable detail=expected-contract-unavailable";
        case VehicleReadFailure::RigidBodyVtableRead:
            return "stage=rigid-body-vtable detail=pointer-unreadable";
        case VehicleReadFailure::RigidBodyVtableMismatch:
            return "stage=rigid-body-vtable detail=mismatch";
        case VehicleReadFailure::HolderAddress:
            return "stage=holder detail=field-address-overflow";
        case VehicleReadFailure::HolderRead:
            return "stage=holder detail=field-unreadable";
        case VehicleReadFailure::HolderNull:
            return "stage=holder detail=null";
        case VehicleReadFailure::InnerRead:
            return "stage=inner detail=pointer-unreadable";
        case VehicleReadFailure::InnerNull:
            return "stage=inner detail=null";
        case VehicleReadFailure::VelocityAddress:
            return "stage=velocity detail=field-address-overflow";
        case VehicleReadFailure::LinearVelocityRead:
            return "stage=velocity detail=linear-vector-unreadable";
        case VehicleReadFailure::AngularVelocityRead:
            return "stage=velocity detail=angular-vector-unreadable";
        case VehicleReadFailure::LinearVelocityBounds:
            return "stage=velocity detail=linear-vector-out-of-bounds";
        case VehicleReadFailure::AngularVelocityBounds:
            return "stage=velocity detail=angular-vector-out-of-bounds";
        case VehicleReadFailure::BasisAddress:
            return "stage=basis detail=field-address-overflow";
        case VehicleReadFailure::BasisRead:
            return "stage=basis detail=vector-unreadable";
        case VehicleReadFailure::BasisInvalid:
            return "stage=basis detail=non-orthonormal-or-out-of-bounds";
        case VehicleReadFailure::SpeedAddress:
            return "stage=speed detail=field-address-overflow";
        case VehicleReadFailure::SpeedRead:
            return "stage=speed detail=value-unreadable-or-nonfinite";
        case VehicleReadFailure::SpeedBounds:
            return "stage=speed detail=value-out-of-bounds";
        case VehicleReadFailure::GroundedAddress:
            return "stage=grounded detail=field-address-overflow";
        case VehicleReadFailure::GroundedRead:
            return "stage=grounded detail=value-unreadable";
        case VehicleReadFailure::GroundedBounds:
            return "stage=grounded detail=value-out-of-bounds";
        case VehicleReadFailure::MeasuredSpeedBounds:
            return "stage=velocity detail=measured-speed-out-of-bounds";
        case VehicleReadFailure::SpeedMismatch:
            return "stage=speed detail=reported-measured-mismatch";
    }
    return "stage=unknown detail=unmapped";
}

void NoteVehicleReadFailure(VehicleReadDiagnostics* diagnostics,
                            VehicleReadFailure failure,
                            std::size_t slot = kPVehicleInstanceLimit) {
    if (diagnostics == nullptr ||
        diagnostics->failure != VehicleReadFailure::None) {
        return;
    }
    diagnostics->failure = failure;
    diagnostics->slot = static_cast<std::uint32_t>(slot);
}

void LogVehicleReadFailureRateLimited(
    const char* context, const VehicleReadDiagnostics& diagnostics) {
    char reason[1040]{};
    if (diagnostics.slot < kPVehicleInstanceLimit) {
        std::snprintf(
            reason, sizeof(reason),
            "vehicle read failed context=%s %s slot=%u populated=%u enabled=%u disabled=%u non_null_players=%u qualified_players=%u duplicate_qualified=%u proven_retired=%u scan_errors=%u candidate=(%p,%p,%p) alternate=(%p,%p,%p)",
            context != nullptr ? context : "unknown",
            VehicleReadFailureText(diagnostics.failure), diagnostics.slot,
            diagnostics.populatedSlots, diagnostics.enabledSlots,
            diagnostics.disabledSlots, diagnostics.nonNullPlayers,
            diagnostics.qualifiedPlayers,
            diagnostics.duplicateQualifiedPlayers,
            diagnostics.provenRetiredSlots,
            diagnostics.scanErrors,
            reinterpret_cast<void*>(diagnostics.candidatePvehicle),
            reinterpret_cast<void*>(diagnostics.candidatePlayer),
            reinterpret_cast<void*>(diagnostics.candidateSimable),
            reinterpret_cast<void*>(diagnostics.alternatePvehicle),
            reinterpret_cast<void*>(diagnostics.alternatePlayer),
            reinterpret_cast<void*>(diagnostics.alternateSimable));
    } else {
        std::snprintf(
            reason, sizeof(reason),
            "vehicle read failed context=%s %s populated=%u enabled=%u disabled=%u non_null_players=%u qualified_players=%u duplicate_qualified=%u proven_retired=%u scan_errors=%u candidate=(%p,%p,%p) alternate=(%p,%p,%p)",
            context != nullptr ? context : "unknown",
            VehicleReadFailureText(diagnostics.failure),
            diagnostics.populatedSlots, diagnostics.enabledSlots,
            diagnostics.disabledSlots, diagnostics.nonNullPlayers,
            diagnostics.qualifiedPlayers,
            diagnostics.duplicateQualifiedPlayers,
            diagnostics.provenRetiredSlots,
            diagnostics.scanErrors,
            reinterpret_cast<void*>(diagnostics.candidatePvehicle),
            reinterpret_cast<void*>(diagnostics.candidatePlayer),
            reinterpret_cast<void*>(diagnostics.candidateSimable),
            reinterpret_cast<void*>(diagnostics.alternatePvehicle),
            reinterpret_cast<void*>(diagnostics.alternatePlayer),
            reinterpret_cast<void*>(diagnostics.alternateSimable));
    }
    LogFailureRateLimited(reason);
}

bool AddAddress(std::uintptr_t base,
                std::size_t offset,
                std::uintptr_t* result) {
    if (result == nullptr ||
        base > static_cast<std::uintptr_t>(-1) - offset) {
        return false;
    }
    *result = base + offset;
    return true;
}

bool ReadableRange(const void* address, std::size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xffu;
    if (protection == PAGE_NOACCESS) {
        return false;
    }
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t regionBegin =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + info.RegionSize;
    return regionEnd >= regionBegin && begin >= regionBegin &&
           size <= regionEnd - begin;
}

bool WritableRange(void* address, std::size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xffu;
    const bool writable = protection == PAGE_READWRITE ||
                          protection == PAGE_WRITECOPY ||
                          protection == PAGE_EXECUTE_READWRITE ||
                          protection == PAGE_EXECUTE_WRITECOPY;
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t regionBegin =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + info.RegionSize;
    return writable && regionEnd >= regionBegin && begin >= regionBegin &&
           size <= regionEnd - begin;
}

bool ExecutableAddress(const void* address) {
    if (!ReadableRange(address, 1)) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }
    const DWORD protection = info.Protect & 0xffu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool ImageRange(const void* address, std::size_t size) {
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    const std::size_t imageSize = g_imageSize.load(std::memory_order_acquire);
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    if (address == nullptr || size == 0 || base == 0 || imageSize == 0 ||
        begin < base) {
        return false;
    }
    const std::uintptr_t offset = begin - base;
    return offset <= imageSize && size <= imageSize - offset &&
           ReadableRange(address, size);
}

bool ReadOnlyImageRange(const void* address, std::size_t size) {
    if (!ImageRange(address, size)) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0 ||
        (info.Protect & 0xffu) != PAGE_READONLY) {
        return false;
    }
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t regionBegin =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + info.RegionSize;
    return regionEnd >= regionBegin && begin >= regionBegin &&
           size <= regionEnd - begin;
}

bool ExecutableImageAddress(const void* address) {
    if (!ImageRange(address, 1)) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xffu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ;
}

bool SafeImageBytesEqual(std::uintptr_t address,
                         const std::uint8_t* expected,
                         std::size_t size) {
    if (expected == nullptr || size == 0 ||
        !ImageRange(reinterpret_cast<const void*>(address), size)) {
        return false;
    }
    bool matches = false;
#if defined(_MSC_VER)
    __try {
        matches = std::memcmp(reinterpret_cast<const void*>(address),
                              expected, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        matches = false;
    }
#else
    matches = std::memcmp(reinterpret_cast<const void*>(address), expected,
                          size) == 0;
#endif
    return matches;
}

bool DecodeRelativeCall(std::uintptr_t callSite,
                        std::uintptr_t* destination,
                        std::uint8_t* bytes = nullptr) {
    if (destination == nullptr ||
        !ImageRange(reinterpret_cast<const void*>(callSite),
                    kRelativeCallSize)) {
        return false;
    }
    std::uint8_t local[kRelativeCallSize]{};
    bool read = false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(local, reinterpret_cast<const void*>(callSite),
                    sizeof(local));
        read = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        read = false;
    }
#else
    std::memcpy(local, reinterpret_cast<const void*>(callSite),
                sizeof(local));
    read = true;
#endif
    if (!read || local[0] != kRelativeCallOpcode) {
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, local + 1, sizeof(displacement));
    const std::int64_t next =
        static_cast<std::int64_t>(callSite) + kRelativeCallSize;
    const std::int64_t target =
        next + static_cast<std::int64_t>(displacement);
    if (target <= 0 ||
        static_cast<std::uint64_t>(target) >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uintptr_t>::max())) {
        return false;
    }
    *destination = static_cast<std::uintptr_t>(target);
    if (bytes != nullptr) {
        std::memcpy(bytes, local, sizeof(local));
    }
    return ExecutableAddress(reinterpret_cast<const void*>(*destination));
}

bool ModuleHeaders(HMODULE module, const IMAGE_NT_HEADERS32** ntHeaders) {
    if (module == nullptr || ntHeaders == nullptr ||
        !ReadableRange(module, sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
    std::uintptr_t ntAddress = 0;
    if (!AddAddress(base, static_cast<std::size_t>(dos->e_lfanew),
                    &ntAddress) ||
        !ReadableRange(reinterpret_cast<const void*>(ntAddress),
                       sizeof(IMAGE_NT_HEADERS32))) {
        return false;
    }
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(ntAddress);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->FileHeader.NumberOfSections == 0 ||
        nt->FileHeader.NumberOfSections > 96 ||
        nt->OptionalHeader.SizeOfImage == 0) {
        return false;
    }
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const std::uint8_t*>(&nt->OptionalHeader) +
        nt->FileHeader.SizeOfOptionalHeader);
    if (!ReadableRange(
            sections,
            static_cast<std::size_t>(nt->FileHeader.NumberOfSections) *
                sizeof(IMAGE_SECTION_HEADER))) {
        return false;
    }
    *ntHeaders = nt;
    return true;
}

bool ModuleExecutableSectionContains(HMODULE module,
                                     std::uintptr_t address) {
    const IMAGE_NT_HEADERS32* nt = nullptr;
    if (!ModuleHeaders(module, &nt)) {
        return false;
    }
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const std::uint8_t*>(&nt->OptionalHeader) +
        nt->FileHeader.SizeOfOptionalHeader);
    for (unsigned int index = 0;
         index < nt->FileHeader.NumberOfSections; ++index) {
        const IMAGE_SECTION_HEADER& section = sections[index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        const std::size_t span = std::max<std::size_t>(
            section.Misc.VirtualSize, section.SizeOfRawData);
        std::uintptr_t begin = 0;
        if (span == 0 ||
            !AddAddress(base, section.VirtualAddress, &begin) ||
            begin < base || address < begin || address - begin >= span ||
            address - base >= nt->OptionalHeader.SizeOfImage) {
            continue;
        }
        return ExecutableAddress(reinterpret_cast<const void*>(address));
    }
    return false;
}

bool ModuleHasBaseName(HMODULE module, const char* expectedName) {
    if (module == nullptr || expectedName == nullptr) {
        return false;
    }
    char path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    const char* fileName = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            fileName = cursor + 1;
        }
    }
    return _stricmp(fileName, expectedName) == 0;
}

bool ModuleFromAddress(std::uintptr_t address, HMODULE* module) {
    if (address == 0 || module == nullptr) {
        return false;
    }
    *module = nullptr;
    return GetModuleHandleExA(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
               reinterpret_cast<LPCSTR>(address), module) != FALSE;
}

bool FindOrbitCameraReturnAddress(HMODULE module,
                                  std::uintptr_t originalTarget,
                                  std::uintptr_t* returnAddress) {
    if (returnAddress == nullptr ||
        originalTarget >
            static_cast<std::uintptr_t>(
                std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    const IMAGE_NT_HEADERS32* nt = nullptr;
    if (!ModuleHeaders(module, &nt)) {
        return false;
    }
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
    const std::uint32_t target32 =
        static_cast<std::uint32_t>(originalTarget);
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const std::uint8_t*>(&nt->OptionalHeader) +
        nt->FileHeader.SizeOfOptionalHeader);
    std::uintptr_t match = 0;
    unsigned int matches = 0;
    for (unsigned int index = 0;
         index < nt->FileHeader.NumberOfSections; ++index) {
        const IMAGE_SECTION_HEADER& section = sections[index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        const std::size_t span = std::max<std::size_t>(
            section.Misc.VirtualSize, section.SizeOfRawData);
        std::uintptr_t begin = 0;
        if (span < 13 ||
            !AddAddress(base, section.VirtualAddress, &begin) ||
            section.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
            span > nt->OptionalHeader.SizeOfImage - section.VirtualAddress ||
            !ReadableRange(reinterpret_cast<const void*>(begin), span)) {
            continue;
        }
        const auto* code = reinterpret_cast<const std::uint8_t*>(begin);
#if defined(_MSC_VER)
        __try {
#endif
            for (std::size_t offset = 0; offset + 13 <= span; ++offset) {
                std::uint32_t immediate = 0;
                std::memcpy(&immediate, code + offset + 1,
                            sizeof(immediate));
                if (code[offset] == 0xB8u && immediate == target32 &&
                    code[offset + 5] == 0x57u &&
                    code[offset + 6] == 0x56u &&
                    code[offset + 7] == 0xFFu &&
                    code[offset + 8] == 0x74u &&
                    code[offset + 9] == 0x24u &&
                    code[offset + 11] == 0xFFu &&
                    code[offset + 12] == 0xD0u) {
                    match = begin + offset + 13;
                    ++matches;
                }
            }
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
#endif
    }
    if (matches != 1 ||
        !ModuleExecutableSectionContains(module, match)) {
        return false;
    }
    *returnAddress = match;
    return true;
}

bool ValidateCameraLookAtEntry(std::uintptr_t originalTarget,
                               HMODULE* existingHookModule) {
    if (existingHookModule == nullptr) {
        return false;
    }
    *existingHookModule = nullptr;
    if (SafeImageBytesEqual(
            originalTarget, kCreateLookAtMatrixSignature,
            sizeof(kCreateLookAtMatrixSignature))) {
        return true;
    }

    std::uint8_t jump[kRelativeCallSize]{};
    if (!ImageRange(reinterpret_cast<const void*>(originalTarget),
                    sizeof(jump))) {
        return false;
    }
    bool read = false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(jump, reinterpret_cast<const void*>(originalTarget),
                    sizeof(jump));
        read = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        read = false;
    }
#else
    std::memcpy(jump, reinterpret_cast<const void*>(originalTarget),
                sizeof(jump));
    read = true;
#endif
    if (!read || jump[0] != 0xE9u) {
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, jump + 1, sizeof(displacement));
    const std::int64_t target64 =
        static_cast<std::int64_t>(originalTarget) +
        kRelativeCallSize + static_cast<std::int64_t>(displacement);
    if (target64 <= 0 ||
        static_cast<std::uint64_t>(target64) >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uintptr_t>::max())) {
        return false;
    }
    const std::uintptr_t target =
        static_cast<std::uintptr_t>(target64);
    HMODULE module = nullptr;
    if (!ModuleFromAddress(target, &module) ||
        !ModuleHasBaseName(module, "NFS.CameraMod.asi") ||
        !ModuleExecutableSectionContains(module, target)) {
        return false;
    }
    *existingHookModule = module;
    return true;
}

void PinModuleContaining(std::uintptr_t address) {
    HMODULE pinned = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCSTR>(address), &pinned);
}

int NFSMW_CDECL DriftCameraLookAtDetour(
    void* outMatrix, nfsmw_drift::Vec3* from, nfsmw_drift::Vec3* to,
    nfsmw_drift::Vec3* up) {
#if defined(_MSC_VER)
    const std::uintptr_t returnAddress =
        reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
    const std::uintptr_t returnAddress = reinterpret_cast<std::uintptr_t>(
        __builtin_return_address(0));
#endif
    const CameraLookAtFn original =
        g_cameraLookAtOriginal.load(std::memory_order_acquire);
    if (original == nullptr) {
        return 0;
    }

    if (returnAddress !=
        g_cameraAllowedReturnAddress.load(std::memory_order_acquire)) {
        return original(outMatrix, from, to, up);
    }

    float angleDegrees = 0.0f;
    AcquireSRWLockExclusive(&g_cameraAngleLock);
    const DWORD now = GetTickCount();
    const float targetDegrees = drift_camera::TargetAngleDegrees(
        true, g_cameraTargetSide.load(std::memory_order_acquire));
    const DWORD elapsedMs = g_lastCameraTick == 0
                                ? 0
                                : static_cast<DWORD>(now - g_lastCameraTick);
    g_lastCameraTick = now;
    const float elapsedSeconds = drift_camera::VisibleFrameStepSeconds(
        static_cast<float>(elapsedMs) * 0.001f);
    angleDegrees = drift_camera::AdvanceAngleDegrees(
        g_cameraTransitionState, targetDegrees, elapsedSeconds);
    ReleaseSRWLockExclusive(&g_cameraAngleLock);

    nfsmw_drift::Vec3 localFrom{};
    nfsmw_drift::Vec3* fromForCall = from;
    if (std::fabs(angleDegrees) > kMinimumOffsetChangeRad &&
        ReadableRange(from, sizeof(*from)) &&
        ReadableRange(to, sizeof(*to))) {
        bool transformed = false;
#if defined(_MSC_VER)
        __try {
            localFrom = drift_camera::RotateHorizontalFrom(
                *from, *to, angleDegrees);
            transformed = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            transformed = false;
        }
#else
        localFrom = drift_camera::RotateHorizontalFrom(
            *from, *to, angleDegrees);
        transformed = true;
#endif
        if (transformed) {
            fromForCall = &localFrom;
        }
    }
    return original(outMatrix, fromForCall, to, up);
}

void PublishDriftCameraTarget(const nfsmw_drift::ControlOutput& output) {
    int side = 0;
    if (output.active) {
        if (output.sideTransitionActive &&
            (output.sideTransitionTargetSide == -1 ||
             output.sideTransitionTargetSide == 1)) {
            side = output.sideTransitionTargetSide;
        } else if (output.driftSide == -1 || output.driftSide == 1) {
            side = output.driftSide;
        }
    }
    AcquireSRWLockExclusive(&g_cameraAngleLock);
    const int previousSide =
        g_cameraTargetSide.load(std::memory_order_relaxed);
    const float targetDegrees =
        drift_camera::TargetAngleDegrees(true, side);
    if (side != previousSide ||
        targetDegrees != g_cameraTransitionState.targetDegrees) {
        // Retarget at publication time so the next LookAt callback cannot
        // charge time that elapsed before this drift-side change.
        (void)drift_camera::AdvanceAngleDegrees(
            g_cameraTransitionState, targetDegrees, 0.0f);
        g_lastCameraTick = GetTickCount();
    }
    g_cameraTargetSide.store(side, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_cameraAngleLock);
}

void ClearDriftCameraTarget() {
    AcquireSRWLockExclusive(&g_cameraAngleLock);
    g_cameraTargetSide.store(0, std::memory_order_release);
    if (g_cameraTransitionState.targetDegrees != 0.0f) {
        (void)drift_camera::AdvanceAngleDegrees(
            g_cameraTransitionState, 0.0f, 0.0f);
        g_lastCameraTick = GetTickCount();
    }
    ReleaseSRWLockExclusive(&g_cameraAngleLock);
}

void ResetDriftCameraState() {
    AcquireSRWLockExclusive(&g_cameraAngleLock);
    g_cameraTargetSide.store(0, std::memory_order_release);
    g_cameraTransitionState = drift_camera::CameraTransitionState{};
    g_lastCameraTick = 0;
    ReleaseSRWLockExclusive(&g_cameraAngleLock);
}

void EnsureDriftCameraHookInstalled() {
    if (g_cameraHookInstalled.load(std::memory_order_acquire) ||
        g_cameraHookAttempted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t callSite = 0;
    std::uintptr_t chainedTarget = 0;
    std::uintptr_t originalTarget = 0;
    if (!AddAddress(base, kCameraLookAtCallRva, &callSite) ||
        !AddAddress(base, kCreateLookAtMatrixRva, &originalTarget) ||
        !ExecutableImageAddress(
            reinterpret_cast<const void*>(originalTarget)) ||
        !DecodeRelativeCall(callSite, &chainedTarget) ||
        chainedTarget ==
            reinterpret_cast<std::uintptr_t>(&DriftCameraLookAtDetour)) {
        Log("alpha camera assist disabled: camera targets failed validation");
        return;
    }
    HMODULE existingLookAtHookModule = nullptr;
    if (!ValidateCameraLookAtEntry(
            originalTarget, &existingLookAtHookModule)) {
        Log("alpha camera assist disabled: unsupported existing LookAt hook");
        return;
    }

    std::uintptr_t allowedReturnAddress = 0;
    HMODULE orbitModule = nullptr;
    const char* mode = "direct";
    if (chainedTarget == originalTarget) {
        allowedReturnAddress = callSite + kRelativeCallSize;
    } else if (ModuleFromAddress(chainedTarget, &orbitModule) &&
               ModuleHasBaseName(orbitModule, "NFSMWOrbitCamera.asi") &&
               ModuleExecutableSectionContains(orbitModule, chainedTarget) &&
               FindOrbitCameraReturnAddress(
                   orbitModule, originalTarget, &allowedReturnAddress)) {
        mode = "orbit-compatible";
    } else {
        Log("alpha camera assist disabled: unsupported camera call owner");
        return;
    }

    void* originalRaw = nullptr;
    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<void*>(originalTarget),
        reinterpret_cast<void*>(&DriftCameraLookAtDetour), &originalRaw);
    if (create != MH_OK || originalRaw == nullptr) {
        char message[280]{};
        std::snprintf(
            message, sizeof(message),
            "alpha camera assist disabled: LookAt hook creation failed (%s)",
            MH_StatusToString(create));
        Log(message);
        return;
    }
    g_cameraLookAtOriginal.store(
        reinterpret_cast<CameraLookAtFn>(originalRaw),
        std::memory_order_release);
    g_cameraAllowedReturnAddress.store(
        allowedReturnAddress, std::memory_order_release);

    const MH_STATUS queue =
        MH_QueueEnableHook(reinterpret_cast<void*>(originalTarget));
    if (queue != MH_OK) {
        const MH_STATUS cleanup =
            MH_RemoveHook(reinterpret_cast<void*>(originalTarget));
        g_cameraAllowedReturnAddress.store(0, std::memory_order_release);
        g_cameraLookAtOriginal.store(nullptr, std::memory_order_release);
        char message[320]{};
        std::snprintf(
            message, sizeof(message),
            "alpha camera assist disabled: LookAt hook queue failed (%s), cleanup=%s",
            MH_StatusToString(queue), MH_StatusToString(cleanup));
        Log(message);
        return;
    }
    const MH_STATUS apply = MH_ApplyQueued();
    if (apply != MH_OK) {
        g_cameraAllowedReturnAddress.store(0, std::memory_order_release);
        const MH_STATUS disable =
            MH_QueueDisableHook(reinterpret_cast<void*>(originalTarget));
        const MH_STATUS rollback = MH_ApplyQueued();
        char message[360]{};
        std::snprintf(
            message, sizeof(message),
            "alpha camera assist disarmed: LookAt hook enable failed (%s), rollback queue=%s apply=%s",
            MH_StatusToString(apply), MH_StatusToString(disable),
            MH_StatusToString(rollback));
        Log(message);
        return;
    }

    g_cameraHookInstalled.store(true, std::memory_order_release);
    PinModuleContaining(
        reinterpret_cast<std::uintptr_t>(&DriftCameraLookAtDetour));
    if (orbitModule != nullptr) {
        PinModuleContaining(chainedTarget);
    }
    if (existingLookAtHookModule != nullptr) {
        PinModuleContaining(reinterpret_cast<std::uintptr_t>(
            existingLookAtHookModule));
    }
    char message[460]{};
    std::snprintf(
        message, sizeof(message),
        "alpha camera assist installed look_at=%p caller=%p mode=%s "
        "look_at_chain=%s angle=%.1fdeg direction=inverted "
        "entry=%.1fs-smoothstep return_switch=%.2fs-smoothstep",
        reinterpret_cast<void*>(originalTarget),
        reinterpret_cast<void*>(allowedReturnAddress), mode,
        existingLookAtHookModule != nullptr ? "camera-mod" : "direct",
        static_cast<double>(drift_camera::kLockedAngleDegrees),
        static_cast<double>(drift_camera::kTransitionDurationSeconds),
        static_cast<double>(drift_camera::kFastTransitionDurationSeconds));
    Log(message);
}

bool SafeReadPointer(const void* address, std::uintptr_t* value) {
    if (value == nullptr || !ReadableRange(address, sizeof(*value))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        *value = *reinterpret_cast<const std::uintptr_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *value = *reinterpret_cast<const std::uintptr_t*>(address);
#endif
    return true;
}

bool SafeReadU32(const void* address, std::uint32_t* value) {
    if (value == nullptr || !ReadableRange(address, sizeof(*value))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        *value = *reinterpret_cast<const std::uint32_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *value = *reinterpret_cast<const std::uint32_t*>(address);
#endif
    return true;
}

bool ReadCurrentVehicleNameKey(
    std::uint32_t* vehicleKey,
    std::array<char,
               vehicle_countersteer::kMaximumVehicleNameLength + 1>*
        vehicleName,
    const char** status) {
    if (vehicleKey == nullptr || vehicleName == nullptr || status == nullptr) {
        return false;
    }
    *vehicleKey = 0;
    *vehicleName = {};
    *status = "global-address";
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t globalAddress = 0;
    std::uintptr_t database = 0;
    std::uintptr_t recordSlot = 0;
    std::uintptr_t record = 0;
    std::uintptr_t nameSlot = 0;
    std::uintptr_t nameAddress = 0;
    if (!AddAddress(base, kCurrentCarDatabaseRva, &globalAddress)) return false;
    *status = "global-contract";
    if (!ImageRange(reinterpret_cast<const void*>(globalAddress),
                    sizeof(std::uintptr_t))) return false;
    *status = "database-pointer";
    if (!SafeReadPointer(reinterpret_cast<const void*>(globalAddress),
                         &database)) return false;
    *status = "database-null";
    if (database == 0) return false;
    *status = "record-address";
    if (!AddAddress(database, kCurrentCarRecordOffset, &recordSlot)) return false;
    *status = "record-pointer";
    if (!SafeReadPointer(reinterpret_cast<const void*>(recordSlot), &record)) {
        return false;
    }
    *status = "record-null";
    if (record == 0) return false;
    *status = "name-address";
    if (!AddAddress(record, kCurrentCarNameOffset, &nameSlot)) return false;
    *status = "name-pointer";
    if (!SafeReadPointer(reinterpret_cast<const void*>(nameSlot),
                         &nameAddress)) return false;
    *status = "name-null";
    if (nameAddress == 0) return false;
    const char* const name = reinterpret_cast<const char*>(nameAddress);
    std::size_t length = 0;
    *status = "name-unreadable";
    for (; length < vehicleName->size(); ++length) {
        if (!ReadableRange(name + length, 1)) return false;
        char character = '\0';
#if defined(_MSC_VER)
        __try {
            character = name[length];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
#else
        character = name[length];
#endif
        if (character == '\0') break;
        (*vehicleName)[length] = character;
    }
    *status = length == 0 ? "name-empty" : "name-too-long";
    if (length == 0 ||
        length > vehicle_countersteer::kMaximumVehicleNameLength) {
        return false;
    }
    *vehicleKey = vehicle_countersteer::HashVehicleName(
        std::string_view(vehicleName->data(), length));
    *status = *vehicleKey != 0 ? "resolved" : "name-invalid";
    return *vehicleKey != 0;
}

int ReadCurrentForwardGear() {
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t globalAddress = 0;
    std::uintptr_t vehicle = 0;
    std::uintptr_t drivetrainAddress = 0;
    std::uintptr_t drivetrain = 0;
    std::uintptr_t gearAddress = 0;
    std::uint32_t rawGear = 0;
    if (!AddAddress(base, kHudVehicleGlobalRva, &globalAddress) ||
        !SafeReadPointer(reinterpret_cast<const void*>(globalAddress),
                         &vehicle) ||
        vehicle == 0 ||
        !AddAddress(vehicle, kHudVehicleDrivetrainOffset,
                    &drivetrainAddress) ||
        !SafeReadPointer(reinterpret_cast<const void*>(drivetrainAddress),
                         &drivetrain) ||
        drivetrain == 0 ||
        !AddAddress(drivetrain, kDrivetrainRawGearOffset, &gearAddress) ||
        !SafeReadU32(reinterpret_cast<const void*>(gearAddress), &rawGear) ||
        rawGear < 2u || rawGear > 8u) {
        return 0;
    }
    return static_cast<int>(rawGear - 1u);
}

bool SafeReadByte(const void* address, std::uint8_t* value) {
    if (value == nullptr || !ReadableRange(address, sizeof(*value))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        *value = *reinterpret_cast<const std::uint8_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *value = *reinterpret_cast<const std::uint8_t*>(address);
#endif
    return true;
}

bool SafeReadFloat(const void* address, float* value) {
    if (value == nullptr || !ReadableRange(address, sizeof(*value))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        *value = *reinterpret_cast<const float*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *value = *reinterpret_cast<const float*>(address);
#endif
    return std::isfinite(*value);
}

void ScanVehicleCollectionChain(std::uintptr_t collection,
                                VehicleSample* sample,
                                bool transmission) {
    if (collection == 0 || sample == nullptr) {
        return;
    }
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
    const char** const stop = transmission
                                  ? &sample->transmissionChainStop
                                  : &sample->collectionChainStop;
    *stop = "depth-limit";
#endif

    std::uintptr_t visited[kMaximumAttributeParentDepth + 1]{};
    std::uint32_t visitedCount = 0;
    bool transmissionCounterFound = false;
    std::uintptr_t current = collection;
    for (std::uint32_t depth = 0;
         depth <= kMaximumAttributeParentDepth && current != 0;
         ++depth) {
        bool alreadyVisited = false;
        for (std::uint32_t index = 0; index < visitedCount; ++index) {
            if (visited[index] == current) {
                alreadyVisited = true;
                break;
            }
        }
        if (alreadyVisited) {
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
            *stop = "cycle";
#endif
            break;
        }
        visited[visitedCount++] = current;

        std::uintptr_t keyAddress = 0;
        std::uint32_t key = 0;
        if (!AddAddress(current, kAttributeCollectionKeyOffset,
                        &keyAddress) ||
            !SafeReadU32(reinterpret_cast<const void*>(keyAddress), &key)) {
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
            *stop = "unreadable-key";
#endif
            break;
        }
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
        if (transmission) {
            sample->transmissionChainKeys[
                sample->transmissionChainLength++] = key;
        } else {
            sample->collectionChainKeys[sample->collectionChainLength++] = key;
        }
#endif
        if (depth == 0) {
            if (transmission) {
                sample->transmissionCollectionKey = key;
            } else {
                sample->directVehicleCollectionKey = key;
            }
        }
        if (!transmission &&
            sample->rearSteeringVisualCollectionKey == 0 &&
            key == sample->vehicleKey) {
            // Add-on vehicles place their real vehicle-name Collection at
            // different parent depths. Keep this visual ownership key
            // independent of performance-part nodes.
            sample->rearSteeringVisualCollectionKey = key;
        }
        if ((transmission ? !transmissionCounterFound
                          : sample->countersteerCollectionKey == 0) &&
            g_vehicleCountersteerMultipliers.Find(key) != nullptr) {
            sample->countersteerCollectionKey = key;
            sample->countersteerCollectionDepth = depth;
            transmissionCounterFound = transmission;
        }
        std::uintptr_t parentAddress = 0;
        std::uintptr_t parent = 0;
        if (!AddAddress(current, kAttributeCollectionParentOffset,
                        &parentAddress) ||
            !SafeReadPointer(reinterpret_cast<const void*>(parentAddress),
                             &parent)) {
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
            *stop = "unreadable-parent";
#endif
            break;
        }
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
        if (parent == 0) {
            *stop = "root";
        }
#endif
        current = parent;
    }
}

bool ReadVehicleCollectionIdentity(std::uintptr_t pvehicle,
                                   VehicleSample* sample) {
    if (pvehicle == 0 || sample == nullptr) {
        return false;
    }
    std::uintptr_t collectionAddress = 0;
    std::uintptr_t collection = 0;
    if (AddAddress(pvehicle, kPVehicleAttributeCollectionOffset,
                   &collectionAddress) &&
        SafeReadPointer(reinterpret_cast<const void*>(collectionAddress),
                        &collection)) {
        ScanVehicleCollectionChain(collection, sample, false);
    }

    std::uintptr_t transmissionSlot = 0;
    std::uintptr_t transmission = 0;
    std::uintptr_t transmissionVtable = 0;
    std::uintptr_t component = 0;
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
    sample->transmissionStatus = "unreadable-pointer";
#endif
    if (AddAddress(pvehicle, kPVehicleTransmissionOffset,
                   &transmissionSlot) &&
        SafeReadPointer(reinterpret_cast<const void*>(transmissionSlot),
                        &transmission) &&
        transmission != 0) {
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
        sample->transmissionInterface = transmission;
        sample->transmissionStatus = "unreadable-vtable";
#endif
        if (SafeReadPointer(reinterpret_cast<const void*>(transmission),
                            &transmissionVtable)) {
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
            sample->transmissionVtable = transmissionVtable;
            sample->transmissionStatus = "unknown-vtable";
#endif
            const std::uintptr_t base =
                g_imageBase.load(std::memory_order_acquire);
            if (base != 0 &&
                transmissionVtable == base + kLiveTransmissionVtableRva &&
                transmission >= kLiveTransmissionInterfaceOffset) {
                const std::uintptr_t liveComponent =
                    transmission - kLiveTransmissionInterfaceOffset;
                std::uintptr_t driveMethod = 0;
                const std::uintptr_t driveSlot = transmissionVtable +
                    kTransmissionDriveTorqueVtableSlot * sizeof(void*);
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
                sample->transmissionStatus = "drive-method-mismatch";
#endif
                if (SafeReadPointer(reinterpret_cast<const void*>(driveSlot),
                                    &driveMethod) &&
                    (driveMethod == base + kTransmissionDriveTorqueRva ||
                     (g_driveTorqueHookInstalled.load(
                          std::memory_order_acquire) &&
                      driveMethod == reinterpret_cast<std::uintptr_t>(
                                         &DriftDriveTorqueDetour)))) {
                    std::uintptr_t liveCollection = 0;
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
                    sample->transmissionStatus = "unreadable-collection";
#endif
                    if (SafeReadPointer(reinterpret_cast<const void*>(
                                            liveComponent +
                                            kLiveTransmissionCollectionOffset),
                                        &liveCollection) &&
                        liveCollection != 0) {
                        sample->transmission = transmission;
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
                        sample->transmissionStatus = "validated";
#endif
                        ScanVehicleCollectionChain(liveCollection, sample,
                                                   true);
                    }
                }
            } else if (base != 0 &&
                transmissionVtable ==
                    base + kTransmissionInterfaceVtableRva &&
                transmission >= kComponentTransmissionInterfaceOffset) {
                component = transmission -
                            kComponentTransmissionInterfaceOffset;
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
                sample->transmissionStatus = "suspension-mismatch";
#endif
                if (sample->suspension != 0 &&
                    component + kComponentSuspensionOffset ==
                        sample->suspension) {
                    std::uintptr_t transmissionCollectionAddress = 0;
                    std::uintptr_t transmissionCollection = 0;
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
                    sample->transmissionStatus = "unreadable-collection";
#endif
                    if (AddAddress(component,
                                   kComponentTransmissionCollectionOffset,
                                   &transmissionCollectionAddress) &&
                        SafeReadPointer(
                            reinterpret_cast<const void*>(
                                transmissionCollectionAddress),
                            &transmissionCollection) &&
                        transmissionCollection != 0) {
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
                        sample->transmissionStatus = "validated";
#endif
                        ScanVehicleCollectionChain(transmissionCollection,
                                                   sample, true);
                    }
                }
            }
        }
    }

    sample->vehicleCollectionKey =
        sample->countersteerCollectionKey != 0
            ? sample->countersteerCollectionKey
            : sample->directVehicleCollectionKey;
    return sample->directVehicleCollectionKey != 0 ||
           sample->transmissionCollectionKey != 0;
}

bool SafeReadVec3(std::uintptr_t address, nfsmw_drift::Vec3* value) {
    return value != nullptr &&
           SafeReadFloat(reinterpret_cast<const void*>(address), &value->x) &&
           SafeReadFloat(reinterpret_cast<const void*>(address + 4), &value->y) &&
           SafeReadFloat(reinterpret_cast<const void*>(address + 8), &value->z);
}

VehicleReadFailure ReadSimablePlayerContract(
    std::uintptr_t pvehicle,
    std::uintptr_t* reportedPlayer,
    bool* isPlayer,
    bool* isOwnedByPlayer) {
    if (reportedPlayer == nullptr || isPlayer == nullptr ||
        isOwnedByPlayer == nullptr) {
        return VehicleReadFailure::SimablePlayerCall;
    }
    *reportedPlayer = 0;
    *isPlayer = false;
    *isOwnedByPlayer = false;

    std::uintptr_t simable = 0;
    if (!AddAddress(pvehicle, kPVehicleSimableOffset, &simable)) {
        return VehicleReadFailure::SimableAddress;
    }
    std::uintptr_t vtable = 0;
    if (!SafeReadPointer(reinterpret_cast<const void*>(simable), &vtable)) {
        return VehicleReadFailure::SimableVtableRead;
    }
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t expectedVtable = 0;
    if (!AddAddress(base, kPVehicleSimableVtableRva, &expectedVtable)) {
        return VehicleReadFailure::SimableVtableContract;
    }
    if (!ReadOnlyImageRange(
            reinterpret_cast<const void*>(expectedVtable),
            (kSimableIsOwnedByPlayerVtableSlot + 1) *
                sizeof(std::uintptr_t))) {
        return VehicleReadFailure::SimableVtableContract;
    }
    if (vtable != expectedVtable) {
        return VehicleReadFailure::SimableVtableMismatch;
    }

    std::uintptr_t getPlayerAddress = 0;
    std::uintptr_t isPlayerAddress = 0;
    std::uintptr_t isOwnedAddress = 0;
    if (!AddAddress(vtable,
                    kSimableGetPlayerVtableSlot * sizeof(std::uintptr_t),
                    &getPlayerAddress) ||
        !AddAddress(vtable,
                    kSimableIsPlayerVtableSlot * sizeof(std::uintptr_t),
                    &isPlayerAddress) ||
        !AddAddress(vtable,
                    kSimableIsOwnedByPlayerVtableSlot *
                        sizeof(std::uintptr_t),
                    &isOwnedAddress)) {
        return VehicleReadFailure::SimableMethodAddress;
    }

    std::uintptr_t getPlayerTarget = 0;
    std::uintptr_t isPlayerTarget = 0;
    std::uintptr_t isOwnedTarget = 0;
    if (!SafeReadPointer(reinterpret_cast<const void*>(getPlayerAddress),
                         &getPlayerTarget) ||
        !SafeReadPointer(reinterpret_cast<const void*>(isPlayerAddress),
                         &isPlayerTarget) ||
        !SafeReadPointer(reinterpret_cast<const void*>(isOwnedAddress),
                         &isOwnedTarget)) {
        return VehicleReadFailure::SimableMethodRead;
    }
    if (!ExecutableImageAddress(
            reinterpret_cast<const void*>(getPlayerTarget)) ||
        !ExecutableImageAddress(
            reinterpret_cast<const void*>(isPlayerTarget)) ||
        !ExecutableImageAddress(
            reinterpret_cast<const void*>(isOwnedTarget))) {
        return VehicleReadFailure::SimableMethodContract;
    }
    std::uintptr_t expectedGetPlayerTarget = 0;
    std::uintptr_t expectedIsPlayerTarget = 0;
    std::uintptr_t expectedIsOwnedTarget = 0;
    if (!AddAddress(base, kSimableGetPlayerRva,
                    &expectedGetPlayerTarget) ||
        !AddAddress(base, kSimableIsPlayerRva,
                    &expectedIsPlayerTarget) ||
        !AddAddress(base, kSimableIsOwnedByPlayerRva,
                    &expectedIsOwnedTarget) ||
        getPlayerTarget != expectedGetPlayerTarget ||
        isPlayerTarget != expectedIsPlayerTarget ||
        isOwnedTarget != expectedIsOwnedTarget) {
        return VehicleReadFailure::SimableMethodMismatch;
    }
    if (!SafeImageBytesEqual(getPlayerTarget,
                             kSimableGetPlayerSignature,
                             sizeof(kSimableGetPlayerSignature)) ||
        !SafeImageBytesEqual(isPlayerTarget,
                             kSimableIsPlayerSignature,
                             sizeof(kSimableIsPlayerSignature)) ||
        !SafeImageBytesEqual(
            isOwnedTarget, kSimableIsOwnedByPlayerSignature,
            sizeof(kSimableIsOwnedByPlayerSignature))) {
        return VehicleReadFailure::SimableMethodMismatch;
    }

    bool callSucceeded = false;
#if defined(_MSC_VER)
    __try {
        *reportedPlayer = reinterpret_cast<std::uintptr_t>(
            reinterpret_cast<SimableGetPlayerFn>(getPlayerTarget)(
                reinterpret_cast<void*>(simable)));
        *isPlayer = reinterpret_cast<SimablePredicateFn>(isPlayerTarget)(
            reinterpret_cast<void*>(simable));
        *isOwnedByPlayer =
            reinterpret_cast<SimablePredicateFn>(isOwnedTarget)(
                reinterpret_cast<void*>(simable));
        callSucceeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        callSucceeded = false;
    }
#else
    *reportedPlayer = reinterpret_cast<std::uintptr_t>(
        reinterpret_cast<SimableGetPlayerFn>(getPlayerTarget)(
            reinterpret_cast<void*>(simable)));
    *isPlayer = reinterpret_cast<SimablePredicateFn>(isPlayerTarget)(
        reinterpret_cast<void*>(simable));
    *isOwnedByPlayer = reinterpret_cast<SimablePredicateFn>(isOwnedTarget)(
        reinterpret_cast<void*>(simable));
    callSucceeded = true;
#endif
    if (!callSucceeded) {
        *reportedPlayer = 0;
        *isPlayer = false;
        *isOwnedByPlayer = false;
        return VehicleReadFailure::SimablePlayerCall;
    }
    return VehicleReadFailure::None;
}

VehicleReadFailure ReadPlayerSimableContract(
    std::uintptr_t player,
    std::uintptr_t* reportedSimable) {
    if (player == 0 || reportedSimable == nullptr) {
        return VehicleReadFailure::PlayerSimableCall;
    }
    *reportedSimable = 0;

    std::uintptr_t vtable = 0;
    if (!SafeReadPointer(reinterpret_cast<const void*>(player), &vtable)) {
        return VehicleReadFailure::PlayerVtableRead;
    }
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t expectedVtable = 0;
    if (!AddAddress(base, kPlayerVtableRva, &expectedVtable) ||
        !ReadOnlyImageRange(
            reinterpret_cast<const void*>(expectedVtable),
            (kPlayerGetSimableVtableSlot + 1) *
                sizeof(std::uintptr_t))) {
        return VehicleReadFailure::PlayerVtableContract;
    }
    if (vtable != expectedVtable) {
        return VehicleReadFailure::PlayerVtableMismatch;
    }

    std::uintptr_t methodAddress = 0;
    if (!AddAddress(vtable,
                    kPlayerGetSimableVtableSlot * sizeof(std::uintptr_t),
                    &methodAddress)) {
        return VehicleReadFailure::PlayerMethodAddress;
    }
    std::uintptr_t methodTarget = 0;
    if (!SafeReadPointer(reinterpret_cast<const void*>(methodAddress),
                         &methodTarget)) {
        return VehicleReadFailure::PlayerMethodRead;
    }
    if (!ExecutableImageAddress(
            reinterpret_cast<const void*>(methodTarget))) {
        return VehicleReadFailure::PlayerMethodContract;
    }
    std::uintptr_t expectedMethodTarget = 0;
    if (!AddAddress(base, kPlayerGetSimableRva,
                    &expectedMethodTarget) ||
        methodTarget != expectedMethodTarget ||
        !SafeImageBytesEqual(methodTarget,
                             kPlayerGetSimableSignature,
                             sizeof(kPlayerGetSimableSignature))) {
        return VehicleReadFailure::PlayerMethodMismatch;
    }

    bool callSucceeded = false;
#if defined(_MSC_VER)
    __try {
        *reportedSimable = reinterpret_cast<std::uintptr_t>(
            reinterpret_cast<PlayerGetSimableFn>(methodTarget)(
                reinterpret_cast<void*>(player)));
        callSucceeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        callSucceeded = false;
    }
#else
    *reportedSimable = reinterpret_cast<std::uintptr_t>(
        reinterpret_cast<PlayerGetSimableFn>(methodTarget)(
            reinterpret_cast<void*>(player)));
    callSucceeded = true;
#endif
    if (!callSucceeded) {
        *reportedSimable = 0;
        return VehicleReadFailure::PlayerSimableCall;
    }
    return VehicleReadFailure::None;
}

bool SameIdentity(const Identity& left, const Identity& right) {
    return left.pvehicle == right.pvehicle &&
           left.player == right.player &&
           left.rigidBody == right.rigidBody &&
           left.holder == right.holder && left.inner == right.inner;
}

void ApplyVehicleCountersteerConfigLocked(std::uint32_t collectionKey,
                                          std::uint32_t identityKey) {
    if (g_controller == nullptr) {
        return;
    }
    const vehicle_countersteer::Entry* const entry =
        g_vehicleCountersteerMultipliers.Find(collectionKey);
    const float multiplier = entry != nullptr ? entry->multiplier : 1.0f;
    nfsmw_drift::AssistConfig effective = g_baseControllerConfig;
    const float requestedAngle =
        g_baseControllerConfig.smartCountersteerAngleRad * multiplier;
    effective.smartCountersteerAngleRad = nfsmw_drift::Clamp(
        requestedAngle, 0.0f, effective.maximumSteerAngleRad);
    g_controller->setConfig(effective);
    g_currentVehicleCollectionKey = identityKey;

    char message[320]{};
    std::snprintf(
        message, sizeof(message),
        "countersteer vehicle profile identity_key=0x%08X collection_key=0x%08X name=%s multiplier=%.6f base_deg=%.3f effective_deg=%.3f",
        identityKey,
        collectionKey,
        entry != nullptr ? entry->name.data() : "unlisted/default",
        static_cast<double>(multiplier),
        static_cast<double>(
            g_baseControllerConfig.smartCountersteerAngleRad *
            (180.0f / nfsmw_drift::kPi)),
        static_cast<double>(
            effective.smartCountersteerAngleRad *
            (180.0f / nfsmw_drift::kPi)));
    Log(message);
}

float VectorLength(nfsmw_drift::Vec3 value) {
    return std::sqrt(nfsmw_drift::Dot(value, value));
}

bool FiniteBoundedVector(nfsmw_drift::Vec3 value, float maximum) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && VectorLength(value) <= maximum;
}

bool ValidBasis(const nfsmw_drift::Basis& body) {
    const float rightLength = VectorLength(body.right);
    const float upLength = VectorLength(body.up);
    const float forwardLength = VectorLength(body.forward);
    if (!std::isfinite(rightLength) || !std::isfinite(upLength) ||
        !std::isfinite(forwardLength) || rightLength < 0.85f ||
        rightLength > 1.15f || upLength < 0.85f || upLength > 1.15f ||
        forwardLength < 0.85f || forwardLength > 1.15f) {
        return false;
    }
    const nfsmw_drift::Vec3 right = body.right / rightLength;
    const nfsmw_drift::Vec3 up = body.up / upLength;
    const nfsmw_drift::Vec3 forward = body.forward / forwardLength;
    return std::fabs(nfsmw_drift::Dot(right, up)) <= 0.15f &&
           std::fabs(nfsmw_drift::Dot(right, forward)) <= 0.15f &&
           std::fabs(nfsmw_drift::Dot(up, forward)) <= 0.15f &&
           nfsmw_drift::Dot(nfsmw_drift::Cross(right, up), forward) >= 0.75f;
}

nfsmw_drift::Basis OrthonormalBody(const nfsmw_drift::Basis& supplied) {
    nfsmw_drift::Vec3 forward = nfsmw_drift::NormalizeOr(
        supplied.forward, {0.0f, 0.0f, 1.0f});
    nfsmw_drift::Vec3 up =
        supplied.up - forward * nfsmw_drift::Dot(supplied.up, forward);
    up = nfsmw_drift::NormalizeOr(up, {0.0f, 1.0f, 0.0f});
    const nfsmw_drift::Vec3 right = nfsmw_drift::NormalizeOr(
        nfsmw_drift::Cross(up, forward), supplied.right);
    up = nfsmw_drift::NormalizeOr(
        nfsmw_drift::Cross(forward, right), up);
    return {right, up, forward};
}

float BodyOffsetRad(const VehicleSample& sample) {
    const nfsmw_drift::Basis body = OrthonormalBody(sample.body);
    const float longitudinal = nfsmw_drift::Dot(
        sample.linearVelocity, body.forward);
    const float lateral = nfsmw_drift::Dot(
        sample.linearVelocity, body.right);
    return nfsmw_drift::Clamp(
        -std::atan2(lateral, std::fabs(longitudinal)),
        -0.5f * nfsmw_drift::kPi, 0.5f * nfsmw_drift::kPi);
}

float BodyYawRateRadS(const VehicleSample& sample) {
    const nfsmw_drift::Basis body = OrthonormalBody(sample.body);
    return nfsmw_drift::Dot(sample.angularVelocity, body.up);
}

bool ReadSuspensionMethod(std::uintptr_t suspension,
                          std::size_t slot,
                          std::uintptr_t* target) {
    if (target == nullptr || suspension == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(suspension),
                       sizeof(std::uintptr_t))) {
        return false;
    }
    std::uintptr_t vtable = 0;
    std::uintptr_t slotAddress = 0;
    return SafeReadPointer(reinterpret_cast<const void*>(suspension),
                           &vtable) &&
           vtable != 0 &&
           AddAddress(vtable, slot * sizeof(std::uintptr_t),
                      &slotAddress) &&
           SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                           target) &&
           ExecutableImageAddress(reinterpret_cast<const void*>(*target));
}

#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
void LogRearWheelSteerConfigDiagnostic() {
    char message[420]{};
    std::snprintf(message, sizeof(message),
                  "v1.1.9 config counter_entries=%u rws_entries=%u base_counter_deg=%.3f max_front_deg=%.3f portable_default_hash=%08X expected_default_hash=EEC2271A",
                  static_cast<unsigned>(g_vehicleCountersteerMultipliers.count),
                  static_cast<unsigned>(g_rearWheelSteeringVehicles.count),
                  static_cast<double>(g_baseControllerConfig.smartCountersteerAngleRad *
                                      180.0f / nfsmw_drift::kPi),
                  static_cast<double>(g_baseControllerConfig.maximumSteerAngleRad *
                                      180.0f / nfsmw_drift::kPi),
                  vehicle_countersteer::HashVehicleName("default"));
    RearWheelSteerDiagnosticWrite(message);
    for (std::size_t index = 0;
         index < g_vehicleCountersteerMultipliers.count; ++index) {
        const auto& entry = g_vehicleCountersteerMultipliers.entries[index];
        std::snprintf(message, sizeof(message),
                      "v1.1.9 counter_config name=%s key=%08X multiplier=%.6f requested_deg=%.3f",
                      entry.name.data(), entry.collectionKey,
                      static_cast<double>(entry.multiplier),
                      static_cast<double>(std::min(
                          g_baseControllerConfig.smartCountersteerAngleRad *
                              entry.multiplier,
                          g_baseControllerConfig.maximumSteerAngleRad) *
                          180.0f / nfsmw_drift::kPi));
        RearWheelSteerDiagnosticWrite(message);
    }
    for (std::size_t index = 0; index < g_rearWheelSteeringVehicles.count;
         ++index) {
        const auto& entry = g_rearWheelSteeringVehicles.entries[index];
        std::snprintf(message, sizeof(message),
                      "v1.1.9 rws_config name=%s key=%08X low_deg=%.3f high_deg=%.3f",
                      entry.name.data(), entry.collectionKey,
                      static_cast<double>(entry.lowSpeedAngleDegrees),
                      static_cast<double>(entry.highSpeedAngleDegrees));
        RearWheelSteerDiagnosticWrite(message);
    }
}

void LogRearWheelSteerDiagnostic(
    const VehicleSample& sample,
    float rawSteering,
    const driver_assist::Output& driverOutput,
    const nfsmw_drift::ControlOutput& driftOutput,
    DWORD now,
    bool computed,
    float angleRad) {
    DWORD previous =
        g_rearWheelSteerDiagnosticTick.load(std::memory_order_relaxed);
    if (previous != 0 &&
        static_cast<DWORD>(now - previous) <
            kRearWheelSteerDiagnosticIntervalMs) {
        return;
    }
    if (!g_rearWheelSteerDiagnosticTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }

    std::uintptr_t vtable = 0;
    std::uintptr_t slot22 = 0;
    if (sample.suspension != 0 &&
        SafeReadPointer(reinterpret_cast<const void*>(sample.suspension),
                        &vtable) &&
        vtable != 0) {
        (void)SafeReadPointer(
            reinterpret_cast<const void*>(
                vtable + kSuspensionGetWheelSteerVtableSlot *
                             sizeof(std::uintptr_t)),
            &slot22);
    }
    const rear_wheel_steering::Entry* entry =
        g_rearWheelSteeringVehicles.Find(
            sample.vehicleKey);
    const vehicle_countersteer::Entry* counterEntry =
        g_vehicleCountersteerMultipliers.Find(
            sample.countersteerCollectionKey);
    const float effectiveCounterAngle = g_controller != nullptr
                                            ? g_controller->config().smartCountersteerAngleRad
                                            : 0.0f;
    const std::uint32_t appliedBits =
        g_latestAppliedSteeringBits.load(std::memory_order_acquire);
    float lastAppliedSteering = 0.0f;
    std::memcpy(&lastAppliedSteering, &appliedBits, sizeof(lastAppliedSteering));
    char message[1400]{};
    std::snprintf(
        message, sizeof(message),
        "v1.3.8 vehicle_name=%s vehicle_status=%s vehicle_key=%08X direct=%08X identity=%08X counter=%08X counter_depth=%u visual=%08X match=%s speed_kmh=%.2f raw_steer=%.3f driver=%u drift=%u computed=%u target_deg=%.3f suspension=%p vtable=%p slot22=%p hook_target=%p hook=%u enabled=%u calls=[%llu,%llu,%llu,%llu] other=%llu overrides=%llu",
        sample.vehicleName[0] != '\0' ? sample.vehicleName.data() : "none",
        sample.vehicleNameStatus, sample.vehicleKey,
        sample.directVehicleCollectionKey, sample.vehicleCollectionKey,
        sample.countersteerCollectionKey,
        sample.countersteerCollectionDepth,
        sample.rearSteeringVisualCollectionKey,
        entry != nullptr ? entry->name.data() : "none",
        sample.speedMps * 3.6f, rawSteering,
        driverOutput.modeActive ? 1u : 0u,
        driftOutput.active ? 1u : 0u, computed ? 1u : 0u,
        angleRad * (180.0f / nfsmw_drift::kPi),
        reinterpret_cast<void*>(sample.suspension),
        reinterpret_cast<void*>(vtable), reinterpret_cast<void*>(slot22),
        reinterpret_cast<void*>(
            g_rearWheelSteerHookTarget.load(std::memory_order_acquire)),
        g_rearWheelSteerHookInstalled.load(std::memory_order_acquire) ? 1u : 0u,
        g_rearWheelSteerOverrideEnabled.load(std::memory_order_acquire) ? 1u : 0u,
        static_cast<unsigned long long>(
            g_rearWheelSteerCalls[0].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_rearWheelSteerCalls[1].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_rearWheelSteerCalls[2].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_rearWheelSteerCalls[3].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_rearWheelSteerOtherCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_rearWheelSteerOverrides.load(std::memory_order_relaxed)));
    RearWheelSteerDiagnosticWrite(message);

    const handling_probe::RearWheelSteeringStats writerStats =
        handling_probe::GetRearWheelSteeringStats();
    std::snprintf(
        message, sizeof(message),
        "v1.3.8 true_rws visual_hook=%u physics_hook=%u visual_writes=%llu visual_rejected_ai=%llu tire_basis_writes=%llu tire_basis_restores=%llu restore_failures=%llu install_stage=%u entry_matches=%u call_matches=%u decoded_rva=%08X create_status=%d enable_status=%d fake_body_yaw=0",
        writerStats.visualHookArmed ? 1u : 0u,
        writerStats.physicsHookArmed ? 1u : 0u,
        static_cast<unsigned long long>(writerStats.visualWrites),
        static_cast<unsigned long long>(writerStats.visualRejected),
        static_cast<unsigned long long>(writerStats.physicsWrites),
        static_cast<unsigned long long>(writerStats.physicsRestores),
        static_cast<unsigned long long>(writerStats.physicsRestoreFailures),
        writerStats.physicsInstallStage,
        writerStats.solverEntryMatches,
        writerStats.solverCallContextMatches,
        writerStats.decodedSolverRva,
        writerStats.physicsHookCreateStatus,
        writerStats.physicsHookEnableStatus);
    RearWheelSteerDiagnosticWrite(message);

#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
    const float radToDeg = 180.0f / nfsmw_drift::kPi;
    int probeUsed = std::snprintf(
        message, sizeof(message),
        "v1.1.9 wheel_probe original_deg=[%.2f,%.2f,%.2f,%.2f] returned_rear_deg=[%.2f,%.2f] callsites=",
        static_cast<double>(BitsToFloat(g_wheelSteerOriginalBits[0].load()) * radToDeg),
        static_cast<double>(BitsToFloat(g_wheelSteerOriginalBits[1].load()) * radToDeg),
        static_cast<double>(BitsToFloat(g_wheelSteerOriginalBits[2].load()) * radToDeg),
        static_cast<double>(BitsToFloat(g_wheelSteerOriginalBits[3].load()) * radToDeg),
        static_cast<double>(BitsToFloat(g_rearWheelSteerReturnBits[0].load()) * radToDeg),
        static_cast<double>(BitsToFloat(g_rearWheelSteerReturnBits[1].load()) * radToDeg));
    for (std::size_t index = 0;
         index < g_rearWheelSteerCallsites.size() &&
         probeUsed > 0 && static_cast<std::size_t>(probeUsed) < sizeof(message);
         ++index) {
        const auto caller = g_rearWheelSteerCallsites[index].load();
        if (caller == 0) continue;
        const int added = std::snprintf(
            message + probeUsed, sizeof(message) - probeUsed, "%s%p:%llu",
            index == 0 ? "" : ",", reinterpret_cast<void*>(caller),
            static_cast<unsigned long long>(
                g_rearWheelSteerCallsiteCounts[index].load()));
        if (added <= 0) break;
        probeUsed += added;
    }
    RearWheelSteerDiagnosticWrite(message);
#endif

    std::uintptr_t renderable = 0;
    std::uintptr_t renderVtable = 0;
    std::array<std::uintptr_t, 8> renderMethods{};
    if (sample.identity.pvehicle != 0 &&
        SafeReadPointer(reinterpret_cast<const void*>(
                            sample.identity.pvehicle +
                            kPVehicleRenderableOffset),
                        &renderable) && renderable != 0 &&
        SafeReadPointer(reinterpret_cast<const void*>(renderable),
                        &renderVtable) && renderVtable != 0) {
        for (std::size_t index = 0; index < renderMethods.size(); ++index) {
            (void)SafeReadPointer(reinterpret_cast<const void*>(
                                      renderVtable + index * sizeof(void*)),
                                  &renderMethods[index]);
        }
    }
    std::snprintf(message, sizeof(message),
                  "v1.1.9 render_probe component=%p vtable=%p methods=[%p,%p,%p,%p,%p,%p,%p,%p]",
                  reinterpret_cast<void*>(renderable),
                  reinterpret_cast<void*>(renderVtable),
                  reinterpret_cast<void*>(renderMethods[0]),
                  reinterpret_cast<void*>(renderMethods[1]),
                  reinterpret_cast<void*>(renderMethods[2]),
                  reinterpret_cast<void*>(renderMethods[3]),
                  reinterpret_cast<void*>(renderMethods[4]),
                  reinterpret_cast<void*>(renderMethods[5]),
                  reinterpret_cast<void*>(renderMethods[6]),
                  reinterpret_cast<void*>(renderMethods[7]));
    RearWheelSteerDiagnosticWrite(message);

    std::size_t used = 0;
    int written = std::snprintf(message, sizeof(message),
                                "v1.1.9 chain pvehicle=%p len=%u stop=%s keys=",
                                reinterpret_cast<void*>(sample.identity.pvehicle),
                                sample.collectionChainLength,
                                sample.collectionChainStop);
    if (written > 0) {
        used = std::min<std::size_t>(static_cast<std::size_t>(written),
                                     sizeof(message) - 1);
        for (std::uint32_t index = 0;
             index < sample.collectionChainLength && used < sizeof(message) - 1;
             ++index) {
            written = std::snprintf(message + used, sizeof(message) - used,
                                    "%s%08X", index == 0 ? "" : ">",
                                    sample.collectionChainKeys[index]);
            if (written <= 0) {
                break;
            }
            used += std::min<std::size_t>(
                static_cast<std::size_t>(written), sizeof(message) - used - 1);
        }
    }
    RearWheelSteerDiagnosticWrite(message);

    std::uint32_t transmissionCounterKey = 0;
    std::uint32_t transmissionCounterDepth = 0;
    for (std::uint32_t depth = 0; depth < sample.transmissionChainLength;
         ++depth) {
        const std::uint32_t key = sample.transmissionChainKeys[depth];
        if (transmissionCounterKey == 0 &&
            g_vehicleCountersteerMultipliers.Find(key) != nullptr) {
            transmissionCounterKey = key;
            transmissionCounterDepth = depth;
        }
    }
    written = std::snprintf(
        message, sizeof(message),
        "v1.3.8 tranny status=%s interface=%p vtable=%p collection=%08X len=%u stop=%s counter_candidate=%08X counter_depth=%u keys=",
        sample.transmissionStatus,
        reinterpret_cast<void*>(sample.transmissionInterface),
        reinterpret_cast<void*>(sample.transmissionVtable),
        sample.transmissionCollectionKey, sample.transmissionChainLength,
        sample.transmissionChainStop, transmissionCounterKey,
        transmissionCounterDepth);
    if (written > 0) {
        used = std::min<std::size_t>(static_cast<std::size_t>(written),
                                     sizeof(message) - 1);
        for (std::uint32_t index = 0;
             index < sample.transmissionChainLength && used < sizeof(message) - 1;
             ++index) {
            written = std::snprintf(message + used, sizeof(message) - used,
                                    "%s%08X", index == 0 ? "" : ">",
                                    sample.transmissionChainKeys[index]);
            if (written <= 0) break;
            used += std::min<std::size_t>(
                static_cast<std::size_t>(written), sizeof(message) - used - 1);
        }
    }
    RearWheelSteerDiagnosticWrite(message);

    std::snprintf(message, sizeof(message),
                  "v1.1.9 power installed=%u enabled=%u transmission=%p last_self=%p calls=%llu player=%llu active=%llu forward=%llu positive=%llu boosts=%llu last_gear=%u last_base=%.3f multiplier=%.3f",
                  g_driveTorqueHookInstalled.load(std::memory_order_acquire)
                      ? 1u : 0u,
                  g_driveTorqueOverrideEnabled.load(std::memory_order_acquire)
                      ? 1u : 0u,
                  reinterpret_cast<void*>(sample.transmission),
                  reinterpret_cast<void*>(g_driveTorqueLastSelf.load(
                      std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_driveTorqueCalls.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_driveTorquePlayerCalls.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_driveTorqueActiveCalls.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_driveTorqueForwardCalls.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_driveTorquePositiveCalls.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_driveTorqueBoosts.load(std::memory_order_relaxed)),
                  g_driveTorqueLastGear.load(std::memory_order_relaxed),
                  static_cast<double>(BitsToFloat(g_driveTorqueLastBaseBits.load(
                      std::memory_order_relaxed))),
                  static_cast<double>(drift_drive_torque::kDriftMultiplier));
    RearWheelSteerDiagnosticWrite(message);

    std::snprintf(message, sizeof(message),
                  "v1.1.9 counter_match=%s multiplier=%.6f effective_deg=%.3f phase=%u active=%u auto=%u force=%u auto_target_deg=%.3f steering_cmd=%.3f last_applied=%.3f applied_serial=%llu",
                  counterEntry != nullptr ? counterEntry->name.data() : "default",
                  static_cast<double>(counterEntry != nullptr
                                          ? counterEntry->multiplier : 1.0f),
                  static_cast<double>(effectiveCounterAngle *
                                      180.0f / nfsmw_drift::kPi),
                  static_cast<unsigned>(driftOutput.phase),
                  driftOutput.active ? 1u : 0u,
                  driftOutput.smartCountersteerActive ? 1u : 0u,
                  driftOutput.forceCountersteer ? 1u : 0u,
                  static_cast<double>(driftOutput.smartCountersteerCommand *
                                      g_baseControllerConfig.maximumSteerAngleRad *
                                      180.0f / nfsmw_drift::kPi),
                  static_cast<double>(driftOutput.steeringCommand),
                  static_cast<double>(lastAppliedSteering),
                  static_cast<unsigned long long>(
                      g_latestSteeringPhysicsSerial.load(
                          std::memory_order_acquire)));
    RearWheelSteerDiagnosticWrite(message);
}
#endif

bool ReadFourWheelSlip(const VehicleSample& sample,
                       bool* fourWheelSlip) {
    if (fourWheelSlip == nullptr) return false;
    *fourWheelSlip = false;
    std::uintptr_t countTarget = 0;
    std::uintptr_t slipTarget = 0;
    std::uintptr_t slipAngleTarget = 0;
    if (!ReadSuspensionMethod(sample.suspension,
                              kSuspensionGetNumWheelsVtableSlot,
                              &countTarget) ||
        !ReadSuspensionMethod(sample.suspension,
                              kSuspensionGetWheelSlipVtableSlot,
                              &slipTarget) ||
        !ReadSuspensionMethod(sample.suspension,
                              kSuspensionGetWheelSlipAngleVtableSlot,
                              &slipAngleTarget)) {
        return false;
    }
    std::uint32_t count = 0;
    bool called = false;
#if defined(_MSC_VER)
    __try {
#endif
        count = reinterpret_cast<SuspensionGetCountFn>(countTarget)(
            reinterpret_cast<void*>(sample.suspension));
        called = true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        called = false;
    }
#endif
    if (!called || count != 4u) return false;

    bool allSlipping = true;
    for (std::uint32_t index = 0; index < 4u; ++index) {
        float slip = 0.0f;
        float slipAngle = 0.0f;
        called = false;
#if defined(_MSC_VER)
        __try {
#endif
            slip = reinterpret_cast<SuspensionGetWheelFloatFn>(slipTarget)(
                reinterpret_cast<void*>(sample.suspension), index);
            slipAngle =
                reinterpret_cast<SuspensionGetWheelFloatFn>(slipAngleTarget)(
                    reinterpret_cast<void*>(sample.suspension), index);
            called = true;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            called = false;
        }
#endif
        if (!called || !std::isfinite(slip) ||
            !std::isfinite(slipAngle) ||
            std::fabs(slip) > kMaximumWheelSlipMagnitude ||
            std::fabs(slipAngle) > nfsmw_drift::kPi) {
            return false;
        }
        allSlipping = allSlipping &&
            (std::fabs(slip) >= kFourWheelSlipRatioThreshold ||
             std::fabs(slipAngle) >= kFourWheelSlipAngleThresholdRad);
    }
    *fourWheelSlip = allSlipping;
    return true;
}

void UpdateDriverAssistLocked(const VehicleSample& sample,
                              const nfsmw_drift::ControlOutput& driftOutput,
                              float rawSteering,
                              float brake,
                              float throttle,
                              float handbrake,
                              float dt,
                              int gear,
                              std::uint64_t physicsSerial,
                              DWORD now) {
    driver_assist::Input input{};
    input.dt = dt;
    input.driftActive = driftOutput.active;
    input.speedMps = sample.speedMps;
    input.longitudinalSpeedMps = nfsmw_drift::Dot(
        sample.linearVelocity, sample.body.forward);
    input.steering = rawSteering;
    input.brake = nfsmw_drift::Clamp(brake, 0.0f, 1.0f);
    input.throttle = nfsmw_drift::Clamp(throttle, 0.0f, 1.0f);
    input.handbrake = nfsmw_drift::Clamp(handbrake, 0.0f, 1.0f);
    input.bodyOffsetRad = BodyOffsetRad(sample);
    input.yawRateRadS = BodyYawRateRadS(sample);
    bool fourWheelSlip = false;
    input.fourWheelSlip =
        ReadFourWheelSlip(sample, &fourWheelSlip) && fourWheelSlip;
    input.gear = gear;
    g_cachedDriverAssistOutput = g_driverAssistController.Update(input);
    g_cachedDriverAssistSerial = physicsSerial;
    const driver_assist::Output& output = g_cachedDriverAssistOutput;
    const bool hudVisible = driver_assist_hud::ShouldShow(
        input.longitudinalSpeedMps, output.lcWorking);
    driver_assist_hud::Publish(
        hudVisible, output.absWorking, output.escWorking,
        output.escRecoveryWorking, output.tcsWorking, output.lcWorking, now);
    LogDriverAssistTelemetry(output, gear, input.brake, input.throttle,
                             input.bodyOffsetRad, input.yawRateRadS);
}

bool ManualYawMotionEligible(const VehicleSample& sample,
                             const nfsmw_drift::AssistConfig& config) {
    const nfsmw_drift::Basis body = OrthonormalBody(sample.body);
    const float longitudinal = nfsmw_drift::Dot(
        sample.linearVelocity, body.forward);
    const float longitudinalForGate = config.allowReverse
                                          ? std::fabs(longitudinal)
                                          : longitudinal;
    return std::isfinite(longitudinalForGate) &&
           sample.speedMps >= config.minSpeedMps &&
           longitudinalForGate >= config.minLongitudinalSpeedMps &&
           sample.groundedWheels >= config.minGroundedWheels;
}

bool IsManualYawMode(nfsmw_drift::YawResponseMode mode) {
    return mode ==
               nfsmw_drift::YawResponseMode::ManualSameDirectionAssist ||
           mode ==
               nfsmw_drift::YawResponseMode::ManualOppositeRecovery;
}

bool HasExpectedVtable(std::uintptr_t object,
                       std::uintptr_t expectedVtable) {
    std::uintptr_t vtable = 0;
    return object != 0 &&
           ImageRange(reinterpret_cast<const void*>(expectedVtable),
                      sizeof(std::uintptr_t)) &&
           SafeReadPointer(reinterpret_cast<const void*>(object), &vtable) &&
           vtable == expectedVtable;
}

bool ReadPlayerVehicle(VehicleSample* sample,
                       VehicleReadDiagnostics* diagnostics) {
    VehicleReadDiagnostics localDiagnostics{};
    VehicleReadDiagnostics* const details =
        diagnostics != nullptr ? diagnostics : &localDiagnostics;
    *details = VehicleReadDiagnostics{};
    if (sample == nullptr) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::InvalidDestination);
        return false;
    }
    *sample = VehicleSample{};

    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t table = 0;
    if (!AddAddress(base, kPVehicleInstancesRva, &table)) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::InstanceTableAddress);
        return false;
    }
    if (!ImageRange(reinterpret_cast<const void*>(table),
                    kPVehicleInstanceLimit * kPVehicleInstanceStride)) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::InstanceTableRange);
        return false;
    }

    std::uintptr_t expectedPVehicleVtable = 0;
    if (!AddAddress(base, kPVehiclePrimaryVtableRva,
                    &expectedPVehicleVtable) ||
        !ReadOnlyImageRange(
            reinterpret_cast<const void*>(expectedPVehicleVtable),
            6 * sizeof(std::uintptr_t))) {
        NoteVehicleReadFailure(
            details, VehicleReadFailure::PVehicleVtableContract);
        return false;
    }

    player_vehicle_selection::Selection selection{};
    bool complete = true;
    for (std::size_t index = 0; index < kPVehicleInstanceLimit; ++index) {
        std::uintptr_t slot = 0;
        std::uintptr_t pvehicle = 0;
        if (!AddAddress(table, index * kPVehicleInstanceStride, &slot)) {
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(
                details, VehicleReadFailure::InstanceSlotAddress, index);
            continue;
        }
        if (!SafeReadPointer(reinterpret_cast<const void*>(slot), &pvehicle)) {
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(
                details, VehicleReadFailure::InstanceSlotRead, index);
            continue;
        }

        std::uintptr_t enabledAddress = 0;
        std::uint8_t enabled = 0;
        if (!AddAddress(slot, kPVehicleInstanceEnabledOffset,
                        &enabledAddress)) {
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(
                details, VehicleReadFailure::InstanceEnabledAddress, index);
            continue;
        }
        if (!SafeReadByte(reinterpret_cast<const void*>(enabledAddress),
                          &enabled)) {
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(
                details, VehicleReadFailure::InstanceEnabledRead, index);
            continue;
        }
        const player_vehicle_selection::InstanceSlotState slotState =
            player_vehicle_selection::ClassifyInstanceSlot(pvehicle,
                                                            enabled);
        if (slotState ==
            player_vehicle_selection::InstanceSlotState::Invalid) {
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(
                details,
                enabled > 1
                    ? VehicleReadFailure::InstanceEnabledValue
                    : VehicleReadFailure::InstanceEnabledWithoutVehicle,
                index);
            continue;
        }
        if (slotState ==
            player_vehicle_selection::InstanceSlotState::Empty) {
            continue;
        }

        ++details->populatedSlots;
        if (slotState ==
            player_vehicle_selection::InstanceSlotState::Disabled) {
            ++details->disabledSlots;
            continue;
        }
        ++details->enabledSlots;
        if (!ReadableRange(reinterpret_cast<const void*>(pvehicle),
                           kPVehicleReadableSize)) {
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(details, VehicleReadFailure::PVehicleRange,
                                   index);
            continue;
        }

        std::uintptr_t primaryVtable = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(pvehicle),
                             &primaryVtable)) {
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(
                details, VehicleReadFailure::PVehicleVtableRead, index);
            continue;
        }
        if (primaryVtable != expectedPVehicleVtable) {
            // A reused pool slot can remain non-null after it no longer holds
            // a PVehicle. Exact mismatch proves it is not this profile's
            // player object, so skip it without poisoning the full scan.
            continue;
        }

        std::uintptr_t playerAddress = 0;
        std::uintptr_t player = 0;
        if (!AddAddress(pvehicle, kPVehiclePlayerOffset, &playerAddress)) {
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(
                details, VehicleReadFailure::PVehiclePlayerAddress, index);
            continue;
        }
        if (!SafeReadPointer(reinterpret_cast<const void*>(playerAddress),
                             &player)) {
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(
                details, VehicleReadFailure::PVehiclePlayerRead, index);
            continue;
        }
        if (player == 0) {
            continue;
        }

        player_vehicle_selection::CandidateObservation candidate{};
        candidate.pvehicle = pvehicle;
        candidate.memberPlayer = player;

        std::uintptr_t dirtyAddress = 0;
        std::uintptr_t objectTypeAddress = 0;
        std::uintptr_t rigidBodyAddress = 0;
        if (!AddAddress(pvehicle, kPVehicleDirtyOffset, &dirtyAddress) ||
            !AddAddress(pvehicle, kPVehicleObjectTypeOffset,
                        &objectTypeAddress) ||
            !AddAddress(pvehicle, kPVehicleRigidBodyOffset,
                        &rigidBodyAddress)) {
            selection.Observe(candidate);
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(
                details, VehicleReadFailure::PVehicleStateAddress, index);
            continue;
        }

        std::uint8_t dirty = 0;
        std::uint32_t objectType = 0;
        std::uintptr_t rigidBody = 0;
        if (!SafeReadByte(reinterpret_cast<const void*>(dirtyAddress),
                          &dirty) ||
            !SafeReadU32(reinterpret_cast<const void*>(objectTypeAddress),
                         &objectType) ||
            !SafeReadPointer(reinterpret_cast<const void*>(rigidBodyAddress),
                             &rigidBody)) {
            selection.Observe(candidate);
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(
                details, VehicleReadFailure::PVehicleStateRead, index);
            continue;
        }

        // Match the SDK's ValidatePVehicle preconditions before invoking the
        // read-only ISimable ownership queries. Old vehicle-pool entries can
        // retain mPlayer after they have become dirty or lost their body.
        candidate.baseStateValid =
            dirty == 0 && objectType != 0 && rigidBody != 0;
        if (!candidate.baseStateValid) {
            selection.Observe(candidate);
            continue;
        }

        if (!AddAddress(pvehicle, kPVehicleSimableOffset,
                        &candidate.simable)) {
            selection.Observe(candidate);
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(details,
                                   VehicleReadFailure::SimableAddress,
                                   index);
            continue;
        }

        VehicleReadFailure playerContractFailure =
            ReadSimablePlayerContract(
                pvehicle, &candidate.reportedPlayer, &candidate.isPlayer,
                &candidate.isOwnedByPlayer);
        if (playerContractFailure == VehicleReadFailure::None &&
            candidate.reportedPlayer != 0 &&
            candidate.reportedPlayer == candidate.memberPlayer &&
            candidate.isPlayer && candidate.isOwnedByPlayer) {
            playerContractFailure = ReadPlayerSimableContract(
                candidate.reportedPlayer,
                &candidate.reportedPlayerSimable);
        }
        selection.Observe(candidate);
        if (playerContractFailure != VehicleReadFailure::None) {
            std::uintptr_t playerCurrentSimable = 0;
            const VehicleReadFailure reverseContractFailure =
                ReadPlayerSimableContract(candidate.memberPlayer,
                                           &playerCurrentSimable);
            if (reverseContractFailure == VehicleReadFailure::None &&
                player_vehicle_selection::ReverseOwnershipProvesRetired(
                    candidate.simable, playerCurrentSimable)) {
                // A validated Player now points at another non-null ISimable,
                // so this half-retired pool entry cannot be the current car.
                ++details->provenRetiredSlots;
                continue;
            }
            // A base-valid PVehicle with a mismatched ISimable subobject can
            // be in a constructor/destructor or pool-reuse transition. Do not
            // let another stale object become the unique writable candidate
            // until every populated PVehicle has a coherent player contract.
            complete = false;
            ++details->scanErrors;
            NoteVehicleReadFailure(details, playerContractFailure, index);
            continue;
        }
    }

    details->nonNullPlayers = selection.nonNullPlayers;
    details->qualifiedPlayers = selection.qualifiedPlayers;
    details->duplicateQualifiedPlayers =
        selection.duplicateQualifiedPlayers;
    details->candidatePvehicle = selection.pvehicle;
    details->candidatePlayer = selection.player;
    details->candidateSimable = selection.simable;
    details->alternatePvehicle = selection.alternatePvehicle;
    details->alternatePlayer = selection.alternatePlayer;
    details->alternateSimable = selection.alternateSimable;

    // Empty slots are valid holes. Any unreadable non-empty slot still makes
    // the fixed-size scan incomplete, so writes remain strictly fail-closed.
    if (!complete) {
        return false;
    }
    if (!selection.HasUniquePlayer()) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::PlayerCandidateCount);
        return false;
    }
    sample->identity.pvehicle = selection.pvehicle;
    sample->identity.player = selection.player;

    const std::uintptr_t pvehicle = sample->identity.pvehicle;
    // The INI names real vehicles. Performance-part Collection names are
    // unrelated and are used only to qualify the visual wheel object.
    (void)ReadCurrentVehicleNameKey(
        &sample->vehicleKey, &sample->vehicleName,
        &sample->vehicleNameStatus);
    std::uintptr_t suspensionAddress = 0;
    if (AddAddress(pvehicle, kPVehicleSuspensionOffset,
                   &suspensionAddress)) {
        std::uintptr_t suspension = 0;
        if (SafeReadPointer(
                reinterpret_cast<const void*>(suspensionAddress),
                &suspension) &&
            suspension != 0 &&
            ReadableRange(reinterpret_cast<const void*>(suspension),
                          sizeof(std::uintptr_t))) {
            sample->suspension = suspension;
        }
    }
    // Per-car tuning is optional. A missing or stripped Collection only
    // selects the default multiplier and must not invalidate a vehicle that
    // already passed the ownership and physics contracts above.
    (void)ReadVehicleCollectionIdentity(pvehicle, sample);
    std::uintptr_t rigidBodyAddress = 0;
    if (!AddAddress(pvehicle, kPVehicleRigidBodyOffset, &rigidBodyAddress)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::RigidBodyAddress);
        return false;
    }
    if (!SafeReadPointer(reinterpret_cast<const void*>(rigidBodyAddress),
                         &sample->identity.rigidBody)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::RigidBodyRead);
        return false;
    }
    if (sample->identity.rigidBody == 0) {
        NoteVehicleReadFailure(details, VehicleReadFailure::RigidBodyNull);
        return false;
    }
    if (!ReadableRange(
            reinterpret_cast<const void*>(sample->identity.rigidBody),
            sizeof(std::uintptr_t))) {
        NoteVehicleReadFailure(details, VehicleReadFailure::RigidBodyRange);
        return false;
    }

    std::uintptr_t expectedRigidBodyVtable = 0;
    if (!AddAddress(base, kRigidBodyVtableRva,
                    &expectedRigidBodyVtable) ||
        !ImageRange(reinterpret_cast<const void*>(expectedRigidBodyVtable),
                    sizeof(std::uintptr_t))) {
        NoteVehicleReadFailure(
            details, VehicleReadFailure::RigidBodyVtableContract);
        return false;
    }
    std::uintptr_t rigidBodyVtable = 0;
    if (!SafeReadPointer(
            reinterpret_cast<const void*>(sample->identity.rigidBody),
            &rigidBodyVtable)) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::RigidBodyVtableRead);
        return false;
    }
    if (rigidBodyVtable != expectedRigidBodyVtable) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::RigidBodyVtableMismatch);
        return false;
    }

    std::uintptr_t holderAddress = 0;
    if (!AddAddress(sample->identity.rigidBody, kRigidBodyHolderOffset,
                    &holderAddress)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::HolderAddress);
        return false;
    }
    if (!SafeReadPointer(reinterpret_cast<const void*>(holderAddress),
                         &sample->identity.holder)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::HolderRead);
        return false;
    }
    if (sample->identity.holder == 0) {
        NoteVehicleReadFailure(details, VehicleReadFailure::HolderNull);
        return false;
    }
    if (!SafeReadPointer(reinterpret_cast<const void*>(sample->identity.holder),
                         &sample->identity.inner)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::InnerRead);
        return false;
    }
    if (sample->identity.inner == 0) {
        NoteVehicleReadFailure(details, VehicleReadFailure::InnerNull);
        return false;
    }

    std::uintptr_t linearAddress = 0;
    std::uintptr_t angularAddress = 0;
    if (!AddAddress(sample->identity.inner, kLinearVelocityOffset,
                    &linearAddress) ||
        !AddAddress(sample->identity.inner, kAngularVelocityOffset,
                    &angularAddress)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::VelocityAddress);
        return false;
    }
    if (!SafeReadVec3(linearAddress, &sample->linearVelocity)) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::LinearVelocityRead);
        return false;
    }
    if (!SafeReadVec3(angularAddress, &sample->angularVelocity)) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::AngularVelocityRead);
        return false;
    }
    if (!FiniteBoundedVector(sample->linearVelocity,
                             kMaximumVectorMagnitude)) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::LinearVelocityBounds);
        return false;
    }
    if (!FiniteBoundedVector(sample->angularVelocity,
                             kMaximumAngularMagnitude)) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::AngularVelocityBounds);
        return false;
    }

    std::uintptr_t rightAddress = 0;
    std::uintptr_t upAddress = 0;
    std::uintptr_t forwardAddress = 0;
    if (!AddAddress(sample->identity.inner, kRightVectorOffset,
                    &rightAddress) ||
        !AddAddress(sample->identity.inner, kUpVectorOffset, &upAddress) ||
        !AddAddress(sample->identity.inner, kForwardVectorOffset,
                    &forwardAddress)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::BasisAddress);
        return false;
    }
    if (!SafeReadVec3(rightAddress, &sample->body.right) ||
        !SafeReadVec3(upAddress, &sample->body.up) ||
        !SafeReadVec3(forwardAddress, &sample->body.forward)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::BasisRead);
        return false;
    }
    if (!ValidBasis(sample->body)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::BasisInvalid);
        return false;
    }

    std::uintptr_t speedAddress = 0;
    if (!AddAddress(pvehicle, kPVehicleSpeedOffset, &speedAddress)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::SpeedAddress);
        return false;
    }
    if (!SafeReadFloat(reinterpret_cast<const void*>(speedAddress),
                       &sample->speedMps)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::SpeedRead);
        return false;
    }
    if (sample->speedMps < 0.0f ||
        sample->speedMps > kMaximumVehicleSpeedMps) {
        NoteVehicleReadFailure(details, VehicleReadFailure::SpeedBounds);
        return false;
    }

    std::uintptr_t groundedAddress = 0;
    if (!AddAddress(pvehicle, kPVehicleGroundedOffset, &groundedAddress)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::GroundedAddress);
        return false;
    }
    if (!SafeReadU32(reinterpret_cast<const void*>(groundedAddress),
                     &sample->groundedWheels)) {
        NoteVehicleReadFailure(details, VehicleReadFailure::GroundedRead);
        return false;
    }
    if (sample->groundedWheels > 4) {
        NoteVehicleReadFailure(details, VehicleReadFailure::GroundedBounds);
        return false;
    }

    const float measuredSpeed = VectorLength(sample->linearVelocity);
    if (!std::isfinite(measuredSpeed) ||
        measuredSpeed > kMaximumVehicleSpeedMps) {
        NoteVehicleReadFailure(details,
                               VehicleReadFailure::MeasuredSpeedBounds);
        return false;
    }
    if (std::fabs(measuredSpeed - sample->speedMps) > 20.0f) {
        NoteVehicleReadFailure(details, VehicleReadFailure::SpeedMismatch);
        return false;
    }
    return true;
}

bool RuntimeSetterContractMatches(const Identity& identity,
                                  SetAngularVelocityFn setter) {
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    if (setter == nullptr || identity.rigidBody == 0 ||
        !HasExpectedVtable(identity.rigidBody,
                           base + kRigidBodyVtableRva)) {
        return false;
    }
    std::uintptr_t slotAddress = 0;
    std::uintptr_t slotTarget = 0;
    std::uintptr_t holderAddress = 0;
    std::uintptr_t holder = 0;
    std::uintptr_t inner = 0;
    return AddAddress(base + kRigidBodyVtableRva,
                      kSetAngularVelocityVtableSlot *
                          sizeof(std::uintptr_t),
                      &slotAddress) &&
           ImageRange(reinterpret_cast<const void*>(slotAddress),
                      sizeof(std::uintptr_t)) &&
           SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                           &slotTarget) &&
           slotTarget == reinterpret_cast<std::uintptr_t>(setter) &&
           AddAddress(identity.rigidBody, kRigidBodyHolderOffset,
                      &holderAddress) &&
           SafeReadPointer(reinterpret_cast<const void*>(holderAddress),
                           &holder) &&
           holder == identity.holder && holder != 0 &&
           SafeReadPointer(reinterpret_cast<const void*>(holder), &inner) &&
           inner == identity.inner && inner != 0;
}

bool SafeSetAngularVelocity(const VehicleSample& sample,
                            nfsmw_drift::Vec3 nextAngularVelocity) {
    const SetAngularVelocityFn setter =
        g_setAngularVelocity.load(std::memory_order_acquire);
    if (!RuntimeSetterContractMatches(sample.identity, setter) ||
        !FiniteBoundedVector(nextAngularVelocity,
                             kMaximumAngularMagnitude)) {
        return false;
    }

    bool called = false;
#if defined(_MSC_VER)
    __try {
        setter(reinterpret_cast<void*>(sample.identity.rigidBody),
               &nextAngularVelocity);
        called = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        called = false;
    }
#else
    setter(reinterpret_cast<void*>(sample.identity.rigidBody),
           &nextAngularVelocity);
    called = true;
#endif
    if (!called) {
        return false;
    }

    std::uintptr_t angularAddress = 0;
    nfsmw_drift::Vec3 readBack{};
    return AddAddress(sample.identity.inner, kAngularVelocityOffset,
                      &angularAddress) &&
           SafeReadVec3(angularAddress, &readBack) &&
           std::fabs(readBack.x - nextAngularVelocity.x) <=
               kYawReadBackToleranceRadS &&
           std::fabs(readBack.y - nextAngularVelocity.y) <=
               kYawReadBackToleranceRadS &&
           std::fabs(readBack.z - nextAngularVelocity.z) <=
               kYawReadBackToleranceRadS;
}

bool SafeSetLinearVelocity(const VehicleSample& sample,
                           nfsmw_drift::Vec3 nextLinearVelocity) {
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    const std::uintptr_t expectedVtable = base + kRigidBodyVtableRva;
    if (!HasExpectedVtable(sample.identity.rigidBody, expectedVtable) ||
        !FiniteBoundedVector(nextLinearVelocity,
                             kMaximumVectorMagnitude)) {
        return false;
    }
    std::uintptr_t slotAddress = 0;
    std::uintptr_t target = 0;
    if (!AddAddress(expectedVtable,
                    kSetLinearVelocityVtableSlot *
                        sizeof(std::uintptr_t),
                    &slotAddress) ||
        !SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                         &target) ||
        !ExecutableImageAddress(reinterpret_cast<const void*>(target))) {
        return false;
    }
    bool called = false;
#if defined(_MSC_VER)
    __try {
#endif
        reinterpret_cast<SetLinearVelocityFn>(target)(
            reinterpret_cast<void*>(sample.identity.rigidBody),
            &nextLinearVelocity);
        called = true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        called = false;
    }
#endif
    std::uintptr_t velocityAddress = 0;
    nfsmw_drift::Vec3 readBack{};
    return called &&
           AddAddress(sample.identity.inner, kLinearVelocityOffset,
                      &velocityAddress) &&
           SafeReadVec3(velocityAddress, &readBack) &&
           VectorLength(readBack - nextLinearVelocity) <=
               kLaunchControlVelocityToleranceMps;
}

bool SafeStopFrontWheelRotation(const VehicleSample& sample) {
    std::uintptr_t countTarget = 0;
    std::uintptr_t getTarget = 0;
    std::uintptr_t setTarget = 0;
    if (!ReadSuspensionMethod(sample.suspension,
                              kSuspensionGetNumWheelsVtableSlot,
                              &countTarget) ||
        !ReadSuspensionMethod(
            sample.suspension,
            kSuspensionGetWheelAngularVelocityVtableSlot,
            &getTarget) ||
        !ReadSuspensionMethod(
            sample.suspension,
            kSuspensionSetWheelAngularVelocityVtableSlot,
            &setTarget)) {
        return false;
    }
    bool called = false;
    std::uint32_t count = 0;
#if defined(_MSC_VER)
    __try {
#endif
        count = reinterpret_cast<SuspensionGetCountFn>(countTarget)(
            reinterpret_cast<void*>(sample.suspension));
        called = true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        called = false;
    }
#endif
    if (!called || count != 4u) return false;

    for (std::int32_t index = 0; index < 2; ++index) {
        float before = 0.0f;
        float after = 0.0f;
        called = false;
#if defined(_MSC_VER)
        __try {
#endif
            before = reinterpret_cast<SuspensionGetWheelFloatFn>(getTarget)(
                reinterpret_cast<void*>(sample.suspension),
                static_cast<std::uint32_t>(index));
            reinterpret_cast<SuspensionSetWheelFloatFn>(setTarget)(
                reinterpret_cast<void*>(sample.suspension), index, 0.0f);
            after = reinterpret_cast<SuspensionGetWheelFloatFn>(getTarget)(
                reinterpret_cast<void*>(sample.suspension),
                static_cast<std::uint32_t>(index));
            called = true;
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            called = false;
        }
#endif
        if (!called || !std::isfinite(before) || !std::isfinite(after) ||
            std::fabs(before) > kMaximumWheelAngularVelocity ||
            std::fabs(after) > kLaunchControlWheelToleranceRadS) {
            return false;
        }
    }
    return true;
}

bool RegularPollPrecondition(void* self) {
    if (self == nullptr || !ReadableRange(self, 0x1840)) {
        return false;
    }
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t vtable = 0;
    std::uintptr_t pollTarget = 0;
    std::uintptr_t slotAddress = 0;
    std::uintptr_t boundDevice = 0;
    std::uint8_t rebindFlag = 0;
    std::uintptr_t globalAddress = 0;
    std::uintptr_t globalValue = 0;
    return SafeReadPointer(self, &vtable) &&
           vtable == base + kRawInputVtableRva &&
           AddAddress(vtable,
                      kInputPollVtableSlot * sizeof(std::uintptr_t),
                      &slotAddress) &&
           SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                           &pollTarget) &&
           pollTarget == g_inputPollTarget.load(std::memory_order_acquire) &&
           SafeReadPointer(reinterpret_cast<const unsigned char*>(self) + 0x24,
                           &boundDevice) &&
           boundDevice == 0 &&
           SafeReadByte(reinterpret_cast<const unsigned char*>(self) + 0x183C,
                        &rebindFlag) &&
           rebindFlag == 0 &&
           AddAddress(base, 0x00582BECu, &globalAddress) &&
           SafeReadPointer(reinterpret_cast<const void*>(globalAddress),
                           &globalValue) &&
           globalValue != 0;
}

bool SnapshotRawInput(void* self,
                      float* left,
                      float* right,
                      float* handbrake,
                      float* throttle,
                      float* brake,
                      std::uintptr_t* mirrorOut) {
    if (self == nullptr || left == nullptr || right == nullptr ||
        handbrake == nullptr || throttle == nullptr || brake == nullptr ||
        mirrorOut == nullptr ||
        !ReadableRange(self, 0x28)) {
        return false;
    }
    const std::uintptr_t expectedTarget =
        g_inputPollTarget.load(std::memory_order_acquire);
    std::uintptr_t vtable = 0;
    std::uintptr_t slotAddress = 0;
    std::uintptr_t target = 0;
    std::uintptr_t mirrorAddress = 0;
    std::uintptr_t mirror = 0;
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    if (!SafeReadPointer(self, &vtable) ||
        vtable != base + kRawInputVtableRva ||
        !AddAddress(vtable, kInputPollVtableSlot * sizeof(std::uintptr_t),
                    &slotAddress) ||
        !ReadableRange(reinterpret_cast<const void*>(slotAddress),
                       sizeof(std::uintptr_t)) ||
        !SafeReadPointer(reinterpret_cast<const void*>(slotAddress), &target) ||
        target != expectedTarget ||
        !AddAddress(reinterpret_cast<std::uintptr_t>(self), 0x20,
                    &mirrorAddress) ||
        !SafeReadPointer(reinterpret_cast<const void*>(mirrorAddress), &mirror) ||
        mirror == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(mirror), 0x400) ||
        !SafeReadFloat(reinterpret_cast<const void*>(mirror), throttle) ||
        !SafeReadFloat(reinterpret_cast<const void*>(mirror + sizeof(float)),
                       brake) ||
        !SafeReadFloat(reinterpret_cast<const void*>(mirror + 2 * sizeof(float)),
                       left) ||
        !SafeReadFloat(reinterpret_cast<const void*>(mirror + 3 * sizeof(float)),
                       right) ||
        !SafeReadFloat(reinterpret_cast<const void*>(mirror + 4 * sizeof(float)),
                       handbrake)) {
        return false;
    }
    if (*throttle < -0.01f || *throttle > 1.01f || *brake < -0.01f ||
        *brake > 1.01f || *left < -0.01f || *left > 1.01f ||
        *right < -0.01f || *right > 1.01f || *handbrake < -0.01f ||
        *handbrake > 1.01f) {
        return false;
    }
    *mirrorOut = mirror;
    return true;
}

struct SteeringRows {
    float left;
    float right;
};

constexpr SteeringRows SteeringRowsForCommand(float command) {
    return {command < 0.0f ? -command : 0.0f,
            command > 0.0f ? command : 0.0f};
}

constexpr bool AutomaticOverrideAllowed(bool currentManualInput,
                                         bool manualInputObserved,
                                         std::uint8_t manualDirectionMask,
                                         int driftSide,
                                         bool smartCountersteerActive,
                                         bool forceCountersteer) {
    if (!smartCountersteerActive || !forceCountersteer) {
        return false;
    }
    if (driftSide != -1 && driftSide != 1) {
        return false;
    }
    const std::uint8_t knownMask =
        manualDirectionMask &
        (nfsmw_drift::kNegativeSteeringDirectionObserved |
         nfsmw_drift::kPositiveSteeringDirectionObserved);
    return !currentManualInput && !manualInputObserved && knownMask == 0;
}

static_assert(SteeringRowsForCommand(-0.5f).left == 0.5f &&
                  SteeringRowsForCommand(-0.5f).right == 0.0f,
              "negative steering must write only the left row");
static_assert(SteeringRowsForCommand(0.5f).left == 0.0f &&
                  SteeringRowsForCommand(0.5f).right == 0.5f,
              "positive steering must write only the right row");
static_assert(AutomaticOverrideAllowed(false, false, 0, 1, true, true) &&
                  AutomaticOverrideAllowed(false, false, 0, -1, true, true) &&
                  !AutomaticOverrideAllowed(false, false, 0, 0, true, true),
              "neutral input must allow automatic control only for a valid drift side");
static_assert(!AutomaticOverrideAllowed(
                  true, true,
                  nfsmw_drift::kNegativeSteeringDirectionObserved,
                  1, true, true) &&
                  !AutomaticOverrideAllowed(
                      true, true,
                      nfsmw_drift::kPositiveSteeringDirectionObserved,
                      1, true, true) &&
                  !AutomaticOverrideAllowed(
                      false, true,
                      nfsmw_drift::kPositiveSteeringDirectionObserved,
                      -1, true, true) &&
                  !AutomaticOverrideAllowed(
                      false, false,
                      nfsmw_drift::kNegativeSteeringDirectionObserved |
                          nfsmw_drift::kPositiveSteeringDirectionObserved,
                      1, true, true) &&
                  !AutomaticOverrideAllowed(true, false, 0, 0, true, true) &&
                  !AutomaticOverrideAllowed(true, false, 0, 1, false, true) &&
                  !AutomaticOverrideAllowed(true, false, 0, 1, true, false),
              "any valid player steering event must release automatic ownership");

void PrepareDriftSteeringWriteLocked(
    const nfsmw_drift::ControlOutput& output,
    float rawSteering,
    float sessionStartSteering,
    bool currentManualInput,
    bool manualInputObserved,
    std::uint8_t manualDirectionMask,
    float dt,
    std::uint64_t physicsSerial,
    bool* writeSteering,
    float* steeringCommand,
    bool* automaticAllowed,
    float* effectiveTarget) {
    if (writeSteering == nullptr || steeringCommand == nullptr ||
        automaticAllowed == nullptr || effectiveTarget == nullptr) {
        return;
    }
    *writeSteering = false;
    *steeringCommand = rawSteering;
    *automaticAllowed = false;
    *effectiveTarget = rawSteering;

    // This keeps the 0.9.5 command sequence. The small ownership flag below
    // only records the automatic-takeover edge; it is not a second veto
    // latch. Per-serial limiter accounting still makes repeated input polls
    // idempotent, while re-engagement can re-anchor directly to the target.
    if (!output.active) {
        g_steeringResponseLimiter.Reset();
        g_steeringAutomaticOwnership = false;
        return;
    }
    if (!g_steeringResponseLimiter.valid()) {
        g_steeringResponseLimiter.BeginSession(sessionStartSteering,
                                                physicsSerial);
    }

    const std::uint8_t knownManualDirections =
        manualDirectionMask &
        (nfsmw_drift::kNegativeSteeringDirectionObserved |
         nfsmw_drift::kPositiveSteeringDirectionObserved);
    const bool automaticAllowedNow = AutomaticOverrideAllowed(
        currentManualInput, manualInputObserved, knownManualDirections,
        output.driftSide, output.smartCountersteerActive,
        output.forceCountersteer);

    // Before the 15-degree acquisition threshold, neutral input is genuine
    // pass-through.  Retire any previous automatic/manual trajectory so the
    // next automatic session starts from the command the game actually saw.
    if (steering_response::WaitingNeutralPassthrough(
            currentManualInput,
            manualInputObserved || knownManualDirections != 0,
            output.steeringAssistMode ==
                nfsmw_drift::SteeringAssistMode::WaitingForDrift)) {
        g_steeringResponseLimiter.Reset();
        g_steeringAutomaticOwnership = false;
        *steeringCommand = rawSteering;
        *effectiveTarget = rawSteering;
        return;
    }

    // A neutral sample during the controller's post-takeover reengagement
    // window is not a new steering target.  Keep the command that is already
    // visible in the game so automatic countersteer can resume from that exact
    // value after 0.20 seconds.  Without this branch the legacy manual path
    // slews toward raw neutral, producing the observed countersteer -> zero ->
    // second-burst sequence.  Waiting-angle frames keep the normal 0.9.5
    // passthrough/transition behavior because they are not reengagement.
    const bool holdManualTakeoverNeutral =
        steering_response::HoldNeutralAfterManualTakeover(
            currentManualInput,
            manualInputObserved || knownManualDirections != 0,
            output.smartCountersteerActive && !automaticAllowedNow,
            output.manualSteeringOverride ||
                output.steeringAssistMode ==
                    nfsmw_drift::SteeringAssistMode::ManualOverride,
            output.steeringAssistMode ==
                nfsmw_drift::SteeringAssistMode::ReengageDelay);
    if (holdManualTakeoverNeutral) {
        g_steeringAutomaticOwnership = false;
        *steeringCommand = g_steeringResponseLimiter.appliedCommand();
        *effectiveTarget = *steeringCommand;
        *writeSteering = true;
        return;
    }
    if (automaticAllowedNow) {
        if (!g_steeringAutomaticOwnership) {
            // This is the 0.9.5 takeover edge: discard the stale manual
            // transition and anchor the visible wheel at the automatic
            // countersteer target.  Subsequent frames use the bounded rate
            // path, preserving the requested steering-rate limit.
            g_steeringResponseLimiter.SynchronizeAutomaticTakeover(
                output.steeringCommand, physicsSerial);
            *steeringCommand = g_steeringResponseLimiter.appliedCommand();
        } else {
            *steeringCommand = g_steeringResponseLimiter.Advance(
                output.steeringCommand, dt,
                steering_response::ScaledMaximumRate(
                    kFullSteeringResponseCalibrationPerSecond),
                physicsSerial);
        }
        g_steeringAutomaticOwnership = true;
        *automaticAllowed = true;
        *effectiveTarget = output.steeringCommand;
    } else {
        g_steeringAutomaticOwnership = false;
        // Manual steering is the restored 0.9.5 fixed-rate path.  A timed
        // transition would make a small remaining countersteer increment take
        // the full compatibility duration and would prevent a pendulum
        // reversal from crossing its observation window.  AdvanceManual is
        // idempotent for repeated polls in one serial and starts from the
        // command visible in the game, while still limiting a full reversal to
        // the calibrated rate.
        *steeringCommand = g_steeringResponseLimiter.AdvanceManual(
            rawSteering, dt, physicsSerial);
    }
    *writeSteering = true;
}

bool SafeWriteSteeringRows(std::uintptr_t mirror,
                           float command,
                           float originalLeft,
                           float originalRight,
                           float originalHandbrake) {
    if (mirror == 0 || !std::isfinite(command) || command < -1.0f ||
        command > 1.0f || !std::isfinite(originalLeft) ||
        !std::isfinite(originalRight) ||
        !std::isfinite(originalHandbrake)) {
        return false;
    }
    std::uintptr_t leftAddress = 0;
    std::uintptr_t rightAddress = 0;
    std::uintptr_t handbrakeAddress = 0;
    if (!AddAddress(mirror, 2 * sizeof(float), &leftAddress) ||
        !AddAddress(mirror, 3 * sizeof(float), &rightAddress) ||
        !AddAddress(mirror, 4 * sizeof(float), &handbrakeAddress)) {
        return false;
    }
    float* const left = reinterpret_cast<float*>(leftAddress);
    float* const right = reinterpret_cast<float*>(rightAddress);
    if (!WritableRange(left, sizeof(*left)) ||
        !WritableRange(right, sizeof(*right))) {
        return false;
    }

    const SteeringRows requested = SteeringRowsForCommand(command);
    bool wrote = false;
#if defined(_MSC_VER)
    __try {
        *left = requested.left;
        *right = requested.right;
        wrote = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wrote = false;
    }
#else
    *left = requested.left;
    *right = requested.right;
    wrote = true;
#endif

    float leftReadBack = 0.0f;
    float rightReadBack = 0.0f;
    float handbrakeReadBack = 0.0f;
    if (wrote && SafeReadFloat(left, &leftReadBack) &&
        SafeReadFloat(right, &rightReadBack) &&
        SafeReadFloat(reinterpret_cast<const void*>(handbrakeAddress),
                      &handbrakeReadBack) &&
        std::fabs(leftReadBack - requested.left) <= 1.0e-5f &&
        std::fabs(rightReadBack - requested.right) <= 1.0e-5f &&
        std::fabs(handbrakeReadBack - originalHandbrake) <= 1.0e-5f) {
        return true;
    }

    // Best-effort rollback avoids leaving a one-sided partial override if a
    // validated write unexpectedly faults or fails read-back.
#if defined(_MSC_VER)
    __try {
        *left = originalLeft;
        *right = originalRight;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    *left = originalLeft;
    *right = originalRight;
#endif
    return false;
}

float BitsToFloat(std::uint32_t bits) {
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits), "float must be 32-bit");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint32_t FloatToBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void DisableRearWheelSteeringOverride() {
    g_rearWheelSteerOverrideEnabled.store(false,
                                           std::memory_order_release);
    g_rearWheelSteerPublishTick.store(0, std::memory_order_release);
    g_rearWheelSteerSuspension.store(0, std::memory_order_release);
    g_rearWheelSteerAngleBits.store(0, std::memory_order_release);
    g_rearWheelSteerSmoothedAngleRad = 0.0f;
    g_rearWheelSteerSlewTick = 0;
    g_rearWheelSteerSlewPvehicle = 0;
    handling_probe::PublishRearWheelSteering(0, 0, 0, 0, 0.0f, false);
}

void DisableDriftDriveTorque() {
    g_driveTorqueOverrideEnabled.store(false, std::memory_order_release);
    g_driveTorquePublishTick.store(0, std::memory_order_release);
    g_driveTorqueTransmission.store(0, std::memory_order_release);
}

float NFSMW_FASTCALL DriftDriveTorqueDetour(void* self,
                                           void* /*unusedEdx*/) {
    const TransmissionGetDriveTorqueFn original =
        g_driveTorqueOriginal.load(std::memory_order_acquire);
    const float base = original != nullptr ? original(self) : 0.0f;
    g_driveTorqueCalls.fetch_add(1, std::memory_order_relaxed);
    g_driveTorqueLastSelf.store(reinterpret_cast<std::uintptr_t>(self),
                                 std::memory_order_relaxed);
    if (reinterpret_cast<std::uintptr_t>(self) !=
        g_driveTorqueTransmission.load(std::memory_order_acquire)) {
        return base;
    }
    g_driveTorquePlayerCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_driveTorqueOverrideEnabled.load(std::memory_order_acquire) ||
        !g_hooksArmed.load(std::memory_order_acquire) ||
        g_permanentFault.load(std::memory_order_acquire)) {
        return base;
    }
    g_driveTorqueActiveCalls.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t rawGear = 0;
    if (!SafeReadU32(reinterpret_cast<const std::uint8_t*>(self) +
                         kTransmissionCurrentGearOffset,
                     &rawGear)) {
        return base;
    }
    g_driveTorqueLastGear.store(rawGear, std::memory_order_relaxed);
    g_driveTorqueLastBaseBits.store(FloatToBits(base),
                                    std::memory_order_relaxed);
    if (rawGear >= drift_drive_torque::kFirstForwardGear &&
        rawGear <= drift_drive_torque::kLastSupportedRawGear) {
        g_driveTorqueForwardCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (std::isfinite(base) && base > 0.0f &&
        base <= drift_drive_torque::kMaximumOriginalTorque) {
        g_driveTorquePositiveCalls.fetch_add(1, std::memory_order_relaxed);
    }
    const DWORD published =
        g_driveTorquePublishTick.load(std::memory_order_acquire);
    if (published == 0 ||
        static_cast<DWORD>(GetTickCount() - published) >
            kPhysicsFreshnessMs) {
        return base;
    }
    const float powered = drift_drive_torque::Apply(base, rawGear, true);
    if (std::isfinite(base) && powered != base) {
        g_driveTorqueBoosts.fetch_add(1, std::memory_order_relaxed);
    }
    return powered;
}

bool EnsureDriftDriveTorqueHookInstalled(const VehicleSample& sample) {
    if (sample.transmission == 0) {
        return false;
    }
    if (g_driveTorqueHookInstalled.load(std::memory_order_acquire)) {
        return true;
    }
    if (g_driveTorqueHookAttempted.exchange(true,
                                            std::memory_order_acq_rel)) {
        return false;
    }
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    const std::uintptr_t vtable = base + kLiveTransmissionVtableRva;
    const std::uintptr_t slotAddress = vtable +
        kTransmissionDriveTorqueVtableSlot * sizeof(void*);
    std::uintptr_t target = 0;
    if (base == 0 ||
        !SafeReadPointer(reinterpret_cast<const void*>(sample.transmission),
                         &target) ||
        target != vtable ||
        !SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                         &target) ||
        target != base + kTransmissionDriveTorqueRva) {
        Log("drift power disabled: transmission vtable or torque slot changed");
        return false;
    }
    DWORD previousProtection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slotAddress), sizeof(void*),
                        PAGE_READWRITE, &previousProtection)) {
        Log("drift power disabled: transmission vtable is not writable");
        return false;
    }
    g_driveTorqueOriginal.store(
        reinterpret_cast<TransmissionGetDriveTorqueFn>(target),
        std::memory_order_release);
    void* const installed = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(slotAddress),
        reinterpret_cast<void*>(&DriftDriveTorqueDetour),
        reinterpret_cast<void*>(target));
    DWORD ignoredProtection = 0;
    const BOOL protectionRestored = VirtualProtect(
        reinterpret_cast<void*>(slotAddress), sizeof(void*),
        previousProtection, &ignoredProtection);
    if (installed != reinterpret_cast<void*>(target)) {
        g_driveTorqueOriginal.store(nullptr, std::memory_order_release);
        Log("drift power disabled: another plugin changed the torque slot");
        return false;
    }
    g_driveTorqueHookInstalled.store(true, std::memory_order_release);
    PinModuleContaining(
        reinterpret_cast<std::uintptr_t>(&DriftDriveTorqueDetour));
    Log(protectionRestored
            ? "drift power installed: player-only GetDriveTorque vtable bridge"
            : "drift power installed: torque vtable protection restore failed");
    return true;
}

void PublishDriftDriveTorqueLocked(const VehicleSample& sample,
                                   const nfsmw_drift::ControlOutput& output,
                                   DWORD now) {
    if (!output.active || !EnsureDriftDriveTorqueHookInstalled(sample)) {
        DisableDriftDriveTorque();
        return;
    }
    g_driveTorqueTransmission.store(sample.transmission,
                                     std::memory_order_release);
    g_driveTorquePublishTick.store(now, std::memory_order_release);
    g_driveTorqueOverrideEnabled.store(true, std::memory_order_release);
}

float NFSMW_FASTCALL RearWheelSteerDetour(void* self,
                                         void* /*unusedEdx*/,
                                         std::uint32_t index) {
    if (index < g_rearWheelSteerCalls.size()) {
        g_rearWheelSteerCalls[index].fetch_add(
            1, std::memory_order_relaxed);
    } else {
        g_rearWheelSteerOtherCalls.fetch_add(
            1, std::memory_order_relaxed);
    }
    const SuspensionGetWheelSteerFn original =
        g_rearWheelSteerOriginal.load(std::memory_order_acquire);
    const float originalAngle =
        original != nullptr ? original(self, index) : 0.0f;
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
    if (index < g_wheelSteerOriginalBits.size() &&
        reinterpret_cast<std::uintptr_t>(self) ==
            g_wheelSteerProbeSuspension.load(std::memory_order_acquire)) {
        g_wheelSteerOriginalBits[index].store(FloatToBits(originalAngle),
                                              std::memory_order_relaxed);
    }
#endif
    if ((index != 2u && index != 3u) ||
        !g_rearWheelSteerOverrideEnabled.load(std::memory_order_acquire)) {
        return originalAngle;
    }

    const DWORD published =
        g_rearWheelSteerPublishTick.load(std::memory_order_acquire);
    if (published == 0 ||
        static_cast<DWORD>(GetTickCount() - published) >
            kPhysicsFreshnessMs ||
        reinterpret_cast<std::uintptr_t>(self) !=
            g_rearWheelSteerSuspension.load(std::memory_order_acquire)) {
        return originalAngle;
    }
    g_rearWheelSteerOverrides.fetch_add(1, std::memory_order_relaxed);
    const float overrideAngle = BitsToFloat(
        g_rearWheelSteerAngleBits.load(std::memory_order_acquire));
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE && defined(_MSC_VER)
    g_rearWheelSteerReturnBits[index - 2u].store(
        FloatToBits(overrideAngle), std::memory_order_relaxed);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    for (std::size_t slot = 0; slot < g_rearWheelSteerCallsites.size();
         ++slot) {
        auto known = g_rearWheelSteerCallsites[slot].load(
            std::memory_order_relaxed);
        if (known == 0 &&
            g_rearWheelSteerCallsites[slot].compare_exchange_strong(
                known, caller, std::memory_order_relaxed)) {
            known = caller;
        }
        if (known == caller) {
            g_rearWheelSteerCallsiteCounts[slot].fetch_add(
                1, std::memory_order_relaxed);
            break;
        }
    }
#endif
    return overrideAngle;
}

bool EnsureRearWheelSteeringHookInstalled(std::uintptr_t suspension) {
    if (g_rearWheelSteerPermanentFault.load(std::memory_order_acquire)) {
        return false;
    }

    std::uintptr_t target = 0;
    if (!ReadSuspensionMethod(suspension,
                              kSuspensionGetWheelSteerVtableSlot,
                              &target)) {
        DisableRearWheelSteeringOverride();
        return false;
    }

    if (g_rearWheelSteerHookInstalled.load(std::memory_order_acquire)) {
        if (target == g_rearWheelSteerHookTarget.load(
                          std::memory_order_acquire)) {
            return true;
        }
        DisableRearWheelSteeringOverride();
        g_rearWheelSteerPermanentFault.store(true,
                                              std::memory_order_release);
        Log("rear-wheel steering disabled: suspension ABI changed after hook installation");
        return false;
    }

    if (g_rearWheelSteerHookAttempted.exchange(
            true, std::memory_order_acq_rel)) {
        return false;
    }

    void* originalRaw = nullptr;
    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(&RearWheelSteerDetour), &originalRaw);
    if (create != MH_OK || originalRaw == nullptr) {
        g_rearWheelSteerPermanentFault.store(true,
                                              std::memory_order_release);
        char message[288]{};
        std::snprintf(
            message, sizeof(message),
            "rear-wheel steering disabled: GetWheelSteer hook creation failed (%s)",
            MH_StatusToString(create));
        Log(message);
        return false;
    }

    g_rearWheelSteerOriginal.store(
        reinterpret_cast<SuspensionGetWheelSteerFn>(originalRaw),
        std::memory_order_release);
    g_rearWheelSteerHookTarget.store(target, std::memory_order_release);
    const MH_STATUS queue =
        MH_QueueEnableHook(reinterpret_cast<void*>(target));
    if (queue != MH_OK) {
        const MH_STATUS cleanup =
            MH_RemoveHook(reinterpret_cast<void*>(target));
        g_rearWheelSteerOriginal.store(nullptr, std::memory_order_release);
        g_rearWheelSteerHookTarget.store(0, std::memory_order_release);
        g_rearWheelSteerPermanentFault.store(true,
                                              std::memory_order_release);
        char message[336]{};
        std::snprintf(
            message, sizeof(message),
            "rear-wheel steering disabled: GetWheelSteer hook queue failed (%s), cleanup=%s",
            MH_StatusToString(queue), MH_StatusToString(cleanup));
        Log(message);
        return false;
    }

    const MH_STATUS apply = MH_ApplyQueued();
    if (apply != MH_OK) {
        const MH_STATUS disable =
            MH_QueueDisableHook(reinterpret_cast<void*>(target));
        const MH_STATUS rollback = MH_ApplyQueued();
        g_rearWheelSteerPermanentFault.store(true,
                                              std::memory_order_release);
        char message[384]{};
        std::snprintf(
            message, sizeof(message),
            "rear-wheel steering disarmed: GetWheelSteer hook enable failed (%s), rollback queue=%s apply=%s",
            MH_StatusToString(apply), MH_StatusToString(disable),
            MH_StatusToString(rollback));
        Log(message);
        return false;
    }

    g_rearWheelSteerHookInstalled.store(true,
                                         std::memory_order_release);
    PinModuleContaining(
        reinterpret_cast<std::uintptr_t>(&RearWheelSteerDetour));
    char message[256]{};
    std::snprintf(
        message, sizeof(message),
        "rear-wheel steering installed GetWheelSteer=%p slot=%llu",
        reinterpret_cast<void*>(target),
        static_cast<unsigned long long>(
            kSuspensionGetWheelSteerVtableSlot));
    Log(message);
    return true;
}

void PublishRearWheelSteeringLocked(
    const VehicleSample& sample,
    float rawSteering,
    const driver_assist::Output& driverOutput,
    const nfsmw_drift::ControlOutput& driftOutput,
    DWORD now) {
#if !NFSMW_ENABLE_REAR_WHEEL_STEERING
    (void)sample;
    (void)rawSteering;
    (void)driverOutput;
    (void)driftOutput;
    (void)now;
    DisableRearWheelSteeringOverride();
    return;
#else
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
    g_wheelSteerProbeSuspension.store(sample.suspension,
                                      std::memory_order_release);
#endif
    float targetAngleRad = 0.0f;
    const bool computed =
        rear_wheel_steering::ComputeOverrideAngleRad(
            g_rearWheelSteeringVehicles, sample.vehicleKey,
            rawSteering, sample.speedMps, driverOutput.modeActive,
            driftOutput.active, targetAngleRad);
    if (!computed) {
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
        LogRearWheelSteerDiagnostic(sample, rawSteering, driverOutput,
                                    driftOutput, now, false, 0.0f);
#endif
        DisableRearWheelSteeringOverride();
        return;
    }

    const float maximumRateRadS =
        rear_wheel_steering::MaximumActuatorRateRadS(sample.speedMps);
    if (g_rearWheelSteerSlewPvehicle != sample.identity.pvehicle ||
        g_rearWheelSteerSlewTick == 0) {
        g_rearWheelSteerSmoothedAngleRad = 0.0f;
        g_rearWheelSteerSlewPvehicle = sample.identity.pvehicle;
        g_rearWheelSteerSlewTick = now;
    }
    const float slewDt = std::min(
        static_cast<float>(static_cast<DWORD>(now - g_rearWheelSteerSlewTick)) /
            1000.0f,
        0.05f);
    g_rearWheelSteerSmoothedAngleRad =
        rear_wheel_steering::SlewAngleRad(
            g_rearWheelSteerSmoothedAngleRad, targetAngleRad, slewDt,
            maximumRateRadS);
    g_rearWheelSteerSlewTick = now;
    const float angleRad = g_rearWheelSteerSmoothedAngleRad;
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
    LogRearWheelSteerDiagnostic(sample, rawSteering, driverOutput,
                                driftOutput, now, true, angleRad);
#endif

    // The legacy GetWheelSteer override stays disabled. Visual and tire-basis
    // writers consume this rate-limited player-only payload directly.
    g_rearWheelSteerSuspension.store(sample.suspension,
                                      std::memory_order_release);
    g_rearWheelSteerAngleBits.store(FloatToBits(angleRad),
                                     std::memory_order_release);
    g_rearWheelSteerPublishTick.store(now, std::memory_order_release);
    g_rearWheelSteerOverrideEnabled.store(false,
                                           std::memory_order_release);
    handling_probe::PublishRearWheelSteering(
        sample.identity.pvehicle, sample.suspension,
        sample.rearSteeringVisualCollectionKey,
        g_physicsSerial.load(std::memory_order_acquire), angleRad, true);
#endif
}

struct AppliedSteeringSnapshot {
    bool valid = false;
    float command = 0.0f;
    std::uint64_t physicsSerial = 0;
    std::uintptr_t pvehicle = 0;
};

void ClearAppliedSteeringSnapshotLocked() {
    g_latestSteeringPhysicsSerial.store(0, std::memory_order_release);
    g_latestSteeringPvehicle.store(0, std::memory_order_release);
    g_latestAppliedSteeringBits.store(0, std::memory_order_release);
}

// Invalidate the commit token before clearing its payload.  A consumer that
// races a reset can therefore never validate the cleared payload as a fresh
// command; the double-token read below also rejects a reset observed midway.
void InvalidateAppliedSteeringSnapshot() {
    AcquireSRWLockExclusive(&g_steeringSnapshotLock);
    ClearAppliedSteeringSnapshotLocked();
    ReleaseSRWLockExclusive(&g_steeringSnapshotLock);
}

void PublishAppliedSteeringSnapshot(std::uintptr_t pvehicle,
                                    std::uint64_t physicsSerial,
                                    float command) {
    AcquireSRWLockExclusive(&g_steeringSnapshotLock);
    if (pvehicle == 0 || physicsSerial == 0 ||
        physicsSerial == static_cast<std::uint64_t>(-1) ||
        !std::isfinite(command) || command < -1.0f || command > 1.0f) {
        ClearAppliedSteeringSnapshotLocked();
        ReleaseSRWLockExclusive(&g_steeringSnapshotLock);
        return;
    }

    // Payload first, commit token last.  Acquire readers that observe the
    // serial are guaranteed to see this complete command/vehicle pair.
    g_latestAppliedSteeringBits.store(FloatToBits(command),
                                      std::memory_order_release);
    g_latestSteeringPvehicle.store(pvehicle, std::memory_order_release);
    g_latestSteeringPhysicsSerial.store(physicsSerial,
                                        std::memory_order_release);
    ReleaseSRWLockExclusive(&g_steeringSnapshotLock);
}

AppliedSteeringSnapshot ReadAppliedSteeringSnapshot(
    std::uint64_t expectedPhysicsSerial, std::uintptr_t expectedPvehicle) {
    AppliedSteeringSnapshot snapshot{};
    if (expectedPhysicsSerial == 0 ||
        expectedPhysicsSerial == static_cast<std::uint64_t>(-1) ||
        expectedPvehicle == 0) {
        return snapshot;
    }

    // The serial is a commit token.  Reading it again after the payload
    // catches a concurrent vehicle reset or a newer physics frame.  All
    // payload members are atomic as well, so even an invalid/racing read is
    // defined and simply falls back to heading-only acceleration.
    AcquireSRWLockShared(&g_steeringSnapshotLock);
    const std::uint64_t firstSerial =
        g_latestSteeringPhysicsSerial.load(std::memory_order_acquire);
    const std::uint32_t commandBits =
        g_latestAppliedSteeringBits.load(std::memory_order_acquire);
    const std::uintptr_t pvehicle =
        g_latestSteeringPvehicle.load(std::memory_order_acquire);
    const std::uint64_t secondSerial =
        g_latestSteeringPhysicsSerial.load(std::memory_order_acquire);
    const float command = BitsToFloat(commandBits);
    ReleaseSRWLockShared(&g_steeringSnapshotLock);
    if (firstSerial == 0 || firstSerial != secondSerial ||
        firstSerial != expectedPhysicsSerial || pvehicle != expectedPvehicle) {
        return snapshot;
    }

    if (!std::isfinite(command) || command < -1.0f || command > 1.0f) {
        return snapshot;
    }
    snapshot.valid = true;
    snapshot.command = command;
    snapshot.physicsSerial = firstSerial;
    snapshot.pvehicle = pvehicle;
    return snapshot;
}

#if NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT
struct RigidBodyRuntimeContract {
    AccelerateFn accelerate = nullptr;
    GetForwardVectorFn getForward = nullptr;
};

bool ValidateRigidBodyRuntimeContract(const Identity& identity,
                                     RigidBodyRuntimeContract* contract,
                                     const char** reason) {
    if (contract == nullptr) {
        if (reason != nullptr) {
            *reason = "contract-output-null";
        }
        return false;
    }
    *contract = RigidBodyRuntimeContract{};
    if (reason != nullptr) {
        *reason = "contract-invalid";
    }
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    if (base == 0 || g_imageSize.load(std::memory_order_acquire) == 0) {
        if (reason != nullptr) {
            *reason = "image-unavailable";
        }
        return false;
    }
    std::uintptr_t expectedVtable = 0;
    if (!AddAddress(base, kRigidBodyVtableRva, &expectedVtable) ||
        !ReadOnlyImageRange(
            reinterpret_cast<const void*>(expectedVtable),
            (kRigidBodyAccelerateVtableSlot + 1) *
                sizeof(std::uintptr_t))) {
        if (reason != nullptr) {
            *reason = "vtable-layout-unavailable";
        }
        return false;
    }
    if (identity.rigidBody == 0 || identity.holder == 0 ||
        identity.inner == 0 ||
        !HasExpectedVtable(identity.rigidBody, expectedVtable)) {
        if (reason != nullptr) {
            *reason = "rigid-body-identity-mismatch";
        }
        return false;
    }

    std::uintptr_t holderAddress = 0;
    std::uintptr_t holder = 0;
    std::uintptr_t inner = 0;
    if (!AddAddress(identity.rigidBody, kRigidBodyHolderOffset,
                    &holderAddress) ||
        !SafeReadPointer(reinterpret_cast<const void*>(holderAddress),
                         &holder) ||
        holder != identity.holder ||
        !SafeReadPointer(reinterpret_cast<const void*>(holder), &inner) ||
        inner != identity.inner) {
        if (reason != nullptr) {
            *reason = "holder-chain-mismatch";
        }
        return false;
    }

    std::uintptr_t modeAddress = 0;
    std::uint8_t mode = 0;
    if (!AddAddress(identity.inner, kRigidBodyModeOffset, &modeAddress) ||
        !SafeReadByte(reinterpret_cast<const void*>(modeAddress), &mode)) {
        if (reason != nullptr) {
            *reason = "mode-unreadable";
        }
        return false;
    }
    if (mode != 0) {
        if (reason != nullptr) {
            *reason = "rigid-body-mode-not-normal";
        }
        return false;
    }

    std::uintptr_t forwardSlotAddress = 0;
    std::uintptr_t forwardTarget = 0;
    std::uintptr_t accelerateSlotAddress = 0;
    std::uintptr_t accelerateTarget = 0;
    if (!AddAddress(expectedVtable,
                    kRigidBodyForwardVtableSlot * sizeof(std::uintptr_t),
                    &forwardSlotAddress) ||
        !AddAddress(expectedVtable,
                    kRigidBodyAccelerateVtableSlot * sizeof(std::uintptr_t),
                    &accelerateSlotAddress) ||
        !SafeReadPointer(reinterpret_cast<const void*>(forwardSlotAddress),
                         &forwardTarget) ||
        !SafeReadPointer(reinterpret_cast<const void*>(accelerateSlotAddress),
                         &accelerateTarget) ||
        !ExecutableImageAddress(reinterpret_cast<const void*>(forwardTarget)) ||
        !ExecutableImageAddress(
            reinterpret_cast<const void*>(accelerateTarget))) {
        if (reason != nullptr) {
            *reason = "vtable-method-unavailable";
        }
        return false;
    }

    std::uintptr_t expectedAccelerate = 0;
    if (!AddAddress(base, kRigidBodyAccelerateRva, &expectedAccelerate) ||
        accelerateTarget != expectedAccelerate ||
        !SafeImageBytesEqual(accelerateTarget, kAccelerateSignature,
                             sizeof(kAccelerateSignature))) {
        if (reason != nullptr) {
            *reason = "accelerate-method-mismatch";
        }
        return false;
    }
    const AccelerateFn installedAccelerate =
        g_rigidbodyAccelerate.load(std::memory_order_acquire);
    if (installedAccelerate == nullptr ||
        reinterpret_cast<std::uintptr_t>(installedAccelerate) !=
            accelerateTarget) {
        if (reason != nullptr) {
            *reason = "accelerate-static-contract-unavailable";
        }
        return false;
    }

    contract->accelerate = reinterpret_cast<AccelerateFn>(accelerateTarget);
    contract->getForward = reinterpret_cast<GetForwardVectorFn>(forwardTarget);
    if (reason != nullptr) {
        *reason = "none";
    }
    return true;
}

bool ReadRuntimeForwardVector(const Identity& identity,
                              const nfsmw_drift::Basis& sampledBody,
                              RigidBodyRuntimeContract* contract,
                              nfsmw_drift::Vec3* normalizedForward,
                              const char** reason) {
    if (normalizedForward == nullptr) {
        if (reason != nullptr) {
            *reason = "forward-output-null";
        }
        return false;
    }
    *normalizedForward = nfsmw_drift::Vec3{};
    RigidBodyRuntimeContract localContract{};
    if (contract == nullptr) {
        contract = &localContract;
    }
    if (!ValidateRigidBodyRuntimeContract(identity, contract, reason)) {
        return false;
    }

    nfsmw_drift::Vec3 forward{};
    bool called = false;
#if defined(_MSC_VER)
    __try {
        contract->getForward(reinterpret_cast<void*>(identity.rigidBody),
                             &forward);
        called = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        called = false;
    }
#else
    contract->getForward(reinterpret_cast<void*>(identity.rigidBody),
                         &forward);
    called = true;
#endif
    if (!called || !std::isfinite(forward.x) || !std::isfinite(forward.y) ||
        !std::isfinite(forward.z)) {
        if (reason != nullptr) {
            *reason = "forward-call-fault";
        }
        return false;
    }
    const float length = VectorLength(forward);
    if (!std::isfinite(length) || length < 0.85f || length > 1.15f) {
        if (reason != nullptr) {
            *reason = "forward-length-invalid";
        }
        return false;
    }
    forward = forward / length;
    const nfsmw_drift::Vec3 sampledForward = nfsmw_drift::NormalizeOr(
        sampledBody.forward, {0.0f, 0.0f, 1.0f});
    if (!std::isfinite(sampledForward.x) ||
        !std::isfinite(sampledForward.y) ||
        !std::isfinite(sampledForward.z) ||
        nfsmw_drift::Dot(forward, sampledForward) < 0.70f) {
        if (reason != nullptr) {
            *reason = "forward-disagrees-with-sample";
        }
        return false;
    }
    *normalizedForward = forward;
    if (reason != nullptr) {
        *reason = "none";
    }
    return true;
}

#if NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE
bool SafeAccelerate(const VehicleSample& sample,
                    const RigidBodyRuntimeContract& contract,
                    const nfsmw_drift::Vec3& direction,
                    float deltaVMps,
                    rigidbody_accel::AppliedDeltaVerification* verification) {
    if (verification != nullptr) {
        *verification = {};
    }
    const float directionLength = VectorLength(direction);
    if (contract.accelerate == nullptr || !FiniteBoundedVector(direction, 1.01f) ||
        !std::isfinite(directionLength) || directionLength < 0.99f ||
        directionLength > 1.01f ||
        !std::isfinite(deltaVMps) || deltaVMps <= 0.0f ||
        deltaVMps > rigidbody_accel::kMaximumDeltaVPerTickMps) {
        return false;
    }
    std::uintptr_t velocityAddress = 0;
    nfsmw_drift::Vec3 before{};
    if (!AddAddress(sample.identity.inner, kLinearVelocityOffset,
                    &velocityAddress) ||
        !SafeReadVec3(velocityAddress, &before)) {
        return false;
    }
    const float beforeMagnitude = VectorLength(before);
    if (!std::isfinite(beforeMagnitude) ||
        beforeMagnitude > rigidbody_accel::kMaximumPlanSpeedMps) {
        return false;
    }
    if (verification != nullptr) {
        verification->beforeVelocity = before;
        verification->plannedDeltaVMps = deltaVMps;
    }

    bool called = false;
#if defined(_MSC_VER)
    __try {
        contract.accelerate(reinterpret_cast<void*>(sample.identity.rigidBody),
                            &direction, deltaVMps);
        called = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        called = false;
    }
#else
    contract.accelerate(reinterpret_cast<void*>(sample.identity.rigidBody),
                        &direction, deltaVMps);
    called = true;
#endif
    nfsmw_drift::Vec3 after{};
    if (!called || !SafeReadVec3(velocityAddress, &after)) {
        return false;
    }

    // Accelerate is synchronous for the validated game implementation.  A
    // successful call must therefore produce the requested forward delta in
    // this immediate read-back, without a material orthogonal component.
    const float afterMagnitude = VectorLength(after);
    const rigidbody_accel::AppliedDeltaVerification applied =
        rigidbody_accel::VerifyAppliedDelta(before, after, direction,
                                            deltaVMps);
    if (verification != nullptr) {
        *verification = applied;
    }
    return std::isfinite(afterMagnitude) && afterMagnitude <= 125.0f &&
           applied.accepted;
}
#endif

void LogRigidBodyAcceleration(const VehicleSample* sample,
                              std::uint64_t serial,
                              float dt,
                              float throttle,
                              float brake,
                              const nfsmw_drift::Vec3& forward,
                              const rigidbody_accel::Plan& plan,
                              const char* reason,
                              bool writeAttempted,
                              bool writeOk,
                              const rigidbody_accel::AppliedDeltaVerification*
                                  verification = nullptr,
                              bool steeringSnapshotValid = false,
                              float appliedSteeringCommand = 0.0f,
                              int driftSide = 0,
                              float handbrake = 0.0f,
                              float handbrakeHeldSeconds = 0.0f) {
    const DWORD now = GetTickCount();
    const bool forceFailureLog = writeAttempted && !writeOk;
    if (!forceFailureLog) {
        DWORD previous = g_lastRigidbodyTelemetryLogTick.load(
            std::memory_order_relaxed);
        if (previous != 0 && static_cast<DWORD>(now - previous) <
                                 kRigidBodyTelemetryLogIntervalMs) {
            return;
        }
        if (!g_lastRigidbodyTelemetryLogTick.compare_exchange_strong(
                previous, now, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return;
        }
    } else {
        g_lastRigidbodyTelemetryLogTick.store(now, std::memory_order_relaxed);
    }
#if NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE
    const char* executionMode = "write";
#else
    const char* executionMode = "dry-run";
#endif
    const std::uintptr_t pvehicle = sample != nullptr
                                        ? sample->identity.pvehicle
                                        : 0;
    const std::uintptr_t rigidBody = sample != nullptr
                                         ? sample->identity.rigidBody
                                         : 0;
    const rigidbody_accel::AppliedDeltaVerification emptyVerification{};
    const rigidbody_accel::AppliedDeltaVerification& applied =
        verification != nullptr ? *verification : emptyVerification;
    // A rejected plan may return before MakePlan records its speed. Keep the
    // telemetry useful in those paths by deriving the same total-speed
    // quantity from the sampled rigid-body velocity.
    float telemetrySpeedMps = plan.speedMps;
    if ((!std::isfinite(telemetrySpeedMps) || telemetrySpeedMps <= 0.0f) &&
        sample != nullptr) {
        telemetrySpeedMps = VectorLength(sample->linearVelocity);
    }
    float directionHeadingDot = 0.0f;
    const float telemetryForwardLength = VectorLength(forward);
    const float telemetryDirectionLength = VectorLength(plan.direction);
    if (std::isfinite(telemetryForwardLength) &&
        telemetryForwardLength > rigidbody_accel::kMinimumDirectionAxisLength &&
        std::isfinite(telemetryDirectionLength) &&
        telemetryDirectionLength > rigidbody_accel::kMinimumDirectionAxisLength) {
        directionHeadingDot = std::clamp(
            nfsmw_drift::Dot(forward / telemetryForwardLength,
                             plan.direction / telemetryDirectionLength),
            -1.0f, 1.0f);
    }
    const bool midpointRequested =
        steeringSnapshotValid && (driftSide == -1 || driftSide == 1) &&
        appliedSteeringCommand * static_cast<float>(driftSide) <
            -rigidbody_accel::kSteeringCommandInputTolerance &&
        directionHeadingDot < 0.99999f;
    const char* directionMode =
        plan.mode == rigidbody_accel::PlanMode::Deceleration
            ? "opposite-velocity"
            : (midpointRequested ? "countersteer-midpoint"
                                 : "body-forward");
    char message[1960]{};
    std::snprintf(
        message, sizeof(message),
        "rigidbody_force execution=%s force_mode=%s accepted=%d reason=%s serial=%llu "
        "pvehicle=%p rigid_body=%p forward=(%.3f,%.3f,%.3f) "
        "longitudinal=%.3f speed_mps=%.3f throttle=%.3f brake=%.3f "
        "handbrake=%.3f handbrake_held_s=%.3f grounded=%u dt=%.5f "
        "target_accel=%.3f ramp=%.3f delta_v=%.6f "
        "steering_snapshot_valid=%d applied_steering=%.5f drift_side=%d "
        "direction_mode=%s accel_direction=(%.5f,%.5f,%.5f) "
        "direction_heading_dot=%.5f "
        "readback_valid=%d before_velocity=(%.6f,%.6f,%.6f) "
        "after_velocity=(%.6f,%.6f,%.6f) "
        "actual_delta=(%.6f,%.6f,%.6f) actual_delta_mag=%.6f "
        "projected_delta=%.6f orthogonal_delta=(%.6f,%.6f,%.6f) "
        "orthogonal_residual=%.6f vertical_residual=%.6f "
        "projected_tolerance=%.6f magnitude_tolerance=%.6f "
        "orthogonal_tolerance=%.6f projected_match=%d magnitude_match=%d "
        "orthogonal_match=%d write_attempted=%d write_ok=%d",
        executionMode, rigidbody_accel::PlanModeName(plan.mode),
        plan.accepted ? 1 : 0,
        reason != nullptr ? reason : rigidbody_accel::RejectReasonName(plan.reason),
        static_cast<unsigned long long>(serial),
        reinterpret_cast<void*>(pvehicle), reinterpret_cast<void*>(rigidBody),
        static_cast<double>(forward.x), static_cast<double>(forward.y),
        static_cast<double>(forward.z),
        static_cast<double>(plan.longitudinalSpeedMps),
        static_cast<double>(telemetrySpeedMps),
        static_cast<double>(throttle), static_cast<double>(brake),
        static_cast<double>(handbrake),
        static_cast<double>(handbrakeHeldSeconds),
        sample != nullptr ? sample->groundedWheels : 0u,
        static_cast<double>(dt),
        static_cast<double>(plan.targetAccelerationMps2),
        static_cast<double>(plan.speedScale), static_cast<double>(plan.deltaVMps),
        steeringSnapshotValid ? 1 : 0,
        static_cast<double>(appliedSteeringCommand),
        driftSide, directionMode,
        static_cast<double>(plan.direction.x),
        static_cast<double>(plan.direction.y),
        static_cast<double>(plan.direction.z),
        static_cast<double>(directionHeadingDot),
        applied.evaluated && applied.inputsValid ? 1 : 0,
        static_cast<double>(applied.beforeVelocity.x),
        static_cast<double>(applied.beforeVelocity.y),
        static_cast<double>(applied.beforeVelocity.z),
        static_cast<double>(applied.afterVelocity.x),
        static_cast<double>(applied.afterVelocity.y),
        static_cast<double>(applied.afterVelocity.z),
        static_cast<double>(applied.actualDelta.x),
        static_cast<double>(applied.actualDelta.y),
        static_cast<double>(applied.actualDelta.z),
        static_cast<double>(applied.actualDeltaVMps),
        static_cast<double>(applied.projectedDeltaVMps),
        static_cast<double>(applied.orthogonalDelta.x),
        static_cast<double>(applied.orthogonalDelta.y),
        static_cast<double>(applied.orthogonalDelta.z),
        static_cast<double>(applied.orthogonalResidualMps),
        static_cast<double>(applied.orthogonalDelta.y),
        static_cast<double>(applied.projectedToleranceMps),
        static_cast<double>(applied.magnitudeToleranceMps),
        static_cast<double>(applied.orthogonalToleranceMps),
        applied.projectedMatch ? 1 : 0,
        applied.magnitudeMatch ? 1 : 0,
        applied.orthogonalMatch ? 1 : 0,
        writeAttempted ? 1 : 0, writeOk ? 1 : 0);
    Log(message);
}

bool ValidateRigidBodyAccelStaticLayout() {
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t vtable = 0;
    std::uintptr_t accel = 0;
    std::uintptr_t accelSlot = 0;
    std::uintptr_t forwardSlot = 0;
    std::uintptr_t forward = 0;
    std::uintptr_t expectedAccel = 0;
    const bool valid =
        AddAddress(base, kRigidBodyVtableRva, &vtable) &&
        ReadOnlyImageRange(
            reinterpret_cast<const void*>(vtable),
            (kRigidBodyAccelerateVtableSlot + 1) * sizeof(std::uintptr_t)) &&
        AddAddress(base, kRigidBodyAccelerateRva, &expectedAccel) &&
        ExecutableImageAddress(reinterpret_cast<const void*>(expectedAccel)) &&
        SafeImageBytesEqual(expectedAccel, kAccelerateSignature,
                            sizeof(kAccelerateSignature)) &&
        AddAddress(vtable,
                   kRigidBodyAccelerateVtableSlot * sizeof(std::uintptr_t),
                   &accelSlot) &&
        AddAddress(vtable,
                   kRigidBodyForwardVtableSlot * sizeof(std::uintptr_t),
                   &forwardSlot) &&
        SafeReadPointer(reinterpret_cast<const void*>(accelSlot), &accel) &&
        SafeReadPointer(reinterpret_cast<const void*>(forwardSlot), &forward) &&
        accel == expectedAccel &&
        ExecutableImageAddress(reinterpret_cast<const void*>(forward));
    g_rigidbodyAccelStaticValid.store(valid, std::memory_order_release);
    g_rigidbodyAccelerate.store(
        valid ? reinterpret_cast<AccelerateFn>(expectedAccel) : nullptr,
        std::memory_order_release);
    if (!valid) {
        Log("rigidbody acceleration experiment disabled: static Accelerate/GetForward layout validation failed");
    } else {
        Log("rigidbody acceleration experiment static layout validated; runtime identity and mode checks remain required");
    }
    return valid;
}

float AdvanceRigidBodyAccelerationRamp(bool eligible, float dt) {
    const float current = BitsToFloat(
        g_rigidbodyAccelRampBits.load(std::memory_order_acquire));
    const float next = rigidbody_accel::AdvanceRamp(
        current, eligible, dt, kForwardAccelRiseSeconds,
        kForwardAccelFallSeconds);
    g_rigidbodyAccelRampBits.store(FloatToBits(next),
                                   std::memory_order_release);
    return next;
}

void ApplyRigidBodyForce(std::uint64_t sinkPhysicsSerial,
                         float dt,
                         DWORD physicsThread) {
    rigidbody_accel::Plan plan{};
    nfsmw_drift::Vec3 forward{};
    const DWORD now = GetTickCount();
    if (!g_rigidbodyAccelStaticValid.load(std::memory_order_acquire)) {
        (void)AdvanceRigidBodyAccelerationRamp(false, dt);
        plan.reason = rigidbody_accel::RejectReason::Disabled;
        LogRigidBodyAcceleration(nullptr, sinkPhysicsSerial, dt, 0.0f, 0.0f,
                                 forward, plan, "static-layout-invalid",
                                 false, false);
        return;
    }

    Identity stableIdentity{};
    nfsmw_drift::ControlOutput output{};
    std::uint64_t outputSerial = 0;
    driver_assist::Output driverOutput{};
    std::uint64_t driverOutputSerial = 0;
    bool runtimeReady = false;
    AcquireSRWLockShared(&g_stateLock);
    stableIdentity = g_stableIdentity;
    output = g_cachedOutput;
    outputSerial = g_cachedOutputSerial;
    driverOutput = g_cachedDriverAssistOutput;
    driverOutputSerial = g_cachedDriverAssistSerial;
    runtimeReady = g_runtimeReady;
    ReleaseSRWLockShared(&g_stateLock);

    const bool sustainedDrift = rigidbody_accel::SustainedDriftEligible(
        output.active, output.phase, output.driftSide);
    if (!runtimeReady) {
        (void)AdvanceRigidBodyAccelerationRamp(false, dt);
        plan.reason = rigidbody_accel::RejectReason::SessionInactive;
        LogRigidBodyAcceleration(nullptr, sinkPhysicsSerial, dt, 0.0f, 0.0f,
                                 forward, plan,
                                 "runtime-not-ready",
                                 false, false);
        return;
    }

    VehicleSample sample{};
    if (!ReadPlayerVehicle(&sample, nullptr)) {
        (void)AdvanceRigidBodyAccelerationRamp(false, dt);
        plan.reason = rigidbody_accel::RejectReason::IdentityUnstable;
        LogRigidBodyAcceleration(nullptr, sinkPhysicsSerial, dt, 0.0f, 0.0f,
                                 forward, plan, "vehicle-read-failed", false,
                                 false);
        return;
    }

    const std::uint64_t inputSerial =
        g_latestInputPhysicsSerial.load(std::memory_order_acquire);
    const std::uintptr_t inputPvehicle =
        g_latestInputPvehicle.load(std::memory_order_acquire);
    const DWORD inputTick = g_latestInputTick.load(std::memory_order_acquire);
    const float throttle = BitsToFloat(
        g_latestThrottleBits.load(std::memory_order_acquire));
    const float brake = BitsToFloat(
        g_latestBrakeBits.load(std::memory_order_acquire));
    const float handbrake = BitsToFloat(
        g_latestHandbrakeBits.load(std::memory_order_acquire));
    const DWORD handbrakeHeldMilliseconds =
        g_latestHandbrakeHeldMilliseconds.load(
            std::memory_order_acquire);
    const float handbrakeHeldSeconds =
        static_cast<float>(handbrakeHeldMilliseconds) / 1000.0f;
    const bool serialValid =
        inputSerial != 0 &&
        inputSerial != static_cast<std::uint64_t>(-1) &&
        inputSerial + 1 == sinkPhysicsSerial && outputSerial == inputSerial &&
        driverOutputSerial == inputSerial;
    const bool inputFresh =
        inputTick != 0 &&
        static_cast<DWORD>(now - inputTick) <= kPhysicsFreshnessMs;
    const bool identityStable =
        SameIdentity(stableIdentity, sample.identity) &&
        inputPvehicle == sample.identity.pvehicle;
    const bool physicsStable =
        g_physicsThread.load(std::memory_order_acquire) == physicsThread &&
        g_physicsSerial.load(std::memory_order_acquire) ==
            sinkPhysicsSerial;
    if (!serialValid || !inputFresh || !identityStable || !physicsStable) {
        (void)AdvanceRigidBodyAccelerationRamp(false, dt);
        plan.reason = rigidbody_accel::RejectReason::IdentityUnstable;
        const char* reason = !serialValid
                                 ? "input-output-serial-mismatch"
                                 : (!inputFresh
                                        ? "input-snapshot-stale"
                                        : (!identityStable
                                               ? "player-identity-mismatch"
                                               : "physics-cadence-mismatch"));
        LogRigidBodyAcceleration(&sample, sinkPhysicsSerial, dt, throttle,
                                 brake, forward, plan, reason, false, false,
                                 nullptr, false, 0.0f, 0, handbrake,
                                 handbrakeHeldSeconds);
        return;
    }

    // Consume only the command committed by the input poll for this exact
    // vehicle and preceding physics serial.  A missing/expired command is not
    // fatal to the conservative acceleration fallback: the planner will keep
    // the body heading when useCountersteerDirection is false.
    const AppliedSteeringSnapshot steeringSnapshot =
        ReadAppliedSteeringSnapshot(inputSerial, sample.identity.pvehicle);

    RigidBodyRuntimeContract contract{};
    const char* contractReason = nullptr;
    if (!ReadRuntimeForwardVector(sample.identity, sample.body, &contract,
                                  &forward, &contractReason)) {
        (void)AdvanceRigidBodyAccelerationRamp(false, dt);
        plan.reason = rigidbody_accel::RejectReason::InvalidForward;
        LogRigidBodyAcceleration(&sample, sinkPhysicsSerial, dt, throttle,
                                 brake, forward, plan, contractReason, false,
                                 false, nullptr, false, 0.0f, 0, handbrake,
                                 handbrakeHeldSeconds);
        return;
    }

    const float handbrakeThreshold =
        g_controller != nullptr
            ? g_controller->config().handbrakeActivationThreshold
            : 0.5f;
    const bool driftDecelerationRequested =
        output.active &&
        handbrakeHeldMilliseconds >=
            kHandbrakeDecelerationHoldMilliseconds &&
        rigidbody_accel::HandbrakeDecelerationEligible(
                             handbrake, handbrakeThreshold,
                             handbrakeHeldSeconds);
    const bool absDecelerationRequested =
        !output.active && driverOutput.modeActive &&
        driverOutput.absWorking &&
        std::isfinite(driverOutput.absDecelerationScale) &&
        driverOutput.absDecelerationScale > 0.0f;
    const bool accelerationDemandEligible =
        sustainedDrift && std::isfinite(dt) && dt > 0.0f &&
        dt <= rigidbody_accel::kMaximumPlanDt &&
        std::isfinite(throttle) &&
        throttle >= rigidbody_accel::kMinimumThrottle && throttle <= 1.0f &&
        std::isfinite(brake) && brake >= 0.0f &&
        brake <= rigidbody_accel::kMaximumBrakeForAcceleration &&
        sample.groundedWheels >= 2;
    const rigidbody_accel::PlanMode selectedMode =
        (driftDecelerationRequested || absDecelerationRequested)
            ? rigidbody_accel::PlanMode::Deceleration
            : (sustainedDrift ? rigidbody_accel::PlanMode::Acceleration
                              : rigidbody_accel::PlanMode::None);
    if (selectedMode == rigidbody_accel::PlanMode::Deceleration) {
        (void)AdvanceRigidBodyAccelerationRamp(false, dt);
        rigidbody_accel::DecelerationInputs input{};
        input.enabled = true;
        input.sessionActive = output.active || driverOutput.modeActive;
        input.identityStable = identityStable;
        input.physicsStable = physicsStable;
        input.grounded = sample.groundedWheels >= 2;
        input.handbrake = absDecelerationRequested ? 1.0f : handbrake;
        input.handbrakeThreshold =
            absDecelerationRequested ? 0.5f : handbrakeThreshold;
        input.handbrakeHeldSeconds =
            absDecelerationRequested ? 1.0f : handbrakeHeldSeconds;
        input.dt = dt;
        input.linearVelocity = sample.linearVelocity;
        input.maximumDecelerationMps2 =
            rigidbody_accel::kConfiguredMaximumDecelerationMps2 *
            (absDecelerationRequested
                 ? driverOutput.absDecelerationScale
                 : 1.0f);
        plan = rigidbody_accel::MakeDecelerationPlan(input);
    } else if (selectedMode == rigidbody_accel::PlanMode::Acceleration) {
        const float ramp = AdvanceRigidBodyAccelerationRamp(
            accelerationDemandEligible, dt);
        rigidbody_accel::Inputs input{};
        input.enabled = true;
        input.sessionActive = sustainedDrift;
        input.identityStable = identityStable;
        input.physicsStable = physicsStable;
        input.grounded = sample.groundedWheels >= 2;
        input.throttle = throttle;
        input.brake = brake;
        input.dt = dt;
        input.forward = forward;
        input.bodyUp = sample.body.up;
        input.appliedSteeringCommand = steeringSnapshot.command;
        input.driftSide = output.driftSide;
        input.maximumSteerAngleRad =
            g_controller != nullptr
                ? g_controller->config().maximumSteerAngleRad
                : 0.0f;
        input.useCountersteerDirection = steeringSnapshot.valid;
        input.linearVelocity = sample.linearVelocity;
        input.targetAccelerationMps2 = kForwardAccelTargetMps2;
        input.ramp = ramp;
        plan = rigidbody_accel::MakePlan(input);
    } else {
        (void)AdvanceRigidBodyAccelerationRamp(false, dt);
        plan.reason = rigidbody_accel::RejectReason::SessionInactive;
    }

    bool writeAttempted = false;
    bool writeOk = false;
    rigidbody_accel::AppliedDeltaVerification appliedVerification{};
#if NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE
    if (plan.accepted &&
        !g_rigidbodyAccelWriterDisabled.load(std::memory_order_acquire)) {
        writeAttempted = true;
        writeOk = SafeAccelerate(sample, contract, plan.direction,
                                 plan.deltaVMps, &appliedVerification);
        if (!writeOk) {
            g_rigidbodyAccelWriterDisabled.store(true,
                                                 std::memory_order_release);
            Log("rigidbody force writer permanently disabled: call or read-back validation failed");
        }
    }
#else
    (void)contract;
#endif
    const char* resultReason = plan.accepted
                                   ? (g_rigidbodyAccelWriterDisabled.load(
                                          std::memory_order_acquire)
                                          ? "writer-disabled"
                                          : (absDecelerationRequested
                                                 ? "driver-ABS"
                                                 : "none"))
                                   : rigidbody_accel::RejectReasonName(
                                         plan.reason);
    LogRigidBodyAcceleration(&sample, sinkPhysicsSerial, dt, throttle, brake,
                             forward, plan, resultReason, writeAttempted,
                             writeOk,
                             writeAttempted ? &appliedVerification : nullptr,
                             steeringSnapshot.valid,
                             steeringSnapshot.command,
                             output.driftSide, handbrake,
                             handbrakeHeldSeconds);
}
#endif

const char* PhaseName(nfsmw_drift::DriftPhase phase) {
    switch (phase) {
    case nfsmw_drift::DriftPhase::Off:
        return "off";
    case nfsmw_drift::DriftPhase::Charging:
        return "charging";
    case nfsmw_drift::DriftPhase::WaitingDirection:
        return "waiting-direction";
    case nfsmw_drift::DriftPhase::Entering:
        return "entering";
    case nfsmw_drift::DriftPhase::Holding:
        return "holding";
    case nfsmw_drift::DriftPhase::SideTransition:
        return "side-transition";
    case nfsmw_drift::DriftPhase::Centering:
        return "centering";
    case nfsmw_drift::DriftPhase::Exiting:
        return "exiting";
    }
    return "unknown";
}

const char* SteeringModeName(nfsmw_drift::SteeringAssistMode mode) {
    switch (mode) {
    case nfsmw_drift::SteeringAssistMode::None:
        return "passthrough";
    case nfsmw_drift::SteeringAssistMode::WaitingForDrift:
        return "waiting-angle";
    case nfsmw_drift::SteeringAssistMode::SmartCountersteer:
        return "auto-countersteer";
    case nfsmw_drift::SteeringAssistMode::CountersteerHandoffDelay:
        return "countersteer-handoff-delay";
    case nfsmw_drift::SteeringAssistMode::ManualOverride:
        return "manual-override";
    case nfsmw_drift::SteeringAssistMode::ReengageDelay:
        return "reengage-delay";
    }
    return "unknown";
}

const char* YawModeName(nfsmw_drift::YawResponseMode mode) {
    switch (mode) {
    case nfsmw_drift::YawResponseMode::None:
        return "none";
    case nfsmw_drift::YawResponseMode::ManualSameDirectionAssist:
        return "manual-same-assist";
    case nfsmw_drift::YawResponseMode::ManualOppositeRecovery:
        return "manual-opposite-recovery";
    case nfsmw_drift::YawResponseMode::AssistOnly:
        return "assist-only";
    case nfsmw_drift::YawResponseMode::EntryAssistOnly:
        return "entry-assist-only";
    case nfsmw_drift::YawResponseMode::AcquireBase15:
        return "acquire-base-15";
    case nfsmw_drift::YawResponseMode::AngleHold:
        return "angle-hold";
    case nfsmw_drift::YawResponseMode::SameDirection:
        return "same-direction";
    case nfsmw_drift::YawResponseMode::DirectionChange:
        return "direction-change";
    case nfsmw_drift::YawResponseMode::Centering:
        return "centering";
    }
    return "unknown";
}

void LogTelemetry(const nfsmw_drift::ControlOutput& output,
                  float rawSteering,
                  float handbrake,
                  float appliedSteering,
                  bool steeringSlewActive,
                  bool automaticAllowed,
                  float effectiveSteeringTarget,
                  bool manualInputObserved,
                  std::uint8_t manualDirectionMask) {
    const DWORD now = GetTickCount();
    DWORD previous = g_lastTelemetryLogTick.load(std::memory_order_relaxed);
    const bool phaseChanged = output.phase != g_lastLoggedPhase;
    const bool steeringModeChanged =
        output.steeringAssistMode != g_lastLoggedSteeringMode;
    if (!phaseChanged && !steeringModeChanged && previous != 0 &&
        static_cast<DWORD>(now - previous) < kTelemetryLogIntervalMs) {
        return;
    }
    if (!phaseChanged && !steeringModeChanged &&
        !g_lastTelemetryLogTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }
    if (phaseChanged || steeringModeChanged) {
        g_lastTelemetryLogTick.store(now, std::memory_order_relaxed);
        g_lastLoggedPhase = output.phase;
        g_lastLoggedSteeringMode = output.steeringAssistMode;
    }
    const float maximumSteerAngle = g_controller != nullptr
                                        ? g_controller->config().maximumSteerAngleRad
                                        : 0.0f;
    const float autoTargetDegrees = output.smartCountersteerActive
                                        ? output.smartCountersteerCommand *
                                              maximumSteerAngle *
                                              (180.0f / nfsmw_drift::kPi)
                                        : 0.0f;
    char message[1280]{};
    std::snprintf(
        message, sizeof(message),
        "alpha phase=%s active=%d side=%d raw_steer=%.3f handbrake=%.3f "
        "offset_deg=%.2f steer_mode=%s neutral_s=%.3f ground_loss_s=%.3f ground_grace=%d "
        "handoff_pending=%d handoff_s=%.3f "
        "auto_target_deg=%.2f steering_target=%.3f effective_target=%.3f "
        "steering_applied=%.3f steering_slew=%d automatic_allowed=%d "
        "manual_observed=%d direction_mask=0x%02X "
        "yaw_rate=%.4f relative_yaw_rate=%.4f yaw_target=%.4f "
        "yaw_delta=%.4f yaw_mode=%s yaw_control_side=%d yaw_strength=%.3f "
        "side_transition=%d transition_target=%d transition_s=%.3f "
        "offset_limited=%d yaw_fault=%d "
        "front_grip=1.000 rear_drive=1.000",
        PhaseName(output.phase), output.active ? 1 : 0, output.driftSide,
        static_cast<double>(rawSteering), static_cast<double>(handbrake),
        static_cast<double>(output.bodyYawOffsetRad *
                            (180.0f / nfsmw_drift::kPi)),
        SteeringModeName(output.steeringAssistMode),
        static_cast<double>(output.steeringNeutralSeconds),
        static_cast<double>(output.groundContactLossSeconds),
        output.groundContactGraceActive ? 1 : 0,
        output.countersteerHandoffPending ? 1 : 0,
        static_cast<double>(output.countersteerHandoffSeconds),
        static_cast<double>(autoTargetDegrees),
        static_cast<double>(output.steeringCommand),
        static_cast<double>(effectiveSteeringTarget),
        static_cast<double>(appliedSteering),
        steeringSlewActive ? 1 : 0,
        automaticAllowed ? 1 : 0,
        manualInputObserved ? 1 : 0,
        static_cast<unsigned>(manualDirectionMask),
        static_cast<double>(output.yawRateRadS),
        static_cast<double>(output.bodyYawOffsetRateRadS),
        static_cast<double>(output.bodyYawRateTargetRadS),
        static_cast<double>(output.angularVelocityDeltaLocal.y),
        YawModeName(output.yawResponseMode),
        output.manualYawControlSide,
        static_cast<double>(output.manualYawStrength),
        output.sideTransitionActive ? 1 : 0,
        output.sideTransitionTargetSide,
        static_cast<double>(output.sideTransitionSeconds),
        output.manualYawBodyOffsetLimited ? 1 : 0,
        g_yawPermanentFault.load(std::memory_order_acquire) ? 1 : 0);
    Log(message);
}

void LogDriverAssistTelemetry(const driver_assist::Output& output,
                              int gear,
                              float brake,
                              float throttle,
                              float bodyOffsetRad,
                              float yawRateRadS) {
    const DWORD now = GetTickCount();
    DWORD previous = g_lastDriverAssistTelemetryLogTick.load(
        std::memory_order_relaxed);
    if (previous != 0 && static_cast<DWORD>(now - previous) <
                             kTelemetryLogIntervalMs) {
        return;
    }
    if (!g_lastDriverAssistTelemetryLogTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }
    char message[640]{};
    std::snprintf(
        message, sizeof(message),
        "driver_assist active=%d ABS=%d ABS_scale=%.3f ESC=%d "
        "ESC_straight=%d ESC_recovery=%d ESC_yaw_delta=%.6f "
        "TCS=%d gear=%d brake=%.3f throttle=%.3f "
        "offset_deg=%.2f yaw_rate=%.4f",
        output.modeActive ? 1 : 0, output.absWorking ? 1 : 0,
        static_cast<double>(output.absDecelerationScale),
        output.escWorking ? 1 : 0,
        output.escStraightWorking ? 1 : 0,
        output.escRecoveryWorking ? 1 : 0,
        static_cast<double>(output.yawVelocityDeltaRadS),
        output.tcsWorking ? 1 : 0, gear,
        static_cast<double>(brake), static_cast<double>(throttle),
        static_cast<double>(bodyOffsetRad * 180.0f / nfsmw_drift::kPi),
        static_cast<double>(yawRateRadS));
    Log(message);
}

void ResetRuntimeState(bool resetController) {
    DisableRearWheelSteeringOverride();
    DisableDriftDriveTorque();
    AcquireSRWLockExclusive(&g_stateLock);
    g_stableIdentity = Identity{};
    g_probationCount = 0;
    g_lastProbationSerial = 0;
    g_lastControllerSerial = 0;
    g_latestInputSequence = 0;
    g_runtimeReady = false;
    g_cachedOutput = nfsmw_drift::ControlOutput{};
    g_cachedOutputSerial = 0;
    g_driverAssistController.Reset(true);
    g_cachedDriverAssistOutput = driver_assist::Output{};
    g_cachedDriverAssistSerial = 0;
    g_manualInputObservedSinceUpdate = false;
    g_manualDirectionMaskSinceUpdate = 0;
    g_pendingManualYaw = PendingManualYaw{};
    g_latestThrottleBits.store(0, std::memory_order_release);
    g_latestBrakeBits.store(0, std::memory_order_release);
    g_latestHandbrakeBits.store(0, std::memory_order_release);
    g_latestHandbrakeHeldMilliseconds.store(0,
                                             std::memory_order_release);
    g_handbrakePressedSinceTick.store(0, std::memory_order_release);
    g_latestInputPhysicsSerial.store(0, std::memory_order_release);
    g_latestInputPvehicle.store(0, std::memory_order_release);
    g_latestInputTick.store(0, std::memory_order_release);
    InvalidateAppliedSteeringSnapshot();
    g_rigidbodyAccelRampBits.store(0, std::memory_order_release);
    g_steeringResponseLimiter.Reset();
    g_steeringAutomaticOwnership = false;
    g_steeringInputHistory.Reset();
    g_lastLoggedPhase = nfsmw_drift::DriftPhase::Off;
    g_lastLoggedSteeringMode = nfsmw_drift::SteeringAssistMode::None;
    g_currentVehicleCollectionKey = 0;
    if (resetController && g_controller != nullptr) {
        g_controller->reset();
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    ClearDriftCameraTarget();
    driver_assist_hud::Publish(false, false, false, false, false, false,
                               GetTickCount());
}

void SuspendRuntimeOutputs(bool resetController) {
    DisableRearWheelSteeringOverride();
    DisableDriftDriveTorque();
    AcquireSRWLockExclusive(&g_stateLock);
    g_runtimeReady = false;
    g_probationCount = 0;
    g_lastProbationSerial = 0;
    g_lastControllerSerial = 0;
    g_latestInputSequence = 0;
    g_cachedOutput = nfsmw_drift::ControlOutput{};
    g_cachedOutputSerial = 0;
    g_driverAssistController.Reset(true);
    g_cachedDriverAssistOutput = driver_assist::Output{};
    g_cachedDriverAssistSerial = 0;
    g_manualInputObservedSinceUpdate = false;
    g_manualDirectionMaskSinceUpdate = 0;
    g_pendingManualYaw = PendingManualYaw{};
    g_latestThrottleBits.store(0, std::memory_order_release);
    g_latestBrakeBits.store(0, std::memory_order_release);
    g_latestHandbrakeBits.store(0, std::memory_order_release);
    g_latestHandbrakeHeldMilliseconds.store(0,
                                             std::memory_order_release);
    g_handbrakePressedSinceTick.store(0, std::memory_order_release);
    g_latestInputPhysicsSerial.store(0, std::memory_order_release);
    g_latestInputPvehicle.store(0, std::memory_order_release);
    g_latestInputTick.store(0, std::memory_order_release);
    InvalidateAppliedSteeringSnapshot();
    g_rigidbodyAccelRampBits.store(0, std::memory_order_release);
    g_steeringResponseLimiter.Reset();
    g_steeringAutomaticOwnership = false;
    g_steeringInputHistory.Reset();
    g_lastLoggedPhase = nfsmw_drift::DriftPhase::Off;
    g_lastLoggedSteeringMode = nfsmw_drift::SteeringAssistMode::None;
    g_currentVehicleCollectionKey = 0;
    if (resetController && g_controller != nullptr) {
        g_controller->reset();
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    ClearDriftCameraTarget();
    driver_assist_hud::Publish(false, false, false, false, false, false,
                               GetTickCount());
}

void ObserveManualYawInputLocked(nfsmw_drift::ControlOutput* output,
                                 const VehicleSample& sample,
                                 float rawSteering,
                                 float dt,
                                 std::uint64_t physicsSerial,
                                 DWORD thread,
                                 DWORD now) {
    ++g_latestInputSequence;
    if (g_latestInputSequence == 0) {
        ++g_latestInputSequence;
    }
    g_pendingManualYaw = PendingManualYaw{};
    const int controlSide =
        output != nullptr ? output->manualYawControlSide : 0;
    const float manualYawStrength =
        output != nullptr ? output->manualYawStrength : 0.0f;
    const nfsmw_drift::YawResponseMode coreYawMode =
        output != nullptr ? output->yawResponseMode
                          : nfsmw_drift::YawResponseMode::None;
    const bool sideTransitionActive =
        output != nullptr && output->sideTransitionActive;
    if (output != nullptr) {
        output->targetYawRateRadS = 0.0f;
        output->bodyYawRateTargetRadS = 0.0f;
        output->bodyYawAccelerationLimitRadS2 = 0.0f;
        output->bodyYawVelocityDeltaLimitRadS = 0.0f;
        output->manualYawStrength = 0.0f;
        output->manualYawControlSide = 0;
        output->yawResponseMode = nfsmw_drift::YawResponseMode::None;
        output->bodyOffsetBrakeActive = false;
        output->manualYawBodyOffsetLimited = false;
        output->angularVelocityDeltaLocal.y = 0.0f;
    }
    if (output == nullptr || g_controller == nullptr ||
        g_yawPermanentFault.load(std::memory_order_acquire) ||
        !output->active || output->forceCountersteer ||
        sideTransitionActive ||
        !IsManualYawMode(coreYawMode) ||
        (controlSide != -1 && controlSide != 1) ||
        !std::isfinite(manualYawStrength) || manualYawStrength <= 0.0f ||
        manualYawStrength > 1.0f ||
        std::fabs(rawSteering) <=
            g_controller->config().directionDeadzone ||
        !ManualYawMotionEligible(sample, g_controller->config())) {
        return;
    }

    const float bodyOffset = BodyOffsetRad(sample);
    const float currentYawRate = BodyYawRateRadS(sample);
    const float sourcePathYawRate =
        output->yawRateRadS - output->bodyYawOffsetRateRadS;
    const float relativeYawRate = currentYawRate - sourcePathYawRate;
    const nfsmw_drift::ManualYawCommand command =
        nfsmw_drift::ComputeManualYawCommand(
            g_controller->config(), bodyOffset, relativeYawRate,
            rawSteering, controlSide, dt, manualYawStrength);
    if (!IsManualYawMode(command.mode) || command.mode != coreYawMode) {
        return;
    }

    output->bodyYawOffsetRad = bodyOffset;
    output->bodyYawOffsetSafetyRad = bodyOffset;
    output->bodyYawOffsetRateRadS = relativeYawRate;
    output->yawRateRadS = currentYawRate;
    output->targetYawRateRadS = command.targetYawRateRadS;
    output->bodyYawRateTargetRadS = command.targetYawRateRadS;
    output->bodyYawAccelerationLimitRadS2 =
        command.accelerationLimitRadS2;
    output->bodyYawVelocityDeltaLimitRadS =
        command.velocityDeltaLimitRadS;
    output->manualYawStrength = manualYawStrength;
    output->manualYawControlSide = controlSide;
    output->yawResponseMode = command.mode;
    output->bodyOffsetBrakeActive = command.dampingActive;
    output->manualYawBodyOffsetLimited = command.bodyOffsetLimited;
    output->angularVelocityDeltaLocal.y =
        command.yawVelocityDeltaRadS;

    g_pendingManualYaw.valid = true;
    g_pendingManualYaw.sourcePhysicsSerial = physicsSerial;
    g_pendingManualYaw.inputSequence = g_latestInputSequence;
    g_pendingManualYaw.publishTick = now;
    g_pendingManualYaw.sourceThread = thread;
    g_pendingManualYaw.identity = sample.identity;
    g_pendingManualYaw.rawSteering = rawSteering;
    g_pendingManualYaw.controlSide = controlSide;
    g_pendingManualYaw.manualYawStrength = manualYawStrength;
    g_pendingManualYaw.expectedMode = coreYawMode;
    g_pendingManualYaw.sourceBodyOffsetRad = bodyOffset;
    g_pendingManualYaw.sourcePathYawRateRadS = sourcePathYawRate;
}

bool TakePendingManualYaw(std::uint64_t sinkPhysicsSerial,
                          PendingManualYaw* pending) {
    if (pending == nullptr) {
        return false;
    }
    *pending = PendingManualYaw{};
    AcquireSRWLockExclusive(&g_stateLock);
    const bool valid = g_pendingManualYaw.valid &&
                       g_pendingManualYaw.inputSequence ==
                           g_latestInputSequence &&
                       g_pendingManualYaw.sourcePhysicsSerial !=
                           static_cast<std::uint64_t>(-1) &&
                       g_pendingManualYaw.sourcePhysicsSerial + 1 ==
                           sinkPhysicsSerial;
    if (valid) {
        *pending = g_pendingManualYaw;
    }
    // A command belongs to exactly one sink. Stale, early, or valid commands
    // are all removed before any read or write attempt, so none can replay.
    g_pendingManualYaw = PendingManualYaw{};
    ReleaseSRWLockExclusive(&g_stateLock);
    return valid;
}

void LogAppliedYaw(const PendingManualYaw& pending,
                   std::uint64_t sinkPhysicsSerial,
                   DWORD ageMs,
                   float bodyOffset,
                   float relativeYawRate,
                   const nfsmw_drift::ManualYawCommand& command,
                   float beforeYawRate,
                   float afterYawRate) {
    const DWORD now = GetTickCount();
    DWORD previous =
        g_lastYawTelemetryLogTick.load(std::memory_order_relaxed);
    if (previous != 0 &&
        static_cast<DWORD>(now - previous) < kTelemetryLogIntervalMs) {
        return;
    }
    if (!g_lastYawTelemetryLogTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }
    char message[720]{};
    std::snprintf(
        message, sizeof(message),
        "alpha yaw_apply mode=%s source=%llu sink=%llu age_ms=%lu "
        "control_side=%d strength=%.3f offset_deg=%.2f "
        "relative_rate=%.4f target=%.4f delta=%.4f "
        "yaw_before=%.4f yaw_after=%.4f identity_ok=1 setter_ok=1",
        YawModeName(command.mode),
        static_cast<unsigned long long>(pending.sourcePhysicsSerial),
        static_cast<unsigned long long>(sinkPhysicsSerial),
        static_cast<unsigned long>(ageMs),
        pending.controlSide,
        static_cast<double>(pending.manualYawStrength),
        static_cast<double>(bodyOffset *
                            (180.0f / nfsmw_drift::kPi)),
        static_cast<double>(relativeYawRate),
        static_cast<double>(command.targetYawRateRadS),
        static_cast<double>(command.yawVelocityDeltaRadS),
        static_cast<double>(beforeYawRate),
        static_cast<double>(afterYawRate));
    Log(message);
}

void DisableManualYawChannel(const char* reason) {
    g_yawPermanentFault.store(true, std::memory_order_release);
    AcquireSRWLockExclusive(&g_stateLock);
    g_pendingManualYaw = PendingManualYaw{};
    ReleaseSRWLockExclusive(&g_stateLock);
    char message[384]{};
    std::snprintf(
        message, sizeof(message),
        "manual yaw permanently disabled while smart countersteer remains active: %s",
        reason != nullptr ? reason : "unknown setter failure");
    Log(message);
}

bool PendingYawEnteredTransitionBoundary(
    const PendingManualYaw& pending, float bodyOffset,
    const nfsmw_drift::AssistConfig& config) {
    if (pending.controlSide != -1 && pending.controlSide != 1) {
        return true;
    }
    const float transitionBand = nfsmw_drift::Clamp(
        config.pendulumTransitionZeroBandRad, 0.0f,
        0.5f * nfsmw_drift::kPi);
    const float side = static_cast<float>(pending.controlSide);
    const float sourceAlignedOffset = pending.sourceBodyOffsetRad * side;
    const float sinkAlignedOffset = bodyOffset * side;
    const bool enteredCenterBand =
        sourceAlignedOffset > transitionBand + kMinimumOffsetChangeRad &&
        sinkAlignedOffset <= transitionBand + kMinimumOffsetChangeRad;
    const bool crossedToOtherSide =
        sinkAlignedOffset < -kMinimumOffsetChangeRad;
    return enteredCenterBand || crossedToOtherSide;
}

void ConsumePendingManualYaw(std::uint64_t sinkPhysicsSerial,
                             float dt,
                             DWORD thread) {
    PendingManualYaw pending{};
    if (!TakePendingManualYaw(sinkPhysicsSerial, &pending)) {
        return;
    }
    if (g_yawPermanentFault.load(std::memory_order_acquire) ||
        g_controller == nullptr || !std::isfinite(dt) || dt <= 0.0f ||
        dt > kMaximumCadenceDt) {
        return;
    }
    const float controlDt = std::min(dt, kMaximumControlDt);
    if ((pending.controlSide != -1 && pending.controlSide != 1) ||
        !std::isfinite(pending.manualYawStrength) ||
        pending.manualYawStrength <= 0.0f ||
        pending.manualYawStrength > 1.0f) {
        g_controller->notifyAttitudeWriteSkipped();
        LogFailureRateLimited("manual yaw command has invalid core side or strength");
        return;
    }
    const DWORD now = GetTickCount();
    const DWORD age = static_cast<DWORD>(now - pending.publishTick);
    if (pending.sourceThread == 0 || pending.sourceThread != thread ||
        age > kPendingYawMaximumAgeMs) {
        LogFailureRateLimited("manual yaw command expired or changed thread");
        return;
    }

    VehicleSample sample{};
    VehicleReadDiagnostics vehicleDiagnostics{};
    if (!ReadPlayerVehicle(&sample, &vehicleDiagnostics)) {
        LogVehicleReadFailureRateLimited("manual-yaw-sink",
                                         vehicleDiagnostics);
        return;
    }
    if (!SameIdentity(sample.identity, pending.identity)) {
        LogFailureRateLimited("manual yaw sink identity changed");
        return;
    }
    if (!ManualYawMotionEligible(sample, g_controller->config())) {
        LogFailureRateLimited("manual yaw sink motion eligibility changed");
        return;
    }

    const float bodyOffset = BodyOffsetRad(sample);
    if (PendingYawEnteredTransitionBoundary(
            pending, bodyOffset, g_controller->config())) {
        g_controller->notifyAttitudeWriteSkipped();
        LogFailureRateLimited(
            "manual yaw command revoked at side-transition boundary");
        return;
    }
    const float currentYawRate = BodyYawRateRadS(sample);
    float relativeYawRate = currentYawRate -
                            pending.sourcePathYawRateRadS;
    const float offsetChange = nfsmw_drift::WrapPi(
        bodyOffset - pending.sourceBodyOffsetRad);
    if (std::fabs(offsetChange) > kMinimumOffsetChangeRad) {
        relativeYawRate = offsetChange / dt;
    }
    relativeYawRate = nfsmw_drift::Clamp(
        relativeYawRate, -kMaximumAngularMagnitude,
        kMaximumAngularMagnitude);
    const nfsmw_drift::ManualYawCommand command =
        nfsmw_drift::ComputeManualYawCommand(
            g_controller->config(), bodyOffset, relativeYawRate,
            pending.rawSteering, pending.controlSide, controlDt,
            pending.manualYawStrength);
    if (!IsManualYawMode(command.mode) ||
        command.mode != pending.expectedMode) {
        LogFailureRateLimited("manual yaw mode changed before physics sink");
        return;
    }

    const float yawDelta = nfsmw_drift::Clamp(
        command.yawVelocityDeltaRadS,
        -kMaximumManualYawDeltaRadS,
        kMaximumManualYawDeltaRadS);
    if (!std::isfinite(yawDelta) ||
        std::fabs(yawDelta) <= kMinimumOffsetChangeRad) {
        return;
    }
    const nfsmw_drift::Vec3 up = nfsmw_drift::NormalizeOr(
        sample.body.up, {0.0f, 1.0f, 0.0f});
    const nfsmw_drift::Vec3 nextAngularVelocity =
        sample.angularVelocity + up * yawDelta;
    if (!FiniteBoundedVector(nextAngularVelocity,
                             kMaximumAngularMagnitude)) {
        LogFailureRateLimited("manual yaw candidate exceeded safety bounds");
        return;
    }
    if (!SafeSetAngularVelocity(sample, nextAngularVelocity)) {
        DisableManualYawChannel("setter contract, call, or read-back failed");
        return;
    }
    g_controller->notifyAttitudeWritePlanned(yawDelta);
    LogAppliedYaw(pending, sinkPhysicsSerial, age, bodyOffset,
                  relativeYawRate, command, currentYawRate,
                  currentYawRate + yawDelta);
}

void ConsumeDriverAssistYaw(std::uint64_t sinkPhysicsSerial,
                            float dt,
                            DWORD thread) {
    if (g_driverEscWriterDisabled.load(std::memory_order_acquire) ||
        !std::isfinite(dt) || dt <= 0.0f || dt > kMaximumCadenceDt) {
        return;
    }
    Identity stableIdentity{};
    nfsmw_drift::ControlOutput driftOutput{};
    driver_assist::Output driverOutput{};
    std::uint64_t driverSerial = 0;
    bool runtimeReady = false;
    AcquireSRWLockShared(&g_stateLock);
    stableIdentity = g_stableIdentity;
    driftOutput = g_cachedOutput;
    driverOutput = g_cachedDriverAssistOutput;
    driverSerial = g_cachedDriverAssistSerial;
    runtimeReady = g_runtimeReady;
    ReleaseSRWLockShared(&g_stateLock);

    if (!runtimeReady || driftOutput.active || !driverOutput.modeActive ||
        !driverOutput.escWorking ||
        driverSerial == 0 || driverSerial + 1 != sinkPhysicsSerial ||
        !std::isfinite(driverOutput.yawVelocityDeltaRadS) ||
        std::fabs(driverOutput.yawVelocityDeltaRadS) <=
            kMinimumOffsetChangeRad) {
        return;
    }
    const std::uint64_t inputSerial =
        g_latestInputPhysicsSerial.load(std::memory_order_acquire);
    const std::uintptr_t inputPvehicle =
        g_latestInputPvehicle.load(std::memory_order_acquire);
    if (inputSerial != driverSerial ||
        g_physicsThread.load(std::memory_order_acquire) != thread ||
        g_physicsSerial.load(std::memory_order_acquire) !=
            sinkPhysicsSerial) {
        return;
    }

    VehicleSample sample{};
    if (!ReadPlayerVehicle(&sample, nullptr) ||
        !SameIdentity(stableIdentity, sample.identity) ||
        inputPvehicle != sample.identity.pvehicle) {
        return;
    }
    const float yawDelta = nfsmw_drift::Clamp(
        driverOutput.yawVelocityDeltaRadS,
        -kMaximumManualYawDeltaRadS,
        kMaximumManualYawDeltaRadS);
    const nfsmw_drift::Vec3 up = nfsmw_drift::NormalizeOr(
        sample.body.up, {0.0f, 1.0f, 0.0f});
    const nfsmw_drift::Vec3 nextAngularVelocity =
        sample.angularVelocity + up * yawDelta;
    if (!FiniteBoundedVector(nextAngularVelocity,
                             kMaximumAngularMagnitude)) {
        return;
    }
    if (!SafeSetAngularVelocity(sample, nextAngularVelocity)) {
        g_driverEscWriterDisabled.store(true, std::memory_order_release);
        Log("driver-assist ESC yaw writer disabled after setter or read-back failure; drift yaw remains available");
    }
}

void ConsumeLaunchControl(std::uint64_t sinkPhysicsSerial,
                          DWORD thread) {
    Identity stableIdentity{};
    nfsmw_drift::ControlOutput driftOutput{};
    driver_assist::Output driverOutput{};
    std::uint64_t driverSerial = 0;
    bool runtimeReady = false;
    AcquireSRWLockShared(&g_stateLock);
    stableIdentity = g_stableIdentity;
    driftOutput = g_cachedOutput;
    driverOutput = g_cachedDriverAssistOutput;
    driverSerial = g_cachedDriverAssistSerial;
    runtimeReady = g_runtimeReady;
    ReleaseSRWLockShared(&g_stateLock);

    if (!runtimeReady || driftOutput.active ||
        !driverOutput.modeActive || !driverOutput.lcWorking ||
        driverSerial == 0 || driverSerial + 1 != sinkPhysicsSerial ||
        g_physicsThread.load(std::memory_order_acquire) != thread ||
        g_physicsSerial.load(std::memory_order_acquire) !=
            sinkPhysicsSerial ||
        g_latestInputPhysicsSerial.load(std::memory_order_acquire) !=
            driverSerial) {
        return;
    }

    VehicleSample sample{};
    if (!ReadPlayerVehicle(&sample, nullptr) ||
        !SameIdentity(stableIdentity, sample.identity) ||
        g_latestInputPvehicle.load(std::memory_order_acquire) !=
            sample.identity.pvehicle) {
        return;
    }
    const nfsmw_drift::Vec3 forward = nfsmw_drift::NormalizeOr(
        sample.body.forward, {0.0f, 0.0f, 1.0f});
    const float longitudinal = nfsmw_drift::Dot(
        sample.linearVelocity, forward);
    if (!std::isfinite(longitudinal)) return;
    if (longitudinal > 0.0f) {
        const nfsmw_drift::Vec3 stoppedVelocity =
            sample.linearVelocity - forward * longitudinal;
        (void)SafeSetLinearVelocity(sample, stoppedVelocity);
    }
    (void)SafeStopFrontWheelRotation(sample);
}

DWORD UpdateHandbrakeHoldSnapshot(float handbrake,
                                  float threshold,
                                  std::uintptr_t pvehicle,
                                  DWORD now) {
    const float clamped = nfsmw_drift::Clamp(handbrake, 0.0f, 1.0f);
    DWORD heldMilliseconds = 0;
    if (std::isfinite(threshold) && threshold >= 0.0f && threshold <= 1.0f &&
        clamped >= threshold) {
        DWORD pressedSince =
            g_handbrakePressedSinceTick.load(std::memory_order_acquire);
        const std::uintptr_t previousPvehicle =
            g_latestInputPvehicle.load(std::memory_order_acquire);
        if (pressedSince == 0 || previousPvehicle != pvehicle) {
            pressedSince = now;
            g_handbrakePressedSinceTick.store(pressedSince,
                                               std::memory_order_release);
        }
        if (pressedSince != 0) {
            heldMilliseconds = static_cast<DWORD>(now - pressedSince);
        }
    } else {
        g_handbrakePressedSinceTick.store(0, std::memory_order_release);
    }
    g_latestHandbrakeBits.store(FloatToBits(clamped),
                                std::memory_order_release);
    g_latestHandbrakeHeldMilliseconds.store(heldMilliseconds,
                                             std::memory_order_release);
    return heldMilliseconds;
}

void ProcessInputSample(void* self, bool regularPoll) {
    if (!g_hooksArmed.load(std::memory_order_acquire) ||
        g_permanentFault.load(std::memory_order_acquire) ||
        g_controller == nullptr) {
        return;
    }

    float left = 0.0f;
    float right = 0.0f;
    float handbrake = 0.0f;
    float throttle = 0.0f;
    float brake = 0.0f;
    std::uintptr_t mirror = 0;
    if (!regularPoll) {
        SuspendRuntimeOutputs(true);
        return;
    }
    if (!SnapshotRawInput(self, &left, &right, &handbrake, &throttle,
                          &brake, &mirror)) {
        LogFailureRateLimited("input snapshot validation failed");
        SuspendRuntimeOutputs(true);
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD thread = GetCurrentThreadId();
    const DWORD physicsThread = g_physicsThread.load(std::memory_order_acquire);
    const DWORD physicsTick = g_lastPhysicsTick.load(std::memory_order_acquire);
    const float dt = BitsToFloat(
        g_lastPhysicsDtBits.load(std::memory_order_acquire));
    const std::uint64_t physicsSerial =
        g_physicsSerial.load(std::memory_order_acquire);
    if (physicsThread == 0 || thread != physicsThread || physicsSerial == 0 ||
        static_cast<DWORD>(now - physicsTick) > kPhysicsFreshnessMs ||
        !std::isfinite(dt) || dt <= 0.0f || dt > kMaximumCadenceDt) {
        LogCadenceFailureRateLimited(thread, physicsThread, physicsSerial,
                                     physicsTick, now, dt);
        SuspendRuntimeOutputs(true);
        return;
    }
    const float controlDt = std::min(dt, kMaximumControlDt);

    VehicleSample sample{};
    VehicleReadDiagnostics vehicleDiagnostics{};
    if (!ReadPlayerVehicle(&sample, &vehicleDiagnostics)) {
        LogVehicleReadFailureRateLimited("input", vehicleDiagnostics);
        SuspendRuntimeOutputs(true);
        return;
    }

    // Publish only a fully validated, same-vehicle input sample. The optional
    // rigid-body experiment consumes exactly the preceding physics serial, so
    // menu/loading input cannot be replayed after a vehicle replacement.
    const float handbrakeThreshold =
        g_controller->config().handbrakeActivationThreshold;
    (void)UpdateHandbrakeHoldSnapshot(
        handbrake, handbrakeThreshold, sample.identity.pvehicle, now);
    g_latestThrottleBits.store(FloatToBits(nfsmw_drift::Clamp(throttle, 0.0f,
                                                               1.0f)),
                               std::memory_order_release);
    g_latestBrakeBits.store(FloatToBits(nfsmw_drift::Clamp(brake, 0.0f, 1.0f)),
                            std::memory_order_release);
    g_latestInputPvehicle.store(sample.identity.pvehicle,
                                std::memory_order_release);
    g_latestInputTick.store(now, std::memory_order_release);
    // Publish the serial last so the physics consumer can use it as the
    // release/acquire boundary for the complete snapshot above.
    g_latestInputPhysicsSerial.store(physicsSerial,
                                     std::memory_order_release);

    const float rawSteering = nfsmw_drift::Clamp(right - left, -1.0f, 1.0f);
    const int currentGear = ReadCurrentForwardGear();
    const bool currentManualInput =
        std::fabs(rawSteering) > g_controller->config().directionDeadzone;
    const int currentManualDirection =
        currentManualInput ? (rawSteering < 0.0f ? -1 : 1) : 0;
    bool writeSteering = false;
    float steeringToWrite = rawSteering;
    bool automaticSteeringAllowed = false;
    float effectiveSteeringTarget = rawSteering;
    AcquireSRWLockExclusive(&g_stateLock);
    const bool vehicleProfileChanged =
        sample.vehicleCollectionKey != 0 &&
        sample.vehicleCollectionKey != g_currentVehicleCollectionKey;
    if (!SameIdentity(g_stableIdentity, sample.identity) ||
        vehicleProfileChanged) {
        DisableRearWheelSteeringOverride();
        DisableDriftDriveTorque();
        g_stableIdentity = sample.identity;
        g_probationCount = 1;
        g_lastProbationSerial = physicsSerial;
        g_lastControllerSerial = 0;
        g_latestInputSequence = 0;
        g_runtimeReady = false;
        g_cachedOutput = nfsmw_drift::ControlOutput{};
        g_cachedOutputSerial = 0;
        g_driverAssistController.Reset(true);
        g_cachedDriverAssistOutput = driver_assist::Output{};
        g_cachedDriverAssistSerial = 0;
        g_manualInputObservedSinceUpdate = false;
        g_manualDirectionMaskSinceUpdate = 0;
        g_pendingManualYaw = PendingManualYaw{};
        g_steeringResponseLimiter.Reset();
        g_steeringAutomaticOwnership = false;
        g_steeringInputHistory.Reset();
        InvalidateAppliedSteeringSnapshot();
        g_steeringInputHistory.Observe(rawSteering, physicsSerial);
        ApplyVehicleCountersteerConfigLocked(
            sample.countersteerCollectionKey,
            sample.vehicleCollectionKey);
        g_controller->reset();
        const bool recoveryProbation = g_hasCompletedInitialProbation;
        const std::uint32_t requiredSamples =
            recoveryProbation
                ? player_vehicle_selection::kRecoveryProbationSamples
                : kProbationSamples;
        ReleaseSRWLockExclusive(&g_stateLock);
        ClearDriftCameraTarget();
        driver_assist_hud::Publish(false, false, false, false, false, false,
                                   now);
        char reason[192]{};
        std::snprintf(
            reason, sizeof(reason),
            "%s changed; starting %u-sample %s probation",
            vehicleProfileChanged ? "player vehicle profile"
                                  : "player identity",
            requiredSamples,
            recoveryProbation ? "recovery" : "initial");
        LogFailureRateLimited(reason);
        return;
    }

    const std::uint64_t lastObservedSerial =
        g_runtimeReady ? g_lastControllerSerial : g_lastProbationSerial;
    if (lastObservedSerial != 0 && physicsSerial < lastObservedSerial) {
        DisableRearWheelSteeringOverride();
        g_stableIdentity = Identity{};
        g_probationCount = 0;
        g_lastProbationSerial = 0;
        g_lastControllerSerial = 0;
        g_latestInputSequence = 0;
        g_runtimeReady = false;
        g_cachedOutput = nfsmw_drift::ControlOutput{};
        g_cachedOutputSerial = 0;
        g_driverAssistController.Reset(true);
        g_cachedDriverAssistOutput = driver_assist::Output{};
        g_cachedDriverAssistSerial = 0;
        g_manualInputObservedSinceUpdate = false;
        g_manualDirectionMaskSinceUpdate = 0;
        g_pendingManualYaw = PendingManualYaw{};
        g_steeringResponseLimiter.Reset();
        g_steeringAutomaticOwnership = false;
        g_steeringInputHistory.Reset();
        InvalidateAppliedSteeringSnapshot();
        g_currentVehicleCollectionKey = 0;
        g_controller->reset();
        ReleaseSRWLockExclusive(&g_stateLock);
        ClearDriftCameraTarget();
        driver_assist_hud::Publish(false, false, false, false, false, false,
                                   now);
        LogFailureRateLimited("physics sequence moved backwards; restarting probation");
        return;
    }

    g_steeringInputHistory.Observe(rawSteering, physicsSerial);

    if (!g_runtimeReady) {
        const std::uint32_t requiredSamples =
            g_hasCompletedInitialProbation
                ? player_vehicle_selection::kRecoveryProbationSamples
                : kProbationSamples;
        if (physicsSerial != g_lastProbationSerial &&
            g_probationCount < requiredSamples) {
            ++g_probationCount;
            g_lastProbationSerial = physicsSerial;
        }
        if (g_probationCount < requiredSamples) {
            ReleaseSRWLockExclusive(&g_stateLock);
            return;
        }
        g_runtimeReady = true;
        const bool recovered = g_hasCompletedInitialProbation;
        g_hasCompletedInitialProbation = true;
        g_controller->reset();
        g_driverAssistController.Reset(true);
        g_cachedDriverAssistOutput = driver_assist::Output{};
        g_cachedDriverAssistSerial = 0;
        g_pendingManualYaw = PendingManualYaw{};
        EnsureDriftCameraHookInstalled();
        Log(recovered
                ? "alpha recovery probation complete: cadence and player identity are stable"
                : "alpha initial probation complete: cadence and player identity are stable");
    }
    const float sessionStartSteering =
        g_steeringInputHistory.PreviousCommand(physicsSerial, rawSteering);

    if (currentManualInput) {
        g_manualInputObservedSinceUpdate = true;
        g_manualDirectionMaskSinceUpdate |=
            nfsmw_drift::SteeringDirectionObservationMask(
                currentManualDirection);
    }

    // The input poll may run more than once per physics frame. Timers and the
    // controller advance once per physics serial, but every poll gets a fresh
    // chance to revoke automatic steering.
    if (g_lastControllerSerial == physicsSerial) {
        const nfsmw_drift::AssistConfig& config = g_controller->config();
        const float longitudinalSpeed = nfsmw_drift::Dot(
            sample.linearVelocity, sample.body.forward);
        // Ground-contact loss is owned by the controller's active-session
        // grace timer. Repeated input polls within one physics frame must not
        // bypass that timer by resetting the controller immediately.
        const bool motionEligible =
            sample.speedMps >= config.minSpeedMps &&
            (config.allowReverse || longitudinalSpeed > 0.0f);
        if (!motionEligible) {
            DisableRearWheelSteeringOverride();
            g_cachedOutput = nfsmw_drift::ControlOutput{};
            g_cachedOutputSerial = 0;
            g_manualInputObservedSinceUpdate = false;
            g_manualDirectionMaskSinceUpdate = 0;
            g_pendingManualYaw = PendingManualYaw{};
            g_steeringResponseLimiter.Reset();
            g_steeringAutomaticOwnership = false;
            g_controller->reset();
            ClearDriftCameraTarget();
        } else if ((currentManualInput ||
                    g_manualInputObservedSinceUpdate ||
                    g_manualDirectionMaskSinceUpdate != 0) &&
                   !AutomaticOverrideAllowed(
                       currentManualInput,
                       g_manualInputObservedSinceUpdate,
                       g_manualDirectionMaskSinceUpdate,
                       g_cachedOutput.driftSide,
                       g_cachedOutput.smartCountersteerActive,
                       g_cachedOutput.forceCountersteer)) {
            nfsmw_drift::ControlOutput manualOutput = g_cachedOutput;
            manualOutput.forceCountersteer = false;
            manualOutput.smartCountersteerActive = false;
            manualOutput.manualSteeringOverride = true;
            manualOutput.steeringAssistMode =
                nfsmw_drift::SteeringAssistMode::ManualOverride;
            manualOutput.smartCountersteerCommand = 0.0f;
            manualOutput.steeringCommand = rawSteering;
            manualOutput.steeringDelta = 0.0f;
            ObserveManualYawInputLocked(
                &manualOutput, sample, rawSteering, controlDt, physicsSerial,
                thread, now);
            PrepareDriftSteeringWriteLocked(
                manualOutput, rawSteering, sessionStartSteering,
                currentManualInput, g_manualInputObservedSinceUpdate,
                g_manualDirectionMaskSinceUpdate,
                dt, physicsSerial,
                &writeSteering, &steeringToWrite,
                &automaticSteeringAllowed,
                &effectiveSteeringTarget);
            PublishDriftCameraTarget(manualOutput);
            PublishRearWheelSteeringLocked(
                sample, rawSteering, g_cachedDriverAssistOutput,
                manualOutput, now);
            LogTelemetry(manualOutput, rawSteering, handbrake,
                         steeringToWrite, writeSteering,
                         automaticSteeringAllowed,
                         effectiveSteeringTarget,
                         g_manualInputObservedSinceUpdate,
                         g_manualDirectionMaskSinceUpdate);
        } else {
            ObserveManualYawInputLocked(
                &g_cachedOutput, sample, rawSteering, controlDt,
                physicsSerial,
                thread, now);
            PrepareDriftSteeringWriteLocked(
                g_cachedOutput, rawSteering, sessionStartSteering,
                currentManualInput, g_manualInputObservedSinceUpdate,
                g_manualDirectionMaskSinceUpdate,
                dt, physicsSerial,
                &writeSteering, &steeringToWrite,
                &automaticSteeringAllowed,
                &effectiveSteeringTarget);
            PublishDriftCameraTarget(g_cachedOutput);
            PublishRearWheelSteeringLocked(
                sample, rawSteering, g_cachedDriverAssistOutput,
                g_cachedOutput, now);
            PublishDriftDriveTorqueLocked(sample, g_cachedOutput, now);
            LogTelemetry(g_cachedOutput, rawSteering, handbrake,
                         steeringToWrite, writeSteering,
                         automaticSteeringAllowed,
                         effectiveSteeringTarget,
                         g_manualInputObservedSinceUpdate,
                         g_manualDirectionMaskSinceUpdate);
        }
        ReleaseSRWLockExclusive(&g_stateLock);
        if (writeSteering &&
            !SafeWriteSteeringRows(mirror, steeringToWrite, left, right,
                                   handbrake)) {
            g_permanentFault.store(true, std::memory_order_release);
            ResetRuntimeState(true);
            Log("alpha permanently disabled: steering mirror write/read-back failed");
            return;
        }
        // Commit after the optional memory write succeeds.  When the bridge
        // deliberately passes through, steeringToWrite is the validated raw
        // command that the original game poll already applied.
        PublishAppliedSteeringSnapshot(sample.identity.pvehicle,
                                       physicsSerial, steeringToWrite);
        return;
    }
    g_lastControllerSerial = physicsSerial;

    nfsmw_drift::VehicleState state{};
    state.dt = controlDt;
    state.steeringOwnershipDt = dt;
    state.body = sample.body;
    state.linearVelocityWorld = sample.linearVelocity;
    state.angularVelocityWorld = sample.angularVelocity;
    state.surfaceNormal = sample.body.up;
    state.hasSurfaceNormal = true;
    state.speedMps = sample.speedMps;
    state.steeringInput = rawSteering;
    const bool manualInputObservedForUpdate =
        g_manualInputObservedSinceUpdate;
    state.manualSteeringInputObserved = manualInputObservedForUpdate;
    state.manualSteeringDirectionMaskObserved =
        g_manualDirectionMaskSinceUpdate;
    state.handbrakeInput = nfsmw_drift::Clamp(handbrake, 0.0f, 1.0f);
    state.groundedWheels =
        static_cast<std::uint8_t>(sample.groundedWheels);
    state.playerControlled = true;
    // ReadPlayerVehicle already established a unique controllable player
    // vehicle. Ground contacts are a controller eligibility signal, not proof
    // that the race ended; treating them as inRace would erase the rearm latch.
    state.inRace = true;
    state.attitudeWriteAvailable =
        !g_yawPermanentFault.load(std::memory_order_acquire) &&
        g_controller->config().enableManualYawAssist;

    nfsmw_drift::ControlOutput output = g_controller->update(state);
    UpdateDriverAssistLocked(sample, output, rawSteering, brake, throttle,
                             handbrake,
                             controlDt, currentGear, physicsSerial, now);
    PublishRearWheelSteeringLocked(
        sample, rawSteering, g_cachedDriverAssistOutput, output, now);
    PublishDriftDriveTorqueLocked(sample, output, now);
    ObserveManualYawInputLocked(
        &output, sample, rawSteering, controlDt, physicsSerial, thread, now);
    g_manualInputObservedSinceUpdate = false;
    g_manualDirectionMaskSinceUpdate = 0;
    g_cachedOutput = output;
    g_cachedOutputSerial = physicsSerial;
    PrepareDriftSteeringWriteLocked(
        output, rawSteering, sessionStartSteering,
        currentManualInput, manualInputObservedForUpdate,
        state.manualSteeringDirectionMaskObserved,
        dt, physicsSerial,
        &writeSteering, &steeringToWrite,
        &automaticSteeringAllowed,
        &effectiveSteeringTarget);
    PublishDriftCameraTarget(output);
    LogTelemetry(output, rawSteering, handbrake,
                 steeringToWrite, writeSteering,
                 automaticSteeringAllowed,
                 effectiveSteeringTarget,
                 manualInputObservedForUpdate,
                 state.manualSteeringDirectionMaskObserved);
    ReleaseSRWLockExclusive(&g_stateLock);

    if (writeSteering &&
        !SafeWriteSteeringRows(mirror, steeringToWrite, left, right,
                               handbrake)) {
        g_permanentFault.store(true, std::memory_order_release);
        ResetRuntimeState(true);
        Log("alpha permanently disabled: steering mirror write/read-back failed");
        return;
    }
    // Publish only after SafeWriteSteeringRows has read back the rows.  The
    // physics sink then sees the exact command, including limiter output and
    // automatic countersteer, for this input serial.
    PublishAppliedSteeringSnapshot(sample.identity.pvehicle,
                                   physicsSerial, steeringToWrite);
}

void NFSMW_FASTCALL InputPollDetour(void* self, void* /*unusedEdx*/) {
    const InputPollFn original =
        g_inputOriginal.load(std::memory_order_acquire);
    // The rebind flag can be cleared by the alternate original path, so this
    // guard must be captured before chaining so only the normal current-row
    // refresh is accepted as a trustworthy input sample.
    const bool regularPoll = RegularPollPrecondition(self);
    original(self, nullptr);
    ProcessInputSample(self, regularPoll);
}

void NFSMW_CDECL ActiveComponentsDetour(float dt) {
    const ActiveComponentsFn original =
        g_activeOriginal.load(std::memory_order_acquire);
    const DWORD thread = GetCurrentThreadId();
    const std::uint64_t sinkPhysicsSerial =
        g_physicsSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_physicsThread.store(thread, std::memory_order_release);
    g_lastPhysicsDtBits.store(FloatToBits(dt), std::memory_order_release);
    g_lastPhysicsTick.store(GetTickCount(), std::memory_order_release);

    if (handling_probe::WantsFrame(thread)) {
        VehicleSample probeSample{};
        if (ReadPlayerVehicle(&probeSample, nullptr)) {
            bool driftActive = false;
            AcquireSRWLockShared(&g_stateLock);
            driftActive = g_runtimeReady &&
                          SameIdentity(g_stableIdentity,
                                       probeSample.identity) &&
                          g_cachedOutputSerial + 1 ==
                              sinkPhysicsSerial &&
                          g_cachedOutput.active;
            ReleaseSRWLockShared(&g_stateLock);

            handling_probe::PlayerFrame probeFrame{};
            probeFrame.pvehicle = probeSample.identity.pvehicle;
            probeFrame.player = probeSample.identity.player;
            probeFrame.rigidBody = probeSample.identity.rigidBody;
            probeFrame.rigidBodyHolder = probeSample.identity.holder;
            probeFrame.rigidBodyInner = probeSample.identity.inner;
            probeFrame.physicsSerial = sinkPhysicsSerial;
            probeFrame.physicsThread = thread;
            probeFrame.groundedWheels = probeSample.groundedWheels;
            probeFrame.driftActive = driftActive;
#if NFSMW_ENABLE_RWS_FORCE_TRACE
            probeFrame.speedMps = probeSample.speedMps;
            probeFrame.collectionKey =
                probeSample.rearSteeringVisualCollectionKey;
            const std::uint64_t inputSerial =
                g_latestSteeringPhysicsSerial.load(std::memory_order_acquire);
            if (inputSerial + 1 == sinkPhysicsSerial &&
                g_latestSteeringPvehicle.load(std::memory_order_acquire) ==
                    probeFrame.pvehicle) {
                probeFrame.steeringCommand = BitsToFloat(
                    g_latestAppliedSteeringBits.load(std::memory_order_acquire));
            }
            AcquireSRWLockShared(&g_stateLock);
            probeFrame.driverAssistActive = g_runtimeReady &&
                SameIdentity(g_stableIdentity, probeSample.identity) &&
                g_cachedDriverAssistSerial + 1 == sinkPhysicsSerial &&
                g_cachedDriverAssistOutput.modeActive;
            ReleaseSRWLockShared(&g_stateLock);
#endif
            handling_probe::BeginFrame(probeFrame);
        } else {
            handling_probe::InvalidateFrame();
        }
    }

    original(dt);

    // The read-only handling window covers only this exact, validated physics
    // call. Closing it before any accepted yaw work prevents stale objects
    // from being sampled during loading or vehicle reconstruction.
    handling_probe::EndFrame();

    // Loading and vehicle reconstruction can make the original physics call
    // take longer than the freshness window. Refresh after it returns
    // so the next input poll is judged against the completed physics sample.
    g_physicsThread.store(thread, std::memory_order_release);
    g_lastPhysicsDtBits.store(FloatToBits(dt), std::memory_order_release);
    g_lastPhysicsTick.store(GetTickCount(), std::memory_order_release);

    if (g_hooksArmed.load(std::memory_order_acquire) &&
        !g_permanentFault.load(std::memory_order_acquire)) {
#if NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT
        ApplyRigidBodyForce(sinkPhysicsSerial, dt, thread);
#endif
        ConsumeLaunchControl(sinkPhysicsSerial, thread);
        ConsumePendingManualYaw(sinkPhysicsSerial, dt, thread);
        ConsumeDriverAssistYaw(sinkPhysicsSerial, dt, thread);
    }
}

bool ValidateInstallLayout(std::uintptr_t inputPollTarget,
                           std::uintptr_t activeComponentsTarget,
                           std::uintptr_t angularVelocitySetterTarget,
                           std::uintptr_t validatedTextStart,
                           std::size_t validatedTextSize) {
    HMODULE module = GetModuleHandleA(nullptr);
    std::uintptr_t moduleBase = 0;
    std::size_t imageSize = 0;
    if (module == nullptr ||
        !nfsmw_main_module_range(&moduleBase, &imageSize) ||
        moduleBase != reinterpret_cast<std::uintptr_t>(module) ||
        moduleBase != target_profile::kExpectedImageBase || imageSize == 0 ||
        inputPollTarget != moduleBase + kInputPollRva ||
        activeComponentsTarget != moduleBase + kActiveComponentsRva ||
        angularVelocitySetterTarget !=
            moduleBase + kSetAngularVelocityRva) {
        return false;
    }
    if (handling_probe_state::ValidateTextRange(
            moduleBase, imageSize, validatedTextStart,
            validatedTextSize) !=
            handling_probe_state::TextRangeFailure::None ||
        !handling_probe_state::TextRangeContains(
            validatedTextStart, validatedTextSize, inputPollTarget, 1) ||
        !handling_probe_state::TextRangeContains(
            validatedTextStart, validatedTextSize,
            activeComponentsTarget, 1) ||
        !handling_probe_state::TextRangeContains(
            validatedTextStart, validatedTextSize,
            angularVelocitySetterTarget, 1)) {
        return false;
    }
    g_imageBase.store(moduleBase, std::memory_order_release);
    g_imageSize.store(imageSize, std::memory_order_release);
    g_inputPollTarget.store(inputPollTarget, std::memory_order_release);

    std::uintptr_t table = 0;
    std::uintptr_t setterSlotAddress = 0;
    std::uintptr_t setterSlotTarget = 0;
    std::uintptr_t pvehicleVtable = 0;
    std::uintptr_t simableVtable = 0;
    std::uintptr_t playerVtable = 0;
    std::uintptr_t getPlayerSlotAddress = 0;
    std::uintptr_t isPlayerSlotAddress = 0;
    std::uintptr_t isOwnedSlotAddress = 0;
    std::uintptr_t getPlayerSlotTarget = 0;
    std::uintptr_t isPlayerSlotTarget = 0;
    std::uintptr_t isOwnedSlotTarget = 0;
    std::uintptr_t expectedGetPlayerTarget = 0;
    std::uintptr_t expectedIsPlayerTarget = 0;
    std::uintptr_t expectedIsOwnedTarget = 0;
    std::uintptr_t playerGetSimableSlotAddress = 0;
    std::uintptr_t playerGetSimableSlotTarget = 0;
    std::uintptr_t expectedPlayerGetSimableTarget = 0;
    return ExecutableAddress(reinterpret_cast<const void*>(inputPollTarget)) &&
           ExecutableAddress(
               reinterpret_cast<const void*>(activeComponentsTarget)) &&
           AddAddress(moduleBase, kPVehicleInstancesRva, &table) &&
           ImageRange(reinterpret_cast<const void*>(table),
                      kPVehicleInstanceLimit * kPVehicleInstanceStride) &&
           ImageRange(reinterpret_cast<const void*>(
                           moduleBase + kRawInputVtableRva),
                       (kInputPollVtableSlot + 1) *
                           sizeof(std::uintptr_t)) &&
            ImageRange(reinterpret_cast<const void*>(
                            moduleBase + kRigidBodyVtableRva),
                        (std::max(kSetAngularVelocityVtableSlot,
                                  kRigidBodyAccelerateVtableSlot) +
                         1) * sizeof(std::uintptr_t)) &&
           AddAddress(moduleBase, kPVehiclePrimaryVtableRva,
                      &pvehicleVtable) &&
           ReadOnlyImageRange(
               reinterpret_cast<const void*>(pvehicleVtable),
               6 * sizeof(std::uintptr_t)) &&
           AddAddress(moduleBase, kPVehicleSimableVtableRva,
                      &simableVtable) &&
           ReadOnlyImageRange(
               reinterpret_cast<const void*>(simableVtable),
               (kSimableIsOwnedByPlayerVtableSlot + 1) *
                   sizeof(std::uintptr_t)) &&
           AddAddress(simableVtable,
                      kSimableGetPlayerVtableSlot *
                          sizeof(std::uintptr_t),
                      &getPlayerSlotAddress) &&
           AddAddress(simableVtable,
                      kSimableIsPlayerVtableSlot *
                          sizeof(std::uintptr_t),
                      &isPlayerSlotAddress) &&
           AddAddress(simableVtable,
                      kSimableIsOwnedByPlayerVtableSlot *
                          sizeof(std::uintptr_t),
                      &isOwnedSlotAddress) &&
           SafeReadPointer(
               reinterpret_cast<const void*>(getPlayerSlotAddress),
               &getPlayerSlotTarget) &&
           SafeReadPointer(
               reinterpret_cast<const void*>(isPlayerSlotAddress),
               &isPlayerSlotTarget) &&
           SafeReadPointer(
               reinterpret_cast<const void*>(isOwnedSlotAddress),
               &isOwnedSlotTarget) &&
           AddAddress(moduleBase, kSimableGetPlayerRva,
                      &expectedGetPlayerTarget) &&
           AddAddress(moduleBase, kSimableIsPlayerRva,
                      &expectedIsPlayerTarget) &&
           AddAddress(moduleBase, kSimableIsOwnedByPlayerRva,
                      &expectedIsOwnedTarget) &&
           getPlayerSlotTarget == expectedGetPlayerTarget &&
           isPlayerSlotTarget == expectedIsPlayerTarget &&
           isOwnedSlotTarget == expectedIsOwnedTarget &&
           ExecutableImageAddress(
               reinterpret_cast<const void*>(getPlayerSlotTarget)) &&
           ExecutableImageAddress(
               reinterpret_cast<const void*>(isPlayerSlotTarget)) &&
           ExecutableImageAddress(
               reinterpret_cast<const void*>(isOwnedSlotTarget)) &&
           SafeImageBytesEqual(
               getPlayerSlotTarget, kSimableGetPlayerSignature,
               sizeof(kSimableGetPlayerSignature)) &&
           SafeImageBytesEqual(
               isPlayerSlotTarget, kSimableIsPlayerSignature,
               sizeof(kSimableIsPlayerSignature)) &&
           SafeImageBytesEqual(
               isOwnedSlotTarget, kSimableIsOwnedByPlayerSignature,
               sizeof(kSimableIsOwnedByPlayerSignature)) &&
           AddAddress(moduleBase, kPlayerVtableRva, &playerVtable) &&
           ReadOnlyImageRange(
               reinterpret_cast<const void*>(playerVtable),
               (kPlayerGetSimableVtableSlot + 1) *
                   sizeof(std::uintptr_t)) &&
           AddAddress(playerVtable,
                      kPlayerGetSimableVtableSlot *
                          sizeof(std::uintptr_t),
                      &playerGetSimableSlotAddress) &&
           SafeReadPointer(
               reinterpret_cast<const void*>(playerGetSimableSlotAddress),
               &playerGetSimableSlotTarget) &&
           AddAddress(moduleBase, kPlayerGetSimableRva,
                      &expectedPlayerGetSimableTarget) &&
           playerGetSimableSlotTarget ==
               expectedPlayerGetSimableTarget &&
           ExecutableImageAddress(reinterpret_cast<const void*>(
               playerGetSimableSlotTarget)) &&
           SafeImageBytesEqual(
               playerGetSimableSlotTarget,
               kPlayerGetSimableSignature,
               sizeof(kPlayerGetSimableSignature)) &&
           AddAddress(moduleBase + kRigidBodyVtableRva,
                      kSetAngularVelocityVtableSlot *
                          sizeof(std::uintptr_t),
                      &setterSlotAddress) &&
           SafeReadPointer(
               reinterpret_cast<const void*>(setterSlotAddress),
               &setterSlotTarget) &&
           setterSlotTarget == angularVelocitySetterTarget;
}

#endif  // NFSMW_ENABLE_ALPHA_BRIDGE

}  // namespace

bool Install(std::uintptr_t inputPollTarget,
             std::uintptr_t activeComponentsTarget,
             std::uintptr_t angularVelocitySetterTarget,
             std::uintptr_t validatedTextStart,
             std::size_t validatedTextSize,
             nfsmw_drift::DriftAssistController* controller,
             const vehicle_countersteer::Registry* vehicleMultipliers,
             const rear_wheel_steering::Registry* rearSteeringVehicles) {
#if !NFSMW_ENABLE_ALPHA_BRIDGE
    (void)inputPollTarget;
    (void)activeComponentsTarget;
    (void)angularVelocitySetterTarget;
    (void)validatedTextStart;
    (void)validatedTextSize;
    (void)controller;
    (void)vehicleMultipliers;
    (void)rearSteeringVehicles;
    return false;
#else
    if (controller == nullptr || !controller->config().enabled ||
        controller->config().activation !=
            nfsmw_drift::ActivationMode::HandbrakeHold ||
        controller->config().actuation !=
            nfsmw_drift::ActuationMode::SteeringAndAttitude ||
        !controller->config().useVelocityRelativeDriftAngle ||
        !ValidateInstallLayout(inputPollTarget, activeComponentsTarget,
                               angularVelocitySetterTarget,
                               validatedTextStart,
                               validatedTextSize)) {
        Log("alpha disabled: configuration, targets, or fixed layout validation failed");
        return false;
    }
#if NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT
    // This optional side channel is independently fail-closed. A mismatch in
    // the experimental Accelerate ABI must never prevent steering/yaw hooks
    // from installing or turn into the primary bridge's permanent fault.
    (void)ValidateRigidBodyAccelStaticLayout();
#else
    g_rigidbodyAccelStaticValid.store(false, std::memory_order_release);
    g_rigidbodyAccelerate.store(nullptr, std::memory_order_release);
#endif
    g_controller = controller;
    g_baseControllerConfig = controller->config();
    g_vehicleCountersteerMultipliers =
        vehicleMultipliers != nullptr
            ? *vehicleMultipliers
            : vehicle_countersteer::Registry{};
#if NFSMW_ENABLE_REAR_WHEEL_STEERING
    g_rearWheelSteeringVehicles =
        rearSteeringVehicles != nullptr
            ? *rearSteeringVehicles
            : rear_wheel_steering::Registry{};
#else
    (void)rearSteeringVehicles;
    g_rearWheelSteeringVehicles = {};
#endif
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
    LogRearWheelSteerConfigDiagnostic();
#endif
    g_currentVehicleCollectionKey = 0;
    g_setAngularVelocity.store(
        reinterpret_cast<SetAngularVelocityFn>(
            angularVelocitySetterTarget),
        std::memory_order_release);
    g_hooksArmed.store(false, std::memory_order_release);
    g_permanentFault.store(false, std::memory_order_release);
    g_yawPermanentFault.store(false, std::memory_order_release);
    g_driverEscWriterDisabled.store(false, std::memory_order_release);
    g_rigidbodyAccelWriterDisabled.store(false, std::memory_order_release);
    g_latestThrottleBits.store(0, std::memory_order_release);
    g_latestBrakeBits.store(0, std::memory_order_release);
    g_latestInputPhysicsSerial.store(0, std::memory_order_release);
    g_latestInputPvehicle.store(0, std::memory_order_release);
    g_latestInputTick.store(0, std::memory_order_release);
    InvalidateAppliedSteeringSnapshot();
    g_rigidbodyAccelRampBits.store(0, std::memory_order_release);
    g_lastRigidbodyTelemetryLogTick.store(0, std::memory_order_release);
    g_lastDriverAssistTelemetryLogTick.store(0,
                                              std::memory_order_release);
    g_hasCompletedInitialProbation = false;
    g_lastCadenceFailureLogTick.store(0, std::memory_order_release);
    g_cameraTargetSide.store(0, std::memory_order_release);
    g_cameraHookAttempted.store(false, std::memory_order_release);
    g_cameraHookInstalled.store(false, std::memory_order_release);
    g_cameraLookAtOriginal.store(nullptr, std::memory_order_release);
    g_cameraAllowedReturnAddress.store(0, std::memory_order_release);
    g_rearWheelSteerOriginal.store(nullptr, std::memory_order_release);
    g_rearWheelSteerHookTarget.store(0, std::memory_order_release);
    g_rearWheelSteerHookAttempted.store(false,
                                         std::memory_order_release);
    g_rearWheelSteerHookInstalled.store(false,
                                         std::memory_order_release);
    g_rearWheelSteerPermanentFault.store(false,
                                          std::memory_order_release);
    DisableRearWheelSteeringOverride();
#if NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
    g_wheelSteerProbeSuspension.store(0, std::memory_order_release);
#endif
    g_driveTorqueOriginal.store(nullptr, std::memory_order_release);
    g_driveTorqueHookAttempted.store(false, std::memory_order_release);
    g_driveTorqueHookInstalled.store(false, std::memory_order_release);
    g_driveTorqueCalls.store(0, std::memory_order_release);
    g_driveTorqueLastSelf.store(0, std::memory_order_release);
    g_driveTorquePlayerCalls.store(0, std::memory_order_release);
    g_driveTorqueActiveCalls.store(0, std::memory_order_release);
    g_driveTorquePositiveCalls.store(0, std::memory_order_release);
    g_driveTorqueForwardCalls.store(0, std::memory_order_release);
    g_driveTorqueBoosts.store(0, std::memory_order_release);
    DisableDriftDriveTorque();
    ResetDriftCameraState();
    ResetRuntimeState(true);

    const MH_STATUS initializeStatus = MH_Initialize();
    if (initializeStatus != MH_OK &&
        initializeStatus != MH_ERROR_ALREADY_INITIALIZED) {
        char message[256]{};
        std::snprintf(message, sizeof(message),
                      "alpha disabled: MinHook initialization failed (%s)",
                      MH_StatusToString(initializeStatus));
        Log(message);
        return false;
    }

    void* const inputTarget = reinterpret_cast<void*>(inputPollTarget);
    void* const activeTarget = reinterpret_cast<void*>(activeComponentsTarget);
    void* inputOriginalRaw = nullptr;
    void* activeOriginalRaw = nullptr;
    const MH_STATUS inputCreate = MH_CreateHook(
        inputTarget, reinterpret_cast<void*>(&InputPollDetour),
        &inputOriginalRaw);
    if (inputCreate != MH_OK || inputOriginalRaw == nullptr) {
        const MH_STATUS cleanup = inputCreate == MH_OK
                                      ? MH_RemoveHook(inputTarget)
                                      : MH_ERROR_NOT_CREATED;
        char message[320]{};
        std::snprintf(message, sizeof(message),
                      "alpha disabled: input hook creation failed (%s), cleanup=%s",
                      MH_StatusToString(inputCreate),
                      MH_StatusToString(cleanup));
        Log(message);
        return false;
    }

    const MH_STATUS activeCreate = MH_CreateHook(
        activeTarget, reinterpret_cast<void*>(&ActiveComponentsDetour),
        &activeOriginalRaw);
    if (activeCreate != MH_OK || activeOriginalRaw == nullptr) {
        const MH_STATUS activeCleanup = activeCreate == MH_OK
                                            ? MH_RemoveHook(activeTarget)
                                            : MH_ERROR_NOT_CREATED;
        const MH_STATUS inputCleanup = MH_RemoveHook(inputTarget);
        char message[384]{};
        std::snprintf(
            message, sizeof(message),
            "alpha disabled: physics hook creation failed (%s), cleanup input=%s physics=%s",
            MH_StatusToString(activeCreate), MH_StatusToString(inputCleanup),
            MH_StatusToString(activeCleanup));
        Log(message);
        return false;
    }

    // Both entries are still disabled. Publish both trampolines before either
    // detour can execute, and never clear them after an ApplyQueued attempt.
    g_inputOriginal.store(reinterpret_cast<InputPollFn>(inputOriginalRaw),
                          std::memory_order_release);
    g_activeOriginal.store(
        reinterpret_cast<ActiveComponentsFn>(activeOriginalRaw),
        std::memory_order_release);

    const MH_STATUS inputQueue = MH_QueueEnableHook(inputTarget);
    const MH_STATUS activeQueue = MH_QueueEnableHook(activeTarget);
    if (inputQueue != MH_OK || activeQueue != MH_OK) {
        const MH_STATUS inputCleanup = MH_RemoveHook(inputTarget);
        const MH_STATUS activeCleanup = MH_RemoveHook(activeTarget);
        g_inputOriginal.store(nullptr, std::memory_order_release);
        g_activeOriginal.store(nullptr, std::memory_order_release);
        char message[448]{};
        std::snprintf(
            message, sizeof(message),
            "alpha disabled: hook queue failed input=%s physics=%s, cleanup input=%s physics=%s",
            MH_StatusToString(inputQueue), MH_StatusToString(activeQueue),
            MH_StatusToString(inputCleanup), MH_StatusToString(activeCleanup));
        Log(message);
        return false;
    }

    const MH_STATUS apply = MH_ApplyQueued();
    if (apply != MH_OK) {
        // ApplyQueued may have enabled only one target. Retain both entries and
        // both trampolines for process life; any survivor stays pass-through.
        const MH_STATUS inputDisable = MH_QueueDisableHook(inputTarget);
        const MH_STATUS activeDisable = MH_QueueDisableHook(activeTarget);
        const MH_STATUS rollback = MH_ApplyQueued();
        char message[448]{};
        std::snprintf(
            message, sizeof(message),
            "alpha disarmed: atomic hook enable failed (%s), retained pass-through rollback input=%s physics=%s apply=%s",
            MH_StatusToString(apply), MH_StatusToString(inputDisable),
            MH_StatusToString(activeDisable), MH_StatusToString(rollback));
        Log(message);
        // Keep the module resident even though writes remain disarmed.
        return true;
    }

    g_hooksArmed.store(true, std::memory_order_release);
    driver_assist_hud::Install();
    // Handling evidence is an optional read-only side channel. Its own
    // profile checks and hook failures are deliberately excluded from the
    // primary bridge's armed state.
    (void)handling_probe::Install(
        g_imageBase.load(std::memory_order_acquire),
        g_imageSize.load(std::memory_order_acquire),
        validatedTextStart, validatedTextSize);
    char message[840]{};
    std::snprintf(
        message, sizeof(message),
        "functional release 1.3.8 installed input=%p physics=%p yaw_setter=%p; 60-sample initial and 3-sample recovery probation, enabled-slot-aware bidirectional player-vehicle qualification, restored distance-proportional 0.9.5 fixed-rate automatic and manual steering with immediate any-direction manual ownership arbitration, next-physics manual yaw sink, and three-degree camera chaining with 2.0-second entry and 1.25-second return or side changes active; active drift contact-loss grace is 3.0 seconds; front-grip and driven-wheel-power writers disabled; integrated rigid-body acceleration writer is build-controlled (5.25 m/s^2 nominal, 61.2%% release scale, 12.5%% applied-wheel-angle countersteer deflection, total-speed attenuation); non-drift ABS/ESC/TCS/LC driver assistance and CustomHud-synchronized native-HUD-visibility DXVK function-detour HUD active; 512-entry vehicle countersteer registry active; true rear-wheel visual and tire-direction steering is build-controlled",
        reinterpret_cast<void*>(inputPollTarget),
        reinterpret_cast<void*>(activeComponentsTarget),
        reinterpret_cast<void*>(angularVelocitySetterTarget));
    Log(message);
    Log("release scope: smart countersteer starts at 15 degrees with a per-vehicle multiple of the 55-degree base target and immediately yields to any valid manual direction (including added countersteer); automatic and manual steering use the restored distance-proportional 0.9.5 fixed-rate path, and neutral ownership does not erase the automatic trajectory; manual input may request a two-second ramped same-direction yaw assist or bounded opposite-direction recovery at 75-percent strength; the drift camera uses the corrected direction with a three-degree target, a 2.0-second entry, and 1.25-second return or side changes; pending old-side yaw is revoked at the pendulum transition boundary; an active handbrake drift tolerates contact loss for three continuous seconds while activation still requires two grounded wheels; driver ESC recovery also reacts to validated four-wheel slip or more than ten degrees of body offset, uses fifteen-percent stronger base authority, and smoothly scales with body offset to two times at 35 degrees; the assist HUD preserves the forward-speed, reverse-hide, and low-speed-LC rules and synchronizes its native visibility with CustomHud's FEngHud lifecycle; entry push, angle hold, automatic centering, front grip, and driven-wheel power remain disabled; true rear-wheel steering actuator rate decreases linearly from 20 degrees per second at 30 km/h to 10 degrees per second at 120 km/h, uses opposite phase below 70 km/h, and same phase from 70 km/h with no high-speed cutoff; version 1.3.8 targets the September 9 signed executable");
    return true;
#endif
}

bool IsArmed() {
#if NFSMW_ENABLE_ALPHA_BRIDGE
    return g_hooksArmed.load(std::memory_order_acquire) &&
           !g_permanentFault.load(std::memory_order_acquire);
#else
    return false;
#endif
}

}  // namespace nfsmw_drift_asi::alpha_bridge
