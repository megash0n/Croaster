#pragma once
// LittleFS backend for the profile library (core/profile_library.h). Mounts the
// file system on the "spiffs" partition of the stock 16 MB table (3.4 MB, unused
// by Croaster), so no partition change and no USB flash is needed; OTA writes
// only the app slots and keeps it. Profiles are in kDir; saved roasts use
// their own directory on the same file system. Built when CPID_ENABLE_LIBRARY is 1.
#if defined(CPID_ENABLE_LIBRARY) && CPID_ENABLE_LIBRARY
#    include <core/profile_library.h>

class LittleFsLibraryStorage : public proflib::LibraryStorage {
public:
    static constexpr const char* kPartition = "spiffs";
    static constexpr const char* kMountPoint = "/littlefs";
    static constexpr const char* kDir = "/profiles";

    explicit LittleFsLibraryStorage(const char* dir = kDir)
        : dir_(dir) {}

    // Mounts (formatting only when the mount fails, e.g. a blank partition) and
    // creates the directory. Mounting again is harmless.
    bool mount();
    bool mounted() const {
        return mounted_;
    }
    size_t usedBytes() const;
    size_t totalBytes() const;

    bool list(std::vector<std::string>& names) override;
    bool read(const std::string& name, std::string& out) override;
    bool write(const std::string& name, const std::string& data) override;
    bool rename(const std::string& from, const std::string& to) override;
    bool remove(const std::string& name) override;

private:
    std::string path(const std::string& name) const {
        return std::string(dir_) + "/" + name;
    }
    const char* dir_;
    bool mounted_ = false;
};
#endif
