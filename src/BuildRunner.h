// BuildRunner - runs a build command in the background and collects output.
//
// The command runs through the user's login shell (so Homebrew / Xcode
// tools are on PATH even when the app is started from Finder) in its own
// process group, so Cancel stops make / cmake and every compiler they
// started. Compiler diagnostics ("file:line:col: error: ...") are parsed
// into a problem list.

#pragma once

#include <sys/types.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Problem {
    std::string file;      // absolute path
    int         line = 0;  // 1-based
    int         col  = 0;
    bool        error = true;
    std::string message;
};

class BuildRunner {
public:
    enum class State { Idle, Running, Succeeded, Failed, Cancelled };

    struct Line {
        std::string text;
        int kind = 0;   // 0 normal, 1 error, 2 warning, 3 info (our own messages)
    };

    ~BuildRunner();

    bool start(const std::string& command, const std::string& dir);
    void cancel();

    State state() const { return m_state.load(); }
    bool  running() const { return m_state.load() == State::Running; }
    double elapsed() const;
    const std::string& command() const { return m_command; }
    const std::string& dir() const { return m_dir; }

    // GUI thread: copies new lines / problems since the last call.
    bool poll(std::vector<Line>& lines, std::vector<Problem>& problems);

    void addInfo(const std::string& text);

    static bool parseDiagnostic(const std::string& line, const std::string& baseDir, Problem& out);

private:
    void run();

    std::thread        m_thread;
    std::atomic<State> m_state{State::Idle};
    std::atomic<pid_t> m_pid{-1};
    std::atomic<bool>  m_cancel{false};
    std::string        m_command, m_dir;
    double             m_startTime = 0, m_endTime = 0;

    std::mutex           m_mutex;
    std::vector<Line>    m_newLines;
    std::vector<Problem> m_newProblems;
};

// Picks a sensible build / run command for a project directory.
struct ProjectCommands {
    std::string build;
    std::string run;      // empty: find the newest executable after building
    std::string kind;     // "CMake", "Make", "Swift", "clil" ...
};
ProjectCommands detectProjectCommands(const std::string& root);

// Newest executable regular file under `dir` (a few levels deep), skipping
// CMake internals. Empty if none.
std::string findNewestExecutable(const std::string& dir);
