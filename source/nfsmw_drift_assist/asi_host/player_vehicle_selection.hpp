#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nfsmw_drift_asi::player_vehicle_selection {

struct CandidateObservation {
    std::uintptr_t pvehicle = 0;
    std::uintptr_t memberPlayer = 0;
    std::uintptr_t reportedPlayer = 0;
    bool baseStateValid = false;
    bool isPlayer = false;
    bool isOwnedByPlayer = false;
    std::uintptr_t simable = 0;
    std::uintptr_t reportedPlayerSimable = 0;
};

inline constexpr std::uint32_t kRecoveryProbationSamples = 3;
inline constexpr std::size_t kMaximumObservedCandidates = 64;

enum class InstanceSlotState : std::uint8_t {
    Empty,
    Disabled,
    Enabled,
    Invalid,
};

constexpr InstanceSlotState ClassifyInstanceSlot(
    std::uintptr_t pvehicle, std::uint8_t enabledByte) noexcept {
    if (enabledByte > 1 || (pvehicle == 0 && enabledByte != 0)) {
        return InstanceSlotState::Invalid;
    }
    if (pvehicle == 0) {
        return InstanceSlotState::Empty;
    }
    return enabledByte == 0 ? InstanceSlotState::Disabled
                            : InstanceSlotState::Enabled;
}

constexpr bool ReverseOwnershipProvesRetired(
    std::uintptr_t candidateSimable,
    std::uintptr_t playerCurrentSimable) noexcept {
    return candidateSimable != 0 && playerCurrentSimable != 0 &&
           candidateSimable != playerCurrentSimable;
}

constexpr bool HasCompletedRecoveryProbation(
    std::uint32_t stableSamples) noexcept {
    return stableSamples >= kRecoveryProbationSamples;
}

struct Selection {
    std::uintptr_t pvehicle = 0;
    std::uintptr_t player = 0;
    std::uintptr_t simable = 0;
    std::uintptr_t alternatePvehicle = 0;
    std::uintptr_t alternatePlayer = 0;
    std::uintptr_t alternateSimable = 0;
    std::uint32_t nonNullPlayers = 0;
    std::uint32_t qualifiedPlayers = 0;
    std::uint32_t duplicateQualifiedPlayers = 0;

    // Slot enablement remains caller-owned. Full runtime-state validation can
    // follow once this pass has found one distinct ownership identity.
    void Observe(const CandidateObservation& candidate) {
        if (candidate.memberPlayer == 0) {
            return;
        }
        ++nonNullPlayers;
        if (!candidate.baseStateValid || !candidate.isPlayer ||
            !candidate.isOwnedByPlayer || candidate.reportedPlayer == 0 ||
            candidate.reportedPlayer != candidate.memberPlayer ||
            candidate.simable == 0 ||
            candidate.reportedPlayerSimable != candidate.simable) {
            return;
        }

        const QualifiedIdentity identity{
            candidate.pvehicle, candidate.reportedPlayer,
            candidate.simable};
        for (std::size_t index = 0; index < storedIdentityCount_; ++index) {
            if (SameIdentity(qualifiedIdentities_[index], identity)) {
                ++duplicateQualifiedPlayers;
                return;
            }
        }

        if (storedIdentityCount_ < qualifiedIdentities_.size()) {
            qualifiedIdentities_[storedIdentityCount_++] = identity;
        }
        ++qualifiedPlayers;
        if (qualifiedPlayers == 1) {
            pvehicle = candidate.pvehicle;
            player = candidate.reportedPlayer;
            simable = candidate.simable;
        } else if (qualifiedPlayers == 2) {
            alternatePvehicle = candidate.pvehicle;
            alternatePlayer = candidate.reportedPlayer;
            alternateSimable = candidate.simable;
        }
    }

    bool HasUniquePlayer() const {
        return qualifiedPlayers == 1 && pvehicle != 0 && player != 0;
    }

private:
    struct QualifiedIdentity {
        std::uintptr_t pvehicle = 0;
        std::uintptr_t player = 0;
        std::uintptr_t simable = 0;
    };

    static constexpr bool SameIdentity(
        const QualifiedIdentity& left,
        const QualifiedIdentity& right) noexcept {
        return left.pvehicle == right.pvehicle &&
               left.player == right.player &&
               left.simable == right.simable;
    }

    std::array<QualifiedIdentity, kMaximumObservedCandidates>
        qualifiedIdentities_{};
    std::size_t storedIdentityCount_ = 0;
};

}  // namespace nfsmw_drift_asi::player_vehicle_selection
