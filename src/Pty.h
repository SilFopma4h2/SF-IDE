// Pty - runs a child process inside a pseudo terminal (forkpty).
//
// Used for the integrated shell, for clil and for lldb: they all expect a
// real terminal (line editing, colours, Ctrl-C). A reader thread collects
// output; the GUI thread drains it once per frame with take().

#pragma once

#include <sys/types.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Pty {
public:
    Pty() = default;
    ~Pty();
    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    // Start argv[0] with the given arguments in `cwd`. `extraEnv` holds
    // "NAME=value" entries added to the child's environment.
    bool start(const std::vector<std::string>& argv, const std::string& cwd,
               int cols, int rows, const std::vector<std::string>& extraEnv = {});
    void stop();  // SIGHUP, then SIGKILL if it does not exit

    bool running() const { return m_running.load(); }
    int  exitStatus() const { return m_exitStatus.load(); }

    void write(const std::string& bytes);
    void resize(int cols, int rows);

    // Moves everything read since the last call into `out`. Returns true if
    // there was new data.
    bool take(std::string& out);

    // Called from the reader thread whenever output arrives (used to wake
    // the event loop).
    std::function<void()> onActivity;

private:
    void readerLoop();

    int               m_fd  = -1;
    std::atomic<pid_t> m_pid{-1};
    std::thread       m_reader;
    std::mutex        m_mutex;
    std::string       m_pending;
    std::atomic<bool> m_running{false};
    std::atomic<int>  m_exitStatus{0};
    int               m_cols = 0, m_rows = 0;
};
