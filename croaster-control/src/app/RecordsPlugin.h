#pragma once
// Saved roasts and green-coffee beans for Croaster PID, registered with Croaster's public onCommand()
// hook. Records live in LittleFS beside the profile library
// (library_storage.h); the format and the rules are in core/records.h. Built
// when CPID_ENABLE_LIBRARY is 1.
//
//   {"command":"roastList"}
//       -> response {"ok":true,"version":1,"max":25,"maxBytes":13000,"maxPoints":900,
//          "usedBytes":..,"totalBytes":..,"roasts":[{"id","name","bytes","valid",
//          "date","unit","bean","beanName","greenG","roastedG","events":[{"code","t"}]}]}
//   {"command":"roastGet","id":"x"}               -> response {"ok":true,"id":"x","roast":{record}}
//   {"command":"roastSave","roast":{...},"id":"x"?}
//       -> response {"ok":true,"id":"x","created":bool,"pruned":["oldest-id",...],
//          "stock":{"bean":"b","stockG":n}?}
//     A failed roastSave still lists "pruned" when a roast was deleted to make
//     room and the new file still could not be written.
//       Without an id a new roast is added; when 25 are kept the oldest is deleted.
//       A new roast with "bean" and "greenG" takes the green weight off that
//       bean's stock ("stockG" -1: the bean records no stock; "error" when it
//       could not be updated).
//   {"command":"roastDelete","id":"x"}            -> response {"ok":true,"id":"x"}
//   beanList, beanGet, beanSave, beanDelete: the same for beans ("beans",
//       "bean"); up to 100, a full store refuses ("full").
// Errors as the library: {"ok":false,"error":code,"reason"?}. Saving and
// deleting are refused while the heater is on or a profile or autotune runs.
#if defined(CPID_ENABLE_LIBRARY) && CPID_ENABLE_LIBRARY
#    include <Arduino.h>
#    include <ArduinoJson.h>

#    include <core/records.h>

#    include "controller.h"
#    include "library_storage.h"

class CroasterApp;

class RecordsPlugin {
public:
    explicit RecordsPlugin(Controller& controller)
        : controller_(controller)
        , roastFs_("/roasts")
        , roasts_(roastFs_, records::kRoastSpec)
        , beanFs_("/beans")
        , beans_(beanFs_, records::kBeanSpec) {}

    // Mounts LittleFS (shared with the library) and indexes the records.
    void begin();
    // Registers the commands. Call before app.begin().
    void attach(CroasterApp& app);

private:
    String list(const char* command, records::Store& store, LittleFsLibraryStorage& fs, const char* key);
    String get(const char* command, records::Store& store, const JsonObject& json, const char* key);
    String saveRoast(const JsonObject& json);
    String saveBean(const JsonObject& json);
    String remove(const char* command, records::Store& store, const JsonObject& json);

    Controller& controller_;
    LittleFsLibraryStorage roastFs_;
    records::Store roasts_;
    LittleFsLibraryStorage beanFs_;
    records::Store beans_;
};
#endif
