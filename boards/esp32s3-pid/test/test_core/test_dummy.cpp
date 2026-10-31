// Croaster dummy readings in the display unit (T61). Called from test_main.cpp
// (runDummyTests). CroasterCore needs Arduino, so its source is checked as text
// and the conversion it uses is evaluated here.
#include <unity.h>

#include <fstream>
#include <sstream>
#include <string>

#include "core/control_types.h"

namespace {

std::string readSource(const char* path) {
    // __FILE__ is absolute or relative to boards/esp32s3-pid, where `pio test` runs.
    std::string here = __FILE__;
    size_t cut = here.rfind("boards/esp32s3-pid/test/");
    std::string root = cut == std::string::npos ? "../../" : here.substr(0, cut);
    std::ifstream f(root + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string readCoreSource() {
    return readSource("croaster/src/CroasterCore.cpp");
}

std::string between(const std::string& s, const char* from, const char* to) {
    size_t a = s.find(from);
    if (a == std::string::npos)
        return "";
    size_t b = s.find(to, a);
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// CroasterCore::convertTemperature(), as pinned below.
double convert(double c, char unit) {
    if (unit == 'F')
        return (c * 9.0 / 5.0) + 32.0;
    if (unit == 'K')
        return c + 273.15;
    return c;
}

} // namespace

void test_dummy_readings_use_display_unit() {
    const std::string src = readCoreSource();
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "CroasterCore.cpp not found");
    const std::string dummy = between(src, "if (useDummyData) {", "return;");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, dummy.find("tempBt = convertTemperature(random(30, 40)) + correctionBt;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, dummy.find("tempEt = convertTemperature(random(30, 40)) + correctionEt;"));
    const std::string conv = between(src, "double CroasterCore::convertTemperature(double tempCelsius)", "\n}");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, conv.find("return (tempCelsius * 9.0 / 5.0) + 32.0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, conv.find("return tempCelsius + 273.15;"));
}

// random(30, 40) is 30..39 C. With no correction the readings land in about
// 86-104 F, 30-40 C and 303-313 K, and the recipe runner (RecipePlugin::inputs,
// toCelsius in the display unit) reads them back as 30..39 C.
void test_dummy_reading_ranges() {
    for (int c = 30; c < 40; c++) {
        const double f = convert(c, 'F'), k = convert(c, 'K'), cc = convert(c, 'C');
        TEST_ASSERT_TRUE(f >= 86.0 && f <= 104.0);
        TEST_ASSERT_TRUE(cc >= 30.0 && cc <= 40.0);
        TEST_ASSERT_TRUE(k >= 303.0 && k <= 313.0);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, (float)c, toCelsius((float)f, 'F'));
        TEST_ASSERT_FLOAT_WITHIN(0.01f, (float)c, toCelsius((float)k, 'K'));
        TEST_ASSERT_FLOAT_WITHIN(0.01f, (float)c, toCelsius((float)cc, 'C'));
    }
}

// The page's Fake readings button follows data "dummy" (T65): Croaster's getter
// returns useDummyData unchanged and ControlPlugin writes it into every payload.
void test_dummy_state_in_data() {
    const std::string header = readSource("croaster/src/CroasterCore.h");
    const std::string getter = between(header, "bool dummyDataEnabled() const {", "}");
    TEST_ASSERT_FALSE(getter.empty());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, getter.find("return useDummyData;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("void toggleDummyData();"));
    const std::string plugin = readSource("croaster-control/src/app/ControlPlugin.cpp");
    const std::string hook = between(plugin, "app.core().onJsonData(", "});");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, hook.find("data[\"dummy\"] = app_->core().dummyDataEnabled();"));
}

void runDummyTests() {
    RUN_TEST(test_dummy_readings_use_display_unit);
    RUN_TEST(test_dummy_reading_ranges);
    RUN_TEST(test_dummy_state_in_data);
}
