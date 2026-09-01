#include "BuildRunner.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

// WIN32_LEAN_AND_MEAN in the header hides jobapi2.h; pull it in explicitly so
// we can use CreateJobObject / AssignObjectToJobObject / TerminateJobObject.
#include <jobapi2.h>

#pragma comment(lib, "advapi32.lib")

namespace {

// Quote a string for use as a command-line argument. Always quotes; escapes
// embedded double-quotes by doubling them. Returns "" for empty input.
std::string quoteArg(const std::string& s)
{
    if (s.empty()) return std::string("\"\"");
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s)
    {
        if (c == '"') out.append("\"\"");
        else          out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// Convert UTF-8 -> wide string (Windows command lines are wide).
std::wstring utf8ToWide(const std::string& s)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    if (len > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(),
                            static_cast<int>(s.size()), &out[0], len);
    return out;
}

// Run a process whose stdout is captured and returned. stderr is merged into
// stdout via the command shell. Returns false on a hard error (cannot start
// the process). The child is added to `job` so terminating the job kills it.
bool runCaptured(const std::wstring& cmdLine,
                 const std::wstring& workDir,
                 HANDLE job,
                 std::string& output,
                 DWORD& exitCode)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe  = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = writePipe;
    si.hStdError  = writePipe;
    si.wShowWindow = SW_HIDE;

    std::wstring mutableCmd = cmdLine;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_BREAKAWAY_FROM_JOB, nullptr,
        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (!ok)
    {
        CloseHandle(readPipe);
        return false;
    }

    if (job)
        AssignProcessToJobObject(job, pi.hProcess);

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Read until EOF
    char buf[4096];
    DWORD bytesRead = 0;
    for (;;)
    {
        BOOL r = ReadFile(readPipe, buf, sizeof(buf), &bytesRead, nullptr);
        if (!r || bytesRead == 0) break;
        output.append(buf, bytesRead);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    return true;
}

// Run a process but stream each line through a callback as it arrives. Returns
// false on a hard error. The child is added to `job` for cleanup.
bool runStreamed(const std::wstring& cmdLine,
                 const std::wstring& workDir,
                 HANDLE job,
                 const BuildRunner::LineCallback& onLine,
                 bool isError,
                 DWORD& exitCode)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe  = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb           = sizeof(si);
    si.dwFlags      = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput   = writePipe;
    si.hStdError    = writePipe;
    si.wShowWindow  = SW_HIDE;

    std::wstring mutableCmd = cmdLine;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_BREAKAWAY_FROM_JOB, nullptr,
        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (!ok)
    {
        CloseHandle(readPipe);
        return false;
    }

    if (job)
        AssignProcessToJobObject(job, pi.hProcess);

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::string line;
    char buf[2048];
    DWORD bytesRead = 0;
    for (;;)
    {
        BOOL r = ReadFile(readPipe, buf, sizeof(buf), &bytesRead, nullptr);
        if (!r || bytesRead == 0)
        {
            if (!line.empty()) { if (onLine) onLine(line, isError); line.clear(); }
            break;
        }
        for (DWORD i = 0; i < bytesRead; ++i)
        {
            char c = buf[i];
            if (c == '\n')
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (onLine) onLine(line, isError);
                line.clear();
            }
            else if (c != '\r')
            {
                line.push_back(c);
            }
        }
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

// --- Construction -----------------------------------------------------------

BuildRunner::BuildRunner() = default;

BuildRunner::~BuildRunner()
{
    cancel();
    // Always join if a previous start() left a joinable thread, even when
    // m_running already cleared. We do this unconditionally: a joinable
    // std::thread that goes out of scope without join/detach terminates.
    if (m_thread.joinable()) m_thread.join();
    if (m_childJob) CloseHandle(m_childJob);
}

void BuildRunner::cancel()
{
    if (m_childJob) terminateChildTree();
}

// --- Public entry point -----------------------------------------------------

bool BuildRunner::start(const std::string& projectDir, const LineCallback& onLine)
{
    if (m_running.load()) return false;

    // A previous run may have finished (m_running == false) but the worker
    // thread handle may still be joinable. Reassigning std::thread without
    // joining first triggers std::terminate(), so always join here.
    if (m_thread.joinable()) m_thread.join();

    if (!resolveToolchain())
    {
        m_status.state   = State::Error;
        m_status.message = "Could not locate Visual Studio Build Tools. "
                           "Install Build Tools and ensure vswhere.exe is available.";
        if (onLine) onLine(m_status.message, true);
        return false;
    }

    // Convert project dir to an absolute path so that child tools (cmake, cmd)
    // resolve it the same way regardless of the current working directory.
    char abs[MAX_PATH] = {};
    if (!GetFullPathNameA(projectDir.c_str(), MAX_PATH, abs, nullptr))
    {
        m_status.state   = State::Error;
        m_status.message = "Invalid project directory: " + projectDir;
        if (onLine) onLine(m_status.message, true);
        return false;
    }
    m_projectDir = abs;
    m_onLine     = onLine;
    m_running.store(true);
    m_status = {State::Configuring, "Starting...", 0};
    if (m_childJob)
    {
        CloseHandle(m_childJob);
        m_childJob = nullptr;
    }
    m_childJob = CreateJobObjectW(nullptr, nullptr);
    if (m_childJob)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(m_childJob, JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));
    }
    m_thread = std::thread(&BuildRunner::run, this);
    return true;
}

// --- Toolchain discovery ----------------------------------------------------

bool BuildRunner::resolveToolchain()
{
    // Use vswhere.exe to find any VS / Build Tools installation that includes
    // the C++ x64 toolset. This is the supported way and avoids hardcoding
    // a specific MSVC version.
    const std::string vswhere =
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (GetFileAttributesA(vswhere.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    std::string out;
    DWORD code = 0;
    std::wstring cmd = L"\"" + utf8ToWide(vswhere) +
        L"\" -latest -products * "
        L"-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
        L"-property installationPath";
    if (!runCaptured(cmd, {}, nullptr, out, code))
        return false;
    if (code != 0) return false;

    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    if (out.empty()) return false;
    m_vsPath = out;
    return true;
}

// --- Run thread -------------------------------------------------------------

void BuildRunner::run()
{
    // Build a VsDevCmd call that sets up the MSVC + Win SDK environment, then
    // runs our command. We use cmd.exe so the "call" and "&&" syntax works.
    auto buildCmd = [&](const std::string& command, const std::string& workDir) -> bool {
        std::wstring devCmd = utf8ToWide(m_vsPath) + L"\\Common7\\Tools\\VsDevCmd.bat";
        std::string devCmdUtf8 = m_vsPath + "\\Common7\\Tools\\VsDevCmd.bat";

        // Put the vswhere directory on PATH so the Visual Studio CMake
        // generator can locate Visual Studio during configuration.
        std::ostringstream full;
        full << "set \"PATH="
             << "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;"
             << "%PATH%\" && "
             << "call \"" << devCmdUtf8
             << "\" -arch=x64 -host_arch=x64 -no_logo >nul && " << command;
        std::wstring wcmd = L"cmd.exe /d /c \"" + utf8ToWide(full.str()) + L"\"";

        DWORD exit = 0;
        bool ok = runStreamed(wcmd, utf8ToWide(workDir), m_childJob, m_onLine, false, exit);
        m_status.exitCode = static_cast<int>(exit);
        return ok && exit == 0;
    };

    // 1) Configure
    const std::string buildDir = m_projectDir + "\\build";
    {
        std::ostringstream cmd;
        cmd << "cmake -S \"" << m_projectDir << "\" -B \"" << buildDir << "\""
            << " -G \"Visual Studio 17 2022\" -A x64"
            << " -DCMAKE_BUILD_TYPE=Debug";
        m_status.message = "Configuring with CMake...";
        if (m_onLine) m_onLine(m_status.message, false);
        if (!buildCmd(cmd.str(), m_projectDir))
        {
            m_status.state   = State::Failed;
            m_status.message = "Build failed (configure)";
            if (m_onLine) m_onLine(m_status.message, true);
            m_running.store(false);
            return;
        }
    }

    // 2) Build
    m_status.state   = State::Building;
    m_status.message = "Building (Debug)...";
    if (m_onLine) m_onLine(m_status.message, false);
    {
        std::ostringstream cmd;
        cmd << "cmake --build \"" << buildDir << "\" --config Debug --parallel";
        if (!buildCmd(cmd.str(), m_projectDir))
        {
            m_status.state   = State::Failed;
            m_status.message = "Build failed (compile/link)";
            if (m_onLine) m_onLine(m_status.message, true);
            m_running.store(false);
            return;
        }
    }

    m_status.state   = State::Succeeded;
    m_status.message = "Build succeeded";
    if (m_onLine) m_onLine(m_status.message, false);
    m_running.store(false);
}

void BuildRunner::terminateChildTree()
{
    if (m_childJob)
    {
        // TerminateProcess on the job kills every process assigned to it.
        TerminateJobObject(m_childJob, 1);
    }
}
