#pragma once
// Saved roasts on the board: JSON records kept one per file, through the same
// storage interface as the profile library (profile_library.h). A Spec sets a
// kind's format version, limits and checks; Store indexes, saves and prunes.
// Pure logic (ArduinoJson and the standard library).
//
// Roast record v1 (directory "/roasts", the newest kMaxRoasts kept):
//   {"version":1,"name":"Kenya light","date":1761900000,"unit":"F",
//    "bean":"kenya-aa","beanName":"Kenya AA","greenG":250,"roastedG":212,"notes":"...",
//    "start":-60,"dt":2,"bt":[...],"et":[...],
//    "events":[{"code":"CHARGE","t":0,"bt":400.1},...]}
//   "date" is Unix seconds from the page's clock. The curve has a point every
//   "dt" seconds from "start", in seconds from CHARGE (from the first sample
//   when there was no CHARGE); null where a probe had no reading. "bean" (an
//   id), "beanName", "recipe" (the recipe that ran, by name), the weights in
//   grams and "notes" are optional.
//
// Bean record v1 (directory "/beans", up to kMaxBeans; a full store refuses):
//   {"version":1,"name":"Nyeri AA","origin":"Kenya","process":"washed",
//    "region":"Nyeri","varietal":"SL28","farm":"...","altitudeMin":1700,"altitudeMax":1900,
//    "moisture":10.5,"density":"high" or 720 (g/L),"harvest":"2026-01","score":87.5,
//    "flavours":["blackcurrant","grapefruit"],"notes":"...","link":"https://...",
//    "blend":true,"components":[{"origin":"Brazil","process":"natural","percent":60}],
//    "favourite":true,"stockG":4200,
//    "purchases":[{"g":5000,"price":42.5,"date":"2026-02-10"}]}
//   name, origin and process are required; the rest are optional. Standard
//   green-coffee specs: altitude in metres above sea level, moisture in
//   percent, density as a class or in g/L, score on the 100-point cupping
//   scale. "stockG" is the green weight on hand.
#include <ArduinoJson.h>

#include <cstddef>
#include <string>
#include <vector>

#include "profile_library.h"

namespace records {

using proflib::Error;
using proflib::LibraryStorage;

constexpr size_t kMaxRoasts = 25;
constexpr size_t kMaxRoastBytes = 13000; // one WebSocket message (15 KB) with its envelope
constexpr size_t kMaxRoastPoints = 900;
constexpr size_t kMaxRoastEvents = 16;
constexpr size_t kMaxBeans = 100;
constexpr size_t kMaxBeanBytes = 6000;
constexpr size_t kMaxPurchases = 50;
constexpr size_t kMaxComponents = 8;
constexpr size_t kMaxFlavours = 16;
constexpr float kMaxStockG = 1000000;

struct Spec {
    int version;          // newest format this firmware reads
    size_t maxCount;      // records kept
    size_t maxBytes;      // serialized record
    bool pruneOldest;     // full: delete the oldest by "date" instead of refusing
    const char* idPrefix; // ids made from the name start with it ("" for none)
    const char* const* summary; // keys copied into list entries
    size_t summaryCount;
    // Checks beyond version, name and size; returns why it fails, or nullptr.
    const char* (*check)(JsonObjectConst rec);
};

extern const Spec kRoastSpec;
extern const Spec kBeanSpec;

// Why a roast record fails its checks; nullptr when it passes.
const char* checkRoast(JsonObjectConst rec);
// Why a bean record fails its checks; nullptr when it passes.
const char* checkBean(JsonObjectConst rec);

struct Meta {
    std::string id;
    std::string name;
    double date = 0; // "date", 0 when missing
    size_t bytes = 0;
    bool valid = true;
    std::string summary; // JSON object of the Spec's summary keys
};

struct Saved {
    std::string id;
    bool created = false;
    std::vector<std::string> pruned; // ids deleted to make room
};

class Store {
public:
    Store(LibraryStorage& fs, const Spec& spec)
        : fs_(fs)
        , spec_(spec) {}

    // Deletes leftover temporary files and indexes the records.
    bool begin();
    bool ready() const {
        return ready_;
    }
    const Spec& spec() const {
        return spec_;
    }
    // Newest first ("date"), then by name.
    const std::vector<Meta>& list() const {
        return index_;
    }
    Error get(const char* id, std::string& out);
    // Validates and writes `rec`. With an id the record is replaced (or created
    // under that id); without one a new id is made from the name. When the kind
    // is full, or a write fails, a pruning kind deletes its oldest record and
    // tries once more. `reason` gets the check's reason for BadRequest.
    Error save(const char* id, JsonObjectConst rec, Saved& out, const char** reason = nullptr);
    Error remove(const char* id);
    // Validates `rec` against the common rules and the Spec's check.
    Error validate(JsonObjectConst rec, const char** reason = nullptr) const;

private:
    const Meta* find(const std::string& id) const;
    const Meta* oldest(const std::string& except) const;
    void put(Meta m);
    void drop(const std::string& id);
    Meta describe(const std::string& id, JsonObjectConst rec, size_t bytes) const;
    bool writeFile(const std::string& id, const std::string& text);

    LibraryStorage& fs_;
    const Spec& spec_;
    std::vector<Meta> index_;
    bool ready_ = false;
};

// Takes `greenG` off the bean's "stockG" (not below 0; a bean without stock
// stays without) and saves it. `stockG` gets the new figure, or -1 when the
// bean has no stock recorded.
Error useStock(Store& beans, const char* beanId, float greenG, float& stockG);
// Puts `greenG` back on the bean's stock (a saved roast moved to another bean,
// unlinked, or its green weight changed). Same rules and `stockG` as useStock.
Error returnStock(Store& beans, const char* beanId, float greenG, float& stockG);

// The stock a roast record takes: its bean id and green weight (both needed;
// otherwise bean is empty and nothing was taken).
struct StockUse {
    std::string bean;
    float greenG = 0;
    bool operator==(const StockUse& o) const {
        return bean == o.bean && (bean.empty() || greenG == o.greenG);
    }
};
StockUse stockUseOf(JsonObjectConst roast);

} // namespace records
