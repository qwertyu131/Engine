#pragma once
// Child processes and temp files for shaderc. Arguments are passed as UTF-16 (CreateProcessW), so
// non-ASCII paths reach the tools intact; never system()/_popen (ANSI codepage).
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eng::sc {

std::filesystem::path fromUtf8(std::string_view utf8);
std::string toUtf8(const std::filesystem::path& p);

// One command-line argument: either UTF-8 text or a path.
struct Arg {
    std::wstring value;
    Arg(const char* utf8) : Arg(std::string_view(utf8)) {}
    Arg(std::string_view utf8);
    Arg(const std::string& utf8) : Arg(std::string_view(utf8)) {}
    Arg(const std::filesystem::path& p) : value(p.wstring()) {}
};

struct ProcessResult {
    int exitCode = -1;
    std::string output; // stdout + stderr, as the tool wrote it
};

// Runs `exe args...` in `cwd`; error only if the process could not be started.
std::expected<ProcessResult, std::string> runProcess(const std::filesystem::path& exe, const std::vector<Arg>& args,
                                                     const std::filesystem::path& cwd);

// Unique directory under %TEMP%/eng-shaderc, removed on destruction.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::expected<std::vector<uint8_t>, std::string> readFile(const std::filesystem::path& p);
std::expected<void, std::string> writeFile(const std::filesystem::path& p, const void* data, size_t size);
inline std::expected<void, std::string> writeFile(const std::filesystem::path& p, std::string_view s) {
    return writeFile(p, s.data(), s.size());
}

} // namespace eng::sc
