#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>

// Everything configurable is in config.h (force-included by platformio.ini).
#include "ConfigCheck.h"

#include <CroasterApp.h>
#include <CroasterDeviceIdentity.h>
#include <app/ControlPlugin.h>
#include <app/RoasterProbe.h>
#include <app/controller.h>
#include <app/unit_store.h>
#include <core/autotune_status.h>
#include <hw/heater_output.h>
#include <hw/thermocouples.h>

// ---------------------------------------------------------------------------
// Board wiring: build the parts selected in config.h and hand them to
// Croaster through its public extension points (constructor injection,
// CroasterDisplay, onCommand/onJsonCommand, onJsonData). Nothing in croaster/
// knows about heaters or fans.
// ---------------------------------------------------------------------------

#if CPID_FAN == CPID_FAN_AC_DIMMER
#    include <hw/fan_dimmer.h>
FanDimmer fan(CPID_PIN_ZERO_CROSS, CPID_PIN_FAN, CPID_MAINS_HZ);
#else
#    include <hw/fan_pwm.h>
FanPwm fan(CPID_PIN_FAN, CPID_FAN_PWM_HZ);
#endif

Thermocouples probes(CMON_THERMOCOUPLE == CMON_TC_MAX6675 ? TcChip::MAX6675 : TcChip::MAX31855, CMON_PIN_TC_SCK,
                     CMON_PIN_TC_MISO, CMON_PIN_TC_CS_ET, CMON_PIN_TC_CS_BT);
HeaterOutput heater(CPID_PIN_HEATER, CPID_HEATER_WINDOW_MS, CPID_HEATER_MAX_PERCENT, CPID_HEATER_ACTIVE_HIGH);

SafetyLimits limits() {
    SafetyLimits l;
    l.overTempC = CPID_OVER_TEMP_C;
    l.maxSetpointC = CPID_MAX_SETPOINT_C;
    l.minFanWithHeat = CPID_MIN_FAN_WITH_HEAT;
    l.requireMains = CPID_REQUIRE_MAINS;
    l.mainsGraceMs = CPID_MAINS_GRACE_MS;
    l.requireTunedGains = CPID_REQUIRE_AUTOTUNE;
    return l;
}
Controller controller(probes, heater, fan, limits());

// The control task owns the SPI bus; Croaster reads its filtered values.
RoasterProbe btProbe(controller, RoasterProbe::Channel::BT);
RoasterProbe etProbe(controller, RoasterProbe::Channel::ET);
CroasterCore core(CMON_DUMMY_DATA, &btProbe, &etProbe);

#if CMON_DISPLAY == CMON_DISPLAY_SSD1306
#    include <CroasterDisplaySSD1306.h>
CroasterDisplaySSD1306 display(core); // Croaster's shared screen (boards/common)
CroasterDisplay* const displayPtr = &display;
#else
CroasterDisplay* const displayPtr = nullptr;
#endif

CroasterApp app(core, displayPtr, CMON_PIN_LED, CMON_LED_ON_LEVEL);

static_assert(CPID_PID_INPUT_ET == static_cast<int>(PidInput::ET) &&
                  CPID_PID_INPUT_BT == static_cast<int>(PidInput::BT) &&
                  CPID_PID_INPUT_MAX == static_cast<int>(PidInput::MAX),
              "config.h: CPID_PID_INPUT_* values must match PidInput");
ControlSettings defaults() {
    ControlSettings d;
    d.gains = {CPID_PID_KP, CPID_PID_KI, CPID_PID_KD};
    d.pidInput = static_cast<PidInput>(CPID_PID_INPUT);
    d.cooldownFan = {CPID_COOLDOWN_FAN_STOP, CPID_COOLDOWN_FAN_TRIP};
    d.watchdog = {CPID_WATCHDOG_ENABLED != 0, CPID_WATCHDOG_TIMEOUT_S};
    d.fanAssist = CPID_FAN_ASSIST != 0;
    d.fanAssistMaxPct = CPID_FAN_ASSIST_MAX;
    return d;
}
ControlPlugin plugin(controller);
#include <app/AutotunePlugin.h>
AutotunePlugin autotuner(controller, plugin, CPID_HEATER_MAX_PERCENT); // autotune report, apply and discard (docs/autotune)
static_assert(CPID_AUTOTUNE_RULE_ZN_PI == static_cast<int>(autotune::Rule::ZieglerNicholsPi) &&
                  CPID_AUTOTUNE_RULE_TL_PI == static_cast<int>(autotune::Rule::TyreusLuybenPi) &&
                  CPID_AUTOTUNE_RULE_TL_PID == static_cast<int>(autotune::Rule::TyreusLuybenPid) &&
                  CPID_AUTOTUNE_RULE_SIMC_PI == static_cast<int>(autotune::Rule::SimcPi),
              "config.h: CPID_AUTOTUNE_RULE_* values must match autotune::Rule");
autotune::Config autotuneConfig() {
    autotune::Config c;
    const float pts[] = {CPID_AUTOTUNE_POINT1_C, CPID_AUTOTUNE_POINT2_C, CPID_AUTOTUNE_POINT3_C,
                         CPID_AUTOTUNE_POINT4_C};
    for (int i = 0; i < autotune::kMaxPoints; i++)
        c.pointsC[i] = pts[i];
    c.pointCount = autotune::kMaxPoints; // zeros are dropped by makePlan
    c.relayStep = CPID_AUTOTUNE_RELAY_STEP;
    c.hysteresisC = CPID_AUTOTUNE_HYSTERESIS_C;
    c.minCycles = CPID_AUTOTUNE_MIN_CYCLES;
    c.maxCycles = CPID_AUTOTUNE_MAX_CYCLES;
    c.maxCv = CPID_AUTOTUNE_MAX_CV_PCT / 100.0f;
    c.maxOvershootC = CPID_AUTOTUNE_OVERSHOOT_C;
    c.abortAboveC = CPID_OVER_TEMP_C;
    c.totalTimeoutS = CPID_AUTOTUNE_TIME_LIMIT_MIN * 60.0f;
    c.rule = static_cast<autotune::Rule>(CPID_AUTOTUNE_RULE);
    c.fanTest = CPID_AUTOTUNE_FAN_TEST != 0;
    const float levels[] = {CPID_AUTOTUNE_FAN_LEVEL1, CPID_AUTOTUNE_FAN_LEVEL2, CPID_AUTOTUNE_FAN_LEVEL3};
    for (int i = 0; i < 3; i++)
        c.fanLevels[i] = levels[i];
    c.fanLevelCount = 3; // zeros are dropped by makeFanLevels
    c.fanTestC = CPID_AUTOTUNE_FAN_TEST_C;
    c.fanShedMaxDropC = CPID_AUTOTUNE_FAN_SHED_DROP_C;
    c.fanTimeoutS = CPID_AUTOTUNE_FAN_TIME_LIMIT_MIN * 60.0f;
    return c;
}
Controller::ModelUse modelUse(bool fanAssist) {
    Controller::ModelUse u;
    u.fanFeedforward = CPID_FAN_FEEDFORWARD != 0;
    u.rampFeedforward = CPID_RAMP_FEEDFORWARD != 0;
    u.fanAssist = fanAssist;
    u.assist.maxExtraPct = CPID_FAN_ASSIST_MAX;
    return u;
}
#include <app/RecipePlugin.h>
RecipePlugin recipes(controller, plugin); // recipe steps run on the board (core/recipe.h)
#if CPID_ENABLE_LIBRARY
#    include <app/LibraryPlugin.h>
LibraryPlugin library(controller); // profile library on LittleFS (Controls > Device library)
#endif

#if CMON_ENABLE_WEBUI
#    include <WebUiServer.h>
WebUiServer::Options webUiOptions() {
    WebUiServer::Options o;
    o.port = CMON_WEBUI_PORT;
    o.controls = true;
    o.board = CMON_BOARD_NAME;
    o.coreId = CPID_CONTROL_CORE == 0 ? 1 : 0; // keep HTTP work off the control task's core
    return o;
}
WebUiServer webUi(webUiOptions()); // starts once WiFi is up (see WebUiServer::loop)
#endif

// Header line for the SSD1306, at most 20 characters: "H50 Fan60 PID" or "TRIP overtemp Fan100".
// "Fan" is spelled out so it can't be read as a Fahrenheit temperature. For
// 10 s after the autotune status changes (including a refused start), the
// header shows it instead ("AT results ready"); a trip still takes precedence.
constexpr uint32_t kAutotuneNoticeMs = 10000;
String statusLine(const Snapshot& s) {
    static uint32_t seenSeq = 0, noticeSinceMs = 0;
    if (s.autotuneStatusSeq != seenSeq) {
        seenSeq = s.autotuneStatusSeq;
        noticeSinceMs = millis();
    }
    // While autotune runs the header shows its step instead ("AT2/3 cyc3/6 350F").
    const bool notice =
        (seenSeq != 0 && millis() - noticeSinceMs < kAutotuneNoticeMs) || s.mode == Mode::Autotune;
    char line[24];
    if (s.trip != SafetyTrip::None) {
        snprintf(line, sizeof line, "TRIP %s Fan%.0f", tripName(s.trip), s.fanPct);
    } else if (notice) {
        const String unit = core.temperatureUnit();
        autotunetext::brief(s, unit.length() ? unit[0] : 'C', line, sizeof line);
    } else {
        snprintf(line, sizeof line, "H%.0f Fan%.0f %s", s.heaterPct, s.fanPct, modeName(s.mode));
    }
    return String(line);
}

// Loop task stack: 16 KB instead of Arduino's 8 KB. Croaster handles WebSocket
// commands inside the WebSockets library's nested read callbacks (about 4 levels
// for a message over 125 bytes), so a command handler starts deep in the stack.
// getControl reports the smallest free amount seen (loopStackFree).
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
    // Serial is the native USB port (HWCDC). Once a host has opened it, each log line
    // waits up to 20 x the TX timeout (100 ms by default, so about 2 s) when the host
    // stops reading, which stalls loop() and with it the WebSocket server. Croaster logs
    // every WebSocket command, so a PC holding the port open could freeze the board for
    // seconds at a time. With 0 the line is dropped instead.
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    Serial.setTxTimeoutMs(0);
#endif
    CroasterDeviceIdentity::setBoardName(CMON_BOARD_NAME);
    // Saved unit, else CMON_TEMP_UNIT. Croaster converts BT/ET/RoR; the plugin converts SV and setpoints.
    UnitStore::attach(app, CMON_TEMP_UNIT);
    Wire.setPins(CMON_PIN_I2C_SDA, CMON_PIN_I2C_SCL); // later Wire.begin() calls use these

    // Outputs safe before anything else: heater off, fan off.
    heater.begin();
    fan.begin();
    probes.begin();
    const ControlSettings settings = ConfigStore::load(defaults()); // NVS is ready from here on
    controller.configureAutotune(autotuneConfig());
    controller.configureModelUse(modelUse(settings.fanAssist));
    {
        static roastermodel::RoasterModel saved; // about 300 B: off the setup stack
        if (ConfigStore::loadModel(saved))
            controller.setRoasterModel(saved);
    }
    controller.begin(settings.gains, settings.gainsTuned, settings.pidInput, settings.cooldownFan.trip,
                     CPID_CONTROL_CORE, configMAX_PRIORITIES - 3);
    plugin.attach(app, settings);
    autotuner.attach(app);
    recipes.attach(app);
#if CPID_ENABLE_LIBRARY
    library.begin(); // mounts LittleFS; the outputs are already safe
    library.attach(app);
    library.setRecipes(&recipes);
#endif
#if CMON_DISPLAY == CMON_DISPLAY_SSD1306
    display.setHeaderText([]() {
        return statusLine(controller.snapshot());
    }); // heater/fan/mode or trip
#endif

    app.begin(); // WiFi (WiFiManager portal on first boot), WebSocket :81, BLE, display

#if !CMON_WIFI_SLEEP
    // After app.begin(): WiFi and BLE are up. Arduino keeps the setting for later WiFi restarts.
    Serial.println(WiFi.setSleep(false) ? "# WiFi sleep: off" : "# WiFi sleep: could not turn off");
#endif
    Serial.printf("# Loop stack: %u B, %u B free\n", (unsigned)getArduinoLoopTaskStackSize(),
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    plugin.begin();
}

void loop() {
    app.loop();
    plugin.loop();
    autotuner.loop();
    recipes.loop();
#if CMON_ENABLE_WEBUI
    webUi.loop();
#endif
}
