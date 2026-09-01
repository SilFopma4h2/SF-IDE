// Terminal implementation. See Terminal.h for design notes.

#include "Terminal.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include <imgui.h>

namespace {

// Convert UTF-8 -> wide string for Windows APIs.
std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(),
                        static_cast<int>(s.size()), &out[0], len);
    return out;
}

// Convert wide -> UTF-8 for our internal string buffers.
std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                  static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                        static_cast<int>(w.size()),
                        &out[0], len, nullptr, nullptr);
    return out;
}

} // namespace

// --- Construction -----------------------------------------------------------

Terminal::Terminal() = default;

Terminal::~Terminal()
{
    stop();
}

// --- Public API -------------------------------------------------------------

void Terminal::start(const std::string& workDir)
{
    ensureStopped();

    m_workDir = workDir;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_lines.clear();
        m_partial.clear();
        m_partialErr.clear();
    }
    m_inputBuf[0] = '\0';
    m_stopRequested.store(false);

    // Build the command line. -NoLogo keeps the banner out of the buffer.
    std::wstring cmdLine = L"powershell.exe -NoLogo";

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdinRead   = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrWrite = nullptr;

    if (!CreatePipe(&m_hStdoutRead, &stdoutWrite, &sa, 0) ||
        !CreatePipe(&stdinRead,   &m_hStdinWrite, &sa, 0) ||
        !CreatePipe(&m_hStderrRead, &stderrWrite, &sa, 0))
    {
        if (m_hStdoutRead) { CloseHandle(m_hStdoutRead); m_hStdoutRead = nullptr; }
        if (m_hStderrRead) { CloseHandle(m_hStderrRead); m_hStderrRead = nullptr; }
        if (m_hStdinWrite) { CloseHandle(m_hStdinWrite); m_hStdinWrite = nullptr; }
        if (stdoutWrite)   CloseHandle(stdoutWrite);
        if (stdinRead)     CloseHandle(stdinRead);
        if (stderrWrite)   CloseHandle(stderrWrite);
        m_statusText = "Failed to create pipes";
        return;
    }
    SetHandleInformation(m_hStdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(m_hStderrRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(m_hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput  = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError  = stderrWrite;
    si.wShowWindow = SW_HIDE;

    std::wstring mutableCmd = cmdLine;
    std::wstring wDir = utf8ToWide(workDir);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr,
        wDir.empty() ? nullptr : wDir.c_str(), &si, &pi);
    // We must close the child's ends of the pipes in this process regardless
    // of whether CreateProcessW succeeded.
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    if (!ok)
    {
        CloseHandle(m_hStdoutRead); m_hStdoutRead = nullptr;
        CloseHandle(m_hStderrRead); m_hStderrRead = nullptr;
        CloseHandle(m_hStdinWrite); m_hStdinWrite = nullptr;
        m_statusText = "Failed to start powershell.exe (error " +
                       std::to_string(GetLastError()) + ")";
        return;
    }
    CloseHandle(pi.hThread);
    m_hProcess = pi.hProcess;
    m_running.store(true);

    // Build the header text now that we know the working dir.
    {
        std::ostringstream os;
        os << "powershell";
        if (!workDir.empty()) os << " | " << workDir;
        m_statusText = os.str();
    }

    // One reader thread per pipe. They run until the pipe hits EOF (process
    // exit) or stop() forces the pipe closed.
    m_stdoutThread = std::thread(&Terminal::readerThread, this, m_hStdoutRead, false);
    m_stderrThread = std::thread(&Terminal::readerThread, this, m_hStderrRead, true);
    // The waiter thread joins the reader threads once the process exits.
    m_waitThread   = std::thread(&Terminal::waiterThread, this);
}

void Terminal::stop()
{
    ensureStopped();
}

void Terminal::ensureStopped()
{
    // If there is nothing in flight, do a quick cleanup and bail.
    bool anyActive = m_running.load()
                  || m_stdoutThread.joinable()
                  || m_stderrThread.joinable()
                  || m_waitThread.joinable();
    if (!anyActive)
    {
        if (m_hProcess)     { CloseHandle(m_hProcess);     m_hProcess = nullptr; }
        if (m_hStdoutRead)  { CloseHandle(m_hStdoutRead);  m_hStdoutRead = nullptr; }
        if (m_hStderrRead)  { CloseHandle(m_hStderrRead);  m_hStderrRead = nullptr; }
        if (m_hStdinWrite)  { CloseHandle(m_hStdinWrite);  m_hStdinWrite = nullptr; }
        return;
    }
    m_stopRequested.store(true);
    if (m_hProcess) TerminateProcess(m_hProcess, 0);

    // Closing our read-end handles will unblock the reader threads at EOF.
    if (m_hStdoutRead) { CloseHandle(m_hStdoutRead); m_hStdoutRead = nullptr; }
    if (m_hStderrRead) { CloseHandle(m_hStderrRead); m_hStderrRead = nullptr; }
    if (m_hStdinWrite) { CloseHandle(m_hStdinWrite); m_hStdinWrite = nullptr; }

    if (m_stdoutThread.joinable()) m_stdoutThread.join();
    if (m_stderrThread.joinable()) m_stderrThread.join();
    if (m_waitThread.joinable())   m_waitThread.join();

    if (m_hProcess) { CloseHandle(m_hProcess); m_hProcess = nullptr; }
    m_running.store(false);
}

void Terminal::submitLine(const std::string& line)
{
    if (!m_running.load() || !m_hStdinWrite) return;
    std::string toWrite = line;
    toWrite.append("\r\n");
    DWORD wrote = 0;
    WriteFile(m_hStdinWrite, toWrite.data(),
              static_cast<DWORD>(toWrite.size()), &wrote, nullptr);
    // Echo into our own output so the user sees what they typed.
    pushLine("> " + line, false);
}

// --- Reader thread ----------------------------------------------------------

void Terminal::readerThread(HANDLE pipe, bool isError)
{
    // The two reader threads share the per-stream partial buffers and the
    // display queue. We take the lock once per read so the per-byte loop
    // does not contend with the render thread.
    std::string partial;
    char buf[2048];
    for (;;)
    {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(pipe, buf, sizeof(buf), &bytesRead, nullptr);
        if (!ok || bytesRead == 0)
            break;  // pipe closed (process exit) or stop()
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (DWORD i = 0; i < bytesRead; ++i)
            {
                char c = buf[i];
                if (c == '\n')
                {
                    if (!partial.empty() && partial.back() == '\r')
                        partial.pop_back();
                    m_lines.push_back({partial, isError});
                    if (m_lines.size() > kMaxLines)
                        m_lines.pop_front();
                    partial.clear();
                }
                else if (c != '\r')
                {
                    partial.push_back(c);
                    if (partial.size() > 4096)
                    {
                        m_lines.push_back({partial, isError});
                        if (m_lines.size() > kMaxLines)
                            m_lines.pop_front();
                        partial.clear();
                    }
                }
            }
        }
    }
    // Flush trailing partial.
    if (!partial.empty())
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_lines.push_back({partial, isError});
        if (m_lines.size() > kMaxLines)
            m_lines.pop_front();
    }
}

void Terminal::waiterThread()
{
    // Wait for the shell process to exit, then make sure the reader threads
    // observe EOF by closing the write end of our pipes (which we still own
    // via m_hStdinWrite). The reader threads will see broken-pipe / zero-
    // byte reads and exit cleanly.
    if (m_hProcess)
        WaitForSingleObject(m_hProcess, INFINITE);

    // Closing stdin forces powershell to receive EOF and exit promptly even
    // if WaitForSingleObject is still blocked for some reason. The reader
    // threads are already independent.
    if (m_hStdinWrite) { CloseHandle(m_hStdinWrite); m_hStdinWrite = nullptr; }
    if (m_hProcess)    { CloseHandle(m_hProcess);    m_hProcess = nullptr; }
    m_running.store(false);
}

void Terminal::pushLine(const std::string& line, bool isError)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_lines.push_back({line, isError});
    if (m_lines.size() > kMaxLines)
        m_lines.pop_front();
}

// --- ImGui panel ------------------------------------------------------------

void Terminal::render()
{
    // Header row: status, clear / restart buttons.
    ImGui::TextUnformatted(m_statusText.c_str());
    ImGui::SameLine();
    if (m_running.load())
    {
        ImGui::SameLine(ImGui::GetWindowWidth() - 110);
        if (ImGui::SmallButton("Stop")) stop();
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear"))
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_lines.clear();
            m_partial.clear();
            m_partialErr.clear();
        }
    }
    else
    {
        ImGui::SameLine(ImGui::GetWindowWidth() - 110);
        if (ImGui::SmallButton("Start")) start(m_workDir);
    }
    ImGui::Separator();

    // Scrollback
    ImGui::BeginChild("##term_scroll", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const auto& ln : m_lines)
        {
            ImVec4 c = ln.isError ? ImVec4(1.0f, 0.55f, 0.55f, 1.0f)
                                  : ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            ImGui::TextUnformatted(ln.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    // Auto-scroll if the user is at the bottom.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // Input line
    if (m_focusInput)
    {
        ImGui::SetKeyboardFocusHere();
        m_focusInput = false;
    }
    ImGui::PushItemWidth(-1);
    bool enter = ImGui::InputText("##term_input", m_inputBuf, sizeof(m_inputBuf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    if (enter && m_running.load())
    {
        submitLine(m_inputBuf);
        m_inputBuf[0] = '\0';
        m_focusInput = true;
    }
}
