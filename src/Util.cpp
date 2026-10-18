#include "Util.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

std::string shellQuote(const std::string& s)
{
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

std::string homeDir()
{
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/tmp";
}

std::string configDir()
{
#if defined(__APPLE__)
    std::string dir = homeDir() + "/Library/Application Support/SF-IDE";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string dir = (xdg && *xdg ? std::string(xdg) : homeDir() + "/.config") + "/sf-ide";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string executableDir()
{
    char buf[PATH_MAX] = {};
#if defined(__APPLE__)
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return ".";
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
#endif
    std::error_code ec;
    fs::path p = fs::canonical(buf, ec);
    return (ec ? fs::path(buf) : p).parent_path().string();
}

std::string findProgram(const std::string& name, const std::vector<std::string>& extraDirs)
{
    auto ok = [](const std::string& p) { return access(p.c_str(), X_OK) == 0 && !fs::is_directory(p); };
    if (name.find('/') != std::string::npos) return ok(name) ? name : std::string();
    for (const auto& d : extraDirs)
        if (ok(d + "/" + name)) return d + "/" + name;
    if (const char* path = std::getenv("PATH"))
    {
        std::string p = path;
        size_t start = 0;
        while (start <= p.size())
        {
            size_t c = p.find(':', start);
            if (c == std::string::npos) c = p.size();
            std::string dir = p.substr(start, c - start);
            if (!dir.empty() && ok(dir + "/" + name)) return dir + "/" + name;
            start = c + 1;
        }
    }
    for (const char* d : { "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin" })
        if (ok(std::string(d) + "/" + name)) return std::string(d) + "/" + name;
    std::string local = homeDir() + "/.local/bin/" + name;
    if (ok(local)) return local;
    return {};
}

std::string relativePath(const std::string& path, const std::string& base)
{
    if (base.empty()) return path;
    std::error_code ec;
    fs::path rel = fs::relative(path, base, ec);
    std::string r = rel.string();
    if (ec || r.empty() || r.rfind("..", 0) == 0) return path;
    return r;
}

std::string userShell()
{
    const char* sh = std::getenv("SHELL");
    if (sh && *sh && access(sh, X_OK) == 0) return sh;
    if (passwd* pw = getpwuid(getuid()); pw && pw->pw_shell && access(pw->pw_shell, X_OK) == 0) return pw->pw_shell;
    return "/bin/sh";
}

bool readTextFile(const std::string& path, std::string& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool writeTextFile(const std::string& path, const std::string& data)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return (std::fclose(f) == 0) && ok;
}

std::string runCapture(const std::string& command, int* exitCode, size_t limit)
{
    std::string out;
    FILE* p = popen(command.c_str(), "r");
    if (!p)
    {
        if (exitCode) *exitCode = -1;
        return out;
    }
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0)
        if (out.size() < limit) out.append(buf, std::min(n, limit - out.size()));
    int st = pclose(p);
    if (exitCode) *exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return out;
}
