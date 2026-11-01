#pragma once
// Cooldown after a stop or a safety trip. The fan runs at the cooldown speed
// until ET falls below the end temperature; then the fan turns off and the
// roaster is idle (Manual, heater 0, fan 0). Over-temperature keeps it running
// (fan 100). Without an ET reading it does not end on its own. An operator
// heater, fan or mode command cancels it: Manual does exactly what is set.
#include <cmath>

class Cooldown {
public:
    void start() {
        active_ = true;
    }
    void cancel() {
        active_ = false;
    }
    bool active() const {
        return active_;
    }
    // True on the control step where the cooldown ends.
    bool update(float etC, float endC, bool overTemp) {
        if (!active_ || overTemp || !std::isfinite(etC) || !(etC < endC))
            return false;
        active_ = false;
        return true;
    }

private:
    bool active_ = false;
};
