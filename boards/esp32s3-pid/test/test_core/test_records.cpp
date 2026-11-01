// Saved roasts on the board (core/records.h).
#include <ArduinoJson.h>
#include <unity.h>

#include <map>
#include <string>

#include "core/records.h"

namespace {
struct MemFs : proflib::LibraryStorage {
    std::map<std::string, std::string> files;
    int failWrites = 0; // the next n writes fail
    bool list(std::vector<std::string>& names) override {
        for (auto& f : files)
            names.push_back(f.first);
        return true;
    }
    bool read(const std::string& n, std::string& out) override {
        auto it = files.find(n);
        if (it == files.end())
            return false;
        out = it->second;
        return true;
    }
    bool write(const std::string& n, const std::string& d) override {
        if (failWrites > 0) {
            failWrites--;
            return false;
        }
        files[n] = d;
        return true;
    }
    bool rename(const std::string& a, const std::string& b) override {
        auto it = files.find(a);
        if (it == files.end())
            return false;
        files[b] = it->second;
        files.erase(it);
        return true;
    }
    bool remove(const std::string& n) override {
        return files.erase(n) == 1;
    }
};

std::string roast(const char* name, double date, const char* extra = "") {
    char buf[512];
    snprintf(buf, sizeof buf,
             R"({"version":1,"name":"%s","date":%.0f,"unit":"F","start":-30,"dt":2,)"
             R"("bt":[300,null,310.5],"et":[400,401,402],"events":[{"code":"CHARGE","t":0,"bt":300}]%s})",
             name, date, extra);
    return buf;
}
records::Error save(records::Store& s, const std::string& text, records::Saved& out, const char* id = "",
                    const char** why = nullptr) {
    JsonDocument d;
    deserializeJson(d, text);
    return s.save(id, d.as<JsonObjectConst>(), out, why);
}
} // namespace

void test_roast_record_checks() {
    MemFs fs;
    records::Store s(fs, records::kRoastSpec);
    TEST_ASSERT_TRUE(s.begin());
    records::Saved out;
    const char* why = nullptr;
    TEST_ASSERT_EQUAL(records::Error::None, save(s, roast("Kenya", 1000, R"(,"greenG":250,"roastedG":212)"), out));
    TEST_ASSERT_EQUAL_STRING("kenya", out.id.c_str());
    TEST_ASSERT_TRUE(out.created);
    TEST_ASSERT_EQUAL(records::Error::BadName, save(s, roast("", 1000), out));
    TEST_ASSERT_EQUAL(records::Error::BadRequest, save(s, roast("x", 0), out, "", &why));
    TEST_ASSERT_EQUAL_STRING("date must be Unix seconds", why);
    TEST_ASSERT_EQUAL(records::Error::BadRequest, save(s, roast("x", 5, R"(,"greenG":-1)"), out, "", &why));
    TEST_ASSERT_EQUAL(records::Error::NewerVersion,
                      save(s, R"({"version":2,"name":"x","date":5,"unit":"F","start":0,"dt":1,"bt":[]})", out));
    TEST_ASSERT_EQUAL(records::Error::BadRequest,
                      save(s, R"({"version":1,"name":"x","date":5,"unit":"F","start":0,"dt":1,"bt":[1],"et":[1,2]})",
                           out, "", &why));
    TEST_ASSERT_EQUAL_STRING("et must match bt", why);
    // List entry: summary keys only, events without their BT.
    TEST_ASSERT_EQUAL(1, (int)s.list().size());
    JsonDocument m;
    deserializeJson(m, s.list()[0].summary);
    TEST_ASSERT_EQUAL(250, m["greenG"].as<int>());
    TEST_ASSERT_EQUAL_STRING("CHARGE", m["events"][0]["code"]);
    TEST_ASSERT_TRUE(m["events"][0]["bt"].isNull());
    TEST_ASSERT_TRUE(m["bt"].isNull());
    // Same name: a new id; with an id: replaced.
    TEST_ASSERT_EQUAL(records::Error::None, save(s, roast("Kenya", 2000), out));
    TEST_ASSERT_EQUAL_STRING("kenya-2", out.id.c_str());
    TEST_ASSERT_EQUAL(records::Error::None, save(s, roast("Kenya renamed", 2000), out, "kenya-2"));
    TEST_ASSERT_FALSE(out.created);
    TEST_ASSERT_EQUAL_STRING("Kenya renamed", s.list()[0].name.c_str()); // newest first
    std::string text;
    TEST_ASSERT_EQUAL(records::Error::None, s.get("kenya", text));
    TEST_ASSERT_TRUE(text.find("\"roastedG\":212") != std::string::npos);
    TEST_ASSERT_EQUAL(records::Error::None, s.remove("kenya"));
    TEST_ASSERT_EQUAL(records::Error::NotFound, s.get("kenya", text));
}

void test_roasts_keep_the_newest() {
    MemFs fs;
    records::Store s(fs, records::kRoastSpec);
    TEST_ASSERT_TRUE(s.begin());
    records::Saved out;
    for (int i = 0; i < (int)records::kMaxRoasts; i++) {
        char name[16];
        snprintf(name, sizeof name, "r%d", i);
        TEST_ASSERT_EQUAL(records::Error::None, save(s, roast(name, 1000 + i), out));
        TEST_ASSERT_EQUAL(0, (int)out.pruned.size());
    }
    TEST_ASSERT_EQUAL(records::Error::None, save(s, roast("new", 5000), out));
    TEST_ASSERT_EQUAL(1, (int)out.pruned.size());
    TEST_ASSERT_EQUAL_STRING("r0", out.pruned[0].c_str()); // the oldest date
    TEST_ASSERT_EQUAL((int)records::kMaxRoasts, (int)s.list().size());
    TEST_ASSERT_EQUAL(0, (int)fs.files.count("r0.json"));
    // Replacing one does not prune.
    TEST_ASSERT_EQUAL(records::Error::None, save(s, roast("r1 again", 1001), out, "r1"));
    TEST_ASSERT_EQUAL(0, (int)out.pruned.size());
    // A failed write (storage full) gives up the oldest once and retries.
    fs.failWrites = 1;
    TEST_ASSERT_EQUAL(records::Error::None, save(s, roast("full", 6000), out));
    TEST_ASSERT_EQUAL(2, (int)out.pruned.size()); // over the count, then for space
    fs.failWrites = 2;
    TEST_ASSERT_EQUAL(records::Error::Storage, save(s, roast("fails", 7000), out));
    TEST_ASSERT_EQUAL(0, (int)fs.files.count("fails.tmp"));
    // A restart re-indexes, newest first, and clears temporary files.
    fs.files["x.tmp"] = "{";
    fs.files["bad.json"] = "{";
    records::Store again(fs, records::kRoastSpec);
    TEST_ASSERT_TRUE(again.begin());
    TEST_ASSERT_EQUAL(0, (int)fs.files.count("x.tmp"));
    TEST_ASSERT_EQUAL_STRING("full", again.list()[0].id.c_str());
    TEST_ASSERT_FALSE(again.list().back().valid); // "bad": date 0, listed last
}

const char* beanWhy(const char* extra) {
    static std::string text;
    text = std::string(R"({"version":1,"name":"Nyeri AA","origin":"Kenya","process":"washed")") + extra + "}";
    static JsonDocument d;
    d.clear();
    deserializeJson(d, text);
    return records::checkBean(d.as<JsonObjectConst>());
}

void test_bean_record_checks() {
    TEST_ASSERT_NULL(beanWhy(""));
    TEST_ASSERT_NULL(beanWhy(R"(,"region":"Nyeri","altitudeMin":1700,"altitudeMax":1900,"moisture":10.5,)"
                             R"("density":"high","score":87.5,"flavours":["blackcurrant"],"blend":true,)"
                             R"("components":[{"origin":"Brazil","process":"natural","percent":60}],)"
                             R"("favourite":true,"stockG":4200,"purchases":[{"g":5000,"price":42.5,"date":"2026-02-10"}])"));
    TEST_ASSERT_NULL(beanWhy(R"(,"density":720)"));
    TEST_ASSERT_NOT_NULL(beanWhy(R"(,"density":"dense")"));
    TEST_ASSERT_NOT_NULL(beanWhy(R"(,"density":50)"));
    TEST_ASSERT_NOT_NULL(beanWhy(R"(,"altitudeMin":2000,"altitudeMax":1500)"));
    TEST_ASSERT_NOT_NULL(beanWhy(R"(,"score":101)"));
    TEST_ASSERT_NOT_NULL(beanWhy(R"(,"stockG":-1)"));
    TEST_ASSERT_NOT_NULL(beanWhy(R"(,"purchases":[{"price":3}])"));
    TEST_ASSERT_NOT_NULL(beanWhy(R"(,"flavours":"berry")"));
    TEST_ASSERT_NOT_NULL(beanWhy(R"(,"favourite":1)"));
    JsonDocument d;
    deserializeJson(d, R"({"version":1,"name":"No origin","process":"washed"})");
    TEST_ASSERT_NOT_NULL(records::checkBean(d.as<JsonObjectConst>()));
}

void test_beans_refuse_when_full_and_roasts_use_stock() {
    MemFs fs;
    records::Store beans(fs, records::kBeanSpec);
    TEST_ASSERT_TRUE(beans.begin());
    records::Saved out;
    const std::string b = R"({"version":1,"name":"Nyeri AA","origin":"Kenya","process":"washed","stockG":300})";
    TEST_ASSERT_EQUAL(records::Error::None, save(beans, b, out));
    const std::string id = out.id;
    float left = 0;
    TEST_ASSERT_EQUAL(records::Error::None, records::useStock(beans, id.c_str(), 250, left));
    TEST_ASSERT_EQUAL_FLOAT(50, left);
    TEST_ASSERT_EQUAL(records::Error::None, records::useStock(beans, id.c_str(), 250, left));
    TEST_ASSERT_EQUAL_FLOAT(0, left); // not below zero
    TEST_ASSERT_EQUAL(records::Error::NotFound, records::useStock(beans, "nope", 10, left));
    TEST_ASSERT_EQUAL(records::Error::None,
                      save(beans, R"({"version":1,"name":"No stock","origin":"Peru","process":"natural"})", out));
    TEST_ASSERT_EQUAL(records::Error::None, records::useStock(beans, out.id.c_str(), 10, left));
    TEST_ASSERT_EQUAL_FLOAT(-1, left); // no stock recorded: unchanged
    for (size_t i = beans.list().size(); i < records::kMaxBeans; i++) {
        char name[16];
        snprintf(name, sizeof name, "Bean %u", (unsigned)i);
        TEST_ASSERT_EQUAL(records::Error::None,
                          save(beans, std::string(R"({"version":1,"name":")") + name + R"(","origin":"X","process":"Y"})", out));
    }
    TEST_ASSERT_EQUAL(records::Error::Full,
                      save(beans, R"({"version":1,"name":"One more","origin":"X","process":"Y"})", out));
    TEST_ASSERT_EQUAL(records::kMaxBeans, beans.list().size());
}

void runRecordTests() {
    RUN_TEST(test_roast_record_checks);
    RUN_TEST(test_roasts_keep_the_newest);
    RUN_TEST(test_bean_record_checks);
    RUN_TEST(test_beans_refuse_when_full_and_roasts_use_stock);
}
