#pragma once

#include <stdint.h>
#include <stddef.h>

#define NFSW_EXHAUST_API_VERSION 6u

#define NFSW_EXHAUST_SIDE_LEFT 0u
#define NFSW_EXHAUST_SIDE_RIGHT 1u

#define NFSW_EXHAUST_FLAME_STANDALONE 0u
#define NFSW_EXHAUST_FLAME_SIMULTANEOUS 1u
#define NFSW_EXHAUST_FLAME_SEQUENTIAL 2u

#if defined(_WIN32)
#define NFSW_EXHAUST_EXPORT __declspec(dllexport)
#define NFSW_EXHAUST_CALL __cdecl
#else
#define NFSW_EXHAUST_EXPORT
#define NFSW_EXHAUST_CALL
#endif

/*
 * C ABI used by a game-specific adapter. Keeping this boundary POD-only lets
 * the adapter be built with the user's ASI SDK without sharing STL objects or
 * CRT ownership across DLLs.
 */
#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

typedef struct NfswExhaustMarkerC {
    int present;
    float x;
    float y;
    float z;
    /* World-space unit quaternion in x, y, z, w order. All zero is treated
     * as the identity quaternion for safely zero-initialized C structs. */
    float qx;
    float qy;
    float qz;
    float qw;
} NfswExhaustMarkerC;

typedef struct NfswExhaustVehicleSnapshotC {
    uint32_t id;
    int valid;
    /* Absolute RPM values (for example 9000.0), never kRPM (9.0). */
    float rpm;
    float maxRpm;
    float redlineRpm;
    int atRedline;
    int atMaxRpm;
    int shiftInProgress;
    int shiftEvent;
    int gearChanged;
    int shiftDirection;
    int32_t gear;
    int driverControlsValid;
    float gasInput;
    float brakeInput;
    float handBrakeInput;
    int nitrousActive;
    NfswExhaustMarkerC leftExhaust;
    NfswExhaustMarkerC rightExhaust;
} NfswExhaustVehicleSnapshotC;

typedef struct NfswExhaustFlameRequestC {
    uint32_t vehicleId;
    uint64_t scheduledAtMs;
    uint64_t emittedAtMs;
    uint32_t effectVariant;
    uint32_t sequenceId;
    /* NFSW_EXHAUST_SIDE_LEFT or NFSW_EXHAUST_SIDE_RIGHT. */
    uint8_t side;
    /* NFSW_EXHAUST_FLAME_*; paired sides share sequenceId. */
    uint8_t pattern;
    uint8_t reserved[2];
    const char* effectId;
    NfswExhaustMarkerC marker;
} NfswExhaustFlameRequestC;

typedef struct NfswExhaustAudioRequestC {
    uint32_t vehicleId;
    uint64_t scheduledAtMs;
    uint64_t emittedAtMs;
    NfswExhaustMarkerC marker;
    /* Zero-based flat slot: 0..11, selected uniformly in one draw. */
    uint8_t clipIndex;
    uint8_t reserved0;
    /* NFSW_EXHAUST_SIDE_LEFT or NFSW_EXHAUST_SIDE_RIGHT. */
    uint8_t side;
    uint8_t reserved[5];
    const char* assetId;
} NfswExhaustAudioRequestC;

typedef uint32_t(NFSW_EXHAUST_CALL* NfswExhaustGetVehicleCountFn)(void* user);
typedef int(NFSW_EXHAUST_CALL* NfswExhaustReadVehicleFn)(
    void* user, uint32_t index, NfswExhaustVehicleSnapshotC* output);
typedef int(NFSW_EXHAUST_CALL* NfswExhaustSpawnFlameFn)(
    void* user, const NfswExhaustFlameRequestC* request);
typedef void(NFSW_EXHAUST_CALL* NfswExhaustPlayAudioFn)(
    void* user, const NfswExhaustAudioRequestC* request);
typedef void(NFSW_EXHAUST_CALL* NfswExhaustSuppressVanillaFn)(
    void* user, uint32_t vehicleId, int enabled);
typedef void(NFSW_EXHAUST_CALL* NfswExhaustLogFn)(
    void* user, const char* message);

typedef struct NfswExhaustCallbacks {
    uint32_t apiVersion;
    uint32_t structSize;
    void* user;
    NfswExhaustGetVehicleCountFn getVehicleCount;
    NfswExhaustReadVehicleFn readVehicle;
    NfswExhaustSpawnFlameFn spawnFlame;
    NfswExhaustPlayAudioFn playAudio;
    NfswExhaustSuppressVanillaFn setVanillaExhaustEnabled;
    NfswExhaustLogFn log;
} NfswExhaustCallbacks;

#pragma pack(pop)

/* Every exported function must be called from one game thread. Adapter
 * callbacks are synchronous and must not call back into this API. */

NFSW_EXHAUST_EXPORT int NFSW_EXHAUST_CALL
NFSW_Exhaust_RegisterCallbacks(const NfswExhaustCallbacks* callbacks);

NFSW_EXHAUST_EXPORT int NFSW_EXHAUST_CALL
NFSW_Exhaust_LoadConfig(const char* path);

NFSW_EXHAUST_EXPORT int NFSW_EXHAUST_CALL
NFSW_Exhaust_LoadAudioManifest(const char* path);

NFSW_EXHAUST_EXPORT void NFSW_EXHAUST_CALL
NFSW_Exhaust_OnFrame(uint64_t nowMs);

NFSW_EXHAUST_EXPORT void NFSW_EXHAUST_CALL NFSW_Exhaust_Reset();

/* Restore currently tracked vehicles and release the callback table. Call
 * before destroying the adapter or the live vehicle IDs it owns. */
NFSW_EXHAUST_EXPORT void NFSW_EXHAUST_CALL NFSW_Exhaust_Shutdown();

NFSW_EXHAUST_EXPORT const char* NFSW_EXHAUST_CALL NFSW_Exhaust_Version();

#ifdef __cplusplus
}  // extern "C"

static_assert(sizeof(NfswExhaustMarkerC) == 32,
              "unexpected exhaust marker ABI layout");
static_assert(offsetof(NfswExhaustMarkerC, qw) == 28,
              "unexpected marker quaternion ABI layout");
static_assert(sizeof(NfswExhaustVehicleSnapshotC) == 132,
              "unexpected vehicle snapshot ABI layout");
static_assert(sizeof(NfswExhaustFlameRequestC) == 72,
              "unexpected flame request ABI layout");
static_assert(sizeof(NfswExhaustAudioRequestC) == 72,
              "unexpected audio request ABI layout");
static_assert(offsetof(NfswExhaustFlameRequestC, scheduledAtMs) == 8,
              "unexpected flame request ABI layout");
static_assert(offsetof(NfswExhaustFlameRequestC, sequenceId) == 28,
              "unexpected flame sequence ABI layout");
static_assert(offsetof(NfswExhaustFlameRequestC, side) == 32,
              "unexpected flame side ABI layout");
static_assert(offsetof(NfswExhaustFlameRequestC, pattern) == 33,
              "unexpected flame pattern ABI layout");
static_assert(offsetof(NfswExhaustFlameRequestC, effectId) == 36,
              "unexpected flame asset ABI layout");
static_assert(offsetof(NfswExhaustAudioRequestC, scheduledAtMs) == 8,
              "unexpected audio request ABI layout");
static_assert(offsetof(NfswExhaustAudioRequestC, clipIndex) == 56,
              "unexpected audio clip ABI layout");
static_assert(offsetof(NfswExhaustAudioRequestC, side) == 58,
              "unexpected audio side ABI layout");
static_assert(offsetof(NfswExhaustAudioRequestC, assetId) == 64,
              "unexpected audio asset ABI layout");
static_assert(offsetof(NfswExhaustCallbacks, user) == 8,
              "unexpected callback table ABI layout");
#endif
