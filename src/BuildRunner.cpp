#include "BuildRunner.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Util.h"
#include "Wake.h"

extern char** environ;
namespace fs = std::filesystem;

namespace {

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string stripAnsi(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[')
        {
            i += 2;
            while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
            continue;
        }
        if (s[i] == '\r') continue;
        out.push_back(s[i]);
    }
    return out;
}

} // namespace

BuildRunner::~BuildRunner()
{
    cancel();
    if (m_thread.joinable()) m_thread.join();
}

bool BuildRunner::start(const std::string& command, const std::string& dir)
{
    if (running()) return false;
    if (m_thread.joinable()) m_thread.join();
    m_command = command;
    m_dir = dir;
    m_cancel = false;
    m_startTime = now();
    m_endTime = 0;
    m_state = State::Running;
    m_thread = std::thread(&BuildRunner::run, this);
    return true;
}

void BuildRunner::cancel()
{
    if (!running()) return;
    m_cancel = true;
    pid_t pid = m_pid.load();
    if (pid > 0) kill(-pid, SIGTERM);  // the whole process group
}

double BuildRunner::elapsed() const
{
    if (m_startTime == 0) return 0;
    return (m_endTime > 0 ? m_endTime : now()) - m_startTime;
}

void BuildRunner::addInfo(const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_newLines.push_back({ text, 3 });
}

bool BuildRunner::poll(std::vector<Line>& lines, std::vector<Problem>& problems)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_newLines.empty() && m_newProblems.empty()) return false;
    for (auto& l : m_newLines) lines.push_back(std::move(l));
    for (auto& p : m_newProblems) problems.push_back(std::move(p));
    m_newLines.clear();
    m_newProblems.clear();
    return true;
}

void BuildRunner::run()
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
    {
        addInfo(std::string("cannot create pipe: ") + std::strerror(errno));
        m_state = State::Failed;
        wakeUi();
        return;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    // `cd` inside the shell: posix_spawn_file_actions_addchdir is not portable.
    std::string shell = userShell();
    std::string script = "cd " + shellQuote(m_dir) + " && " + m_command;
    std::vector<std::string> argvS = { shell, "-l", "-c", script };
    if (shell == "/bin/sh") argvS = { shell, "-c", script };
    std::vector<char*> argv;
    for (auto& a : argvS) argv.push_back(a.data());
    argv.push_back(nullptr);

    // Plain output is easier to parse; tools still print colour on request.
    std::vector<std::string> envS;
    for (char** e = environ; *e; ++e)
        if (std::strncmp(*e, "CLICOLOR_FORCE=", 15) != 0) envS.push_back(*e);
    envS.push_back("TERM=dumb");
    std::vector<char*> envp;
    for (auto& e : envS) envp.push_back(e.data());
    envp.push_back(nullptr);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(pipefd[1]);
    if (rc != 0)
    {
        close(pipefd[0]);
        addInfo(std::string("cannot start shell: ") + std::strerror(rc));
        m_state = State::Failed;
        wakeUi();
        return;
    }
    m_pid = pid;

    std::string partial;
    char buf[8192];
    auto emit = [&](const std::string& raw) {
        std::string text = stripAnsi(raw);
        Problem p;
        bool isDiag = parseDiagnostic(text, m_dir, p);
        int kind = 0;
        if (isDiag) kind = p.error ? 1 : 2;
        else if (text.find("error:") != std::string::npos || text.find("Error ") != std::string::npos ||
                 text.rfind("make: ***", 0) == 0)
            kind = 1;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_newLines.push_back({ text, kind });
        if (isDiag) m_newProblems.push_back(std::move(p));
    };
    for (;;)
    {
        ssize_t n = read(pipefd[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        partial.append(buf, size_t(n));
        size_t pos;
        bool any = false;
        while ((pos = partial.find('\n')) != std::string::npos)
        {
            emit(partial.substr(0, pos));
            partial.erase(0, pos + 1);
            any = true;
        }
        if (any) wakeUi();
    }
    if (!partial.empty()) emit(partial);
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    m_pid = -1;
    m_endTime = now();
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (m_cancel) m_state = State::Cancelled;
    else m_state = (code == 0) ? State::Succeeded : State::Failed;
    char msg[128];
    std::snprintf(msg, sizeof msg, "%s in %.1fs (exit %d)",
                  m_cancel ? "Cancelled" : (code == 0 ? "Build succeeded" : "Build failed"), elapsed(), code);
    addInfo(msg);
    wakeUi();
}

// "path:line:col: error: message", "path:line: warning: message",
// "path(line): error ..." is not produced by clang / gcc so it is ignored.
bool BuildRunner::parseDiagnostic(const std::string& line, const std::string& baseDir, Problem& out)
{
    static const char* kinds[] = { ": error: ", ": fatal error: ", ": warning: ", ": note: " };
    size_t kpos = std::string::npos;
    int which = -1;
    for (int i = 0; i < 4; ++i)
    {
        size_t p = line.find(kinds[i]);
        if (p != std::string::npos && p < kpos) { kpos = p; which = i; }
    }
    if (kpos == std::string::npos || which == 3) return false;  // notes are noise in the list

    std::string loc = line.substr(0, kpos);
    // loc = path:line[:col]
    int nums[2] = { 0, 0 };
    int count = 0;
    size_t end = loc.size();
    for (int i = 0; i < 2; ++i)
    {
        size_t colon = loc.rfind(':', end - 1);
        if (colon == std::string::npos || colon + 1 >= end) break;
        std::string num = loc.substr(colon + 1, end - colon - 1);
        if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) break;
        nums[i] = std::atoi(num.c_str());
        end = colon;
        ++count;
        if (end == 0) break;
    }
    if (count == 0) return false;
    std::string file = loc.substr(0, end);
    while (!file.empty() && file.front() == ' ') file.erase(0, 1);
    if (file.empty()) return false;

    out.line = count == 2 ? nums[1] : nums[0];
    out.col = count == 2 ? nums[0] : 0;
    out.error = which <= 1;
    out.message = line.substr(kpos + std::strlen(kinds[which]));
    std::error_code ec;
    fs::path p(file);
    if (p.is_relative()) p = fs::path(baseDir) / p;
    fs::path canon = fs::weakly_canonical(p, ec);
    out.file = ec ? p.lexically_normal().string() : canon.string();
    return true;
}

// --- project detection ---------------------------------------------------------

ProjectCommands detectProjectCommands(const std::string& root)
{
    ProjectCommands pc;
    auto has = [&](const char* name) { return fs::exists(fs::path(root) / name); };
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    std::string jobs = std::to_string(cpus > 0 ? cpus : 4);
    if (has("CMakeLists.txt"))
    {
        pc.kind = "CMake";
        pc.build = "( [ -f build/CMakeCache.txt ] || cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug "
                   "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON ) && cmake --build build -j " + jobs;
    }
    else if (has("Makefile") || has("makefile") || has("GNUmakefile"))
    {
        pc.kind = "Make";
        pc.build = "make -j" + jobs;
    }
    else if (has("Package.swift"))
    {
        pc.kind = "Swift";
        pc.build = "swift build";
        pc.run = "swift run";
    }
    else if (has("meson.build"))
    {
        pc.kind = "Meson";
        pc.build = "( [ -d build ] || meson setup build ) && meson compile -C build";
    }
    return pc;
}

std::string findNewestExecutable(const std::string& dir)
{
    std::error_code ec;
    std::string best;
    std::filesystem::file_time_type bestTime{};
    auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        const auto& p = it->path();
        std::string name = p.filename().string();
        if (it->is_directory(ec))
        {
            bool bundle = name.size() > 5 && name.compare(name.size() - 5, 5, ".dSYM") == 0;
            if (name == "CMakeFiles" || name == ".git" || name == "_deps" || bundle || it.depth() >= 3)
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        struct stat st;
        if (stat(p.c_str(), &st) != 0 || !(st.st_mode & S_IXUSR)) continue;
        std::string ext = p.extension().string();
        if (ext == ".sh" || ext == ".py" || ext == ".dylib" || ext == ".so" || ext == ".a" || ext == ".o" ||
            name.find(".so.") != std::string::npos)
            continue;
        auto t = it->last_write_time(ec);
        if (best.empty() || t > bestTime)
        {
            best = p.string();
            bestTime = t;
        }
    }
    return best;
}
