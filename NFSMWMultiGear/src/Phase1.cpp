/*
 * Phase 4 opens configured eighth through twelfth forward gears without
 * extending the game's inline layout. Gear eight still uses the verified
 * spare inline slot; gears nine through twelve live in a per-transmission
 * sidecar and are exposed through narrow consumers' hooks.
 */

#include <nfsmw_sdk/nfsmw_sdk.h>

#include "../GearConfig.h"
#include "../PowerMultiplier.h"
#include "DiagnosticLog.h"
#include "GameAccess.h"
#include "HudGearDisplay.h"
#include "StartupGate.h"

#include <MinHook.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <process.h>

namespace {

static_assert(sizeof(void*) == 4, "NFSMWMultiGear must be built for Win32");

constexpr unsigned kMaxParentDepth = 8;
constexpr unsigned kMaxLoggedRatioSlots = 14;
constexpr LONG kMaxLoggedMatches = 64;
constexpr LONG kMaxObservedTargets = 32;
constexpr unsigned kMaxCallerSites = 32;
constexpr LONG kMaxTraceLines = 256;
constexpr std::size_t kPhysicalLayoutOffset = 0x160u;
constexpr std::size_t kPhysicalCurrentGearOffset = 0x84u;
constexpr std::size_t kLayoutCurrentGearOffset = 0x68u;
constexpr std::size_t kControllerCurrentGearOffset = 0x64u;
constexpr std::uint16_t kNativeTotalSlots = 9;
constexpr std::uint16_t kEighthGearTotalSlots = 10;
constexpr std::uint32_t kEighthGearRawIndex = 9;
constexpr std::uint32_t kMaxVirtualRawIndex = 13;
constexpr std::uint32_t kMaxVirtualTotalSlots = 14;
constexpr unsigned kSidecarRatioCount = 5; // raw indices 9..13
constexpr std::size_t kRatioOffset = 0x00u;
constexpr std::size_t kEfficiencyOffset = 0x40u;
constexpr std::size_t kNextKnownLayoutFieldOffset = 0x70u;
constexpr std::uintptr_t kShiftCurveAddress = 0x00401AE0u;
constexpr std::uintptr_t kShiftCurveScaleAddress = 0x00891068u;
constexpr std::uintptr_t kShiftCurveBiasAddress = 0x0089096Cu;
constexpr std::uintptr_t kPrimaryEfficiencyPowerReturn = 0x006B0038u;
constexpr std::uintptr_t kTorqueEfficiencyPowerReturn = 0x006A152Du;
constexpr std::uintptr_t kControllerEfficiencyPowerReturn = 0x006A38F6u;

using TransmissionCtorFn =
    void*(NFSMW_FASTCALL*)(void* self, void* edx, std::uint32_t key);
using PrivateCountFn =
    std::uint32_t(NFSMW_THISCALL*)(const void* privateHeader);
using PrivateGetElementFn = const void*(NFSMW_THISCALL*)(
    const void* privateHeader, std::uint32_t index);
using ShiftGearFn =
    std::uint32_t(NFSMW_THISCALL*)(void* physical, std::int32_t requestedGear);
using GearFloatFn =
    float(NFSMW_THISCALL*)(void* transmission, std::uint32_t index);
using GearRatioPairFn = float(NFSMW_THISCALL*)(
    void* transmission, std::uint32_t first, std::uint32_t second);
using GearDecisionFn = std::uint32_t(NFSMW_THISCALL*)(
    void* transmission, std::uint32_t gear, float reference);
using AutomaticControllerFn = void(NFSMW_THISCALL*)(void* transmission);
using TransmissionSetGearFn = std::uint32_t(NFSMW_THISCALL*)(
    void* transmission, std::int32_t requestedGear);
using TransmissionTopGearFn = std::uint32_t(NFSMW_THISCALL*)(
    void* transmission);
using GearPairValueFn = float(NFSMW_THISCALL*)(
    void* transmission, std::uint32_t first, std::uint32_t second);
using TransmissionUpdateFn = std::uint32_t(NFSMW_THISCALL*)(
    void* transmission, std::uint32_t gear, float reference);
using AdditionalShiftDecisionFn = float(NFSMW_THISCALL*)(
    void* transmission, float reference, std::uint32_t fromGear,
    std::uint32_t toGear);
using DerivedGearValueFn = float(NFSMW_THISCALL*)(
    void* transmission, std::uint32_t index);
using EmbeddedFloatGetterFn = float(NFSMW_THISCALL*)(void* value);
using EmbeddedFlagGetterFn = unsigned char(NFSMW_THISCALL*)(void* value);
using ShiftCurveFn = float(__cdecl*)(float reference, float delta,
                                     float base);

enum class MatchKind { None, Direct, Parent };

struct Match {
    const gearconfig::Entry* entry = nullptr;
    MatchKind kind = MatchKind::None;
    const void* collection = nullptr;
    std::uint32_t key = 0;
    unsigned depth = 0;
};

struct AttributeSnapshot {
    const void* storage = nullptr;
    game_access::PrivateHeader header{};
    std::uint16_t offset = 0;
    std::uint16_t definitionMaxCount = 0;
    bool valid = false;
};

enum class ArrayKind { Ratio, Efficiency };

enum class PowerRoute : unsigned {
    Primary = 0,
    Torque = 1,
    Controller = 2,
};

enum class EighthPatchState : LONG {
    NotAttempted = 0,
    Applying = 1,
    Enabled = 2,
    Skipped = 3,
    Failed = 4,
};

static_assert(sizeof(game_access::PrivateHeader) +
                      (kEighthGearRawIndex + 1u) * sizeof(float) ==
                  0x30u,
              "ratio extension must end before layout+0x30");
static_assert(kEfficiencyOffset + sizeof(game_access::PrivateHeader) +
                      (kEighthGearRawIndex + 1u) * sizeof(float) ==
                  kNextKnownLayoutFieldOffset,
              "eighth efficiency must end exactly at the next known field");

struct TargetObservation {
    volatile LONG valid = 0;
    LONG id = 0;
    const void* collection = nullptr;
    const void* layout = nullptr;
    const void* ratioHeader = nullptr;
    const void* efficiencyHeader = nullptr;
    std::uint32_t collectionKey = 0;
    char configName[64] = {};
    volatile LONG ratioCountCallers[kMaxCallerSites] = {};
    volatile LONG efficiencyCountCallers[kMaxCallerSites] = {};
    volatile LONG ratioElementCallers[kMaxCallerSites] = {};
    volatile LONG efficiencyElementCallers[kMaxCallerSites] = {};
    volatile LONG ratioElementIndices = 0;
    volatile LONG efficiencyElementIndices = 0;
    volatile LONG invalidElementLogged = 0;
    volatile LONG shiftCallers[kMaxCallerSites] = {};
    volatile LONG shiftPhysicals[kMaxCallerSites] = {};
    volatile LONG shiftEvents = 0;
    volatile LONG invalidShiftLogged = 0;
    volatile LONG powerAppliedEvents = 0;
    volatile LONG eighthPatchState =
        static_cast<LONG>(EighthPatchState::NotAttempted);
    volatile LONG sidecarEnabled = 0;
    std::uint32_t maxForwardGears = 0;
    std::uint32_t totalRatioSlots = 0;
    std::uint32_t sidecarRatioCount = 0;
    float eighthRatio = 0.0f;
    float baseEfficiency = 0.0f;
    float eighthEfficiency = 0.0f;
    float sidecarRatios[kSidecarRatioCount] = {};
    float sidecarEfficiencies[kSidecarRatioCount] = {};
    float powerMultipliers[kSidecarRatioCount] = {};
};

gearconfig::Table* g_config = nullptr;
TransmissionCtorFn g_originalCtor = nullptr;
PrivateCountFn g_originalPrivateCount = nullptr;
PrivateGetElementFn g_originalPrivateGetElement = nullptr;
ShiftGearFn g_originalShiftGear = nullptr;
GearFloatFn g_originalGearRatioGetter = nullptr;
GearFloatFn g_originalGearEfficiencyGetter = nullptr;
GearRatioPairFn g_originalGearRatioPair = nullptr;
GearDecisionFn g_originalGearDecision = nullptr;
AutomaticControllerFn g_originalAutomaticControllerA = nullptr;
AutomaticControllerFn g_originalAutomaticControllerB = nullptr;
TransmissionSetGearFn g_originalTransmissionSetGear = nullptr;
TransmissionTopGearFn g_originalTransmissionTopGear = nullptr;
TransmissionSetGearFn g_originalAlternateTransmissionSetGear = nullptr;
TransmissionTopGearFn g_originalAlternateTransmissionTopGear = nullptr;
GearPairValueFn g_originalAlternateGearPair = nullptr;
GearFloatFn g_originalEffectiveGearRatio = nullptr;
TransmissionUpdateFn g_originalTransmissionUpdate = nullptr;
GearFloatFn g_originalEffectiveRatioHelper = nullptr;
AdditionalShiftDecisionFn g_originalAdditionalShiftDecision = nullptr;
DerivedGearValueFn g_originalDerivedShiftValue = nullptr;
DerivedGearValueFn g_originalDerivedEfficiencyValue = nullptr;
GearFloatFn g_originalLayoutRatioGetter = nullptr;
GearFloatFn g_originalLayoutEfficiencyGetter = nullptr;
GearFloatFn g_originalLayoutEffectiveRatio = nullptr;
GearFloatFn g_originalTorqueEfficiencyRatio = nullptr;
GearFloatFn g_originalControllerRatioGetter = nullptr;
GearFloatFn g_originalControllerEfficiencyGetter = nullptr;
GearFloatFn g_originalControllerEffectiveRatio = nullptr;
volatile LONG g_matchSequence = 0;
volatile LONG g_hasTargets = 0;
volatile LONG g_targetCount = 0;
volatile LONG g_traceLines = 0;
volatile LONG g_traceBudgetNotice = 0;
volatile LONG g_targetCapacityNotice = 0;
SRWLOCK g_targetLock = SRWLOCK_INIT;
TargetObservation g_targets[kMaxObservedTargets] = {};

bool IsPublished(TargetObservation* target) {
    return InterlockedCompareExchange(&target->valid, 0, 0) != 0;
}

bool IsEighthGearEnabled(TargetObservation* target) {
    return target != nullptr &&
           InterlockedCompareExchange(
               &target->eighthPatchState, 0, 0) ==
               static_cast<LONG>(EighthPatchState::Enabled);
}

bool IsSidecarEnabled(TargetObservation* target) {
    return target != nullptr &&
           InterlockedCompareExchange(&target->sidecarEnabled, 0, 0) != 0 &&
           IsEighthGearEnabled(target);
}

bool IsVirtualGearIndex(TargetObservation* target, std::uint32_t index) {
    return IsEighthGearEnabled(target) &&
           index >= kEighthGearRawIndex &&
           index < (target->totalRatioSlots == 0
                        ? kEighthGearTotalSlots
                        : target->totalRatioSlots);
}

bool IsSidecarGearIndex(TargetObservation* target, std::uint32_t index) {
    return IsSidecarEnabled(target) && index >= kEighthGearRawIndex &&
           index <= kMaxVirtualRawIndex &&
           index < target->totalRatioSlots;
}

float SidecarRatio(TargetObservation* target, std::uint32_t index) {
    if (target == nullptr || index < kEighthGearRawIndex ||
        index > kMaxVirtualRawIndex) {
        return 0.0f;
    }
    return target->sidecarRatios[index - kEighthGearRawIndex];
}

float SidecarEfficiency(TargetObservation* target, std::uint32_t index) {
    if (target == nullptr || index < kEighthGearRawIndex ||
        index > kMaxVirtualRawIndex) {
        return 0.0f;
    }
    return target->sidecarEfficiencies[index - kEighthGearRawIndex];
}

bool IsNativeInlineFloatHeader(const game_access::PrivateHeader& header) {
    return header.capacity == kNativeTotalSlots &&
           header.count == kNativeTotalSlots &&
           header.elementSize == sizeof(float) && header.metadata == 0x0009u &&
           game_access::PrivateDataOffset(header) ==
               sizeof(game_access::PrivateHeader);
}

TargetObservation* FindTargetByLayout(const void* layout) {
    if (layout == nullptr ||
        InterlockedCompareExchange(&g_hasTargets, 0, 0) == 0) {
        return nullptr;
    }
    const LONG count = InterlockedCompareExchange(&g_targetCount, 0, 0);
    for (LONG i = 0; i < count; ++i) {
        TargetObservation* target = &g_targets[i];
        if (IsPublished(target) && target->layout == layout) return target;
    }
    return nullptr;
}

TargetObservation* FindTargetByHeader(const void* header, ArrayKind* kind) {
    if (header == nullptr || kind == nullptr ||
        InterlockedCompareExchange(&g_hasTargets, 0, 0) == 0) {
        return nullptr;
    }
    const LONG count = InterlockedCompareExchange(&g_targetCount, 0, 0);
    for (LONG i = 0; i < count; ++i) {
        TargetObservation* target = &g_targets[i];
        if (!IsPublished(target)) continue;
        if (target->ratioHeader == header) {
            *kind = ArrayKind::Ratio;
            return target;
        }
        if (target->efficiencyHeader == header) {
            *kind = ArrayKind::Efficiency;
            return target;
        }
    }
    return nullptr;
}

TargetObservation* FindTargetForPhysical(const void* physical,
                                         const void** layout) {
    if (layout != nullptr) *layout = nullptr;
    if (physical == nullptr ||
        InterlockedCompareExchange(&g_hasTargets, 0, 0) == 0) {
        return nullptr;
    }

    const void* candidate = nullptr;
    if (!game_access::ReadField(physical, kPhysicalLayoutOffset, &candidate)) {
        return nullptr;
    }
    if (layout != nullptr) *layout = candidate;
    return FindTargetByLayout(candidate);
}

// Several drivetrain helper classes keep the same transmission layout at a
// different member offset. Keep the lookup centralized so a malformed object
// can only make a helper fall back to the game's original implementation.
TargetObservation* FindTargetForLayoutField(const void* object,
                                             std::size_t offset,
                                             const void** layout) {
    if (layout != nullptr) *layout = nullptr;
    if (object == nullptr ||
        InterlockedCompareExchange(&g_hasTargets, 0, 0) == 0) {
        return nullptr;
    }
    const void* candidate = nullptr;
    if (!game_access::ReadField(object, offset, &candidate) ||
        candidate == nullptr) {
        return nullptr;
    }
    if (layout != nullptr) *layout = candidate;
    return FindTargetByLayout(candidate);
}

TargetObservation* FindTargetForBaseObject(const void* object,
                                            const void** layout) {
    if (layout != nullptr) *layout = nullptr;
    if (object == nullptr) return nullptr;

    const void* candidate = nullptr;
    TargetObservation* target = FindTargetForPhysical(object, &candidate);
    if (target != nullptr) {
        if (layout != nullptr) *layout = candidate;
        return target;
    }

    target = FindTargetForLayoutField(object, 0xF4u, &candidate);
    if (target == nullptr) {
        target = FindTargetForLayoutField(object, 0xF0u, &candidate);
    }
    if (target != nullptr && layout != nullptr) *layout = candidate;
    return target;
}

// 0x00691F90/0x00692F10/0x00693010 are called with the large transmission
// object used by the automatic path.  Most instances expose the ratio layout
// through +0x160, but a few construction paths publish only the outer
// object's +0xF4 header.  Keep this fallback local to these helpers; the
// physical-car paths must continue to use their verified +0x160 lookup.
TargetObservation* FindTargetForTransmissionObject(const void* object,
                                                     const void** layout) {
    if (layout != nullptr) *layout = nullptr;
    TargetObservation* target = FindTargetForPhysical(object, layout);
    if (target != nullptr) return target;
    return FindTargetForLayoutField(object, 0xF4u, layout);
}

bool ReadEmbeddedFloatGetter(const void* object, std::size_t fieldOffset,
                             std::size_t vtableOffset, float* value) {
    if (object == nullptr || value == nullptr) return false;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(object);
    if (fieldOffset > UINTPTR_MAX - base) return false;
    const auto* field = reinterpret_cast<const std::uint8_t*>(base + fieldOffset);

    const void* vtable = nullptr;
    if (!game_access::SafeRead(field, &vtable, sizeof(vtable)) ||
        vtable == nullptr || vtableOffset > UINTPTR_MAX -
                                      reinterpret_cast<std::uintptr_t>(vtable)) {
        return false;
    }
    const void* functionAddress = nullptr;
    const auto* slot = reinterpret_cast<const std::uint8_t*>(
        reinterpret_cast<std::uintptr_t>(vtable) + vtableOffset);
    if (!game_access::SafeRead(slot, &functionAddress,
                               sizeof(functionAddress)) ||
        functionAddress == nullptr) {
        return false;
    }

    float result = 0.0f;
#if defined(_MSC_VER)
    __try {
        result = reinterpret_cast<EmbeddedFloatGetterFn>(
            const_cast<void*>(functionAddress))(const_cast<void*>(
            static_cast<const void*>(field)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    result = reinterpret_cast<EmbeddedFloatGetterFn>(
        const_cast<void*>(functionAddress))(const_cast<void*>(
        static_cast<const void*>(field)));
#endif
    if (!std::isfinite(result)) return false;
    *value = result;
    return true;
}

bool ReadEmbeddedFlagGetter(const void* object, std::size_t fieldOffset,
                            std::size_t vtableOffset, bool* value) {
    if (object == nullptr || value == nullptr) return false;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(object);
    if (fieldOffset > UINTPTR_MAX - base) return false;
    const auto* field = reinterpret_cast<const std::uint8_t*>(base + fieldOffset);

    const void* vtable = nullptr;
    if (!game_access::SafeRead(field, &vtable, sizeof(vtable)) ||
        vtable == nullptr || vtableOffset > UINTPTR_MAX -
                                      reinterpret_cast<std::uintptr_t>(vtable)) {
        return false;
    }
    const void* functionAddress = nullptr;
    const auto* slot = reinterpret_cast<const std::uint8_t*>(
        reinterpret_cast<std::uintptr_t>(vtable) + vtableOffset);
    if (!game_access::SafeRead(slot, &functionAddress,
                               sizeof(functionAddress)) ||
        functionAddress == nullptr) {
        return false;
    }

    unsigned char result = 0;
#if defined(_MSC_VER)
    __try {
        result = reinterpret_cast<EmbeddedFlagGetterFn>(
            const_cast<void*>(functionAddress))(const_cast<void*>(
            static_cast<const void*>(field)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    result = reinterpret_cast<EmbeddedFlagGetterFn>(
        const_cast<void*>(functionAddress))(const_cast<void*>(
        static_cast<const void*>(field)));
#endif
    *value = result != 0;
    return true;
}

bool EvaluateShiftCurve(float reference, float delta, float base,
                        float* result) {
    if (result == nullptr || !std::isfinite(reference) ||
        !std::isfinite(delta) || !std::isfinite(base)) {
        return false;
    }

    float scale = 0.0f;
    float bias = 0.0f;
    if (!game_access::SafeRead(
            reinterpret_cast<const void*>(kShiftCurveScaleAddress), &scale,
            sizeof(scale)) ||
        !game_access::SafeRead(
            reinterpret_cast<const void*>(kShiftCurveBiasAddress), &bias,
            sizeof(bias)) ||
        !std::isfinite(scale) || !std::isfinite(bias)) {
        return false;
    }

    float curve = 0.0f;
#if defined(_MSC_VER)
    __try {
        curve = reinterpret_cast<ShiftCurveFn>(kShiftCurveAddress)(
            reference, delta, base);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    curve = reinterpret_cast<ShiftCurveFn>(kShiftCurveAddress)(reference,
                                                                 delta, base);
#endif
    if (!std::isfinite(curve)) return false;
    curve = curve * scale + bias;
    if (!std::isfinite(curve)) return false;
    *result = curve;
    return true;
}

TargetObservation* FindTargetForEmbeddedTransmission(
    const void* transmission, const void** layout) {
    if (layout != nullptr) *layout = nullptr;
    constexpr std::uintptr_t kEmbeddedOffset = 0x4Cu;
    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(transmission);
    if (address < kEmbeddedOffset) return nullptr;
    const void* base = reinterpret_cast<const void*>(address - kEmbeddedOffset);
    return FindTargetForBaseObject(base, layout);
}

// The secondary embedded transmission class stores the ratio Private header
// at +0xA4 instead of +0xA8.  The header is the most stable identity for a
// registered target, so use it directly rather than assuming the outer object
// has the primary physical layout fields.
TargetObservation* FindTargetForAlternateTransmission(
    const void* transmission) {
    if (transmission == nullptr ||
        InterlockedCompareExchange(&g_hasTargets, 0, 0) == 0) {
        return nullptr;
    }
    const void* header = nullptr;
    if (!game_access::ReadField(transmission, 0xA4u, &header) ||
        header == nullptr) {
        return nullptr;
    }
    ArrayKind kind = ArrayKind::Ratio;
    return FindTargetByHeader(header, &kind);
}

bool IsHighSidecarGearIndex(TargetObservation* target, std::uint32_t index) {
    return IsSidecarGearIndex(target, index) && index > kEighthGearRawIndex;
}

// Once a configured transmission has been validated, raw indices above the
// verified inline slot must never fall through to a native fixed-size array.
// Callers that have an explicit sidecar implementation handle the valid
// range first; every other high index fails closed.  Unmatched objects keep
// the game's original behavior because `target` is null.
bool IsProtectedHighGearIndex(TargetObservation* target,
                              std::uint32_t index) {
    return IsEighthGearEnabled(target) && index > kEighthGearRawIndex;
}

TargetObservation* RegisterTarget(const gearconfig::Entry& entry,
                                  std::uint32_t collectionKey,
                                  const void* collection,
                                  const void* layout,
                                  const AttributeSnapshot& ratio,
                                  const AttributeSnapshot& efficiency,
                                  bool* created) {
    if (created != nullptr) *created = false;
    if (!ratio.valid || !efficiency.valid || layout == nullptr) return nullptr;

    AcquireSRWLockExclusive(&g_targetLock);
    const LONG count = InterlockedCompareExchange(&g_targetCount, 0, 0);
    for (LONG i = 0; i < count; ++i) {
        TargetObservation* existing = &g_targets[i];
        if (IsPublished(existing) && existing->layout == layout) {
            ReleaseSRWLockExclusive(&g_targetLock);
            return existing;
        }
    }

    if (count >= kMaxObservedTargets) {
        ReleaseSRWLockExclusive(&g_targetLock);
        return nullptr;
    }
    const LONG targetIndex = count;
    TargetObservation* target = &g_targets[targetIndex];

    std::memset(target, 0, sizeof(*target));
    target->id = targetIndex + 1;
    target->collection = collection;
    target->layout = layout;
    target->ratioHeader = ratio.storage;
    target->efficiencyHeader = efficiency.storage;
    target->collectionKey = collectionKey;
    strncpy_s(target->configName, sizeof(target->configName),
              entry.name.c_str(), _TRUNCATE);
    InterlockedExchange(&target->valid, 1);
    InterlockedExchange(&g_targetCount, targetIndex + 1);
    InterlockedExchange(&g_hasTargets, 1);
    ReleaseSRWLockExclusive(&g_targetLock);

    if (created != nullptr) *created = true;
    return target;
}

const char* ArrayName(ArrayKind kind) {
    return kind == ArrayKind::Ratio ? "GEAR_RATIO" : "GEAR_EFFICIENCY";
}

volatile LONG* CountCallerSlots(TargetObservation* target, ArrayKind kind) {
    return kind == ArrayKind::Ratio ? target->ratioCountCallers
                                    : target->efficiencyCountCallers;
}

volatile LONG* ElementCallerSlots(TargetObservation* target, ArrayKind kind) {
    return kind == ArrayKind::Ratio ? target->ratioElementCallers
                                    : target->efficiencyElementCallers;
}

volatile LONG* ElementIndexMask(TargetObservation* target, ArrayKind kind) {
    return kind == ArrayKind::Ratio ? &target->ratioElementIndices
                                    : &target->efficiencyElementIndices;
}

bool MarkCallerOnce(volatile LONG* slots, const void* caller) {
    LONG key = static_cast<LONG>(reinterpret_cast<std::uintptr_t>(caller));
    if (key == 0) key = 1;
    for (unsigned i = 0; i < kMaxCallerSites; ++i) {
        const LONG current = InterlockedCompareExchange(&slots[i], 0, 0);
        if (current == key) return false;
        if (current == 0 &&
            InterlockedCompareExchange(&slots[i], key, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool MarkIndexOnce(volatile LONG* mask, std::uint32_t index,
                   volatile LONG* invalidLogged) {
    if (index < 30) {
        const LONG bit = static_cast<LONG>(1u << index);
        return (InterlockedOr(mask, bit) & bit) == 0;
    }
    return InterlockedCompareExchange(invalidLogged, 1, 0) == 0;
}

bool MarkShiftOnce(TargetObservation* target, std::int32_t requested,
                   bool accepted) {
    if (requested >= 0 && requested < 15) {
        const unsigned bitIndex = static_cast<unsigned>(requested) +
                                  (accepted ? 16u : 0u);
        const LONG bit = static_cast<LONG>(1u << bitIndex);
        return (InterlockedOr(&target->shiftEvents, bit) & bit) == 0;
    }
    return InterlockedCompareExchange(&target->invalidShiftLogged, 1, 0) == 0;
}

bool MarkPowerAppliedOnce(TargetObservation* target, PowerRoute route,
                          std::uint32_t index) {
    if (target == nullptr || index < kEighthGearRawIndex ||
        index > kMaxVirtualRawIndex) {
        return false;
    }
    const unsigned bitIndex =
        static_cast<unsigned>(route) * kSidecarRatioCount +
        static_cast<unsigned>(index - kEighthGearRawIndex);
    const LONG bit = static_cast<LONG>(1u << bitIndex);
    return (InterlockedOr(&target->powerAppliedEvents, bit) & bit) == 0;
}

const char* PowerRouteName(PowerRoute route) {
    switch (route) {
        case PowerRoute::Primary:
            return "primary";
        case PowerRoute::Torque:
            return "torque";
        case PowerRoute::Controller:
            return "controller";
    }
    return "unknown";
}

bool ReserveTraceLine() {
    const LONG line = InterlockedIncrement(&g_traceLines);
    if (line <= kMaxTraceLines) return true;
    if (InterlockedCompareExchange(&g_traceBudgetNotice, 1, 0) == 0) {
        diagnostic_log::Write(
            "TRACE_SUPPRESSED global trace budget reached (%ld)",
            kMaxTraceLines);
    }
    return false;
}

std::uint32_t CallerRva(const void* caller) {
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(caller);
    if (address < game_access::kImageBase ||
        address >= game_access::kImageBase +
                       game_access::kV13EnglishCollectorsImageSize) {
        return 0xFFFFFFFFu;
    }
    return static_cast<std::uint32_t>(address - game_access::kImageBase);
}

bool ModulePath(char* output, std::size_t size, const char* leaf) {
    if (output == nullptr || size == 0 || leaf == nullptr) return false;
    output[0] = '\0';

    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ModulePath), &self)) {
        return false;
    }

    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(self, path, sizeof(path));
    if (length == 0 || length >= sizeof(path)) return false;

    char* slash = std::strrchr(path, '\\');
    if (slash == nullptr) return false;
    slash[1] = '\0';
    return _snprintf_s(output, size, _TRUNCATE, "%s%s", path, leaf) >= 0;
}

const gearconfig::Entry* FindConfigured(std::uint32_t key) {
    return g_config == nullptr
               ? nullptr
               : g_config->FindHash(key, &game_access::BStringHash);
}

bool WasVisited(const void* pointer, const void* const* visited,
                unsigned visitedCount) {
    for (unsigned i = 0; i < visitedCount; ++i) {
        if (visited[i] == pointer) return true;
    }
    return false;
}

Match FindCollectionMatch(const void* collection) {
    Match match{};
    if (collection == nullptr) return match;

    std::uint32_t key = 0;
    if (!game_access::ReadField(collection,
                                game_access::kCollectionKeyOffset, &key)) {
        return match;
    }

    if (const gearconfig::Entry* entry = FindConfigured(key)) {
        match.entry = entry;
        match.kind = MatchKind::Direct;
        match.collection = collection;
        match.key = key;
        return match;
    }

    const void* visited[kMaxParentDepth + 1] = {};
    unsigned visitedCount = 1;
    visited[0] = collection;
    const void* current = collection;

    for (unsigned depth = 1; depth <= kMaxParentDepth; ++depth) {
        const void* parent = nullptr;
        if (!game_access::ReadField(current,
                                    game_access::kCollectionParentOffset,
                                    &parent) ||
            parent == nullptr || WasVisited(parent, visited, visitedCount)) {
            break;
        }

        visited[visitedCount++] = parent;
        std::uint32_t parentKey = 0;
        if (game_access::ReadField(parent,
                                   game_access::kCollectionKeyOffset,
                                   &parentKey)) {
            if (const gearconfig::Entry* entry = FindConfigured(parentKey)) {
                match.entry = entry;
                match.kind = MatchKind::Parent;
                match.collection = parent;
                match.key = parentKey;
                match.depth = depth;
                return match;
            }
        }
        current = parent;
    }

    return match;
}

void LogFloatArray(LONG sequence, const char* label, const void* storage,
                   const game_access::PrivateHeader& header, bool emit) {
    if (!emit) return;
    if (header.elementSize != sizeof(float)) {
        diagnostic_log::Write(
            "MATCH #%ld %s values skipped: elementSize=%u (expected 4)",
            sequence, label,
            static_cast<unsigned>(header.elementSize));
        return;
    }
    if (header.count > header.capacity) {
        diagnostic_log::Write(
            "MATCH #%ld %s values skipped: count=%u exceeds capacity=%u",
            sequence, label,
            static_cast<unsigned>(header.count),
            static_cast<unsigned>(header.capacity));
        return;
    }

    const unsigned count =
        std::min<unsigned>(header.count, kMaxLoggedRatioSlots);
    if (count == 0) {
        diagnostic_log::Write("MATCH #%ld %s values: empty", sequence, label);
        return;
    }

    float ratios[kMaxLoggedRatioSlots] = {};
    const std::uintptr_t storageAddress =
        reinterpret_cast<std::uintptr_t>(storage);
    const std::size_t dataOffset = game_access::PrivateDataOffset(header);
    if (dataOffset > UINTPTR_MAX - storageAddress ||
        !game_access::SafeRead(
            reinterpret_cast<const void*>(
                storageAddress + dataOffset),
            ratios, count * sizeof(float))) {
        diagnostic_log::Write("MATCH #%ld %s values unreadable", sequence,
                              label);
        return;
    }

    char line[640] = {};
    int written = _snprintf_s(line, sizeof(line), _TRUNCATE,
                              "MATCH #%ld %s values:", sequence, label);
    if (written < 0) return;
    std::size_t used = static_cast<std::size_t>(written);
    for (unsigned i = 0; i < count && used < sizeof(line); ++i) {
        written = _snprintf_s(
            line + used, sizeof(line) - used, _TRUNCATE,
            " [%u]=%.6g", i, static_cast<double>(ratios[i]));
        if (written < 0) break;
        used += static_cast<std::size_t>(written);
    }
    diagnostic_log::Write("%s", line);
}

bool LogAttributeArray(LONG sequence, const char* label, std::uint32_t key,
                       const void* collectionClass, const void* layout,
                       AttributeSnapshot* snapshot, bool emit) {
    if (snapshot != nullptr) *snapshot = {};
    game_access::AttributeDefinition definition{};
    const void* definitionAddress = nullptr;
    if (!game_access::ReadAttributeDefinition(
            collectionClass, key, &definition, &definitionAddress)) {
        if (emit) {
            diagnostic_log::Write(
                "MATCH #%ld %s definition key=0x%08X unavailable", sequence,
                label, static_cast<unsigned>(key));
        }
        return false;
    }

    if (emit) {
        diagnostic_log::Write(
            "MATCH #%ld %s definition=%p key=0x%08X type=0x%08X "
            "offset=0x%04X size=%u maxCount=%u flags=0x%02X alignment=%u",
            sequence, label, definitionAddress,
            static_cast<unsigned>(definition.key),
            static_cast<unsigned>(definition.type),
            static_cast<unsigned>(definition.offset),
            static_cast<unsigned>(definition.size),
            static_cast<unsigned>(definition.maxCount),
            static_cast<unsigned>(definition.flags),
            static_cast<unsigned>(definition.alignment));
    }

    if (definition.key != key ||
        (definition.flags & game_access::kDefinitionArray) == 0 ||
        (definition.flags & game_access::kDefinitionInLayout) == 0 ||
        definition.size != sizeof(float)) {
        if (emit) {
            diagnostic_log::Write(
                "MATCH #%ld %s storage skipped: definition is not the "
                "expected in-layout float array",
                sequence, label);
        }
        return false;
    }

    const std::uintptr_t layoutAddress =
        reinterpret_cast<std::uintptr_t>(layout);
    if (definition.offset > UINTPTR_MAX - layoutAddress) {
        if (emit) {
            diagnostic_log::Write("MATCH #%ld %s storage address overflow",
                                  sequence, label);
        }
        return false;
    }
    const void* storage = reinterpret_cast<const void*>(
        layoutAddress + static_cast<std::uintptr_t>(definition.offset));

    game_access::PrivateHeader header{};
    if (!game_access::SafeRead(storage, &header, sizeof(header))) {
        if (emit) {
            diagnostic_log::Write("MATCH #%ld %s header unreadable at %p",
                                  sequence, label, storage);
        }
        return false;
    }

    if (emit) {
        diagnostic_log::Write(
            "MATCH #%ld %s storage=%p capacity=%u count=%u elementSize=%u "
            "metadata=0x%04X",
            sequence, label, storage, static_cast<unsigned>(header.capacity),
            static_cast<unsigned>(header.count),
            static_cast<unsigned>(header.elementSize),
            static_cast<unsigned>(header.metadata));
    }
    LogFloatArray(sequence, label, storage, header, emit);

    const std::uintptr_t storageAddress =
        reinterpret_cast<std::uintptr_t>(storage);
    if (header.elementSize != sizeof(float) || header.count > header.capacity ||
        game_access::PrivateDataOffset(header) >
            UINTPTR_MAX - storageAddress) {
        return false;
    }
    if (snapshot != nullptr) {
        snapshot->storage = storage;
        snapshot->header = header;
        snapshot->offset = definition.offset;
        snapshot->definitionMaxCount = definition.maxCount;
        snapshot->valid = true;
    }
    return true;
}

void TryEnableEighthGear(TargetObservation* target, const Match& match,
                         std::uint32_t ctorKey, bool keyReadable,
                         std::uint32_t collectionKey, const void* layout,
                         const AttributeSnapshot& ratio,
                         const AttributeSnapshot& efficiency) {
    if (target == nullptr || match.entry == nullptr) return;

    const LONG claimed = InterlockedCompareExchange(
        &target->eighthPatchState,
        static_cast<LONG>(EighthPatchState::Applying),
        static_cast<LONG>(EighthPatchState::NotAttempted));
    if (claimed != static_cast<LONG>(EighthPatchState::NotAttempted)) return;

    const auto skip = [&](const char* reason) {
        diagnostic_log::Write(
            "PATCH_SKIPPED target=%ld config='%s' requestedForward=%u "
            "reason='%s'",
            target->id, target->configName, match.entry->maxForwardGears,
            reason);
        InterlockedExchange(&target->eighthPatchState,
                            static_cast<LONG>(EighthPatchState::Skipped));
    };
    const auto fail = [&](const char* reason, bool rollbackComplete) {
        diagnostic_log::Write(
            "PATCH_FAILED target=%ld config='%s' reason='%s' rollback=%s",
            target->id, target->configName, reason,
            rollbackComplete ? "complete" : "partial");
        InterlockedExchange(&target->eighthPatchState,
                            static_cast<LONG>(EighthPatchState::Failed));
    };

    if (match.kind != MatchKind::Direct) {
        skip("writes require a direct collection-name match");
        return;
    }
    if (!keyReadable || ctorKey != collectionKey || match.key != collectionKey) {
        skip("constructor and collection keys are not an exact match");
        return;
    }
    const unsigned requestedForward = match.entry->maxForwardGears;
    const std::size_t requiredRatios = match.entry->RequiredRatioCount();
    if (requestedForward < gearconfig::kMinForwardGears ||
        requestedForward > gearconfig::kMaxForwardGears ||
        match.entry->ratioCount != requiredRatios ||
        (match.entry->powerMultiplierCount != 0 &&
         match.entry->powerMultiplierCount != requiredRatios)) {
        skip("all ratios through the requested maximum gear are required");
        return;
    }
    if (ratio.offset != kRatioOffset ||
        efficiency.offset != kEfficiencyOffset ||
        ratio.definitionMaxCount != kNativeTotalSlots ||
        efficiency.definitionMaxCount != kNativeTotalSlots) {
        skip("attribute definitions do not match the verified layout");
        return;
    }

    game_access::PrivateHeader liveRatio{};
    game_access::PrivateHeader liveEfficiency{};
    if (!game_access::SafeRead(ratio.storage, &liveRatio, sizeof(liveRatio)) ||
        !game_access::SafeRead(efficiency.storage, &liveEfficiency,
                              sizeof(liveEfficiency))) {
        fail("could not re-read live attribute headers", true);
        return;
    }
    if (std::memcmp(&liveRatio, &ratio.header, sizeof(liveRatio)) != 0 ||
        std::memcmp(&liveEfficiency, &efficiency.header,
                    sizeof(liveEfficiency)) != 0) {
        fail("attribute headers changed during validation", true);
        return;
    }
    if (!IsNativeInlineFloatHeader(liveRatio) ||
        !IsNativeInlineFloatHeader(liveEfficiency)) {
        skip("live arrays are not the verified 9-slot inline layout");
        return;
    }

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(layout);
    if (layout == nullptr || kNextKnownLayoutFieldOffset > UINTPTR_MAX - base) {
        fail("layout address overflow", true);
        return;
    }
    auto* layoutBytes = reinterpret_cast<std::uint8_t*>(
        const_cast<void*>(layout));
    void* expectedRatioHeader = layoutBytes + kRatioOffset;
    void* expectedEfficiencyHeader = layoutBytes + kEfficiencyOffset;
    if (ratio.storage != expectedRatioHeader ||
        efficiency.storage != expectedEfficiencyHeader) {
        skip("array headers are not at the verified layout offsets");
        return;
    }

    void* ratioSlot = layoutBytes + sizeof(game_access::PrivateHeader) +
                      kEighthGearRawIndex * sizeof(float);
    void* efficiencySlot = layoutBytes + kEfficiencyOffset +
                           sizeof(game_access::PrivateHeader) +
                           kEighthGearRawIndex * sizeof(float);
    const void* seventhEfficiencySlot =
        layoutBytes + kEfficiencyOffset + sizeof(game_access::PrivateHeader) +
        (kEighthGearRawIndex - 1u) * sizeof(float);
    if (reinterpret_cast<std::uint8_t*>(efficiencySlot) + sizeof(float) !=
        layoutBytes + kNextKnownLayoutFieldOffset) {
        fail("eighth efficiency would overlap the next layout field", true);
        return;
    }

    float baseEfficiency = 0.0f;
    std::uint32_t originalRatioBits = 0;
    std::uint32_t originalEfficiencyBits = 0;
    if (!game_access::SafeRead(seventhEfficiencySlot, &baseEfficiency,
                               sizeof(baseEfficiency)) ||
        !std::isfinite(baseEfficiency) || baseEfficiency <= 0.0f ||
        !game_access::SafeRead(ratioSlot, &originalRatioBits,
                              sizeof(originalRatioBits)) ||
        !game_access::SafeRead(efficiencySlot, &originalEfficiencyBits,
                              sizeof(originalEfficiencyBits))) {
        fail("could not validate the eighth-gear storage slots", true);
        return;
    }
    if (!game_access::IsWritable(ratioSlot, sizeof(float)) ||
        !game_access::IsWritable(efficiencySlot, sizeof(float))) {
        fail("verified layout storage is not writable", true);
        return;
    }

    const float eighthRatio = match.entry->ratios[0];
    float publishedEfficiencies[kSidecarRatioCount] = {};
    float configuredEfficiencies[kSidecarRatioCount] = {};
    if (!gear_power::BuildEfficiencyViews(
            baseEfficiency, match.entry->powerMultipliers.data(),
            requiredRatios,
            publishedEfficiencies, configuredEfficiencies)) {
        fail("configured power multiplier produces an invalid efficiency",
             true);
        return;
    }
    // The raw-9 slot is part of several native whole-table calculations.
    // Publishing a multiplied value here changes shared derived data for
    // gears 1-7. Keep every static efficiency view neutral and apply the
    // multiplier only in a current-gear runtime getter.
    const float eighthEfficiency = publishedEfficiencies[0];
    bool ratioSlotNeedsRestore = false;
    bool efficiencySlotNeedsRestore = false;

    const auto rollback = [&]() {
        bool complete = true;
        if (ratioSlotNeedsRestore &&
            !game_access::SafeWrite(ratioSlot, &originalRatioBits,
                                    sizeof(originalRatioBits))) {
            complete = false;
        }
        if (efficiencySlotNeedsRestore &&
            !game_access::SafeWrite(efficiencySlot, &originalEfficiencyBits,
                                    sizeof(originalEfficiencyBits))) {
            complete = false;
        }
        return complete;
    };

    ratioSlotNeedsRestore = true;
    if (!game_access::SafeWrite(ratioSlot, &eighthRatio,
                                sizeof(eighthRatio))) {
        fail("could not write the eighth-gear ratio", rollback());
        return;
    }
    efficiencySlotNeedsRestore = true;
    if (!game_access::SafeWrite(efficiencySlot, &eighthEfficiency,
                                sizeof(eighthEfficiency))) {
        fail("could not write the eighth-gear efficiency", rollback());
        return;
    }

    game_access::PrivateHeader verifiedRatio{};
    game_access::PrivateHeader verifiedEfficiency{};
    float verifiedRatioValue = 0.0f;
    float verifiedEfficiencyValue = 0.0f;
    const bool verified =
        game_access::SafeRead(ratio.storage, &verifiedRatio,
                              sizeof(verifiedRatio)) &&
        game_access::SafeRead(efficiency.storage, &verifiedEfficiency,
                              sizeof(verifiedEfficiency)) &&
        game_access::SafeRead(ratioSlot, &verifiedRatioValue,
                              sizeof(verifiedRatioValue)) &&
        game_access::SafeRead(efficiencySlot, &verifiedEfficiencyValue,
                              sizeof(verifiedEfficiencyValue)) &&
        std::memcmp(&verifiedRatio, &liveRatio, sizeof(verifiedRatio)) == 0 &&
        std::memcmp(&verifiedEfficiency, &liveEfficiency,
                    sizeof(verifiedEfficiency)) == 0 &&
        std::memcmp(&verifiedRatioValue, &eighthRatio, sizeof(float)) == 0 &&
        std::memcmp(&verifiedEfficiencyValue, &eighthEfficiency,
                    sizeof(float)) == 0;
    if (!verified) {
        fail("post-write verification failed", rollback());
        return;
    }

    target->eighthRatio = eighthRatio;
    target->baseEfficiency = baseEfficiency;
    target->eighthEfficiency = eighthEfficiency;
    target->maxForwardGears = requestedForward;
    target->totalRatioSlots = requestedForward + 2u;
    target->sidecarRatioCount = static_cast<std::uint32_t>(requiredRatios);
    for (unsigned i = 0; i < kSidecarRatioCount; ++i) {
        target->sidecarRatios[i] = 0.0f;
        target->sidecarEfficiencies[i] = 0.0f;
        target->powerMultipliers[i] = 0.0f;
    }
    for (std::size_t i = 0; i < requiredRatios && i < kSidecarRatioCount;
         ++i) {
        target->sidecarRatios[i] = match.entry->ratios[i];
        target->sidecarEfficiencies[i] = publishedEfficiencies[i];
        target->powerMultipliers[i] = match.entry->powerMultipliers[i];
    }
    InterlockedExchange(&target->sidecarEnabled,
                        requestedForward > gearconfig::kMinForwardGears ? 1
                                                                         : 0);
    InterlockedExchange(&target->eighthPatchState,
                        static_cast<LONG>(EighthPatchState::Enabled));
    diagnostic_log::Write(
        "PATCH_ENABLED target=%ld config='%s' mode=%s forward=%u rawIndex=%u "
        "ratio=%.9g baseEfficiency=%.9g powerMultiplier=%.9g "
        "publishedEfficiency=%.9g poweredEfficiency=%.9g "
        "ratioSlot=%p efficiencySlot=%p "
        "nativeSlots=9 virtualSlots=%u sidecarRatios=%u headers=unchanged",
        target->id, target->configName,
        requestedForward > gearconfig::kMinForwardGears ? "sidecar"
                                                        : "inplace",
        requestedForward, static_cast<unsigned>(kEighthGearRawIndex),
        static_cast<double>(eighthRatio),
        static_cast<double>(baseEfficiency),
        static_cast<double>(match.entry->powerMultipliers[0]),
        static_cast<double>(eighthEfficiency),
        static_cast<double>(configuredEfficiencies[0]), ratioSlot,
        efficiencySlot,
        static_cast<unsigned>(target->totalRatioSlots),
        static_cast<unsigned>(target->sidecarRatioCount));
    for (std::size_t i = 0; i < requiredRatios; ++i) {
        diagnostic_log::Write(
            "GEAR_CONFIGURED target=%ld config='%s' forwardGear=%u "
            "rawIndex=%u ratio=%.9g powerMultiplier=%.9g "
            "publishedEfficiency=%.9g poweredEfficiency=%.9g",
            target->id, target->configName, static_cast<unsigned>(8u + i),
            static_cast<unsigned>(kEighthGearRawIndex + i),
            static_cast<double>(target->sidecarRatios[i]),
            static_cast<double>(target->powerMultipliers[i]),
            static_cast<double>(target->sidecarEfficiencies[i]),
            static_cast<double>(configuredEfficiencies[i]));
    }
}

void DiagnoseMatch(void* wrapper, void* result, std::uint32_t ctorKey) {
    const void* collection = nullptr;
    if (!game_access::ReadField(wrapper,
                                game_access::kWrapperCollectionOffset,
                                &collection) ||
        collection == nullptr) {
        return;
    }

    const Match match = FindCollectionMatch(collection);
    if (match.entry == nullptr) return;

    std::uint32_t collectionKey = 0;
    const void* parent = nullptr;
    const void* collectionClass = nullptr;
    const void* layout = nullptr;
    const bool keyReadable = game_access::ReadField(
        collection, game_access::kCollectionKeyOffset, &collectionKey);
    const bool parentReadable = game_access::ReadField(
        collection, game_access::kCollectionParentOffset, &parent);
    const bool classReadable = game_access::ReadField(
        collection, game_access::kCollectionClassOffset, &collectionClass);
    const bool layoutReadable = game_access::ReadField(
        collection, game_access::kCollectionLayoutOffset, &layout);

    const LONG sequence = InterlockedIncrement(&g_matchSequence);
    const bool emitMatch = sequence <= kMaxLoggedMatches;
    if (!emitMatch && sequence == kMaxLoggedMatches + 1) {
        diagnostic_log::Write(
            "MATCH log limit reached (%ld); later MATCH details suppressed",
            kMaxLoggedMatches);
    }
    if (emitMatch) {
        diagnostic_log::Write(
            "MATCH #%ld kind=%s depth=%u config='%s' requestedForward=%u "
            "configuredRatios=%zu",
            sequence, match.kind == MatchKind::Direct ? "direct" : "parent",
            match.depth, match.entry->name.c_str(),
            match.entry->maxForwardGears, match.entry->ratioCount);
        diagnostic_log::Write(
            "MATCH #%ld ctorKey=0x%08X wrapper=%p result=%p collection=%p",
            sequence, static_cast<unsigned>(ctorKey), wrapper, result,
            collection);
        diagnostic_log::Write(
            "MATCH #%ld collection mKey=%s0x%08X mParent=%s%p mClass=%s%p "
            "mLayout=%s%p",
            sequence, keyReadable ? "" : "unreadable/",
            static_cast<unsigned>(collectionKey),
            parentReadable ? "" : "unreadable/", parent,
            classReadable ? "" : "unreadable/", collectionClass,
            layoutReadable ? "" : "unreadable/", layout);
        if (match.kind == MatchKind::Parent) {
            diagnostic_log::Write(
                "MATCH #%ld matched parent collection=%p mKey=0x%08X "
                "depth=%u",
                sequence, match.collection, static_cast<unsigned>(match.key),
                match.depth);
        }
    }

    if (!classReadable || collectionClass == nullptr || !layoutReadable ||
        layout == nullptr) {
        if (emitMatch) {
            diagnostic_log::Write(
                "MATCH #%ld attribute diagnostics unavailable: "
                "null/unreadable mClass or mLayout",
                sequence);
        }
        return;
    }

    AttributeSnapshot ratio{};
    AttributeSnapshot efficiency{};
    LogAttributeArray(sequence, "GEAR_RATIO", game_access::kGearRatioKey,
                      collectionClass, layout, &ratio, emitMatch);
    LogAttributeArray(sequence, "GEAR_EFFICIENCY",
                      game_access::kGearEfficiencyKey, collectionClass,
                      layout, &efficiency, emitMatch);

    if (!ratio.valid || !efficiency.valid || ratio.offset != kRatioOffset ||
        efficiency.offset != kEfficiencyOffset) {
        if (emitMatch) {
            diagnostic_log::Write(
                "MATCH #%ld TARGET_SKIPPED unexpected or unsafe array layout",
                sequence);
        }
        return;
    }

    if (match.kind != MatchKind::Direct) {
        if (emitMatch) {
            diagnostic_log::Write(
                "MATCH #%ld TARGET_SKIPPED parent-only match is diagnostic; "
                "a direct collection match is required",
                sequence);
        }
        return;
    }

    bool created = false;
    TargetObservation* target = RegisterTarget(
        *match.entry, match.key, collection, layout, ratio, efficiency,
        &created);
    if (target == nullptr) {
        if (InterlockedCompareExchange(&g_targetCapacityNotice, 1, 0) == 0) {
            diagnostic_log::Write(
                "TARGET_SKIPPED registry capacity reached (%ld)",
                kMaxObservedTargets);
        }
        return;
    }

    if (created || emitMatch) {
        diagnostic_log::Write(
            "%s target=%ld config='%s' key=0x%08X collection=%p layout=%p "
            "ratioHeader=%p efficiencyHeader=%p ratioCount=%u "
            "efficiencyCount=%u",
            created ? "TARGET_REGISTER" : "TARGET_REUSE", target->id,
            target->configName, static_cast<unsigned>(target->collectionKey),
            target->collection, target->layout, target->ratioHeader,
            target->efficiencyHeader,
            static_cast<unsigned>(ratio.header.count),
            static_cast<unsigned>(efficiency.header.count));
    }
    TryEnableEighthGear(target, match, ctorKey, keyReadable, collectionKey,
                        layout, ratio, efficiency);
}

void* NFSMW_FASTCALL TransmissionCtorDetour(void* self, void* edx,
                                            std::uint32_t key) {
    TransmissionCtorFn original = g_originalCtor;
    if (original == nullptr) {
        diagnostic_log::Write("fatal: constructor trampoline is null");
        return self;
    }

    void* result = original(self, edx, key);
#if defined(_MSC_VER)
    __try {
        DiagnoseMatch(self, result, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        diagnostic_log::Write(
            "diagnostic read fault for ctorKey=0x%08X; collection unchanged",
            static_cast<unsigned>(key));
    }
#else
    DiagnoseMatch(self, result, key);
#endif
    return result;
}

bool IsFixedTenSlotConsumer(const void* caller) {
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(caller);
    // These routines pass fixed ten-slot scratch/output buffers to the
    // game's derived-table builder. Do not expose the larger sidecar count
    // to them; the extra values remain in plugin-owned storage.
    return (address >= 0x00673430u && address <= 0x00673510u) ||
           (address >= 0x00673980u && address <= 0x00673B20u) ||
           (address >= 0x00691E30u && address <= 0x00691FF0u) ||
           (address >= 0x00692560u && address <= 0x00692620u) ||
           (address >= 0x0066C5F0u && address <= 0x0066C680u) ||
           // 6A1560 scans a fixed native ratio list directly. Keep its
           // count at the verified ten-slot prefix until that long routine
           // is replaced with a sidecar-aware implementation.
           (address >= 0x006A1560u && address <= 0x006A1729u) ||
           // 6A1A22 indexes the native fixed-size ratio list after its
           // count call; clamp that caller before it can form an unsafe
           // sidecar index.
           address == 0x006A1A22u ||
           // The same routine immediately queries GEAR_EFFICIENCY and
           // indexes that fixed-size list through its second count call.
           address == 0x006A1A3Bu ||
           // 6A32EF is the return address immediately after the
           // Private::Count call in the alternate setter. High requests are
           // handled by its dedicated detour; keep the native fallback
           // bounded to the in-layout ten-slot prefix.
           address == 0x006A32EFu ||
           // 6B029B is the return address immediately after the
           // Private::Count call in the secondary top-gear getter.
           address == 0x006B029Bu ||
           // 6B022B is the return address immediately after the
           // Private::Count call in the primary top-gear getter.
           address == 0x006B022Bu;
}

std::uint32_t VirtualSlotCount(TargetObservation* target) {
    if (target == nullptr || target->totalRatioSlots == 0) {
        return kEighthGearTotalSlots;
    }
    return std::min<std::uint32_t>(target->totalRatioSlots,
                                   kMaxVirtualTotalSlots);
}

std::uint32_t NFSMW_FASTCALL PrivateCountDetour(const void* privateHeader,
                                                 void*) {
    PrivateCountFn original = g_originalPrivateCount;
    if (original == nullptr) return 0;

    const void* caller = _ReturnAddress();
    const std::uint32_t nativeResult = original(privateHeader);

    ArrayKind kind = ArrayKind::Ratio;
    TargetObservation* target = FindTargetByHeader(privateHeader, &kind);
    game_access::PrivateHeader liveHeader{};
    const bool headerValid =
        target != nullptr &&
        game_access::SafeRead(privateHeader, &liveHeader, sizeof(liveHeader)) &&
        IsNativeInlineFloatHeader(liveHeader);
    const bool virtualized = IsEighthGearEnabled(target) &&
                             headerValid && nativeResult == kNativeTotalSlots;
    const bool clampForFixedConsumer =
        virtualized && IsSidecarEnabled(target) &&
        IsFixedTenSlotConsumer(caller);
    const std::uint32_t result =
        virtualized
            ? (clampForFixedConsumer ? kEighthGearTotalSlots
                                     : VirtualSlotCount(target))
            : nativeResult;
    if (target == nullptr ||
        !MarkCallerOnce(CountCallerSlots(target, kind), caller) ||
        !ReserveTraceLine()) {
        return result;
    }

    diagnostic_log::Write(
        "TRACE COUNT target=%ld config='%s' array=%s caller=%p "
        "callerRva=0x%08X nativeResult=%u result=%u virtualized=%s",
        target->id, target->configName, ArrayName(kind), caller,
        static_cast<unsigned>(CallerRva(caller)),
        static_cast<unsigned>(nativeResult), static_cast<unsigned>(result),
        virtualized ? (clampForFixedConsumer ? "clamped" : "yes") : "no");
    return result;
}

const void* NFSMW_FASTCALL PrivateGetElementDetour(
    const void* privateHeader, void*, std::uint32_t index) {
    PrivateGetElementFn original = g_originalPrivateGetElement;
    if (original == nullptr) return nullptr;

    const void* caller = _ReturnAddress();
    ArrayKind kind = ArrayKind::Ratio;
    TargetObservation* target = FindTargetByHeader(privateHeader, &kind);
    if (target == nullptr) return original(privateHeader, index);

    game_access::PrivateHeader liveHeader{};
    const bool headerValid =
        game_access::SafeRead(privateHeader, &liveHeader, sizeof(liveHeader)) &&
        IsNativeInlineFloatHeader(liveHeader);
    const bool virtualized = IsVirtualGearIndex(target, index) && headerValid;
    if (IsProtectedHighGearIndex(target, index) && !virtualized) {
        return nullptr;
    }
    const void* result = nullptr;
    if (virtualized) {
        if (IsSidecarEnabled(target)) {
            result = (kind == ArrayKind::Ratio)
                         ? static_cast<const void*>(
                               &target->sidecarRatios[index -
                                                       kEighthGearRawIndex])
                         : static_cast<const void*>(
                               &target->sidecarEfficiencies[index -
                                                             kEighthGearRawIndex]);
        } else {
            const auto* bytes = static_cast<const std::uint8_t*>(privateHeader);
            result = bytes + sizeof(game_access::PrivateHeader) +
                     kEighthGearRawIndex * sizeof(float);
        }
    } else {
        result = original(privateHeader, index);
    }

    const bool newCaller =
        MarkCallerOnce(ElementCallerSlots(target, kind), caller);
    const bool newIndex =
        MarkIndexOnce(ElementIndexMask(target, kind), index,
                      &target->invalidElementLogged);
    if ((!newCaller && !newIndex) || !ReserveTraceLine()) return result;

    float value = 0.0f;
    const bool valueReadable =
        headerValid && result != nullptr &&
        game_access::SafeRead(result, &value, sizeof(value));
    if (valueReadable) {
        diagnostic_log::Write(
            "TRACE ELEMENT target=%ld config='%s' array=%s caller=%p "
            "callerRva=0x%08X index=%u result=%p value=%.9g virtualized=%s",
            target->id, target->configName, ArrayName(kind), caller,
            static_cast<unsigned>(CallerRva(caller)),
            static_cast<unsigned>(index), result, static_cast<double>(value),
            virtualized ? "yes" : "no");
    } else {
        diagnostic_log::Write(
            "TRACE ELEMENT target=%ld config='%s' array=%s caller=%p "
            "callerRva=0x%08X index=%u result=%p value=unavailable "
            "virtualized=%s",
            target->id, target->configName, ArrayName(kind), caller,
            static_cast<unsigned>(CallerRva(caller)),
            static_cast<unsigned>(index), result,
            virtualized ? "yes" : "no");
    }
    return result;
}

bool ReadNativeGearValue(TargetObservation* target, ArrayKind kind,
                         std::uint32_t index, float* value) {
    if (target == nullptr || value == nullptr || index >= kNativeTotalSlots) {
        return false;
    }
    const void* headerAddress =
        kind == ArrayKind::Ratio ? target->ratioHeader : target->efficiencyHeader;
    game_access::PrivateHeader header{};
    if (!game_access::SafeRead(headerAddress, &header, sizeof(header)) ||
        header.elementSize != sizeof(float) || index >= header.count ||
        index >= header.capacity) {
        return false;
    }
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(headerAddress);
    const std::size_t dataOffset = game_access::PrivateDataOffset(header);
    const std::uintptr_t element =
        base + dataOffset + static_cast<std::uintptr_t>(index) * sizeof(float);
    return game_access::SafeRead(reinterpret_cast<const void*>(element), value,
                                 sizeof(*value));
}

bool ReadConfiguredGearValue(TargetObservation* target, ArrayKind kind,
                             std::uint32_t index, float* value) {
    if (target == nullptr || value == nullptr ||
        !IsVirtualGearIndex(target, index)) {
        return false;
    }
    if (index == kEighthGearRawIndex) {
        *value = kind == ArrayKind::Ratio ? target->eighthRatio
                                          : target->eighthEfficiency;
        return std::isfinite(*value) && *value > 0.0f;
    }
    if (!IsSidecarGearIndex(target, index)) return false;
    *value = kind == ArrayKind::Ratio ? SidecarRatio(target, index)
                                      : SidecarEfficiency(target, index);
    return std::isfinite(*value) && *value > 0.0f;
}

bool ReadConfiguredPowerMultiplier(TargetObservation* target,
                                   std::uint32_t index, float* value) {
    if (target == nullptr || value == nullptr ||
        !IsVirtualGearIndex(target, index) ||
        index < kEighthGearRawIndex || index > kMaxVirtualRawIndex) {
        return false;
    }
    *value = target->powerMultipliers[index - kEighthGearRawIndex];
    return std::isfinite(*value) && *value > 0.0f;
}

bool ReadRuntimeGearEfficiency(TargetObservation* target,
                               std::uint32_t index,
                               const void* currentGearObject,
                               std::size_t currentGearOffset,
                               const void* caller,
                               std::uintptr_t expectedCaller,
                               PowerRoute route, float* value) {
    float baseEfficiency = 0.0f;
    float powerMultiplier = 0.0f;
    if (!ReadConfiguredGearValue(target, ArrayKind::Efficiency, index,
                                 &baseEfficiency) ||
        !ReadConfiguredPowerMultiplier(target, index, &powerMultiplier)) {
        return false;
    }

    std::int32_t currentGear = -1;
    // A failed current-gear read deliberately leaves the static/base value in
    // effect. It must never make a multiplier visible to a table scan.
    game_access::ReadField(currentGearObject, currentGearOffset, &currentGear);
    const bool verifiedRuntimePowerCall =
        reinterpret_cast<std::uintptr_t>(caller) == expectedCaller;
    const bool selected = gear_power::SelectRuntimeEfficiency(
        baseEfficiency, powerMultiplier, index, currentGear,
        kEighthGearRawIndex, kMaxVirtualRawIndex, verifiedRuntimePowerCall,
        value);
    const bool applied =
        selected && verifiedRuntimePowerCall && currentGear >= 0 &&
        index == static_cast<std::uint32_t>(currentGear);
    if (applied && MarkPowerAppliedOnce(target, route, index) &&
        ReserveTraceLine()) {
        diagnostic_log::Write(
            "TRACE POWER_APPLIED target=%ld config='%s' route=%s caller=%p "
            "callerRva=0x%08X rawIndex=%u current=%d baseEfficiency=%.9g "
            "powerMultiplier=%.9g result=%.9g",
            target->id, target->configName, PowerRouteName(route), caller,
            static_cast<unsigned>(CallerRva(caller)),
            static_cast<unsigned>(index), static_cast<int>(currentGear),
            static_cast<double>(baseEfficiency),
            static_cast<double>(powerMultiplier),
            static_cast<double>(*value));
    }
    return selected;
}

bool ReadGearValue(TargetObservation* target, ArrayKind kind,
                   std::uint32_t index, float* value) {
    if (ReadConfiguredGearValue(target, kind, index, value)) return true;
    return ReadNativeGearValue(target, kind, index, value);
}

bool ReadLayoutFloat(TargetObservation* target, std::size_t offset,
                     float* value);

bool ReadDecisionEffectiveRatio(void* transmission,
                                TargetObservation* target,
                                std::uint32_t index, float* value) {
    if (target == nullptr || value == nullptr || !IsSidecarEnabled(target) ||
        index >= VirtualSlotCount(target) || index <= 1u) {
        return false;
    }

    float ratio = 0.0f;
    if (!ReadGearValue(target, ArrayKind::Ratio, index, &ratio) ||
        !std::isfinite(ratio) || ratio <= 0.0f) {
        return false;
    }

    // 0x692F10 uses the native helper for indices below the sidecar range.
    // For raw 9..13, calculate the same final-drive * ratio value from the
    // sidecar so no out-of-line native array access can occur.
    if (index > kEighthGearRawIndex) {
        float finalDrive = 0.0f;
        if (!ReadLayoutFloat(target, 0x78u, &finalDrive) ||
            !std::isfinite(finalDrive)) {
            return false;
        }
        const float result = finalDrive * ratio;
        if (!std::isfinite(result)) return false;
        *value = result;
        return true;
    }

    if (g_originalEffectiveRatioHelper != nullptr && transmission != nullptr) {
        const float native = g_originalEffectiveRatioHelper(transmission, index);
        if (std::isfinite(native)) {
            *value = native;
            return true;
        }
    }

    // The fallback is useful during early construction, when the original
    // helper's private header is not published yet but the layout is known.
    float finalDrive = 0.0f;
    if (!ReadLayoutFloat(target, 0x78u, &finalDrive)) return false;
    const float result = finalDrive * ratio;
    if (!std::isfinite(result)) return false;
    *value = result;
    return true;
}

bool ComputeAdditionalShiftDecision(void* transmission,
                                    TargetObservation* target,
                                    float reference,
                                    std::uint32_t fromGear,
                                    std::uint32_t toGear,
                                    float* result) {
    // 0x00693010 reads a separate per-gear threshold table at
    // [this+0x9C+fromGear*4].  That table is not GEAR_RATIO and there is no
    // verified sidecar representation for it yet.  Returning a calculated
    // value from the ordinary ratio would silently change shift behavior, so
    // keep this helper fail-closed until the table is reverse engineered.
    (void)transmission;
    (void)target;
    (void)reference;
    (void)fromGear;
    (void)toGear;
    if (result != nullptr) *result = 0.0f;
    return false;
}

bool ReadLayoutFloat(TargetObservation* target, std::size_t offset,
                     float* value) {
    if (target == nullptr || value == nullptr || target->layout == nullptr) {
        return false;
    }
    return game_access::ReadField(target->layout, offset, value) &&
           std::isfinite(*value);
}

float NFSMW_FASTCALL LayoutRatioGetterDetour(void* object, void*,
                                             std::uint32_t index) {
    GearFloatFn original = g_originalLayoutRatioGetter;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForLayoutField(object, 0xF4u, &layout);
    float value = 0.0f;
    if (IsHighSidecarGearIndex(target, index)) {
        return ReadConfiguredGearValue(target, ArrayKind::Ratio, index, &value)
                   ? value
                   : 0.0f;
    }
    if (IsProtectedHighGearIndex(target, index)) return 0.0f;
    return original(object, index);
}

float NFSMW_FASTCALL LayoutEfficiencyGetterDetour(void* object, void*,
                                                   std::uint32_t index) {
    GearFloatFn original = g_originalLayoutEfficiencyGetter;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForLayoutField(object, 0xF4u, &layout);
    float value = 0.0f;
    if (IsVirtualGearIndex(target, index)) {
        // No direct live-power caller of 0x006A0F20 has been verified. Keep
        // this generic layout view neutral so background table work cannot
        // publish a high-gear multiplier into shared derived state.
        return ReadConfiguredGearValue(target, ArrayKind::Efficiency, index,
                                       &value)
                   ? value
                   : 0.0f;
    }
    if (IsProtectedHighGearIndex(target, index)) return 0.0f;
    return original(object, index);
}

float NFSMW_FASTCALL LayoutEffectiveRatioDetour(void* object, void*,
                                                std::uint32_t index) {
    GearFloatFn original = g_originalLayoutEffectiveRatio;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForLayoutField(object, 0xF4u, &layout);
    float ratio = 0.0f;
    float finalDrive = 0.0f;
    if (IsHighSidecarGearIndex(target, index)) {
        return ReadConfiguredGearValue(target, ArrayKind::Ratio, index,
                                       &ratio) &&
                       ReadLayoutFloat(target, 0x7Cu, &finalDrive)
                   ? finalDrive * ratio
                   : 0.0f;
    }
    if (IsProtectedHighGearIndex(target, index)) return 0.0f;
    return original(object, index);
}

float NFSMW_FASTCALL ControllerRatioGetterDetour(void* object, void*,
                                                 std::uint32_t index) {
    GearFloatFn original = g_originalControllerRatioGetter;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForLayoutField(object, 0xF0u, &layout);
    float value = 0.0f;
    if (IsHighSidecarGearIndex(target, index)) {
        return ReadConfiguredGearValue(target, ArrayKind::Ratio, index, &value)
                   ? value
                   : 0.0f;
    }
    if (IsProtectedHighGearIndex(target, index)) return 0.0f;
    return original(object, index);
}

float NFSMW_FASTCALL ControllerEfficiencyGetterDetour(void* object, void*,
                                                       std::uint32_t index) {
    GearFloatFn original = g_originalControllerEfficiencyGetter;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForLayoutField(object, 0xF0u, &layout);
    float value = 0.0f;
    if (IsVirtualGearIndex(target, index)) {
        const void* caller = _ReturnAddress();
        return ReadRuntimeGearEfficiency(target, index, object,
                                         kControllerCurrentGearOffset, caller,
                                         kControllerEfficiencyPowerReturn,
                                         PowerRoute::Controller, &value)
                   ? value
                   : 0.0f;
    }
    if (IsProtectedHighGearIndex(target, index)) return 0.0f;
    return original(object, index);
}

float NFSMW_FASTCALL ControllerEffectiveRatioDetour(void* object, void*,
                                                    std::uint32_t index) {
    GearFloatFn original = g_originalControllerEffectiveRatio;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForLayoutField(object, 0xF0u, &layout);
    float ratio = 0.0f;
    float finalDrive = 0.0f;
    if (IsHighSidecarGearIndex(target, index)) {
        return ReadConfiguredGearValue(target, ArrayKind::Ratio, index,
                                       &ratio) &&
                       ReadLayoutFloat(target, 0x7Cu, &finalDrive)
                   ? finalDrive * ratio
                   : 0.0f;
    }
    if (IsProtectedHighGearIndex(target, index)) return 0.0f;
    return original(object, index);
}

float NFSMW_FASTCALL TorqueEfficiencyRatioDetour(void* object, void*,
                                                 std::uint32_t index) {
    GearFloatFn original = g_originalTorqueEfficiencyRatio;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForLayoutField(object, 0xF4u, &layout);
    if (!IsVirtualGearIndex(target, index)) {
        if (IsProtectedHighGearIndex(target, index)) return 0.0f;
        return original(object, index);
    }

    float baseRatio = 0.0f;
    float requestedRatio = 0.0f;
    float baseEfficiency = 0.0f;
    float requestedEfficiency = 0.0f;
    const void* caller = _ReturnAddress();
    if (!ReadConfiguredGearValue(target, ArrayKind::Ratio,
                                 kEighthGearRawIndex, &baseRatio) ||
        !ReadConfiguredGearValue(target, ArrayKind::Ratio, index,
                                  &requestedRatio) ||
        !ReadConfiguredGearValue(target, ArrayKind::Efficiency,
                                  kEighthGearRawIndex, &baseEfficiency) ||
        !ReadRuntimeGearEfficiency(target, index, object,
                                   kLayoutCurrentGearOffset, caller,
                                   kTorqueEfficiencyPowerReturn,
                                   PowerRoute::Torque,
                                   &requestedEfficiency)) {
        return 0.0f;
    }

    // The native result has ratio and efficiency in its denominator. Use the
    // neutral raw-9 calculation as a proxy, then replace both terms. Runtime
    // efficiency is multiplied only when this query is for the current gear.
    const float base = original(object, kEighthGearRawIndex);
    float result = 0.0f;
    return gear_power::ScaleRatioEfficiencyResult(
               base, baseRatio, baseEfficiency, requestedRatio,
               requestedEfficiency, &result)
               ? result
               : 0.0f;
}

float NFSMW_FASTCALL DerivedShiftValueDetour(void* object, void*,
                                             std::uint32_t index) {
    DerivedGearValueFn original = g_originalDerivedShiftValue;
    if (original == nullptr) return 0.0f;
    TargetObservation* target = FindTargetForPhysical(object, nullptr);
    if (IsHighSidecarGearIndex(target, index)) {
        // No native storage exists for raw indices 10..13. Returning zero is
        // a safe sentinel for callers that are not yet sidecar-aware.
        return 0.0f;
    }
    if (IsProtectedHighGearIndex(target, index)) return 0.0f;
    return original(object, index);
}

float NFSMW_FASTCALL DerivedEfficiencyValueDetour(void* object, void*,
                                                  std::uint32_t index) {
    DerivedGearValueFn original = g_originalDerivedEfficiencyValue;
    if (original == nullptr) return 0.0f;
    TargetObservation* target = FindTargetForPhysical(object, nullptr);
    if (IsHighSidecarGearIndex(target, index)) {
        return 0.0f;
    }
    if (IsProtectedHighGearIndex(target, index)) return 0.0f;
    return original(object, index);
}

float NFSMW_FASTCALL GearRatioGetterDetour(void* transmission, void*,
                                           std::uint32_t index) {
    GearFloatFn original = g_originalGearRatioGetter;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForPhysical(transmission, &layout);
    float value = 0.0f;
    if (IsHighSidecarGearIndex(target, index)) {
        return ReadConfiguredGearValue(target, ArrayKind::Ratio, index, &value)
                   ? value
                   : 0.0f;
    }
    if (ReadConfiguredGearValue(target, ArrayKind::Ratio, index, &value)) {
        return value;
    }
    if (IsProtectedHighGearIndex(target, index)) return 0.0f;
    return original(transmission, index);
}

float NFSMW_FASTCALL GearEfficiencyGetterDetour(void* transmission, void*,
                                                std::uint32_t index) {
    GearFloatFn original = g_originalGearEfficiencyGetter;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForPhysical(transmission, &layout);
    float value = 0.0f;
    if (IsVirtualGearIndex(target, index)) {
        const void* caller = _ReturnAddress();
        return ReadRuntimeGearEfficiency(target, index, transmission,
                                         kPhysicalCurrentGearOffset, caller,
                                         kPrimaryEfficiencyPowerReturn,
                                         PowerRoute::Primary, &value)
                   ? value
                   : 0.0f;
    }
    if (IsProtectedHighGearIndex(target, index)) return 0.0f;
    return original(transmission, index);
}

float NFSMW_FASTCALL GearRatioPairDetour(void* transmission, void*,
                                         std::uint32_t first,
                                         std::uint32_t second) {
    GearRatioPairFn original = g_originalGearRatioPair;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForPhysical(transmission, &layout);
    if ((IsProtectedHighGearIndex(target, first) &&
         !IsVirtualGearIndex(target, first)) ||
        (IsProtectedHighGearIndex(target, second) &&
         !IsVirtualGearIndex(target, second))) {
        return 0.0f;
    }
    if (!IsVirtualGearIndex(target, first) &&
        !IsVirtualGearIndex(target, second)) {
        return original(transmission, first, second);
    }

    float numerator = 0.0f;
    float denominator = 0.0f;
    if (!ReadGearValue(target, ArrayKind::Ratio, first, &numerator) ||
        !ReadGearValue(target, ArrayKind::Ratio, second, &denominator) ||
        !std::isfinite(numerator) || !std::isfinite(denominator) ||
        numerator <= 0.0f || denominator <= 0.000001f) {
        return 0.0f;
    }
    return numerator / denominator;
}

float NFSMW_FASTCALL EffectiveGearRatioDetour(void* transmission, void*,
                                              std::uint32_t index) {
    GearFloatFn original = g_originalEffectiveGearRatio;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForPhysical(transmission, &layout);
    float ratio = 0.0f;
    if (IsHighSidecarGearIndex(target, index) &&
        !ReadConfiguredGearValue(target, ArrayKind::Ratio, index, &ratio)) {
        return 0.0f;
    }
    if (!ReadConfiguredGearValue(target, ArrayKind::Ratio, index, &ratio)) {
        if (IsProtectedHighGearIndex(target, index)) return 0.0f;
        return original(transmission, index);
    }
    float multiplier = 0.0f;
    if (!ReadLayoutFloat(target, 0x7Cu, &multiplier)) return 0.0f;
    return multiplier * ratio;
}

float NFSMW_FASTCALL EffectiveRatioHelperDetour(void* transmission, void*,
                                                std::uint32_t index) {
    GearFloatFn original = g_originalEffectiveRatioHelper;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForTransmissionObject(transmission, &layout);
    float ratio = 0.0f;
    if (IsHighSidecarGearIndex(target, index) &&
        !ReadConfiguredGearValue(target, ArrayKind::Ratio, index, &ratio)) {
        return 0.0f;
    }
    if (!ReadConfiguredGearValue(target, ArrayKind::Ratio, index, &ratio)) {
        if (IsProtectedHighGearIndex(target, index)) return 0.0f;
        return original(transmission, index);
    }
    float multiplier = 0.0f;
    if (!ReadLayoutFloat(target, 0x78u, &multiplier)) return 0.0f;
    return multiplier * ratio;
}

std::uint32_t NFSMW_FASTCALL TransmissionUpdateDetour(
    void* transmission, void*, std::uint32_t gear, float reference) {
    TransmissionUpdateFn original = g_originalTransmissionUpdate;
    if (original == nullptr) return 0;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForTransmissionObject(transmission, &layout);
    if (IsProtectedHighGearIndex(target, gear) &&
        !IsSidecarGearIndex(target, gear)) {
        return 0;
    }
    if (!IsSidecarGearIndex(target, gear) || gear <= kEighthGearRawIndex) {
        return original(transmission, gear, reference);
    }

    // The native helper has only the verified ten-slot derived prefix. Run
    // its proven math against gear eight, scaling the reference by the ratio
    // change; all direct high-gear reads are supplied by the sidecar hooks.
    float baseRatio = 0.0f;
    float requestedRatio = 0.0f;
    if (!ReadConfiguredGearValue(target, ArrayKind::Ratio,
                                 kEighthGearRawIndex, &baseRatio) ||
        !ReadConfiguredGearValue(target, ArrayKind::Ratio, gear,
                                 &requestedRatio) ||
        baseRatio <= 0.000001f || !std::isfinite(reference)) {
        return 0;
    }
    const float adjustedReference =
        reference * (requestedRatio / baseRatio);
    return original(transmission, kEighthGearRawIndex, adjustedReference);
}

float NFSMW_FASTCALL AdditionalShiftDecisionDetour(
    void* transmission, void*, float reference, std::uint32_t fromGear,
    std::uint32_t toGear) {
    AdditionalShiftDecisionFn original = g_originalAdditionalShiftDecision;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForTransmissionObject(transmission, &layout);
    if (IsProtectedHighGearIndex(target, fromGear) ||
        IsProtectedHighGearIndex(target, toGear)) {
        return 0.0f;
    }
    if (!IsSidecarEnabled(target)) {
        return original(transmission, reference, fromGear, toGear);
    }
    // The native function is safe for the verified inline prefix (raw 0..9),
    // including the configured eighth gear.  Any sidecar index would make
    // its [this+0x9C+from*4] read cross into unrelated object fields, and the
    // threshold table has not been reconstructed yet.
    if (fromGear > kEighthGearRawIndex || toGear > kEighthGearRawIndex) {
        return 0.0f;
    }
    return original(transmission, reference, fromGear, toGear);
}

std::uint32_t NFSMW_FASTCALL GearDecisionDetour(
    void* object, void*, std::uint32_t gear, float reference) {
    GearDecisionFn original = g_originalGearDecision;
    if (original == nullptr) return 0;

    const void* layout = nullptr;
    TargetObservation* target = FindTargetForBaseObject(object, &layout);
    if (IsProtectedHighGearIndex(target, gear)) return 0;
    if (!IsSidecarEnabled(target) || gear <= kEighthGearRawIndex) {
        return original(object, gear, reference);
    }
    // 6A0D90 has an inline [object+0x9C+gear*4] read.  Its table is not a
    // ratio array and cannot represent sidecar indices 10..13 by simply
    // substituting raw 9; doing so produces a plausible but wrong automatic
    // decision.  Keep the automatic path fail-closed until that controller is
    // reimplemented with a sidecar table.  Manual ShiftGear remains enabled.
    if (!IsVirtualGearIndex(target, gear)) return 0;
    (void)reference;
    return 0;
}

std::uint32_t NFSMW_FASTCALL TransmissionTopGearDetour(void* transmission,
                                                       void*) {
    TransmissionTopGearFn original = g_originalTransmissionTopGear;
    if (original == nullptr) return 0;
    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForEmbeddedTransmission(transmission, &layout);
    if (IsEighthGearEnabled(target)) {
        return VirtualSlotCount(target) - 1u;
    }
    return original(transmission);
}

std::uint32_t NFSMW_FASTCALL TransmissionSetGearDetour(
    void* transmission, void*, std::int32_t requestedGear) {
    TransmissionSetGearFn original = g_originalTransmissionSetGear;
    if (original == nullptr) return 0;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForEmbeddedTransmission(transmission, &layout);
    if (!IsEighthGearEnabled(target) || requestedGear < 0) {
        return original(transmission, requestedGear);
    }
    const auto requestedIndex = static_cast<std::uint32_t>(requestedGear);
    if (IsProtectedHighGearIndex(target, requestedIndex) &&
        !IsVirtualGearIndex(target, requestedIndex)) {
        return 0;
    }
    if (requestedGear < static_cast<std::int32_t>(kEighthGearRawIndex) ||
        requestedIndex >= VirtualSlotCount(target)) {
        return original(transmission, requestedGear);
    }

    std::int32_t currentGear = 0;
    if (!game_access::ReadField(transmission, 0x1Cu, &currentGear) ||
        currentGear == requestedGear) {
        return 0;
    }

    float ratio = 0.0f;
    float finalDrive = 0.0f;
    if (!ReadConfiguredGearValue(target, ArrayKind::Ratio,
                                 static_cast<std::uint32_t>(requestedGear),
                                 &ratio) ||
        !ReadLayoutFloat(target, 0x7Cu, &finalDrive) ||
        !std::isfinite(ratio) || !std::isfinite(finalDrive)) {
        return 0;
    }

    float reverseScale = 1.0f;
    if (requestedGear < currentGear &&
        !game_access::SafeRead(reinterpret_cast<const void*>(0x008910F0u),
                               &reverseScale, sizeof(reverseScale))) {
        return 0;
    }
    if (!std::isfinite(reverseScale)) return 0;
    const float effective = finalDrive * ratio * reverseScale;
    if (!std::isfinite(effective)) return 0;

    auto* bytes = static_cast<std::uint8_t*>(transmission);
    float oldEffective = 0.0f;
    if (!game_access::ReadField(transmission, 0x24u, &oldEffective)) {
        return 0;
    }
    if (!game_access::SafeWrite(bytes + 0x24u, &effective, sizeof(effective)) ||
        !game_access::SafeWrite(bytes + 0x1Cu, &requestedGear,
                                sizeof(requestedGear))) {
        game_access::SafeWrite(bytes + 0x24u, &oldEffective,
                               sizeof(oldEffective));
        game_access::SafeWrite(bytes + 0x1Cu, &currentGear,
                               sizeof(currentGear));
        return 0;
    }
    return 1;
}

std::uint32_t NFSMW_FASTCALL AlternateTransmissionTopGearDetour(
    void* transmission, void*) {
    TransmissionTopGearFn original = g_originalAlternateTransmissionTopGear;
    if (original == nullptr) return 0;

    TargetObservation* target =
        FindTargetForAlternateTransmission(transmission);
    if (IsEighthGearEnabled(target)) {
        return VirtualSlotCount(target) - 1u;
    }
    return original(transmission);
}

std::uint32_t NFSMW_FASTCALL AlternateTransmissionSetGearDetour(
    void* transmission, void*, std::int32_t requestedGear) {
    TransmissionSetGearFn original = g_originalAlternateTransmissionSetGear;
    if (original == nullptr) return 0;

    TargetObservation* target =
        FindTargetForAlternateTransmission(transmission);
    if (!IsEighthGearEnabled(target) || requestedGear < 0) {
        return original(transmission, requestedGear);
    }
    const auto requestedIndex = static_cast<std::uint32_t>(requestedGear);
    if (IsProtectedHighGearIndex(target, requestedIndex) &&
        !IsVirtualGearIndex(target, requestedIndex)) {
        return 0;
    }
    if (requestedGear < static_cast<std::int32_t>(kEighthGearRawIndex) ||
        requestedIndex >= VirtualSlotCount(target)) {
        return original(transmission, requestedGear);
    }

    std::int32_t currentGear = 0;
    if (!game_access::ReadField(transmission, 0x18u, &currentGear) ||
        currentGear == requestedGear) {
        return 0;
    }

    float ratio = 0.0f;
    float finalDrive = 0.0f;
    if (!ReadConfiguredGearValue(target, ArrayKind::Ratio,
                                 static_cast<std::uint32_t>(requestedGear),
                                 &ratio) ||
        !ReadLayoutFloat(target, 0x7Cu, &finalDrive) ||
        !std::isfinite(finalDrive)) {
        return 0;
    }

    float reverseScale = 1.0f;
    if (requestedGear < currentGear &&
        !game_access::SafeRead(reinterpret_cast<const void*>(0x008910F0u),
                               &reverseScale, sizeof(reverseScale))) {
        return 0;
    }
    const float effective = finalDrive * ratio * reverseScale;
    if (!std::isfinite(effective)) return 0;

    auto* bytes = static_cast<std::uint8_t*>(transmission);
    float oldEffective = 0.0f;
    if (!game_access::ReadField(transmission, 0x1Cu, &oldEffective) ||
        !game_access::SafeWrite(bytes + 0x1Cu, &effective,
                                sizeof(effective)) ||
        !game_access::SafeWrite(bytes + 0x18u, &requestedGear,
                                sizeof(requestedGear))) {
        game_access::SafeWrite(bytes + 0x1Cu, &oldEffective,
                               sizeof(oldEffective));
        game_access::SafeWrite(bytes + 0x18u, &currentGear,
                               sizeof(currentGear));
        return 0;
    }
    return 1;
}

float NFSMW_FASTCALL AlternateGearPairDetour(
    void* transmission, void*, std::uint32_t first, std::uint32_t second) {
    GearPairValueFn original = g_originalAlternateGearPair;
    if (original == nullptr) return 0.0f;

    const void* layout = nullptr;
    TargetObservation* target =
        FindTargetForEmbeddedTransmission(transmission, &layout);
    if ((IsProtectedHighGearIndex(target, first) &&
         !IsVirtualGearIndex(target, first)) ||
        (IsProtectedHighGearIndex(target, second) &&
         !IsVirtualGearIndex(target, second))) {
        return 0.0f;
    }
    if (!IsSidecarEnabled(target) ||
        (!IsVirtualGearIndex(target, first) &&
         !IsVirtualGearIndex(target, second))) {
        return original(transmission, first, second);
    }

    // A configured target must never pass an unknown high index through the
    // native helper.  It only indexes by `first`, but malformed high values
    // still need to be rejected before they can influence the direction map.
    if ((first > kEighthGearRawIndex && !IsVirtualGearIndex(target, first)) ||
        (second > kEighthGearRawIndex && !IsVirtualGearIndex(target, second))) {
        return 0.0f;
    }

    // Preserve the native equal-gear sentinel.  The proxy below intentionally
    // collapses high `first` values to raw 9, so handling equality up front is
    // required; otherwise a high/high equal pair would be mistaken for a
    // downshift (raw 9 -> raw 8).
    if (first == second) {
        const std::uint32_t safe = first > kEighthGearRawIndex
                                       ? kEighthGearRawIndex
                                       : first;
        return original(transmission, safe, safe);
    }

    // The native helper only indexes its fixed transition tables by `first`.
    // Keep its comparison direction while substituting raw-9, then scale the
    // result when the first gear is a sidecar entry.  When only `second` is a
    // sidecar gear, retain a safe greater-than sentinel instead of collapsing
    // it to raw 9; raw 9 -> raw 10 must remain an ascending pair.
    const bool firstVirtual = IsVirtualGearIndex(target, first) &&
                              first > kEighthGearRawIndex;
    const std::uint32_t safeFirst = firstVirtual ? kEighthGearRawIndex : first;
    std::uint32_t safeSecond = second;
    if (firstVirtual) {
        safeSecond = second > first ? kEighthGearRawIndex + 1u
                                     : kEighthGearRawIndex - 1u;
    } else if (IsVirtualGearIndex(target, second) &&
               second > kEighthGearRawIndex) {
        safeSecond = kEighthGearRawIndex + 1u;
    }
    if (safeFirst == safeSecond || safeFirst < 2u || safeSecond < 2u) {
        return 0.0f;
    }
    const float native = original(transmission, safeFirst, safeSecond);
    if (!firstVirtual) return std::isfinite(native) ? native : 0.0f;

    float baseRatio = 0.0f;
    float requestedRatio = 0.0f;
    if (!ReadConfiguredGearValue(target, ArrayKind::Ratio,
                                 kEighthGearRawIndex, &baseRatio) ||
        !ReadConfiguredGearValue(target, ArrayKind::Ratio, first,
                                 &requestedRatio) ||
        baseRatio <= 0.000001f || !std::isfinite(native)) {
        return 0.0f;
    }
    const float result = native * (requestedRatio / baseRatio);
    return std::isfinite(result) ? result : 0.0f;
}

void NFSMW_FASTCALL AutomaticShiftUpdateDetour(void* object, void*) {
    AutomaticControllerFn original = g_originalAutomaticControllerA;
    if (original == nullptr) return;

    const void* layout = nullptr;
    TargetObservation* target = FindTargetForBaseObject(object, &layout);
    std::int32_t current = 0;
    if (IsEighthGearEnabled(target) &&
        game_access::ReadField(object, 0x68u, &current) &&
        current > static_cast<std::int32_t>(kEighthGearRawIndex)) {
        // 6A1740 has two raw table reads whose offsets collide with live
        // object fields for gears 10..13. Leave the state untouched until a
        // sidecar-specific threshold is available; this is fail-closed and
        // also covers a stale/out-of-range high current value.
        return;
    }
    original(object);
}

void NFSMW_FASTCALL AutomaticShiftControllerDetour(void* object, void*) {
    AutomaticControllerFn original = g_originalAutomaticControllerB;
    if (original == nullptr) return;

    const void* layout = nullptr;
    TargetObservation* target = FindTargetForBaseObject(object, &layout);
    std::int32_t current = 0;
    if (IsEighthGearEnabled(target) &&
        game_access::ReadField(object, 0x64u, &current) &&
        current > static_cast<std::int32_t>(kEighthGearRawIndex)) {
        return;
    }
    original(object);
}

bool ApplySidecarShift(void* physical, TargetObservation* target,
                       std::int32_t requestedGear, std::int32_t before,
                       std::uint32_t* result) {
    if (result != nullptr) *result = 0;
    if (!IsSidecarEnabled(target) || physical == nullptr ||
        requestedGear < 0 ||
        // Raw index 9 is the verified in-layout eighth gear. Let the game
        // execute its native transition for it; the sidecar owns only 10..13.
        requestedGear <= static_cast<std::int32_t>(kEighthGearRawIndex) ||
        static_cast<std::uint32_t>(requestedGear) >= VirtualSlotCount(target) ||
        requestedGear == before) {
        return false;
    }

    float ratio = 0.0f;
    if (!ReadGearValue(target, ArrayKind::Ratio,
                       static_cast<std::uint32_t>(requestedGear), &ratio)) {
        return false;
    }
    float finalDrive = 0.0f;
    if (!ReadLayoutFloat(target, 0x7Cu, &finalDrive)) return false;

    float reverseScale = 1.0f;
    if (requestedGear < before &&
        !game_access::SafeRead(reinterpret_cast<const void*>(0x008910F0u),
                               &reverseScale, sizeof(reverseScale))) {
        return false;
    }
    if (!std::isfinite(ratio) || !std::isfinite(finalDrive) ||
        !std::isfinite(reverseScale)) {
        return false;
    }
    const float effective = finalDrive * ratio * reverseScale;
    if (!std::isfinite(effective)) return false;

    std::int32_t oldGear = 0;
    float oldEffective = 0.0f;
    std::int32_t oldState = 0;
    if (!game_access::ReadField(physical, kPhysicalCurrentGearOffset,
                                &oldGear) ||
        !game_access::ReadField(physical, 0x88u, &oldEffective) ||
        !game_access::ReadField(physical, 0x198u, &oldState)) {
        return false;
    }

    auto* bytes = static_cast<std::uint8_t*>(physical);
    if (!game_access::SafeWrite(bytes + 0x88u, &effective, sizeof(effective)) ||
        !game_access::SafeWrite(bytes + kPhysicalCurrentGearOffset,
                                &requestedGear, sizeof(requestedGear))) {
        game_access::SafeWrite(bytes + 0x88u, &oldEffective,
                               sizeof(oldEffective));
        game_access::SafeWrite(bytes + kPhysicalCurrentGearOffset, &oldGear,
                               sizeof(oldGear));
        return false;
    }
    if (oldState == 0) {
        const std::int32_t newState = 2;
        if (!game_access::SafeWrite(bytes + 0x198u, &newState,
                                    sizeof(newState))) {
            game_access::SafeWrite(bytes + 0x88u, &oldEffective,
                                   sizeof(oldEffective));
            game_access::SafeWrite(bytes + kPhysicalCurrentGearOffset,
                                   &oldGear, sizeof(oldGear));
            return false;
        }
    }
    if (result != nullptr) *result = 1;
    return true;
}

std::uint32_t NFSMW_FASTCALL ShiftGearDetour(void* physical, void*,
                                              std::int32_t requestedGear) {
    ShiftGearFn original = g_originalShiftGear;
    if (original == nullptr) return 0;

    const void* caller = _ReturnAddress();
    const void* layout = nullptr;
    TargetObservation* target = FindTargetForPhysical(physical, &layout);
    if (target == nullptr) return original(physical, requestedGear);

    std::int32_t before = 0;
    const bool beforeReadable = game_access::ReadField(
        physical, kPhysicalCurrentGearOffset, &before);
    game_access::PrivateHeader ratioHeader{};
    const bool headerReadable = game_access::SafeRead(
        target->ratioHeader, &ratioHeader, sizeof(ratioHeader));
    const bool virtualized = IsEighthGearEnabled(target) && headerReadable &&
                             IsNativeInlineFloatHeader(ratioHeader);

    std::uint32_t result = 0;
    bool sidecarShifted = false;
    // Once an object is virtualized, raw indices above the verified in-layout
    // eighth gear are protected even when the particular index is malformed
    // or the current-gear field cannot be read.  Falling back to the native
    // routine in either case would reintroduce its fixed-array access.
    const bool protectedHighRequest =
        IsEighthGearEnabled(target) && requestedGear >
                                          static_cast<std::int32_t>(
                                              kEighthGearRawIndex);
    if (protectedHighRequest) {
        // Raw 10..13 have no native storage.  Apply the complete guarded
        // sidecar transition directly and never expose the request to the
        // game's fixed-array ShiftGear implementation.
        if (beforeReadable &&
            IsSidecarGearIndex(target, static_cast<std::uint32_t>(
                                      requestedGear))) {
            sidecarShifted = ApplySidecarShift(physical, target, requestedGear,
                                                before, &result);
        }
        if (!sidecarShifted) result = 0;
    } else {
        // Raw 9 and all unconfigured objects retain the native behavior.
        result = original(physical, requestedGear);
    }

    std::int32_t after = 0;
    const bool afterReadable = game_access::ReadField(
        physical, kPhysicalCurrentGearOffset, &after);
    const bool newCaller = MarkCallerOnce(target->shiftCallers, caller);
    const bool newPhysical = MarkCallerOnce(target->shiftPhysicals, physical);
    const bool newEvent = MarkShiftOnce(target, requestedGear, result != 0);
    if ((!newCaller && !newPhysical && !newEvent) || !ReserveTraceLine()) {
        return result;
    }

    diagnostic_log::Write(
        "TRACE SHIFT target=%ld config='%s' physical=%p layout=%p caller=%p "
        "callerRva=0x%08X requested=%d currentBefore=%s%d "
        "currentAfter=%s%d nativeSlots=%s%u slots=%s%u result=%u",
        target->id, target->configName, physical, layout, caller,
        static_cast<unsigned>(CallerRva(caller)),
        static_cast<int>(requestedGear), beforeReadable ? "" : "unreadable/",
        static_cast<int>(before), afterReadable ? "" : "unreadable/",
        static_cast<int>(after), headerReadable ? "" : "unreadable/",
        headerReadable ? static_cast<unsigned>(ratioHeader.count) : 0u,
        headerReadable ? "" : "unreadable/",
        headerReadable
            ? static_cast<unsigned>(virtualized ? VirtualSlotCount(target)
                                                : ratioHeader.count)
            : 0u,
        static_cast<unsigned>(result));
    return result;
}

bool InstallDiagnosticHooks(char* reason, std::size_t reasonSize) {
    struct HookSpec {
        const char* name;
        std::uintptr_t target;
        void* detour;
        void** original;
        bool created;
    };

    HookSpec hooks[] = {
        {"TransmissionCtor", game_access::kTransmissionCtor,
         reinterpret_cast<void*>(&TransmissionCtorDetour),
         reinterpret_cast<void**>(&g_originalCtor), false},
        {"Private::Count", game_access::kPrivateCount,
         reinterpret_cast<void*>(&PrivateCountDetour),
         reinterpret_cast<void**>(&g_originalPrivateCount), false},
        {"Private::GetElement", game_access::kPrivateGetElement,
         reinterpret_cast<void*>(&PrivateGetElementDetour),
         reinterpret_cast<void**>(&g_originalPrivateGetElement), false},
        {"ShiftGear", game_access::kShiftGear,
         reinterpret_cast<void*>(&ShiftGearDetour),
         reinterpret_cast<void**>(&g_originalShiftGear), false},
        {"GearRatioGetter", game_access::kGearRatioGetter,
         reinterpret_cast<void*>(&GearRatioGetterDetour),
         reinterpret_cast<void**>(&g_originalGearRatioGetter), false},
        {"GearEfficiencyGetter", game_access::kGearEfficiencyGetter,
         reinterpret_cast<void*>(&GearEfficiencyGetterDetour),
         reinterpret_cast<void**>(&g_originalGearEfficiencyGetter), false},
        {"GearRatioPair", game_access::kGearRatioPointer,
         reinterpret_cast<void*>(&GearRatioPairDetour),
          reinterpret_cast<void**>(&g_originalGearRatioPair), false},
         {"AlternateGearPair", game_access::kGearRatioPair,
          reinterpret_cast<void*>(&AlternateGearPairDetour),
          reinterpret_cast<void**>(&g_originalAlternateGearPair), false},
         {"TransmissionShift", game_access::kTransmissionShift,
         reinterpret_cast<void*>(&TransmissionSetGearDetour),
           reinterpret_cast<void**>(&g_originalTransmissionSetGear), false},
          {"TransmissionTopGear", game_access::kTransmissionGetTopGear,
           reinterpret_cast<void*>(&TransmissionTopGearDetour),
           reinterpret_cast<void**>(&g_originalTransmissionTopGear), false},
          {"AlternateTransmissionShift",
           game_access::kAlternateTransmissionShift,
           reinterpret_cast<void*>(&AlternateTransmissionSetGearDetour),
           reinterpret_cast<void**>(&g_originalAlternateTransmissionSetGear),
           false},
          {"AlternateTransmissionTopGear",
           game_access::kAlternateTransmissionGetTopGear,
           reinterpret_cast<void*>(&AlternateTransmissionTopGearDetour),
           reinterpret_cast<void**>(&g_originalAlternateTransmissionTopGear),
           false},
          {"EffectiveGearRatio", game_access::kEffectiveGearRatio,
         reinterpret_cast<void*>(&EffectiveGearRatioDetour),
         reinterpret_cast<void**>(&g_originalEffectiveGearRatio), false},
        {"TransmissionUpdate", game_access::kTransmissionUpdate,
         reinterpret_cast<void*>(&TransmissionUpdateDetour),
         reinterpret_cast<void**>(&g_originalTransmissionUpdate), false},
        {"EffectiveRatioHelper", game_access::kEffectiveRatioHelper,
         reinterpret_cast<void*>(&EffectiveRatioHelperDetour),
         reinterpret_cast<void**>(&g_originalEffectiveRatioHelper), false},
         {"AdditionalShiftDecision", game_access::kAdditionalShiftDecision,
          reinterpret_cast<void*>(&AdditionalShiftDecisionDetour),
          reinterpret_cast<void**>(&g_originalAdditionalShiftDecision), false},
         {"AutomaticShiftDecision", game_access::kAutomaticShiftDecision,
          reinterpret_cast<void*>(&GearDecisionDetour),
          reinterpret_cast<void**>(&g_originalGearDecision), false},
         {"AutomaticShiftUpdate", game_access::kAutomaticShiftUpdate,
          reinterpret_cast<void*>(&AutomaticShiftUpdateDetour),
          reinterpret_cast<void**>(&g_originalAutomaticControllerA), false},
         {"AutomaticShiftController",
          game_access::kAutomaticShiftController,
          reinterpret_cast<void*>(&AutomaticShiftControllerDetour),
          reinterpret_cast<void**>(&g_originalAutomaticControllerB), false},
        {"DerivedShiftValue", game_access::kDerivedShiftValue,
         reinterpret_cast<void*>(&DerivedShiftValueDetour),
         reinterpret_cast<void**>(&g_originalDerivedShiftValue), false},
        {"DerivedEfficiencyValue", game_access::kDerivedEfficiencyValue,
         reinterpret_cast<void*>(&DerivedEfficiencyValueDetour),
         reinterpret_cast<void**>(&g_originalDerivedEfficiencyValue), false},
        {"LayoutRatioGetter", game_access::kLayoutRatioGetter,
         reinterpret_cast<void*>(&LayoutRatioGetterDetour),
         reinterpret_cast<void**>(&g_originalLayoutRatioGetter), false},
        {"LayoutEfficiencyGetter", game_access::kLayoutEfficiencyGetter,
         reinterpret_cast<void*>(&LayoutEfficiencyGetterDetour),
         reinterpret_cast<void**>(&g_originalLayoutEfficiencyGetter), false},
        {"LayoutEffectiveRatio", game_access::kLayoutEffectiveRatio,
         reinterpret_cast<void*>(&LayoutEffectiveRatioDetour),
         reinterpret_cast<void**>(&g_originalLayoutEffectiveRatio), false},
        {"TorqueEfficiencyRatio", game_access::kTorqueEfficiencyRatio,
         reinterpret_cast<void*>(&TorqueEfficiencyRatioDetour),
         reinterpret_cast<void**>(&g_originalTorqueEfficiencyRatio), false},
        {"ControllerRatioGetter", game_access::kControllerRatioGetter,
         reinterpret_cast<void*>(&ControllerRatioGetterDetour),
         reinterpret_cast<void**>(&g_originalControllerRatioGetter), false},
        {"ControllerEfficiencyGetter",
         game_access::kControllerEfficiencyGetter,
         reinterpret_cast<void*>(&ControllerEfficiencyGetterDetour),
         reinterpret_cast<void**>(&g_originalControllerEfficiencyGetter),
         false},
        {"ControllerEffectiveRatio", game_access::kControllerEffectiveRatio,
         reinterpret_cast<void*>(&ControllerEffectiveRatioDetour),
         reinterpret_cast<void**>(&g_originalControllerEffectiveRatio), false},
    };
    constexpr std::size_t hookCount = sizeof(hooks) / sizeof(hooks[0]);

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        _snprintf_s(reason, reasonSize, _TRUNCATE,
                    "MH_Initialize failed (%d)", static_cast<int>(init));
        return false;
    }
    const bool initializedHere = init == MH_OK;

    const auto rollback = [&]() {
        bool complete = true;
        for (std::size_t i = hookCount; i-- > 0;) {
            if (!hooks[i].created) continue;
            MH_DisableHook(reinterpret_cast<void*>(hooks[i].target));
            const MH_STATUS remove =
                MH_RemoveHook(reinterpret_cast<void*>(hooks[i].target));
            if (remove == MH_OK) {
                *hooks[i].original = nullptr;
            } else {
                complete = false;
            }
        }
        if (initializedHere && complete && MH_Uninitialize() != MH_OK) {
            complete = false;
        }
        return complete;
    };

    for (HookSpec& hook : hooks) {
        const MH_STATUS create = MH_CreateHook(
            reinterpret_cast<void*>(hook.target), hook.detour, hook.original);
        if (create != MH_OK) {
            const bool cleanup = rollback();
            _snprintf_s(reason, reasonSize, _TRUNCATE,
                        "MH_CreateHook %s failed (%d); rollback=%s", hook.name,
                        static_cast<int>(create), cleanup ? "complete" : "partial");
            return false;
        }
        hook.created = true;
    }

    for (HookSpec& hook : hooks) {
        const MH_STATUS queued =
            MH_QueueEnableHook(reinterpret_cast<void*>(hook.target));
        if (queued != MH_OK) {
            const bool cleanup = rollback();
            _snprintf_s(reason, reasonSize, _TRUNCATE,
                        "MH_QueueEnableHook %s failed (%d); rollback=%s",
                        hook.name, static_cast<int>(queued),
                        cleanup ? "complete" : "partial");
            return false;
        }
    }

    const MH_STATUS applied = MH_ApplyQueued();
    if (applied != MH_OK) {
        const bool cleanup = rollback();
        _snprintf_s(reason, reasonSize, _TRUNCATE,
                    "MH_ApplyQueued failed (%d); rollback=%s",
                    static_cast<int>(applied),
                    cleanup ? "complete" : "partial");
        return false;
    }
    return true;
}

void LogConfig(const gearconfig::Table& table, const char* path) {
    diagnostic_log::Write("config='%s' opened=%s accepted=%zu diagnostics=%zu",
                          path, table.FileOpened() ? "yes" : "no",
                          table.AcceptedCount(), table.Diagnostics().size());

    for (const gearconfig::Diagnostic& item : table.Diagnostics()) {
        diagnostic_log::Write(
            "config %s line=%zu: %s",
            item.severity == gearconfig::Diagnostic::Severity::Error
                ? "error"
                : "warning",
            item.line, item.message.c_str());
    }
    for (const gearconfig::Entry& entry : table.Entries()) {
        diagnostic_log::Write(
            "config entry line=%zu name='%s' forward=%u slots=%u "
            "ratios=%zu/%zu powerMultipliers=%zu/%zu format=%s",
            entry.line, entry.name.c_str(), entry.maxForwardGears,
            entry.totalRatioSlots, entry.ratioCount,
            entry.RequiredRatioCount(), entry.powerMultiplierCount,
            entry.RequiredRatioCount(),
            entry.HasExplicitPowerMultipliers() ? "paired" : "legacy");
    }
}

void LogCustomHudInstallResult(
    hud_gear_display::CustomHudInstallResult result,
    const char* reason,
    bool deferred) {
    const char* timing = deferred ? "deferred" : "startup";
    switch (result) {
        case hud_gear_display::CustomHudInstallResult::Installed:
            diagnostic_log::Write(
                "HUD CustomHud compatibility enabled (%s): %s", timing,
                reason);
            break;
        case hud_gear_display::CustomHudInstallResult::NotLoaded:
            diagnostic_log::Write(
                "HUD CustomHud compatibility waiting (%s): %s", timing,
                reason);
            break;
        case hud_gear_display::CustomHudInstallResult::UnsupportedBuild:
            diagnostic_log::Write(
                "HUD CustomHud compatibility skipped safely (%s): %s",
                timing, reason);
            break;
        case hud_gear_display::CustomHudInstallResult::Failed:
            diagnostic_log::Write(
                "HUD CustomHud compatibility failed safely (%s): %s",
                timing, reason);
            break;
    }
}

unsigned __stdcall CustomHudInstallRetryThread(void*) {
    constexpr unsigned kRetryCount = 100;
    constexpr DWORD kRetryDelayMs = 100;

    for (unsigned attempt = 1; attempt <= kRetryCount; ++attempt) {
        Sleep(kRetryDelayMs);

        char reason[256] = {};
        const hud_gear_display::CustomHudInstallResult result =
            hud_gear_display::InstallCustomHud(reason, sizeof(reason));
        if (result == hud_gear_display::CustomHudInstallResult::NotLoaded) {
            continue;
        }

        LogCustomHudInstallResult(result, reason, true);
        return 0;
    }

    diagnostic_log::Write(
        "HUD CustomHud compatibility inactive: CustomHud.asi was not "
        "loaded during the 10000 ms startup window");
    return 0;
}

bool StartCustomHudInstallRetry() {
    const uintptr_t thread = _beginthreadex(
        nullptr, 0, &CustomHudInstallRetryThread, nullptr, 0, nullptr);
    if (thread == 0) return false;
    CloseHandle(reinterpret_cast<HANDLE>(thread));
    return true;
}

}  // namespace

#ifndef NFSMW_MULTIGEAR_VERSION
#define NFSMW_MULTIGEAR_VERSION "dev"
#endif

NFSMW_PLUGIN_DECLARE("NFSMW MultiGear", NFSMW_MULTIGEAR_VERSION,
                     "Codex")

NFSMW_PLUGIN_MAIN() {
    char configPath[MAX_PATH] = {};
    char logPath[MAX_PATH] = {};
    if (!ModulePath(configPath, sizeof(configPath), "NFSMWMultiGear.cfg") ||
        !ModulePath(logPath, sizeof(logPath), "NFSMWMultiGear.log")) {
        OutputDebugStringA(
            "[NFSMWMultiGear] could not resolve plugin directory; disabled\n");
        return NFSMW_FAIL;
    }
    if (!diagnostic_log::Open(logPath)) {
        OutputDebugStringA(
            "[NFSMWMultiGear] could not create NFSMWMultiGear.log; disabled\n");
        return NFSMW_FAIL;
    }
    diagnostic_log::Write(
        "phase=4 schema=5 version=%s mode=sidecar-gears-8-to-12 "
        "probeSet=constructor-count-element-shift-transmission-consumers",
        NFSMW_MULTIGEAR_VERSION);

    char reason[512] = {};
    game_access::ExecutableFingerprint fingerprint{};
    if (!game_access::QueryExecutableFingerprint(
            &fingerprint, reason, sizeof(reason))) {
        diagnostic_log::Write("executable fingerprint incomplete: %s", reason);
        diagnostic_log::Write(
            "unsupported build; hook not installed: %s", reason);
        return NFSMW_FAIL;
    }
    diagnostic_log::Write(
            "executable fingerprint path='%s' fileSize=%llu md5=%s "
            "timeDateStamp=0x%08X imageSize=0x%08X entryRva=0x%08X "
            "machine=0x%04X "
            "preferredBase=0x%08X loadedBase=0x%08X",
        fingerprint.path,
        static_cast<unsigned long long>(fingerprint.fileSize),
        fingerprint.md5,
        static_cast<unsigned>(fingerprint.timeDateStamp),
        static_cast<unsigned>(fingerprint.imageSize),
        static_cast<unsigned>(fingerprint.entryPointRva),
        static_cast<unsigned>(fingerprint.machine),
        static_cast<unsigned>(fingerprint.preferredImageBase),
        static_cast<unsigned>(fingerprint.loadedBase));

    if (!startup_gate::ValidateExecutable(
            fingerprint.path, fingerprint.fileSize, fingerprint.md5, reason,
            sizeof(reason))) {
        diagnostic_log::Write(
            "startup gate rejected executable; no hooks installed: %s",
            reason);
        return NFSMW_FAIL;
    }
    diagnostic_log::Write(
        "startup gate executable verified: verificationCode=%u footerSize=%u "
        "sha256=%s",
        static_cast<unsigned>(startup_gate::kVerificationCode),
        static_cast<unsigned>(startup_gate::kFooterSize),
        startup_gate::kStampedExecutableSha256);

    game_access::BuildProfile buildProfile =
        game_access::BuildProfile::Unsupported;
    if (!game_access::VerifySupportedBuild(
            fingerprint, &buildProfile, reason, sizeof(reason))) {
        diagnostic_log::Write("unsupported build; hook not installed: %s",
                              reason);
        return NFSMW_FAIL;
    }
    diagnostic_log::Write(
        "supported build verified: profile='%s' size=%llu md5=%s "
        "constructor=0x%08X getDefinition=0x%08X count=0x%08X "
        "element=0x%08X shiftGear=0x%08X",
        game_access::BuildProfileName(buildProfile),
        static_cast<unsigned long long>(fingerprint.fileSize),
        fingerprint.md5,
        static_cast<unsigned>(game_access::kTransmissionCtor),
        static_cast<unsigned>(game_access::kClassGetDefinition),
        static_cast<unsigned>(game_access::kPrivateCount),
        static_cast<unsigned>(game_access::kPrivateGetElement),
        static_cast<unsigned>(game_access::kShiftGear));

    startup_gate::BindingResult binding{};
    if (!startup_gate::EnsureCurrentDeviceBinding(
            fingerprint.path, &binding, reason, sizeof(reason))) {
        diagnostic_log::Write(
            "startup gate rejected device binding; no hooks installed: %s",
            reason);
        return NFSMW_FAIL;
    }
    const char* bindingStatus = "verified";
    if (binding.status == startup_gate::BindingStatus::Created) {
        bindingStatus = "created";
    } else if (binding.status == startup_gate::BindingStatus::Migrated) {
        bindingStatus = "migrated";
    }
    diagnostic_log::Write(
        "device binding %s: path='%s' identitySources=0x%X",
        bindingStatus, binding.path, binding.sourceMask);

    static gearconfig::Table table;
    if (!table.Load(configPath)) {
        LogConfig(table, configPath);
        diagnostic_log::Write("disabled: configuration could not be read");
        return NFSMW_FAIL;
    }
    LogConfig(table, configPath);

    if (table.AcceptedCount() == 0) {
        diagnostic_log::Write(
            "idle: no valid configured transmissions; hook not installed");
        return NFSMW_OK;
    }

    g_config = &table;
    if (!InstallDiagnosticHooks(reason, sizeof(reason))) {
        g_config = nullptr;
        diagnostic_log::Write("hook installation failed; disabled: %s", reason);
        return NFSMW_FAIL;
    }

    char hudReason[256] = {};
    if (hud_gear_display::Install(hudReason, sizeof(hudReason))) {
        diagnostic_log::Write(
            "HUD vanilla gear display enabled: numeric gears 9-12");
    } else {
        diagnostic_log::Write(
            "HUD vanilla gear display skipped: %s", hudReason);
    }

    hudReason[0] = '\0';
    const hud_gear_display::CustomHudInstallResult customHud =
        hud_gear_display::InstallCustomHud(hudReason, sizeof(hudReason));
    LogCustomHudInstallResult(customHud, hudReason, false);
    if (customHud == hud_gear_display::CustomHudInstallResult::NotLoaded) {
        diagnostic_log::Write(
            "HUD CustomHud compatibility retry %s: interval=100 ms "
            "window=10000 ms",
            StartCustomHudInstallRetry() ? "started" : "could not start");
    }

    diagnostic_log::Write(
        "ready: NFSMW MultiGear active; traceLimit=%ld; "
        "exact direct configured targets only; manual gears 8-12 enabled, "
        "automatic gears 9-12 fail-closed",
        kMaxTraceLines);
    return NFSMW_OK;
}
