#pragma once
// The Drop event's name on the page. The event code stays DROP; a drum that
// cools in place can show "Cooldown" instead. Only the label changes.
// Saved in NVS (namespace "control", key "dropCd") by ConfigStore, not in
// config.h. Default is Drop.
#include <cstring>

namespace droplabel {

// True for "Drop" or "Cooldown". Anything else is refused.
inline bool parse(const char* s, bool& cooldown) {
    if (!s)
        return false;
    if (!std::strcmp(s, "Drop")) {
        cooldown = false;
        return true;
    }
    if (!std::strcmp(s, "Cooldown")) {
        cooldown = true;
        return true;
    }
    return false;
}

inline const char* name(bool cooldown) {
    return cooldown ? "Cooldown" : "Drop";
}

} // namespace droplabel
