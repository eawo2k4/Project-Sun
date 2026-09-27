#pragma once

// Files for a Win16 task: a read-only view of the program's own directory,
// shared by KERNEL (OpenFile, _lopen, _lread, ...) and INT 21h (the C
// runtime's open/read/seek), plus INI files (GetProfileString and friends).
//
// Path mapping (Win16 paths are ANSI, 8.3, with drive letters):
//   * relative paths               -> the program's directory
//   * X:\WINDOWS\..., X:\WINDOWS\SYSTEM\...  -> the program's directory (where
//     a game's INI and support files are)
//   * other absolute paths         -> as on the host, but only inside the
//     program's directory
// Anything else is refused, and so is writing, for now: a game gets "access
// denied" instead of modifying files (saving comes with a per-game save
// directory later). INI writes are kept in memory for the run.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace retro::win16 {

namespace dos {
constexpr uint16_t FileNotFound = 2, PathNotFound = 3, TooManyFiles = 4, AccessDenied = 5,
                   InvalidHandle = 6;
}  // namespace dos

class FileSystem {
public:
    // The program's .exe: its directory is the root of everything the task sees.
    void SetProgram(const std::filesystem::path& exe);
    const std::filesystem::path& Root() const { return root_; }
    // The program's path as the task sees it (GetModuleFileName, argv[0]).
    std::string ProgramPath() const { return programPath_; }

    // Maps a Win16 path to a host path; false with a reason if not allowed.
    bool Resolve(const std::string& path, std::filesystem::path& out, std::string& why) const;

    // Opens for reading (mode bits 0-1 = 0). Returns a DOS handle (>= 5), or
    // -1 with a DOS error code.
    int Open(const std::string& path, uint16_t mode, uint16_t& error);
    // A read-only handle on bytes in memory (AccessResource: a resource, as if
    // read from the module file). -1 if no handles are left.
    int OpenMemory(std::string bytes);
    bool Close(int handle);
    bool IsOpen(int handle) const { return files_.count(handle) != 0; }
    // Bytes read (0 at end of file), or -1 for a bad handle.
    int32_t Read(int handle, uint8_t* dst, uint32_t bytes);
    // New position, or -1 (bad handle, or before the start). origin: 0 set, 1 current, 2 end.
    int32_t Seek(int handle, int32_t offset, int origin);
    bool Exists(const std::string& path) const;

private:
    std::filesystem::path root_ = std::filesystem::current_path();
    std::string programPath_ = "C:\\WINDOWS\\PROGRAM.EXE";
    int FreeHandle() const;  // -1 if none

    std::map<int, std::unique_ptr<std::istream>> files_;
};

// WIN.INI and private INI files, read from disk through the FileSystem, with
// writes kept in memory for the rest of the run.
class Profiles {
public:
    explicit Profiles(const FileSystem& files) : files_(files) {}

    // file empty = WIN.INI. key empty = the section's keys (NUL-separated).
    // section empty = the file's sections. False if not found.
    bool Get(const std::string& file, const std::string& section, const std::string& key,
             std::string& out);
    // value null = delete the key; key null = delete the section.
    void Write(const std::string& file, const std::string& section, const std::string* key,
               const std::string* value);

private:
    struct Section {
        std::string name;
        std::vector<std::pair<std::string, std::string>> entries;
    };
    std::vector<Section>& Load(const std::string& file);

    const FileSystem& files_;
    std::map<std::string, std::vector<Section>> cache_;  // by upper-case file name
};

}  // namespace retro::win16
