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
//   id), "beanName", the weights in grams and "notes" are optional.
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

// Why a roast record fails its checks; nullptr when it passes.
const char* checkRoast(JsonObjectConst rec);

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

} // namespace records
