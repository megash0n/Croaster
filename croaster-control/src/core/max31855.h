#pragma once
// Pure decoder for one 32-bit MAX31855 frame (datasheet table 3).
// Replaces Adafruit_MAX31855 + Adafruit_BusIO: the transport is 4 SPI bytes,
// all the logic is here and is host-testable.
#include <cmath>
#include <initializer_list>
#include <cstdint>

struct ThermoReading {
    float thermocoupleC; // NaN when !ok
    float internalC;     // cold-junction (board) temperature
    uint8_t fault;       // bit0 open, bit1 short to GND, bit2 short to VCC, bit7 no device
    bool ok;
};

namespace max31855 {
constexpr uint8_t kFaultOpen = 0x01;
constexpr uint8_t kFaultShortGnd = 0x02;
constexpr uint8_t kFaultShortVcc = 0x04;
constexpr uint8_t kFaultNoDevice = 0x80;

inline ThermoReading decode(uint32_t raw) {
    ThermoReading r{};
    // D31..D18: 14-bit signed thermocouple temp, 0.25 C/LSB
    int32_t tc = static_cast<int32_t>((raw >> 18) & 0x3FFF);
    if (tc & 0x2000)
        tc -= 0x4000;
    // D15..D4: 12-bit signed internal temp, 0.0625 C/LSB
    int32_t in = static_cast<int32_t>((raw >> 4) & 0x0FFF);
    if (in & 0x0800)
        in -= 0x1000;
    r.internalC = in * 0.0625f;
    r.fault = static_cast<uint8_t>(raw & 0x07);
    const bool faultFlag = (raw & 0x00010000u) != 0; // D16
    if (raw == 0 || raw == 0xFFFFFFFFu)
        r.fault |= kFaultNoDevice; // MISO stuck low/high
    r.ok = !faultFlag && r.fault == 0;
    r.thermocoupleC = r.ok ? tc * 0.25f : NAN;
    return r;
}

// Board (cold-junction) temperature for "Amb": the preferred chip's, or the
// other chip's when the preferred one doesn't answer. NaN when neither has one
// (no chip fitted, or MAX6675, which has no cold-junction output).
inline float boardTemperatureC(const ThermoReading& preferred, const ThermoReading& other) {
    if (!(preferred.fault & kFaultNoDevice) && std::isfinite(preferred.internalC))
        return preferred.internalC;
    if (!(other.fault & kFaultNoDevice) && std::isfinite(other.internalC))
        return other.internalC;
    return NAN;
}
// Box (enclosure) temperature: the average cold-junction temperature of the
// chips that report one. Skips a missing chip, a MAX6675 (NaN) and a value
// outside the MAX31855's -40..125 C range. NaN when none is usable.
inline float boxTemperatureC(const ThermoReading& a, const ThermoReading& b) {
    float sum = 0;
    int n = 0;
    for (const ThermoReading* r : {&a, &b}) {
        if (!(r->fault & kFaultNoDevice) && std::isfinite(r->internalC) && r->internalC >= -40 &&
            r->internalC <= 125) {
            sum += r->internalC;
            n++;
        }
    }
    return n ? sum / n : NAN;
}
} // namespace max31855
