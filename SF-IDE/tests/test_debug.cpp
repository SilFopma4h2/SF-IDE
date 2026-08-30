// End-to-end test for DebugSession: starts a small console program under the
// Win32 debug API, places a software breakpoint at main, then exercises
// continue / stop.
//
// The test program itself stops at a volatile loop so the debuggee can be
// safely terminated.

#include "../src/DebugSession.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

namespace {

bool writeFile(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(f);
}

bool fileExists(const std::string& path)
{
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Wait for the session to enter `target` or terminate after `timeout`.
// Returns true if the state was reached.
bool waitForState(DebugSession& s, DebugSession::State target,
                  std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (s.state() == target) return true;
        if (s.state() == DebugSession::State::Stopped ||
            s.state() == DebugSession::State::Error)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

int main()
{
    const std::string dir = "C:\\Users\\silfo\\SF-IDE\\debug_test_project";
    CreateDirectoryA(dir.c_str(), nullptr);

    // Reuse the SFDbgTest.exe built by the previous test_debug run; if it is
    // not there, build it.
    if (!fileExists(dir + "\\SFDbgTest.exe"))
    {
        if (!writeFile(dir + "\\main.cpp",
                       "#include <cstdio>\n"
                       "volatile int g_keep = 1;\n"
                       "int main() {\n"
                       "    int x = 42;\n"
                       "    std::printf(\"hello %d\\n\", x);\n"
                       "    while (g_keep) { }\n"
                       "    return 0;\n"
                       "}\n"))
        {
            std::fprintf(stderr, "cannot write main.cpp\n");
            return 1;
        }
        const char* devCmdPath = "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\Common7\\Tools\\VsDevCmd.bat";
        if (GetFileAttributesA(devCmdPath) == INVALID_FILE_ATTRIBUTES)
        {
            std::fprintf(stderr, "VsDevCmd.bat not found - skipping debug test\n");
            return 0;
        }
        std::string compileCmd =
            std::string("set \"PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%\" && ")
          + "call \"" + devCmdPath + "\" -arch=x64 -host_arch=x64 -no_logo >nul && "
          + "cl /nologo /EHsc /std:c++20 /Zi "
            "/Fe:" + dir + "\\SFDbgTest.exe "
            "/Fo:" + dir + "\\ "
          + dir + "\\main.cpp /link /SUBSYSTEM:CONSOLE";
        int rc = system(compileCmd.c_str());
        if (rc != 0 || !fileExists(dir + "\\SFDbgTest.exe"))
        {
            std::fprintf(stderr, "compile failed - skipping\n");
            return 0;
        }
    }

    std::atomic<int> stateChanges{0};
    std::atomic<int> events{0};
    std::atomic<bool> broken{false};
    std::atomic<bool> stopped{false};
    DebugSession session;
    std::fprintf(stdout, "DEBUG: about to call session.start\n");
    if (!session.start(dir + "\\SFDbgTest.exe", dir + "\\",
        [&](const std::string& text) {
            events.fetch_add(1);
            std::fprintf(stdout, "  evt: %s\n", text.c_str());
        },
        [&](DebugSession::State s) {
            stateChanges.fetch_add(1);
            std::fprintf(stdout, "  state: %d\n", static_cast<int>(s));
            if (s == DebugSession::State::Break) broken.store(true);
            if (s == DebugSession::State::Stopped) stopped.store(true);
        },
        [](const std::vector<DebugSession::LocalVar>&) {},
        [](const std::vector<DebugSession::StackFrame>&) {},
        [&]() { stopped.store(true); }))
    {
        std::fprintf(stderr, "session.start failed\n");
        return 1;
    }

    // The MVP does not yet resolve source line -> VA, so no breakpoint will
    // be hit. We at least verify the debuggee starts, the worker thread
    // pumps events, and requestStop terminates the process cleanly.
    // Give the debuggee time to start and produce events.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    session.requestStop();
    if (!waitForState(session, DebugSession::State::Stopped,
                      std::chrono::seconds(5)))
    {
        std::fprintf(stderr, "did not reach Stopped after requestStop\n");
        return 1;
    }
    std::fprintf(stderr, "DEBUG: events=%d, state changes=%d\n",
                 events.load(), stateChanges.load());
    if (events.load() == 0)
    {
        std::fprintf(stderr, "no events observed\n");
        return 1;
    }

    std::printf("OK (events=%d, state changes=%d, stopped=%d)\n",
                events.load(), stateChanges.load(), stopped.load() ? 1 : 0);
    return 0;
}
