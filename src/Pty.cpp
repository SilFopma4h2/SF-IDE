#include "Pty.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

Pty::~Pty()
{
    stop();
}

bool Pty::start(const std::vector<std::string>& argv, const std::string& cwd,
                int cols, int rows, const std::vector<std::string>& extraEnv)
{
    stop();
    if (argv.empty()) return false;

    struct winsize ws = {};
    ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
    ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
    m_cols = ws.ws_col;
    m_rows = ws.ws_row;

    // Build argv/env before forking: only async-signal-safe calls after fork.
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    int fd = -1;
    pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid < 0) return false;

    if (pid == 0)
    {
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) { /* stay where we are */ }
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "SF-IDE", 1);
        for (const auto& e : extraEnv) putenv(const_cast<char*>(e.c_str()));
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        execvp(args[0], args.data());
        std::fprintf(stderr, "sf-ide: cannot run %s: %s\r\n", args[0], std::strerror(errno));
        _exit(127);
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);
    m_fd = fd;
    m_pid = pid;
    m_exitStatus = 0;
    m_running = true;
    m_reader = std::thread(&Pty::readerLoop, this);
    return true;
}

void Pty::readerLoop()
{
    char buf[16384];
    for (;;)
    {
        struct pollfd p = { m_fd, POLLIN, 0 };
        int r = poll(&p, 1, 250);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        if (r == 0)
        {
            // The child may have exited without closing the pty (grandchildren
            // holding it open). Check without blocking.
            int st = 0;
            if (waitpid(m_pid, &st, WNOHANG) == m_pid)
            {
                m_exitStatus = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
                m_pid = -1;
                break;
            }
            continue;
        }
        ssize_t n = ::read(m_fd, buf, sizeof buf);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) break;  // EIO on Linux, 0 on macOS: the child closed the pty
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.append(buf, static_cast<size_t>(n));
        }
        if (onActivity) onActivity();
    }
    if (m_pid > 0)
    {
        int st = 0;
        if (waitpid(m_pid, &st, 0) == m_pid)
            m_exitStatus = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        m_pid = -1;
    }
    m_running = false;
    if (onActivity) onActivity();
}

void Pty::stop()
{
    if (m_pid > 0)
    {
        kill(m_pid, SIGHUP);
        for (int i = 0; i < 20 && m_running; ++i) usleep(10000);
        if (m_running && m_pid > 0) kill(m_pid, SIGKILL);
    }
    if (m_reader.joinable()) m_reader.join();
    if (m_fd >= 0) { close(m_fd); m_fd = -1; }
    m_running = false;
}

void Pty::write(const std::string& bytes)
{
    if (m_fd < 0 || !m_running) return;
    size_t off = 0;
    while (off < bytes.size())
    {
        ssize_t n = ::write(m_fd, bytes.data() + off, bytes.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

void Pty::resize(int cols, int rows)
{
    if (m_fd < 0 || (cols == m_cols && rows == m_rows) || cols < 2 || rows < 1) return;
    m_cols = cols;
    m_rows = rows;
    struct winsize ws = {};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ioctl(m_fd, TIOCSWINSZ, &ws);  // the kernel sends SIGWINCH to the child
}

bool Pty::take(std::string& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending.empty()) return false;
    out.swap(m_pending);
    m_pending.clear();
    return true;
}
