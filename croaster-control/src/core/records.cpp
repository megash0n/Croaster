#include "records.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace records {

namespace {
const char* const kJson = ".json";
const char* const kTmp = ".tmp";

bool endsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}
char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}
// Newest first, then name (case-insensitive), then id.
bool newerFirst(const Meta& a, const Meta& b) {
    if (a.date != b.date)
        return a.date > b.date;
    const size_t n = std::min(a.name.size(), b.name.size());
    for (size_t i = 0; i < n; i++) {
        const char x = lower(a.name[i]), y = lower(b.name[i]);
        if (x != y)
            return x < y;
    }
    if (a.name.size() != b.name.size())
        return a.name.size() < b.name.size();
    return a.id < b.id;
}
// 1..max code points, no control characters, not only spaces.
bool textOk(JsonVariantConst v, size_t max) {
    if (!v.is<const char*>())
        return false;
    const char* s = v.as<const char*>();
    bool visible = false;
    for (const char* c = s; *c; c++) {
        if (static_cast<unsigned char>(*c) < 0x20)
            return false;
        if (*c != ' ')
            visible = true;
    }
    const size_t n = proflib::utf8Length(s);
    return visible && n >= 1 && n <= max;
}
bool optionalText(JsonVariantConst v, size_t max) {
    return v.isNull() || (v.is<const char*>() && proflib::utf8Length(v.as<const char*>()) <= max);
}
bool optionalNumber(JsonVariantConst v, float lo, float hi) {
    if (v.isNull())
        return true;
    if (!v.is<float>())
        return false;
    const float x = v.as<float>();
    return std::isfinite(x) && x >= lo && x <= hi;
}
// A list of up to `max` numbers or nulls.
bool series(JsonVariantConst v, size_t max, size_t& count) {
    JsonArrayConst a = v.as<JsonArrayConst>();
    if (a.isNull() || a.size() > max)
        return false;
    for (JsonVariantConst x : a)
        if (!x.isNull() && !(x.is<float>() && std::isfinite(x.as<float>())))
            return false;
    count = a.size();
    return true;
}

const char* const kRoastSummary[] = {"date", "unit", "bean", "beanName", "greenG", "roastedG", "events"};
const char* const kBeanSummary[] = {"origin", "process", "region", "varietal", "blend", "favourite", "score", "stockG"};

constexpr size_t kMaxField = 60; // region, varietal, farm, flavour, origin, process
bool requiredText(JsonVariantConst v) {
    return textOk(v, kMaxField);
}
bool optionalBool(JsonVariantConst v) {
    return v.isNull() || v.is<bool>();
}
} // namespace

const char* checkRoast(JsonObjectConst r) {
    if (!(r["date"].is<double>() && r["date"].as<double>() > 0))
        return "date must be Unix seconds";
    const char* unit = r["unit"].is<const char*>() ? r["unit"].as<const char*>() : "";
    if (!((unit[0] == 'C' || unit[0] == 'F' || unit[0] == 'K') && unit[1] == '\0'))
        return "unit must be C, F or K";
    if (!optionalText(r["bean"], proflib::kMaxIdLen) || !optionalText(r["beanName"], proflib::kMaxNameChars))
        return "bean must be an id and a name";
    if (!optionalNumber(r["greenG"], 0, 100000) || !optionalNumber(r["roastedG"], 0, 100000))
        return "weights must be grams, 0..100000";
    if (!optionalText(r["notes"], proflib::kMaxNotesChars))
        return "notes must be text up to 1024 characters";
    if (!(r["start"].is<float>() && std::isfinite(r["start"].as<float>())))
        return "start must be seconds";
    if (!(r["dt"].is<float>() && r["dt"].as<float>() > 0 && r["dt"].as<float>() <= 60))
        return "dt must be 0..60 s";
    size_t bt = 0, et = 0;
    if (!series(r["bt"], kMaxRoastPoints, bt))
        return "bt must be a list of up to 900 readings";
    if (!r["et"].isNull() && (!series(r["et"], kMaxRoastPoints, et) || et != bt))
        return "et must match bt";
    JsonArrayConst ev = r["events"].as<JsonArrayConst>();
    if (!r["events"].isNull() && (ev.isNull() || ev.size() > kMaxRoastEvents))
        return "events must be a list of up to 16";
    for (JsonVariantConst e : ev)
        if (!textOk(e["code"], 12) || !(e["t"].is<float>() && std::isfinite(e["t"].as<float>())))
            return "each event needs a code and a time";
    return nullptr;
}

const Spec kRoastSpec = {1, kMaxRoasts, kMaxRoastBytes, true, "",
                         kRoastSummary, sizeof(kRoastSummary) / sizeof(kRoastSummary[0]), checkRoast};

const char* checkBean(JsonObjectConst r) {
    if (!requiredText(r["origin"]) || !requiredText(r["process"]))
        return "origin and process are required";
    for (const char* k : {"region", "varietal", "farm"})
        if (!optionalText(r[k], kMaxField))
            return "region, varietal and farm must be text up to 60 characters";
    if (!optionalNumber(r["altitudeMin"], 0, 9000) || !optionalNumber(r["altitudeMax"], 0, 9000))
        return "altitude must be 0..9000 m";
    if (r["altitudeMin"].is<float>() && r["altitudeMax"].is<float>() &&
        r["altitudeMin"].as<float>() > r["altitudeMax"].as<float>())
        return "altitude min must not exceed max";
    if (!optionalNumber(r["moisture"], 0, 30))
        return "moisture must be 0..30 %";
    JsonVariantConst d = r["density"];
    if (!d.isNull() && !(d.is<float>() ? optionalNumber(d, 300, 1000)
                                        : d.is<const char*>() && (!std::strcmp(d.as<const char*>(), "high") ||
                                                                  !std::strcmp(d.as<const char*>(), "medium") ||
                                                                  !std::strcmp(d.as<const char*>(), "low"))))
        return "density must be high, medium, low or 300..1000 g/L";
    if (!optionalText(r["harvest"], 20))
        return "harvest must be a date or season up to 20 characters";
    if (!optionalNumber(r["score"], 0, 100))
        return "score must be 0..100";
    if (!r["flavours"].isNull()) {
        JsonArrayConst f = r["flavours"].as<JsonArrayConst>();
        if (f.isNull() || f.size() > kMaxFlavours)
            return "flavours must be a list of up to 16";
        for (JsonVariantConst x : f)
            if (!textOk(x, kMaxField))
                return "each flavour must be text up to 60 characters";
    }
    if (!optionalText(r["notes"], proflib::kMaxNotesChars))
        return "notes must be text up to 1024 characters";
    if (!optionalText(r["link"], 200))
        return "link must be up to 200 characters";
    if (!optionalBool(r["blend"]) || !optionalBool(r["favourite"]))
        return "blend and favourite must be true or false";
    if (!r["components"].isNull()) {
        JsonArrayConst c = r["components"].as<JsonArrayConst>();
        if (c.isNull() || c.size() > kMaxComponents)
            return "components must be a list of up to 8";
        for (JsonVariantConst x : c)
            if (!optionalText(x["origin"], kMaxField) || !optionalText(x["process"], kMaxField) ||
                !optionalNumber(x["percent"], 0, 100))
                return "each component has an origin, a process and a percent";
    }
    if (!optionalNumber(r["stockG"], 0, kMaxStockG))
        return "stock must be 0..1000000 g";
    if (!r["purchases"].isNull()) {
        JsonArrayConst p = r["purchases"].as<JsonArrayConst>();
        if (p.isNull() || p.size() > kMaxPurchases)
            return "purchases must be a list of up to 50";
        for (JsonVariantConst x : p)
            if (!(x["g"].is<float>() && optionalNumber(x["g"], 0, kMaxStockG)) || !optionalNumber(x["price"], 0, 1e6) ||
                !optionalText(x["date"], 20))
                return "each purchase needs grams, with an optional price and date";
    }
    return nullptr;
}

const Spec kBeanSpec = {1, kMaxBeans, kMaxBeanBytes, false, "",
                        kBeanSummary, sizeof(kBeanSummary) / sizeof(kBeanSummary[0]), checkBean};

Error Store::validate(JsonObjectConst r, const char** reason) const {
    if (r.isNull())
        return Error::BadRequest;
    JsonVariantConst v = r["version"];
    if (!v.is<int>() || v.as<int>() < 1)
        return Error::BadVersion;
    if (v.as<int>() > spec_.version)
        return Error::NewerVersion;
    if (!textOk(r["name"], proflib::kMaxNameChars))
        return Error::BadName;
    if (spec_.check)
        if (const char* why = spec_.check(r)) {
            if (reason)
                *reason = why;
            return Error::BadRequest;
        }
    return Error::None;
}

Meta Store::describe(const std::string& id, JsonObjectConst r, size_t bytes) const {
    Meta m;
    m.id = id;
    m.bytes = bytes;
    m.valid = validate(r) == Error::None;
    m.name = r["name"].is<const char*>() ? r["name"].as<const char*>() : id.c_str();
    m.date = r["date"] | 0.0;
    JsonDocument s;
    JsonObject o = s.to<JsonObject>();
    for (size_t i = 0; i < spec_.summaryCount; i++) {
        const char* key = spec_.summary[i];
        JsonVariantConst x = r[key];
        if (x.isNull())
            continue;
        if (std::strcmp(key, "events") == 0) { // event codes and times only
            JsonArray a = o[key].to<JsonArray>();
            for (JsonVariantConst e : x.as<JsonArrayConst>()) {
                JsonObject b = a.add<JsonObject>();
                b["code"] = e["code"];
                b["t"] = e["t"];
            }
        } else {
            o[key] = x;
        }
    }
    serializeJson(s, m.summary);
    return m;
}

const Meta* Store::find(const std::string& id) const {
    for (const Meta& m : index_)
        if (m.id == id)
            return &m;
    return nullptr;
}

const Meta* Store::oldest(const std::string& except) const {
    for (auto it = index_.rbegin(); it != index_.rend(); ++it)
        if (it->id != except)
            return &*it;
    return nullptr;
}

void Store::put(Meta m) {
    drop(m.id);
    index_.insert(std::upper_bound(index_.begin(), index_.end(), m, newerFirst), std::move(m));
}

void Store::drop(const std::string& id) {
    for (auto it = index_.begin(); it != index_.end(); ++it)
        if (it->id == id) {
            index_.erase(it);
            return;
        }
}

bool Store::begin() {
    index_.clear();
    ready_ = false;
    std::vector<std::string> names;
    if (!fs_.list(names))
        return false;
    std::string text;
    for (const std::string& name : names) {
        if (endsWith(name, kTmp)) {
            fs_.remove(name);
            continue;
        }
        if (!endsWith(name, kJson))
            continue;
        const std::string id = name.substr(0, name.size() - std::strlen(kJson));
        // Twice the limit at most: files left by a larger limit stay listed for deleting.
        if (!proflib::validId(id.c_str()) || index_.size() >= 2 * spec_.maxCount || !fs_.read(name, text))
            continue;
        JsonDocument doc;
        if (deserializeJson(doc, text) || !doc.is<JsonObject>()) {
            Meta m;
            m.id = id;
            m.name = id;
            m.bytes = text.size();
            m.valid = false;
            m.summary = "{}";
            put(std::move(m));
            continue;
        }
        put(describe(id, doc.as<JsonObjectConst>(), text.size()));
    }
    ready_ = true;
    return true;
}

Error Store::get(const char* id, std::string& out) {
    if (!ready_)
        return Error::Unavailable;
    if (!proflib::validId(id))
        return Error::BadId;
    if (!find(id))
        return Error::NotFound;
    return fs_.read(std::string(id) + kJson, out) ? Error::None : Error::Storage;
}

bool Store::writeFile(const std::string& id, const std::string& text) {
    const std::string tmp = id + kTmp;
    if (fs_.write(tmp, text) && fs_.rename(tmp, id + kJson))
        return true;
    fs_.remove(tmp);
    return false;
}

Error Store::save(const char* reqId, JsonObjectConst r, Saved& out, const char** reason) {
    out = Saved();
    if (!ready_)
        return Error::Unavailable;
    const Error e = validate(r, reason);
    if (e != Error::None)
        return e;
    const size_t bytes = measureJson(r);
    if (bytes > spec_.maxBytes)
        return Error::TooLarge;
    std::string id;
    if (reqId && *reqId) {
        if (!proflib::validId(reqId))
            return Error::BadId;
        id = reqId;
    } else {
        const std::string base = std::string(spec_.idPrefix) + proflib::slug(r["name"]);
        id = base;
        for (unsigned n = 2; find(id); n++)
            id = base + "-" + std::to_string(n);
    }
    const bool exists = find(id) != nullptr;
    while (!exists && index_.size() >= spec_.maxCount) {
        const Meta* old = spec_.pruneOldest ? oldest(id) : nullptr;
        if (!old)
            return Error::Full;
        const std::string gone = old->id;
        if (!fs_.remove(gone + kJson))
            return Error::Storage;
        drop(gone);
        out.pruned.push_back(gone);
    }
    std::string text;
    text.reserve(bytes);
    serializeJson(r, text);
    if (!writeFile(id, text)) {
        // Probably out of space: a pruning kind gives up its oldest record once.
        const Meta* old = spec_.pruneOldest ? oldest(id) : nullptr;
        if (!old)
            return Error::Storage;
        const std::string gone = old->id;
        if (!fs_.remove(gone + kJson))
            return Error::Storage;
        drop(gone);
        out.pruned.push_back(gone);
        if (!writeFile(id, text))
            return Error::Storage;
    }
    put(describe(id, r, text.size()));
    out.id = id;
    out.created = !exists;
    return Error::None;
}

Error Store::remove(const char* id) {
    if (!ready_)
        return Error::Unavailable;
    if (!proflib::validId(id))
        return Error::BadId;
    if (!find(id))
        return Error::NotFound;
    if (!fs_.remove(std::string(id) + kJson))
        return Error::Storage;
    drop(id);
    return Error::None;
}

Error useStock(Store& beans, const char* beanId, float greenG, float& stockG) {
    stockG = -1;
    std::string text;
    const Error e = beans.get(beanId, text);
    if (e != Error::None)
        return e;
    JsonDocument doc;
    if (deserializeJson(doc, text) || !doc.is<JsonObject>())
        return Error::Storage;
    JsonObject b = doc.as<JsonObject>();
    if (!b["stockG"].is<float>())
        return Error::None;
    const float left = std::max(0.0f, b["stockG"].as<float>() - std::max(0.0f, greenG));
    b["stockG"] = std::round(left * 10) / 10;
    Saved saved;
    const Error w = beans.save(beanId, b, saved);
    if (w == Error::None)
        stockG = b["stockG"].as<float>();
    return w;
}

} // namespace records
