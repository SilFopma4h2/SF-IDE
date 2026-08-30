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
                       "#include <cstdio>\n"                              // 1
                       "volatile int g_keep = 1;\n"                       // 2
                       "int main() {\n"                                    // 3
                       "    int x = 42;\n"                                // 4
                       "    std::printf(\"hello %d\\n\", x);\n"            // 5
                       "    while (g_keep) { }\n"                         // 6
                       "    return 0;\n"                                  // 7
                       "}\n"))                                             // 8
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

    // Place a breakpoint on the line `std::printf("hello %d\n", x);` which
    // is line 5 in main.cpp above. The address resolver should match it to
    // the first instruction of that line in the debuggee.
    session.addBreakpoint("main.cpp", 5);

    // Wait up to 10 seconds for the breakpoint to be hit. dbghelp line
    // resolution can take a moment on the first call because the PDB has to
    // be parsed.
    if (!waitForState(session, DebugSession::State::Break,
                      std::chrono::seconds(10)))
    {
        std::fprintf(stderr, "breakpoint was not hit\n");
        session.requestStop();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::fprintf(stderr, "DEBUG: events=%d, broken=%d\n",
                     events.load(), broken.load() ? 1 : 0);
        return 1;
    }
    std::fprintf(stdout, "OK: breakpoint hit\n");

    // Continue so the debuggee runs into the while(g_keep) loop, then stop.
    session.requestContinue();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    session.requestStop();
    if (!waitForState(session, DebugSession::State::Stopped,
                      std::chrono::seconds(5)))
    {
        std::fprintf(stderr, "did not reach Stopped after requestStop\n");
        return 1;
    }
    if (events.load() == 0)
    {
        std::fprintf(stderr, "no events observed\n");
        return 1;
    }

    std::printf("OK (events=%d, state changes=%d, stopped=%d, broken=%d)\n",
                events.load(), stateChanges.load(),
                stopped.load() ? 1 : 0, broken.load() ? 1 : 0);
    return 0;
}
