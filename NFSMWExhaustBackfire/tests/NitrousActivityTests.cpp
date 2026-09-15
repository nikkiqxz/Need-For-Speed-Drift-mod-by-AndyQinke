#include "NitrousActivity.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

}  // namespace

int main() {
    using nfsmw_exhaust::native_adapter::isNitrousEmitterActive;
    using nfsmw_exhaust::native_adapter::kNitrousActivityGraceMs;

    expect(!isNitrousEmitterActive(0, 1000, 1000),
           "a vehicle without NOS emitters must remain inactive");
    expect(!isNitrousEmitterActive(2, 0, 1000),
           "installed NOS without a real emitter update must remain inactive");
    expect(isNitrousEmitterActive(2, 1000, 1000),
           "a real NOS emitter update must activate NOS");
    expect(isNitrousEmitterActive(2, 1000,
                                  1000 + kNitrousActivityGraceMs),
           "NOS must remain active through the low-frame-rate grace window");
    expect(!isNitrousEmitterActive(2, 1000,
                                   1001 + kNitrousActivityGraceMs),
           "NOS must become inactive after emitter updates stop");
    expect(!isNitrousEmitterActive(2, 1000, 999),
           "a non-monotonic timestamp must fail closed");

    std::puts("Nitrous activity tests passed");
    return 0;
}
