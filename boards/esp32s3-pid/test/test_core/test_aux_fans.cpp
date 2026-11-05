// Box temperature and the optional fans (core/aux_fans.h, max31855::boxTemperatureC).
#include <unity.h>

#include <cmath>

#include "core/aux_fans.h"
#include "core/max31855.h"
#include "core/max6675.h"

using namespace auxfans;

static void test_box_temperature_averages_valid_chips() {
    const ThermoReading a = max31855::decode((100u << 18) | (400u << 4)); // internal 25 C
    const ThermoReading b = max31855::decode((100u << 18) | (480u << 4)); // internal 30 C
    const ThermoReading absent = max31855::decode(0);
    TEST_ASSERT_EQUAL_FLOAT(27.5f, max31855::boxTemperatureC(a, b));
    TEST_ASSERT_EQUAL_FLOAT(25.0f, max31855::boxTemperatureC(a, absent)); // one chip only
    TEST_ASSERT_TRUE(std::isnan(max31855::boxTemperatureC(absent, absent)));
    const ThermoReading m = max6675::decode(0x0640); // no internal temperature
    TEST_ASSERT_TRUE(std::isnan(max31855::boxTemperatureC(m, m)));
    ThermoReading wild = a;
    wild.internalC = 200; // outside the chip's range
    TEST_ASSERT_EQUAL_FLOAT(30.0f, max31855::boxTemperatureC(wild, b));
}

static void test_box_temperature_falls_back_to_chip() {
    BoxTemp t = boxTemperature(31, 48);
    TEST_ASSERT_EQUAL_FLOAT(31, t.c);
    TEST_ASSERT_EQUAL(BoxSource::Sensor, t.source);
    t = boxTemperature(NAN, 48);
    TEST_ASSERT_EQUAL_FLOAT(48, t.c);
    TEST_ASSERT_EQUAL_STRING("chip", boxSourceName(t.source));
    t = boxTemperature(NAN, NAN);
    TEST_ASSERT_EQUAL(BoxSource::None, t.source);
}

static void test_thermo_fan_hysteresis_and_override() {
    ThermoFan f;
    TEST_ASSERT_FALSE(f.update(40, 45, Override::Auto));
    TEST_ASSERT_TRUE(f.update(45, 45, Override::Auto));
    TEST_ASSERT_TRUE(f.update(43, 45, Override::Auto)); // inside the band: stays on
    TEST_ASSERT_FALSE(f.update(42, 45, Override::Auto));
    TEST_ASSERT_FALSE(f.update(44, 45, Override::Auto)); // inside the band: stays off
    TEST_ASSERT_TRUE(f.update(NAN, 45, Override::Auto)); // no reading: run
    TEST_ASSERT_FALSE(f.update(90, 45, Override::Off));
    TEST_ASSERT_TRUE(f.update(20, 45, Override::On));
    Override o = Override::Auto;
    TEST_ASSERT_TRUE(parseOverride("off", o));
    TEST_ASSERT_EQUAL(Override::Off, o);
    TEST_ASSERT_FALSE(parseOverride("max", o));
    TEST_ASSERT_EQUAL_STRING("on", overrideName(Override::On));
}

static void test_scroll_fan_follows_cooldown() {
    ScrollFan s;
    TEST_ASSERT_FALSE(s.update(Mode::Manual, 0, false)); // off at boot
    TEST_ASSERT_TRUE(s.update(Mode::Manual, 0, true));   // stop / emergency / trip cooldown
    TEST_ASSERT_FALSE(s.update(Mode::Manual, 0, false)); // cooldown ended
}

static void test_scroll_fan_disabled_stays_off() {
    ScrollFan s;
    TEST_ASSERT_FALSE(s.update(Mode::Manual, 0, true, false)); // enabled off: no cooldown run
    TEST_ASSERT_FALSE(s.on());
    TEST_ASSERT_TRUE(s.update(Mode::Manual, 0, true, true)); // re-enabled during cooldown
    TEST_ASSERT_TRUE(s.on());
}

static void test_scroll_fan_manual_only_when_idle() {
    ScrollFan s;
    TEST_ASSERT_FALSE(s.setManual(true, Mode::Pid, 0, false));
    TEST_ASSERT_FALSE(s.setManual(true, Mode::Manual, 20, false));
    TEST_ASSERT_FALSE(s.setManual(true, Mode::Manual, 0, true));
    TEST_ASSERT_TRUE(s.setManual(true, Mode::Manual, 0, false));
    TEST_ASSERT_TRUE(s.update(Mode::Manual, 0, false));
    TEST_ASSERT_FALSE(s.update(Mode::Manual, 30, false)); // preheat: off
    TEST_ASSERT_FALSE(s.manual());
    s.setManual(true, Mode::Manual, 0, false);
    TEST_ASSERT_TRUE(s.update(Mode::Manual, 0, true));   // stop starts a cooldown
    TEST_ASSERT_FALSE(s.update(Mode::Manual, 0, false)); // and the manual request ended with it
    s.setManual(true, Mode::Manual, 0, false);
    TEST_ASSERT_TRUE(s.setManual(false, Mode::Manual, 0, false));
    TEST_ASSERT_FALSE(s.update(Mode::Manual, 0, false));
}

void runAuxFanTests() {
    RUN_TEST(test_box_temperature_averages_valid_chips);
    RUN_TEST(test_box_temperature_falls_back_to_chip);
    RUN_TEST(test_thermo_fan_hysteresis_and_override);
    RUN_TEST(test_scroll_fan_follows_cooldown);
    RUN_TEST(test_scroll_fan_disabled_stays_off);
    RUN_TEST(test_scroll_fan_manual_only_when_idle);
}
