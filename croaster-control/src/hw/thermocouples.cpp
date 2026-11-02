#include "thermocouples.h"

#include <Arduino.h>
#include <SPI.h>

#include "core/max6675.h"

namespace {
SPIClass tcSpi(FSPI);
const SPISettings kSettings(1000000, MSBFIRST, SPI_MODE0); // both chips >= 4 MHz; 1 MHz for long leads
} // namespace

void Thermocouples::begin() {
    pinMode(csEt_, OUTPUT);
    pinMode(csBt_, OUTPUT);
    digitalWrite(csEt_, HIGH);
    digitalWrite(csBt_, HIGH);
    tcSpi.begin(sck_, miso_, -1, -1);
}

ThermoReading Thermocouples::readChip(int cs) {
    const int bytes = chip_ == TcChip::MAX31855 ? 4 : 2;
    tcSpi.beginTransaction(kSettings);
    digitalWrite(cs, LOW);
    delayMicroseconds(1);
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++)
        v = (v << 8) | tcSpi.transfer(0x00);
    digitalWrite(cs, HIGH);
    tcSpi.endTransaction();
    return chip_ == TcChip::MAX31855 ? max31855::decode(v) : max6675::decode(static_cast<uint16_t>(v));
}

ThermoSample Thermocouples::read() {
    const ThermoReading e = readChip(csEt_);
    const ThermoReading b = readChip(csBt_);
    ThermoSample s{};
    s.etRaw = e.thermocoupleC;
    s.btRaw = b.thermocoupleC;
    s.etFault = e.fault;
    s.btFault = b.fault;
    s.ambient = max31855::boardTemperatureC(b, e);
    s.box = max31855::boxTemperatureC(e, b);
    s.et = et_.push(e);
    s.bt = bt_.push(b);
    return s;
}
