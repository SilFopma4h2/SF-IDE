// BuildRunner drives an external MSVC + CMake build for a user project.
//
// It uses vswhere to locate Visual Studio Build Tools, sources VsDevCmd.bat
// to populate the environment (PATH, INCLUDE, LIB) so cl.exe / link.exe / the
// Windows SDK are available, then runs `cmake -S <src> -B <build>` and
// `cmake --build <build> --config Debug` asynchronously. stdout/stderr are
// captured line-by-line and forwarded to the GUI through a callback.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class BuildRunner {
public:
    enum class State { Idle, Configuring, Building, Succeeded, Failed, Error };

    struct Status {
        State        state = State::Idle;
        std::string  message;       // human-readable status text
        int          exitCode = 0;  // last subprocess exit code
    };

    using LineCallback = std::function<void(const std::string& line, bool isError)>;

    BuildRunner();
    ~BuildRunner();

    // Start a configure+build cycle for a project. `projectDir` must contain a
    // CMakeLists.txt. The runner copies the configured build directory into
    // `<projectDir>/build`. If another build is already running, returns false.
    bool start(const std::string& projectDir, const LineCallback& onLine);

    // Cancel a running build (best-effort: terminates the child process tree).
    void cancel();

    State state() const { return m_status.state; }
    const Status& status() const { return m_status; }

    // Detected Visual Studio path; empty if not found.
    const std::string& vsInstallPath() const { return m_vsPath; }

    // Last error if state() == State::Error.
    const std::string& lastError() const { return m_status.message; }

private:
    void run();
    bool resolveToolchain();      // populates m_vsPath and m_msvcVersion
    bool runSubprocess(const std::string& cmdLine,
                       const std::string& workDir,
                       bool isErrorStream,
                       int& exitCode);

    Status             m_status;
    std::atomic<bool>  m_running{false};
    std::thread        m_thread;
    LineCallback       m_onLine;
    std::string        m_projectDir;
    std::string        m_vsPath;          // e.g. C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
    HANDLE             m_childJob = nullptr;
    PROCESS_INFORMATION m_current{};

    void terminateChildTree();
};
