#include "BuildRunner.h"

#include <cctype>
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

#include "IdeUtil.h"
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
    DiagnosticParser parser(m_dir);
    auto emit = [&](const std::string& raw) {
        std::string text = stripAnsi(raw);
        Problem p;
        bool isDiag = parser.feed(text, p);
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

namespace {

std::string resolvePath(const std::string& file, const std::string& baseDir)
{
    std::error_code ec;
    fs::path p(file);
    if (p.is_relative()) p = fs::path(baseDir) / p;
    fs::path canon = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal().string() : canon.string();
}

bool allDigits(const std::string& s)
{
    return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

// "path:line[:col]" at the start of `loc` (the whole string). False if not.
bool splitLocation(const std::string& loc, std::string& file, int& line, int& col)
{
    int nums[2] = { 0, 0 };
    int count = 0;
    size_t end = loc.size();
    for (int i = 0; i < 2 && end > 0; ++i)
    {
        size_t colon = loc.rfind(':', end - 1);
        if (colon == std::string::npos || colon + 1 >= end) break;
        std::string num = loc.substr(colon + 1, end - colon - 1);
        if (!allDigits(num)) break;
        nums[i] = std::atoi(num.c_str());
        end = colon;
        ++count;
    }
    if (count == 0) return false;
    file = loc.substr(0, end);
    while (!file.empty() && file.front() == ' ') file.erase(0, 1);
    if (file.empty() || (file.find(' ') != std::string::npos && file.find('/') == std::string::npos)) return false;
    line = count == 2 ? nums[1] : nums[0];
    col = count == 2 ? nums[0] : 0;
    return line > 0;
}

bool looksLikeSource(const std::string& file)
{
    size_t dot = file.find_last_of('.');
    size_t slash = file.find_last_of('/');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash) && dot + 1 < file.size() &&
           file.size() - dot <= 6;
}

} // namespace

// Single-line formats:
//   clang / gcc / swiftc / javac / kotlinc / go vet:  path:line[:col]: error|warning: message
//   go build / ruby -c:                               path:line[:col]: message
//   tsc / C# / MSBuild:                               path(line,col): error CODE: message
//   php -l:                                           ... message in path on line N
//   bash -n:                                          path: line N: message
bool BuildRunner::parseDiagnostic(const std::string& line, const std::string& baseDir, Problem& out)
{
    // tsc / dotnet: "src/a.ts(3,5): error TS2304: Cannot find name 'x'."
    {
        size_t paren = line.find('(');
        size_t close = paren == std::string::npos ? paren : line.find("): ", paren);
        if (close != std::string::npos && paren > 0 && looksLikeSource(line.substr(0, paren)))
        {
            std::string nums = line.substr(paren + 1, close - paren - 1);
            size_t comma = nums.find(',');
            std::string rest = line.substr(close + 3);
            bool err = rest.rfind("error", 0) == 0, warn = rest.rfind("warning", 0) == 0;
            if ((err || warn) && allDigits(nums.substr(0, comma)))
            {
                out.file = resolvePath(line.substr(0, paren), baseDir);
                out.line = std::atoi(nums.c_str());
                out.col = comma == std::string::npos ? 0 : std::atoi(nums.c_str() + comma + 1);
                out.error = err;
                size_t colon = rest.find(": ");
                out.message = colon == std::string::npos ? rest : rest.substr(colon + 2);
                return true;
            }
        }
    }
    // php: "PHP Parse error:  syntax error, unexpected ... in /p/a.php on line 3"
    {
        size_t on = line.rfind(" on line ");
        size_t in = on == std::string::npos ? on : line.rfind(" in ", on);
        if (in != std::string::npos && allDigits(line.substr(on + 9)) &&
            (line.find("error") != std::string::npos || line.find("Warning") != std::string::npos))
        {
            out.file = resolvePath(line.substr(in + 4, on - in - 4), baseDir);
            out.line = std::atoi(line.c_str() + on + 9);
            out.col = 0;
            out.error = line.find("Warning") == std::string::npos;
            std::string msg = line.substr(0, in);
            size_t colon = msg.find(":  ");
            if (colon == std::string::npos) colon = msg.find(": ");
            out.message = colon == std::string::npos ? msg : msg.substr(colon + (msg[colon + 2] == ' ' ? 3 : 2));
            return true;
        }
    }
    // bash -n: "script.sh: line 3: syntax error near unexpected token `fi'"
    {
        size_t l = line.find(": line ");
        if (l != std::string::npos && l > 0 && looksLikeSource(line.substr(0, l)))
        {
            size_t colon = line.find(": ", l + 7);
            std::string num = line.substr(l + 7, colon == std::string::npos ? std::string::npos : colon - l - 7);
            if (allDigits(num))
            {
                out.file = resolvePath(line.substr(0, l), baseDir);
                out.line = std::atoi(num.c_str());
                out.col = 0;
                out.error = true;
                out.message = colon == std::string::npos ? "" : line.substr(colon + 2);
                return true;
            }
        }
    }

    // gcc-style with a severity word.
    static const char* kinds[] = { ": error: ", ": fatal error: ", ": warning: ", ": note: ", ": Error: " };
    size_t kpos = std::string::npos;
    int which = -1;
    for (int i = 0; i < 5; ++i)
    {
        size_t p = line.find(kinds[i]);
        if (p != std::string::npos && p < kpos) { kpos = p; which = i; }
    }
    if (kpos != std::string::npos)
    {
        if (which == 3) return false;  // notes are noise in the list
        std::string file;
        int ln = 0, col = 0;
        if (!splitLocation(line.substr(0, kpos), file, ln, col)) return false;
        out.file = resolvePath(file, baseDir);
        out.line = ln;
        out.col = col;
        out.error = which != 2;
        out.message = line.substr(kpos + std::strlen(kinds[which]));
        return true;
    }

    // Perl: "syntax error at /p/a.pl line 1, near ..." / "Died at a.pl line 3."
    {
        size_t at = line.find(" at ");
        size_t ln = at == std::string::npos ? at : line.find(" line ", at + 4);
        if (ln != std::string::npos && line.find(' ', at + 4) == ln)
        {
            std::string file = line.substr(at + 4, ln - at - 4);
            size_t e = ln + 6;
            while (e < line.size() && std::isdigit((unsigned char)line[e])) ++e;
            std::error_code ec;
            if (e > ln + 6 && looksLikeSource(file) && fs::is_regular_file(resolvePath(file, baseDir), ec))
            {
                out.file = resolvePath(file, baseDir);
                out.line = std::atoi(line.c_str() + ln + 6);
                out.col = 0;
                out.error = true;
                out.message = line.substr(0, at);
                return true;
            }
        }
    }

    // Go / Ruby / Lua: "path:line[:col]: message" for a source file that exists,
    // possibly after the tool's name ("ruby: /p/a.rb:2: syntax error ...").
    if (line.empty() || line[0] == ' ' || line[0] == '\t') return false;
    {
        size_t colon = line.find(": ");
        if (colon != std::string::npos && colon < 16 && line.find_first_of("/.", 0) > colon &&
            colon + 2 < line.size() && (line[colon + 2] == '/' || line[colon + 2] == '.'))
            return parseDiagnostic(line.substr(colon + 2), baseDir, out);
    }
    size_t sep = std::string::npos;
    for (size_t i = 0; i + 1 < line.size(); ++i)
    {
        if (line[i] == ':' && line[i + 1] == ' ')
        {
            sep = i;
            break;
        }
    }
    if (sep == std::string::npos) return false;
    std::string file;
    int ln = 0, col = 0;
    if (!splitLocation(line.substr(0, sep), file, ln, col) || !looksLikeSource(file)) return false;
    std::string full = resolvePath(file, baseDir);
    std::error_code ec;
    if (!fs::is_regular_file(full, ec)) return false;
    out.file = full;
    out.line = ln;
    out.col = col;
    std::string msg = line.substr(sep + 2);
    out.error = msg.find("warning") != 0;
    out.message = msg;
    return true;
}

bool DiagnosticParser::feed(const std::string& line, Problem& out)
{
    // Rust: "error[E0425]: cannot find value `x`" then "  --> src/main.rs:3:5".
    if (line.rfind("error", 0) == 0 || line.rfind("warning", 0) == 0)
    {
        size_t colon = line.find(": ");
        size_t w = line[0] == 'e' ? 5 : 7;  // after "error" / "warning"
        bool head = colon != std::string::npos && line.size() > w && (line[w] == '[' || line[w] == ':');
        if (head && line.rfind("warning: unused manifest", 0) != 0)
        {
            m_rustMsg = line.substr(colon + 2);
            m_rustError = line[0] == 'e';
        }
    }
    size_t arrow = line.find("--> ");
    if (arrow != std::string::npos && !m_rustMsg.empty() && line.find_first_not_of(' ') == arrow)
    {
        std::string file;
        int ln = 0, col = 0;
        if (splitLocation(line.substr(arrow + 4), file, ln, col))
        {
            out.file = resolvePath(file, m_base);
            out.line = ln;
            out.col = col;
            out.error = m_rustError;
            out.message = m_rustMsg;
            m_rustMsg.clear();
            return true;
        }
    }

    // Python: '  File "/p/a.py", line 3, in f' ... 'NameError: name x is not defined'.
    {
        size_t f = line.find("File \"");
        size_t q = f == std::string::npos ? f : line.find("\", line ", f + 6);
        if (q != std::string::npos)
        {
            std::string file = line.substr(f + 6, q - f - 6);
            if (file.empty() || file[0] != '<')  // skip "<frozen ...>" frames
            {
                m_pendFile = resolvePath(file, m_base);
                m_pendLine = std::atoi(line.c_str() + q + 8);
            }
            return false;
        }
    }
    // Node: "/p/a.js:3" alone on a line, the error follows a few lines later.
    if (!line.empty() && line[0] == '/' && line.find(' ') == std::string::npos)
    {
        std::string file;
        int ln = 0, col = 0;
        if (splitLocation(line, file, ln, col) && looksLikeSource(file))
        {
            m_pendFile = resolvePath(file, m_base);
            m_pendLine = ln;
            return false;
        }
    }
    if (!m_pendFile.empty() && !line.empty() && line[0] != ' ')
    {
        // "SyntaxError: invalid syntax", "ZeroDivisionError: division by zero", "TypeError: ..."
        size_t colon = line.find(':');
        std::string name = line.substr(0, colon);
        bool errorName = colon != std::string::npos && name.find(' ') == std::string::npos &&
                         (name.size() > 5 && (name.compare(name.size() - 5, 5, "Error") == 0 ||
                                              name.find("Exception") != std::string::npos ||
                                              name.find("Warning") != std::string::npos));
        if (errorName || (colon == std::string::npos && name.find("Error") != std::string::npos))
        {
            out.file = m_pendFile;
            out.line = m_pendLine;
            out.col = 0;
            out.error = name.find("Warning") == std::string::npos;
            out.message = line;
            m_pendFile.clear();
            return true;
        }
    }
    return BuildRunner::parseDiagnostic(line, m_base, out);
}

// --- project detection ---------------------------------------------------------

ProjectCommands detectProjectCommands(const std::string& root)
{
    ProjectCommands pc;
    auto has = [&](const char* name) { return fs::exists(fs::path(root) / name); };
    auto hasExt = [&](const char* ext) {
        std::error_code ec;
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == ext) return true;
        return false;
    };
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    std::string jobs = std::to_string(cpus > 0 ? cpus : 4);
    if (has("CMakeLists.txt"))
    {
        pc.kind = "CMake";
        pc.build = "( [ -f build/CMakeCache.txt ] || cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug "
                   "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON ) && cmake --build build -j " + jobs;
    }
    else if (has("Cargo.toml"))
    {
        pc.kind = "Cargo";
        pc.build = "cargo build --message-format short";
        pc.run = "cargo run";
    }
    else if (has("go.mod"))
    {
        pc.kind = "Go";
        pc.build = "go build ./... && go vet ./...";
        pc.run = "go run .";
    }
    else if (has("Package.swift"))
    {
        pc.kind = "Swift";
        pc.build = "swift build";
        pc.run = "swift run";
    }
    else if (has("build.zig"))
    {
        pc.kind = "Zig";
        pc.build = "zig build";
        pc.run = "zig build run";
    }
    else if (has("package.json"))
    {
        // npm scripts: build / start / dev when the project defines them.
        std::string pkg;
        readTextFile(root + "/package.json", pkg);
        auto script = [&](const char* name) { return pkg.find(std::string("\"") + name + "\"") != std::string::npos; };
        std::string pm = has("pnpm-lock.yaml") ? "pnpm" : has("yarn.lock") ? "yarn" : has("bun.lockb") || has("bun.lock") ? "bun" : "npm";
        std::string runp = pm == "npm" ? "npm run " : pm + " run ";
        pc.kind = "Node (" + pm + ")";
        pc.build = (has("node_modules") ? "" : pm + " install && ") +
                   (script("build") ? runp + "build" : script("typecheck") ? runp + "typecheck"
                    : has("tsconfig.json") ? "npx --yes tsc --noEmit --pretty false" : "true");
        pc.run = script("dev") ? runp + "dev" : script("start") ? runp + "start" : "";
    }
    else if (has("deno.json") || has("deno.jsonc"))
    {
        pc.kind = "Deno";
        pc.build = "deno check .";
        pc.run = "deno task start";
    }
    else if (has("pom.xml"))
    {
        pc.kind = "Maven";
        pc.build = (has("mvnw") ? "./mvnw" : "mvn") + std::string(" -q compile");
        pc.run = (has("mvnw") ? "./mvnw" : "mvn") + std::string(" -q exec:java");
    }
    else if (has("build.gradle") || has("build.gradle.kts"))
    {
        std::string g = has("gradlew") ? "./gradlew" : "gradle";
        pc.kind = "Gradle";
        pc.build = g + " build -q";
        pc.run = g + " run -q";
    }
    else if (hasExt(".sln") || hasExt(".csproj") || hasExt(".fsproj"))
    {
        pc.kind = ".NET";
        pc.build = "dotnet build -nologo -clp:NoSummary";
        pc.run = "dotnet run";
    }
    else if (has("mix.exs"))
    {
        pc.kind = "Elixir (mix)";
        pc.build = "mix compile";
        pc.run = "mix run";
    }
    else if (has("stack.yaml") || hasExt(".cabal"))
    {
        pc.kind = "Haskell";
        pc.build = has("stack.yaml") ? "stack build" : "cabal build";
        pc.run = has("stack.yaml") ? "stack run" : "cabal run";
    }
    else if (has("pubspec.yaml"))
    {
        pc.kind = "Dart";
        pc.build = "dart analyze";
        pc.run = "dart run";
    }
    else if (has("Makefile") || has("makefile") || has("GNUmakefile"))
    {
        pc.kind = "Make";
        pc.build = "make -j" + jobs;
    }
    else if (has("meson.build"))
    {
        pc.kind = "Meson";
        pc.build = "( [ -d build ] || meson setup build ) && meson compile -C build";
    }
    else if (has("pyproject.toml") || has("requirements.txt") || has("setup.py"))
    {
        // Python projects have no build step; Run uses the open file.
        pc.kind = "Python";
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
