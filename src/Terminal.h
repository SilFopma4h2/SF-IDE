// Terminal - an integrated, Windows-native shell panel for SF-IDE.
//
// Spawns PowerShell with redirected stdin/stdout/stderr, captures output
// line-by-line, and forwards it to the GUI through a thread-safe queue.
// Keyboard input is typed into an ImGui input box and sent to the shell on
// Enter. Output is shown in a scrollable text region.
//
// No WSL, no MSYS, no MinGW: the child process is Windows
// powershell.exe (or whatever the user overrides it to).

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Terminal {
public:
    Terminal();
    ~Terminal();

    // Start the shell. If a session is already active it is stopped first.
    // `workDir` becomes the shell's initial working directory.
    void start(const std::string& workDir = {});

    // Terminate the running shell (if any). Safe to call when idle.
    void stop();

    bool running() const { return m_running.load(); }

    // Append a line of input to be sent to the shell on the next update.
    // The newline is added automatically.
    void submitLine(const std::string& line);

    // Render the ImGui panel. The panel fills the parent window's content
    // region; callers should put it inside their own window.
    void render();

private:
    void readerThread(HANDLE pipe, bool isError);
    void waiterThread();
    void pushLine(const std::string& line, bool isError);
    void ensureStopped();

    // Process / pipes
    HANDLE m_hProcess    = nullptr;
    HANDLE m_hStdinWrite = nullptr;
    HANDLE m_hStdoutRead = nullptr;
    HANDLE m_hStderrRead = nullptr;
    // One reader thread per pipe so a slow stderr does not block stdout and
    // vice versa. We used to multiplex on a single thread via
    // WaitForMultipleObjects, but with non-overlapped pipes that call only
    // signals "data MAY be available" — neither side is actually readable
    // until more bytes arrive, which interleaves output and stalls reads.
    std::thread m_stdoutThread;
    std::thread m_stderrThread;
    std::thread m_waitThread;       // joins the reader threads at process exit
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};

    // Output buffer
    struct Line { std::string text; bool isError = false; };
    std::mutex m_mutex;
    std::deque<Line> m_lines;       // displayed
    std::string m_partial;          // unterminated tail (stdout)
    std::string m_partialErr;       // unterminated tail (stderr)
    static constexpr std::size_t kMaxLines = 5000;

    // State for the ImGui input box
    char m_inputBuf[1024] = {};
    bool m_focusInput    = false;
    std::string m_workDir;

    // Status text shown in the header (e.g. "powershell | C:\path").
    std::string m_statusText;
};
