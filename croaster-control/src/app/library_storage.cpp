#if defined(CPID_ENABLE_LIBRARY) && CPID_ENABLE_LIBRARY
#    include "library_storage.h"

#    include <LittleFS.h>

bool LittleFsLibraryStorage::mount() {
    // formatOnFail: a blank or foreign partition is formatted once; a mounted
    // file system is never formatted.
    mounted_ = LittleFS.begin(true, kMountPoint, 4, kPartition);
    if (mounted_ && !LittleFS.exists(dir_))
        mounted_ = LittleFS.mkdir(dir_);
    return mounted_;
}

size_t LittleFsLibraryStorage::usedBytes() const {
    return mounted_ ? LittleFS.usedBytes() : 0;
}

size_t LittleFsLibraryStorage::totalBytes() const {
    return mounted_ ? LittleFS.totalBytes() : 0;
}

bool LittleFsLibraryStorage::list(std::vector<std::string>& names) {
    if (!mounted_)
        return false;
    File dir = LittleFS.open(dir_);
    if (!dir || !dir.isDirectory())
        return false;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile())
        if (!f.isDirectory())
            names.emplace_back(f.name()); // the name without the directory
    return true;
}

bool LittleFsLibraryStorage::read(const std::string& name, std::string& out) {
    File f = LittleFS.open(path(name).c_str(), "r");
    if (!f)
        return false;
    const size_t n = f.size();
    if (n > 32768) // not one of ours; do not pull it into RAM
        return false;
    out.resize(n);
    const bool ok = n == 0 || f.read(reinterpret_cast<uint8_t*>(&out[0]), n) == n;
    f.close();
    return ok;
}

bool LittleFsLibraryStorage::write(const std::string& name, const std::string& data) {
    File f = LittleFS.open(path(name).c_str(), "w");
    if (!f)
        return false;
    const bool ok = f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size()) == data.size();
    f.close(); // LittleFS commits the file on close
    return ok;
}

bool LittleFsLibraryStorage::rename(const std::string& from, const std::string& to) {
    return LittleFS.rename(path(from).c_str(), path(to).c_str());
}

bool LittleFsLibraryStorage::remove(const std::string& name) {
    return LittleFS.remove(path(name).c_str());
}
#endif
