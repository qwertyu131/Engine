#include "Process.h"

#include <atomic>
#include <fstream>
#include <stdexcept>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace eng::sc {

fs::path fromUtf8(std::string_view utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string toUtf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

Arg::Arg(std::string_view utf8) : value(fromUtf8(utf8).wstring()) {}

namespace {

// Quote one argument per the MSVC CRT rules (CommandLineToArgvW compatible).
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
        } else {
            cmd.append(backslashes, L'\\');
        }
        cmd += arg[i];
    }
    cmd += L'"';
}

std::string lastErrorText(const char* what) {
    return std::string(what) + " failed (Win32 error " + std::to_string(GetLastError()) + ")";
}

} // namespace

std::expected<ProcessResult, std::string> runProcess(const fs::path& exe, const std::vector<Arg>& args, const fs::path& cwd) {
    std::wstring cmd;
    appendQuoted(cmd, exe.wstring());
    for (const Arg& a : args) {
        cmd += L' ';
        appendQuoted(cmd, a.value);
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return std::unexpected(lastErrorText("CreatePipe"));
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    const std::wstring cwdW = cwd.wstring();
    const BOOL ok = CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   cwdW.empty() ? nullptr : cwdW.c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return std::unexpected(lastErrorText("CreateProcessW") + " for '" + toUtf8(exe) + "'");
    }

    ProcessResult result;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
        result.output.append(buf, n);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    result.exitCode = static_cast<int>(code);
    return result;
}

TempDir::TempDir() {
    static std::atomic<unsigned> counter{0};
    const fs::path base = fs::temp_directory_path() / "eng-shaderc";
    for (int attempt = 0;; ++attempt) {
        if (attempt == 1000) {
            throw std::runtime_error("shaderc: cannot create a temp directory under '" + toUtf8(base) + "'");
        }
        fs::path candidate = base / (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + "-" +
                                     std::to_string(GetTickCount64()));
        std::error_code ec;
        fs::create_directories(base, ec);
        if (fs::create_directory(candidate, ec)) {
            path_ = std::move(candidate);
            return;
        }
    }
}

TempDir::~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
}

std::expected<std::vector<uint8_t>, std::string> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::unexpected("cannot read '" + toUtf8(p) + "'");
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return data;
}

std::expected<void, std::string> writeFile(const fs::path& p, const void* data, size_t size) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        return std::unexpected("cannot write '" + toUtf8(p) + "'");
    }
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!f) {
        return std::unexpected("write failed for '" + toUtf8(p) + "'");
    }
    return {};
}

} // namespace eng::sc
