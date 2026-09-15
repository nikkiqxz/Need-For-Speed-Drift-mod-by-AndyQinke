#pragma once

#include "Types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nfsmw_exhaust {

class IGameBridge {
public:
    virtual ~IGameBridge() = default;

    /* Called once per game frame on the game thread. */
    virtual std::size_t collectVehicles(VehicleSnapshot* output,
                                        std::size_t capacity) = 0;

    /* Return false when the engine rejected the effect request. */
    virtual bool spawnExhaustFlame(const FlameRequest& request) = 0;
    virtual void playBackfireAudio(const AudioRequest& request) = 0;

    /* Called only while the vehicle ID is known to be live. */
    virtual void setVanillaExhaustEnabled(VehicleId vehicleId,
                                          bool enabled) = 0;

    virtual void log(const char* message) = 0;
};

}  // namespace nfsmw_exhaust
