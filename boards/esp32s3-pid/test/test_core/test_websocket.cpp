// Croaster's WebSocket client count and the broadcast the ICRM app reads (T60).
// Called from test_main.cpp (runWebSocketTests).
#include <unity.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <ArduinoJson.h>

#include "core/control_protocol.h"

namespace {

// Client slots as links2004/WebSockets 2.7.3 handles them: a TCP connection takes a
// slot in the header state; CONNECTED is reported only after the upgrade, but
// clientDisconnect() reports DISCONNECTED for every slot it frees, upgraded or not.
// The status is updated before the event is raised.
struct FakeServer {
    enum Status { Free, Header, Connected };
    static constexpr int kSlots = 5;
    Status slot[kSlots] = {Free, Free, Free, Free, Free};
    int counted = 0;   // CroasterWebSocketManager::clientConnected
    bool upstreamRule; // true: ++/-- as upstream had it; false: recount (T60)

    explicit FakeServer(bool upstream)
        : upstreamRule(upstream) {}

    int connectedClients() const {
        int n = 0;
        for (Status s : slot)
            n += s == Connected;
        return n;
    }
    int open() {
        for (int i = 0; i < kSlots; i++)
            if (slot[i] == Free) {
                slot[i] = Header;
                return i;
            }
        return -1;
    }
    void handshake(int i) {
        slot[i] = Connected;
        counted = upstreamRule ? counted + 1 : connectedClients();
    }
    void close(int i) {
        slot[i] = Free;
        counted = upstreamRule ? counted - 1 : connectedClients();
    }
    bool broadcasts() const { // CroasterWebSocketManager::isClientConnected()
        return counted > 0;
    }
};

// 14 connections closed before the upgrade (a page giving up on a slow handshake,
// a proxy health check, a port probe), then the app connects.
void unfinishedThenApp(FakeServer& s) {
    for (int i = 0; i < 14; i++)
        s.close(s.open());
    s.handshake(s.open());
}

std::string readRepoFile(const char* relative) {
    // __FILE__ is absolute or relative to boards/esp32s3-pid, where `pio test` runs.
    std::string here = __FILE__;
    size_t cut = here.rfind("boards/esp32s3-pid/test/");
    std::string root = cut == std::string::npos ? "../../" : here.substr(0, cut);
    std::ifstream f(root + relative);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string between(const std::string& s, const char* from, const char* to) {
    size_t a = s.find(from);
    if (a == std::string::npos)
        return "";
    size_t b = s.find(to, a);
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

} // namespace

void test_ws_upstream_count_goes_negative() {
    FakeServer s(true);
    unfinishedThenApp(s);
    TEST_ASSERT_EQUAL(-13, s.counted); // "WebSocket Client Connected -13" in the 2026-10-31 log
    TEST_ASSERT_FALSE(s.broadcasts()); // the app never gets data
}

void test_ws_count_after_unfinished_connections() {
    FakeServer s(false);
    unfinishedThenApp(s);
    TEST_ASSERT_EQUAL(1, s.counted);
    TEST_ASSERT_TRUE(s.broadcasts());

    int page = s.open(); // the page connects, then a stray connection closes while both are up
    s.handshake(page);
    s.close(s.open());
    TEST_ASSERT_EQUAL(2, s.counted);
    s.close(page);
    TEST_ASSERT_EQUAL(1, s.counted);
    TEST_ASSERT_TRUE(s.broadcasts());
    for (int i = 0; i < FakeServer::kSlots; i++)
        if (s.slot[i] == FakeServer::Connected)
            s.close(i);
    TEST_ASSERT_EQUAL(0, s.counted);
    TEST_ASSERT_FALSE(s.broadcasts());
}

// The firmware source uses the recount and still broadcasts getJsonData() unchanged.
void test_ws_manager_source_counts_from_library() {
    std::string src = readRepoFile("croaster/src/CroasterWebSocketManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "CroasterWebSocketManager.cpp not found");
    std::string disc = between(src, "case WStype_DISCONNECTED:", "break;");
    std::string conn = between(src, "case WStype_CONNECTED:", "break;");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, disc.find("clientConnected = server.connectedClients();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, conn.find("clientConnected = server.connectedClients();"));
    TEST_ASSERT_EQUAL(std::string::npos, src.find("clientConnected--"));
    TEST_ASSERT_EQUAL(std::string::npos, src.find("clientConnected++"));
    std::string bc = between(src, "void CroasterWebSocketManager::broadcastData()", "\n}");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, bc.find("croaster->intervalSendData() * 1000"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, bc.find("String jsonData = croaster->getJsonData();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, bc.find("server.broadcastTXT(jsonData);"));
}

// IiemB's broadcast: {"id","name","data":{"interval","timer","BT","DBT","ET","DET","cBT","cET","temp"}},
// BT/DBT and ET/DET left out (not null) while the probe reads NaN; add-on fields come after "temp".
void test_broadcast_upstream_fields_in_order() {
    std::string src = readRepoFile("croaster/src/CroasterCore.cpp");
    std::string fn = between(src, "String CroasterCore::getJsonData(int id)", "\n}");
    TEST_ASSERT_FALSE_MESSAGE(fn.empty(), "getJsonData not found");
    const char* seq[] = {
        "doc[\"id\"] = id;",
        "doc[\"name\"] = ssidName();",
        "JsonObject data = doc[\"data\"].to<JsonObject>();",
        "data[\"interval\"] = intervalSend;",
        "data[\"timer\"] = timer;",
        "if (!isnan(tempBt)) {",
        "data[\"BT\"] = roundTo2(tempBt);",
        "data[\"DBT\"] = roundTo2(rorBt);",
        "if (!isnan(tempEt)) {",
        "data[\"ET\"] = roundTo2(tempEt);",
        "data[\"DET\"] = roundTo2(rorEt);",
        "data[\"cBT\"] = roundTo2(correctionBt);",
        "data[\"cET\"] = roundTo2(correctionEt);",
        "data[\"temp\"] = tempUnit;",
        "if (jsonDataExtension)",
        "serializeJson(doc, jsonOutput);",
    };
    size_t at = 0;
    for (const char* line : seq) {
        size_t next = fn.find(line, at);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, next, line);
        at = next + std::strlen(line);
    }
}

// The control fields added after "temp" never replace the app's fields, and the
// payload stays plain ASCII JSON without NaN or Infinity in any state.
void test_broadcast_extension_keeps_app_fields() {
    const char* upstream = "{\"id\":0,\"name\":\"[449B] Croaster V0.62\",\"data\":{\"interval\":3,\"timer\":78559.796,"
                           "\"BT\":201.5,\"DBT\":12.25,\"ET\":350.1,\"DET\":-3.5,\"cBT\":0,\"cET\":0,\"temp\":\"F\"}}";
    const char* keys[] = {"interval", "timer", "BT", "DBT", "ET", "DET", "cBT", "cET", "temp"};

    Snapshot base;
    base.heaterPct = 42;
    base.fanPct = 60;
    base.setpoint = 200;
    Snapshot nan = base;
    nan.et = nan.bt = nan.ambient = nan.setpoint = NAN;
    nan.heaterPct = INFINITY;
    Snapshot at = base;
    at.mode = Mode::Autotune;
    at.autotune = AutotuneStatus::Running;
    Snapshot failed = base;
    failed.autotune = AutotuneStatus::Failed;
    failed.autotuneReason = "no heat-up";
    failed.trip = SafetyTrip::OverTemp;
    Snapshot profile = base;
    profile.mode = Mode::Profile;
    profile.trip = SafetyTrip::CommsLost;
    const Snapshot* cases[] = {&base, &nan, &at, &failed, &profile};

    for (const Snapshot* s : cases)
        for (char unit : {'F', 'C'}) {
            JsonDocument doc;
            TEST_ASSERT_TRUE(deserializeJson(doc, upstream) == DeserializationError::Ok);
            JsonDocument before;
            before.set(doc);
            JsonObject data = doc["data"];
            controlproto::writeDataFields(*s, unit, data);

            for (const char* k : keys)
                TEST_ASSERT_TRUE_MESSAGE(data[k] == before["data"][k], k);
            TEST_ASSERT_TRUE(doc["id"] == before["id"]);
            TEST_ASSERT_TRUE(doc["name"] == before["name"]);

            std::string out;
            serializeJson(doc, out);
            for (unsigned char c : out)
                TEST_ASSERT_TRUE_MESSAGE(c >= 0x20 && c < 0x7f, out.c_str());
            TEST_ASSERT_EQUAL(std::string::npos, out.find("NaN"));
            TEST_ASSERT_EQUAL(std::string::npos, out.find("nan"));
            TEST_ASSERT_EQUAL(std::string::npos, out.find("Infinity"));
            TEST_ASSERT_EQUAL(0u, out.find(std::string(upstream, std::strlen(upstream) - 2))); // upstream prefix intact
            JsonDocument back;
            TEST_ASSERT_TRUE(deserializeJson(back, out) == DeserializationError::Ok);
        }
}

void runWebSocketTests() {
    RUN_TEST(test_ws_upstream_count_goes_negative);
    RUN_TEST(test_ws_count_after_unfinished_connections);
    RUN_TEST(test_ws_manager_source_counts_from_library);
    RUN_TEST(test_broadcast_upstream_fields_in_order);
    RUN_TEST(test_broadcast_extension_keeps_app_fields);
}
