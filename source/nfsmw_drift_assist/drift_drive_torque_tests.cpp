#include "asi_host/drift_drive_torque.hpp"

#include <cmath>
#include <limits>

int main() {
    using nfsmw_drift_asi::drift_drive_torque::Apply;
    if (Apply(300.0f, 2, true) != 900.0f ||
        Apply(300.0f, 9, true) != 900.0f ||
        Apply(300.0f, 13, true) != 900.0f ||
        Apply(300.0f, 14, true) != 300.0f ||
        Apply(300.0f, 1, true) != 300.0f ||
        Apply(300.0f, 0, true) != 300.0f ||
        Apply(300.0f, 2, false) != 300.0f ||
        Apply(-300.0f, 2, true) != -300.0f ||
        Apply(0.0f, 2, true) != 0.0f ||
        Apply(1000001.0f, 2, true) != 1000001.0f) {
        return 1;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    return std::isnan(Apply(nan, 2, true)) ? 0 : 1;
}
