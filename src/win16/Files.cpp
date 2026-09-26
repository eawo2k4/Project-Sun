#include "win16/Files.h"

#include <algorithm>
#include <cctype>

namespace retro::win16 {
namespace {

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool EqualNoCase(const std::string& a, const std::string& b) { return Upper(a) == Upper(b); }

std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// `path` inside (or equal to) `root`, compared case-insensitively like the host.
bool Inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p) {
        if (r->empty()) continue;  // trailing separator
        if (p == path.end() || !EqualNoCase(p->string(), r->string())) return false;
    }
    return true;
}

}  // namespace

void FileSystem::SetProgram(const std::filesystem::path& exe) {
    const std::filesystem::path full = std::filesystem::absolute(exe).lexically_normal();
    root_ = full.parent_path();
    try {
        programPath_ = Upper(full.string());  // ANSI, as a Win16 program expects
    } catch (const std::exception&) {        // not representable in the ANSI code page
        programPath_ = "C:\\WINDOWS\\PROGRAM.EXE";
    }
}

bool FileSystem::Resolve(const std::string& path, std::filesystem::path& out, std::string& why) const {
    std::string p = path;
    std::replace(p.begin(), p.end(), '/', '\\');
    if (p.empty()) {
        why = "empty path";
        return false;
    }
    std::string rest = p;
    bool absolute = false;
    if (p.size() >= 2 && p[1] == ':') {
        rest = p.substr(2);
        absolute = !rest.empty() && rest[0] == '\\';
    } else if (p[0] == '\\') {
        absolute = true;
    }
    std::filesystem::path candidate;
    const std::string upper = Upper(rest);
    if (absolute && (upper.rfind("\\WINDOWS\\SYSTEM\\", 0) == 0)) {
        candidate = root_ / rest.substr(16);
    } else if (absolute && upper.rfind("\\WINDOWS\\", 0) == 0) {
        candidate = root_ / rest.substr(9);
    } else if (absolute) {
        candidate = std::filesystem::path(p);  // as on the host
    } else {
        candidate = root_ / rest;  // relative (a drive-relative "C:FILE" too)
    }
    candidate = candidate.lexically_normal();
    if (!Inside(candidate, root_)) {
        why = "outside the program's directory";
        return false;
    }
    // Links and junctions must not lead out either.
    std::error_code ec, ecRoot;
    const std::filesystem::path real = std::filesystem::weakly_canonical(candidate, ec);
    const std::filesystem::path realRoot = std::filesystem::weakly_canonical(root_, ecRoot);
    if (!ec && !ecRoot && !Inside(real, realRoot)) {
        why = "links outside the program's directory";
        return false;
    }
    out = candidate;
    return true;
}

int FileSystem::Open(const std::string& path, uint16_t mode, uint16_t& error) {
    if ((mode & 3) != 0) {  // write or read/write
        error = dos::AccessDenied;
        return -1;
    }
    std::filesystem::path host;
    std::string why;
    if (!Resolve(path, host, why)) {
        error = dos::PathNotFound;
        return -1;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(host, ec)) {
        error = dos::FileNotFound;
        return -1;
    }
    auto file = std::make_unique<std::ifstream>(host, std::ios::binary);
    if (!*file) {
        error = dos::AccessDenied;
        return -1;
    }
    int handle = 5;  // 0-4: stdin, stdout, stderr, aux, prn
    while (files_.count(handle)) ++handle;
    if (handle > 255) {
        error = dos::TooManyFiles;
        return -1;
    }
    files_[handle] = std::move(file);
    return handle;
}

bool FileSystem::Close(int handle) { return files_.erase(handle) != 0; }

int32_t FileSystem::Read(int handle, uint8_t* dst, uint32_t bytes) {
    const auto it = files_.find(handle);
    if (it == files_.end()) return -1;
    std::ifstream& f = *it->second;
    f.clear();
    f.read(reinterpret_cast<char*>(dst), std::streamsize(bytes));
    const int32_t got = int32_t(f.gcount());
    f.clear();  // end of file isn't an error
    return got;
}

int32_t FileSystem::Seek(int handle, int32_t offset, int origin) {
    const auto it = files_.find(handle);
    if (it == files_.end() || origin < 0 || origin > 2) return -1;
    std::ifstream& f = *it->second;
    f.clear();
    const std::ios::seekdir dir = origin == 0 ? std::ios::beg : origin == 1 ? std::ios::cur : std::ios::end;
    const std::streamoff before = f.tellg();
    f.seekg(offset, dir);
    if (!f || f.tellg() < 0) {
        f.clear();
        f.seekg(before);
        return -1;
    }
    return int32_t(f.tellg());
}

bool FileSystem::Exists(const std::string& path) const {
    std::filesystem::path host;
    std::string why;
    std::error_code ec;
    return Resolve(path, host, why) && std::filesystem::is_regular_file(host, ec);
}

// --- Profiles ---------------------------------------------------------------------------

std::vector<Profiles::Section>& Profiles::Load(const std::string& file) {
    const std::string name = file.empty() ? "WIN.INI" : file;
    const std::string key = Upper(name);
    const auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    std::vector<Section>& sections = cache_[key];

    // A bare name is in the Windows directory, which maps to the program's.
    std::filesystem::path host;
    std::string why;
    if (!files_.Resolve(name, host, why)) return sections;
    std::ifstream in(host);
    std::string line;
    Section* current = nullptr;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            sections.push_back({Trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)), {}});
            current = &sections.back();
            continue;
        }
        const size_t eq = line.find('=');
        if (!current || eq == std::string::npos) continue;
        current->entries.emplace_back(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
    }
    return sections;
}

bool Profiles::Get(const std::string& file, const std::string& section, const std::string& key,
                   std::string& out) {
    std::vector<Section>& sections = Load(file);
    out.clear();
    if (section.empty()) {  // all section names, NUL-separated
        for (const Section& s : sections) out += s.name + '\0';
        return !sections.empty();
    }
    for (const Section& s : sections) {
        if (!EqualNoCase(s.name, section)) continue;
        if (key.empty()) {  // all keys, NUL-separated
            for (const auto& [k, v] : s.entries) out += k + '\0';
            return true;
        }
        for (const auto& [k, v] : s.entries) {
            if (EqualNoCase(k, key)) {
                out = v;
                return true;
            }
        }
    }
    return false;
}

void Profiles::Write(const std::string& file, const std::string& section, const std::string* key,
                     const std::string* value) {
    std::vector<Section>& sections = Load(file);
    auto s = std::find_if(sections.begin(), sections.end(),
                          [&](const Section& x) { return EqualNoCase(x.name, section); });
    if (!key) {  // delete the section
        if (s != sections.end()) sections.erase(s);
        return;
    }
    if (s == sections.end()) {
        if (!value) return;
        sections.push_back({section, {}});
        s = sections.end() - 1;
    }
    auto e = std::find_if(s->entries.begin(), s->entries.end(),
                          [&](const auto& kv) { return EqualNoCase(kv.first, *key); });
    if (!value) {
        if (e != s->entries.end()) s->entries.erase(e);
    } else if (e != s->entries.end()) {
        e->second = *value;
    } else {
        s->entries.emplace_back(*key, *value);
    }
}

}  // namespace retro::win16
