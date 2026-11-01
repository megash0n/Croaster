#if defined(CPID_ENABLE_LIBRARY) && CPID_ENABLE_LIBRARY
#    include "RecordsPlugin.h"

#    include <CroasterApp.h>

namespace {
// Same envelope as Croaster's genResponseCommand(): {"command":name,"response":{...}}.
JsonObject beginReply(JsonDocument& doc, const char* command) {
    doc["command"] = command;
    return doc["response"].to<JsonObject>();
}
String finishReply(const JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    return out;
}
String failure(const char* command, records::Error e, const char* reason = nullptr) {
    JsonDocument doc;
    JsonObject r = beginReply(doc, command);
    r["ok"] = false;
    r["error"] = proflib::errorCode(e);
    if (reason)
        r["reason"] = reason;
    return finishReply(doc);
}
const char* idOf(const JsonObject& json) {
    return json["id"].is<const char*>() ? json["id"].as<const char*>() : "";
}
} // namespace

void RecordsPlugin::begin() {
    const bool ok = roastFs_.mount() && roasts_.begin();
    const bool beansOk = beanFs_.mount() && beans_.begin();
    Serial.printf("# Records: roasts %s (%u), beans %s (%u)\n", ok ? "ready" : "unavailable",
                  (unsigned)roasts_.list().size(), beansOk ? "ready" : "unavailable", (unsigned)beans_.list().size());
}

void RecordsPlugin::attach(CroasterApp& app) {
    CroasterCommandHandler& c = app.commands();
    c.onCommand("roastList", [this](const JsonObject&) { return list("roastList", roasts_, roastFs_, "roasts"); });
    c.onCommand("roastGet", [this](const JsonObject& json) { return get("roastGet", roasts_, json, "roast"); });
    c.onCommand("roastSave", [this](const JsonObject& json) { return saveRoast(json); });
    c.onCommand("roastDelete", [this](const JsonObject& json) { return remove("roastDelete", roasts_, json); });
    c.onCommand("beanList", [this](const JsonObject&) { return list("beanList", beans_, beanFs_, "beans"); });
    c.onCommand("beanGet", [this](const JsonObject& json) { return get("beanGet", beans_, json, "bean"); });
    c.onCommand("beanSave", [this](const JsonObject& json) { return saveBean(json); });
    c.onCommand("beanDelete", [this](const JsonObject& json) { return remove("beanDelete", beans_, json); });
}

String RecordsPlugin::list(const char* command, records::Store& store, LittleFsLibraryStorage& fs, const char* key) {
    if (!store.ready())
        return failure(command, records::Error::Unavailable);
    JsonDocument doc;
    JsonObject r = beginReply(doc, command);
    r["ok"] = true;
    r["version"] = store.spec().version;
    r["max"] = store.spec().maxCount;
    r["maxBytes"] = store.spec().maxBytes;
    if (&store == &roasts_)
        r["maxPoints"] = records::kMaxRoastPoints;
    r["usedBytes"] = fs.usedBytes();
    r["totalBytes"] = fs.totalBytes();
    JsonArray a = r[key].to<JsonArray>();
    for (const records::Meta& m : store.list()) {
        JsonObject o = a.add<JsonObject>();
        o["id"] = m.id.c_str(); // copied: the index may change before the reply is sent
        o["name"] = m.name.c_str();
        o["bytes"] = m.bytes;
        o["valid"] = m.valid;
        JsonDocument s;
        if (!deserializeJson(s, m.summary))
            for (JsonPairConst kv : s.as<JsonObjectConst>())
                o[kv.key()] = kv.value();
    }
    return finishReply(doc);
}

String RecordsPlugin::get(const char* command, records::Store& store, const JsonObject& json, const char* key) {
    std::string text;
    const char* id = idOf(json);
    const records::Error e = store.get(id, text);
    if (e != records::Error::None)
        return failure(command, e);
    // Validated when saved: spliced in as text. The id is [a-z0-9_-], no escaping needed.
    String out;
    out.reserve(text.size() + 80);
    out += "{\"command\":\"";
    out += command;
    out += "\",\"response\":{\"ok\":true,\"id\":\"";
    out += id;
    out += "\",\"";
    out += key;
    out += "\":";
    out += text.c_str();
    out += "}}";
    return out;
}

String RecordsPlugin::saveRoast(const JsonObject& json) {
    if (const char* busy = proflib::busyReason(controller_.snapshot()))
        return failure("roastSave", records::Error::Busy, busy);
    if (!json["roast"].is<JsonObject>())
        return failure("roastSave", records::Error::BadRequest);
    records::Saved saved;
    const char* reason = nullptr;
    const records::Error e = roasts_.save(idOf(json), json["roast"].as<JsonObjectConst>(), saved, &reason);
    if (e != records::Error::None)
        return failure("roastSave", e, reason);
    JsonDocument doc;
    JsonObject r = beginReply(doc, "roastSave");
    r["ok"] = true;
    r["id"] = saved.id.c_str();
    r["created"] = saved.created;
    JsonArray p = r["pruned"].to<JsonArray>();
    for (const std::string& id : saved.pruned)
        p.add(id.c_str());
    JsonObjectConst rec = json["roast"].as<JsonObjectConst>();
    if (saved.created && rec["bean"].is<const char*>() && rec["greenG"].is<float>()) {
        float left = -1;
        const records::Error s = records::useStock(beans_, rec["bean"].as<const char*>(), rec["greenG"].as<float>(), left);
        JsonObject st = r["stock"].to<JsonObject>();
        st["bean"] = rec["bean"];
        if (s == records::Error::None)
            st["stockG"] = left;
        else
            st["error"] = proflib::errorCode(s);
    }
    return finishReply(doc);
}

String RecordsPlugin::saveBean(const JsonObject& json) {
    if (const char* busy = proflib::busyReason(controller_.snapshot()))
        return failure("beanSave", records::Error::Busy, busy);
    if (!json["bean"].is<JsonObject>())
        return failure("beanSave", records::Error::BadRequest);
    records::Saved saved;
    const char* reason = nullptr;
    const records::Error e = beans_.save(idOf(json), json["bean"].as<JsonObjectConst>(), saved, &reason);
    if (e != records::Error::None)
        return failure("beanSave", e, reason);
    JsonDocument doc;
    JsonObject r = beginReply(doc, "beanSave");
    r["ok"] = true;
    r["id"] = saved.id.c_str();
    r["created"] = saved.created;
    return finishReply(doc);
}

String RecordsPlugin::remove(const char* command, records::Store& store, const JsonObject& json) {
    if (const char* busy = proflib::busyReason(controller_.snapshot()))
        return failure(command, records::Error::Busy, busy);
    const char* id = idOf(json);
    const records::Error e = store.remove(id);
    if (e != records::Error::None)
        return failure(command, e);
    JsonDocument doc;
    JsonObject r = beginReply(doc, command);
    r["ok"] = true;
    r["id"] = id;
    return finishReply(doc);
}
#endif
