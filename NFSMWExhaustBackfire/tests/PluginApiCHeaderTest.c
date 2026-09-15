#include "nfsmw_exhaust/PluginApi.hpp"

static uint32_t NFSW_EXHAUST_CALL vehicle_count(void* user) {
    (void)user;
    return 0;
}

int main(void) {
    NfswExhaustCallbacks callbacks = {0};
    NfswExhaustMarkerC marker = {0};
    NfswExhaustFlameRequestC flame = {0};
    callbacks.getVehicleCount = vehicle_count;
    marker.present = 1;
    marker.qw = 1.0f;
    flame.side = NFSW_EXHAUST_SIDE_RIGHT;
    flame.pattern = NFSW_EXHAUST_FLAME_SEQUENTIAL;
    return callbacks.getVehicleCount(0) == 0 && marker.present == 1 &&
                   marker.qw == 1.0f &&
                   flame.side == NFSW_EXHAUST_SIDE_RIGHT &&
                   flame.pattern == NFSW_EXHAUST_FLAME_SEQUENTIAL
               ? 0
               : 1;
}
