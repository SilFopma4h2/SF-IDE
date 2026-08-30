// DebugSession implementation. See DebugSession.h for the design.

#include "DebugSession.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace {

std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                  static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                        static_cast<int>(w.size()),
                        &out[0], len, nullptr, nullptr);
    return out;
}

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

// Resolve a single stack frame using only the dbghelp APIs.
DebugSession::StackFrame resolveFrame(DWORD64 address)
{
    DebugSession::StackFrame frame;
    frame.address = address;

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD displacement = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), address,
                             &displacement, &line))
    {
        frame.sourceFile = line.FileName;
        frame.sourceLine = static_cast<int>(line.LineNumber);
    }
    char buf[512] = {};
    DWORD64 symDisp = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &symDisp,
                    reinterpret_cast<PSYMBOL_INFO>(buf)))
    {
        PSYMBOL_INFO si = reinterpret_cast<PSYMBOL_INFO>(buf);
        frame.function = si->Name;
    }
    if (frame.function.empty()) frame.function = "<unknown>";
    return frame;
}

} // namespace

// --- Construction -----------------------------------------------------------

DebugSession::DebugSession() = default;

DebugSession::~DebugSession()
{
    requestStop();
    if (m_thread.joinable()) m_thread.join();
    if (m_process) { CloseHandle(m_process); m_process = nullptr; }
}

// --- Public API -------------------------------------------------------------

bool DebugSession::start(const std::string& executable,
                         const std::string& workingDir,
                         const EventCallback&    onEvent,
                         const StateCallback&    onState,
                         const LocalsCallback&   onLocals,
                         const FramesCallback&   onFrames,
                         const StoppedCallback&  onStopped)
{
    if (m_running.load()) return false;
    if (executable.empty()) return false;

    m_onEvent   = onEvent;
    m_onState   = onState;
    m_onLocals  = onLocals;
    m_onFrames  = onFrames;
    m_onStopped = onStopped;
    m_stopRequested.store(false);
    m_breakpoints.clear();

    // Win32 debugging is per-thread: only the thread that called
    // CreateProcessW with DEBUG_PROCESS can call WaitForDebugEvent for the
    // resulting child. We therefore pass the launch parameters to the worker
    // thread and let it do the create+pump itself.
    char absExe[MAX_PATH] = {};
    if (!GetFullPathNameA(executable.c_str(), MAX_PATH, absExe, nullptr))
        return false;
    if (!workingDir.empty())
    {
        char absDir[MAX_PATH] = {};
        if (!GetFullPathNameA(workingDir.c_str(), MAX_PATH, absDir, nullptr))
            return false;
        m_pendingDir = absDir;
    }
    else
    {
        m_pendingDir.clear();
    }
    m_pendingExe = absExe;

    m_running.store(true);
    m_state.store(State::Running);
    emitState(State::Running);
    m_thread = std::thread(&DebugSession::runLoop, this);
    return true;
}

void DebugSession::addBreakpoint(const std::string& sourceFile, int line)
{
    Breakpoint bp;
    bp.sourceFile = sourceFile;
    bp.sourceLine = line;
    m_breakpoints.push_back(bp);
    emitEvent("Breakpoint queued at " + sourceFile + ":" +
              std::to_string(line) +
              " (address resolution not implemented in MVP)");
}

void DebugSession::clearBreakpoints()
{
    uninstallAllBreakpoints();
    m_breakpoints.clear();
}

void DebugSession::requestContinue()
{
    if (m_state.load() != State::Break) return;
    ContinueDebugEvent(m_pid, m_steppingThread, DBG_CONTINUE);
    m_state.store(State::Running);
    emitState(State::Running);
}

void DebugSession::requestStepOver()
{
    if (m_state.load() != State::Break) return;
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                          FALSE, m_steppingThread);
    if (!h) { requestContinue(); return; }
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(h, &ctx))
    {
        ctx.EFlags |= 0x100; // TRAP
        SetThreadContext(h, &ctx);
    }
    CloseHandle(h);
    ContinueDebugEvent(m_pid, m_steppingThread, DBG_CONTINUE);
    m_state.store(State::Stepping);
    emitState(State::Stepping);
}

void DebugSession::requestStepInto()
{
    requestStepOver();
}

void DebugSession::requestStop()
{
    if (!m_process) return;
    m_stopRequested.store(true);
    TerminateProcess(m_process, 0);
}

// --- Worker thread: minimal debug event pump -------------------------------
//
// The loop is intentionally tiny. We pump WaitForDebugEvent /
// ContinueDebugEvent directly and forward events to the GUI. All state
// transitions (Break, Stepping, etc.) are computed from the events we
// observe, not from a separate state machine. This keeps the engine
// straightforward to reason about: every callback fires because a real Win32
// debug event was just observed.

void DebugSession::runLoop()
{
    std::wstring wExe = utf8ToWide(m_pendingExe);
    std::wstring wDir = m_pendingDir.empty() ? std::wstring{} : utf8ToWide(m_pendingDir);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOW;
    PROCESS_INFORMATION pi = {};
    wchar_t cmdLine[1024] = {};
    std::wstring quoted = L"\"" + wExe + L"\"";
    wcsncpy_s(cmdLine, quoted.c_str(), _TRUNCATE);
    BOOL ok = CreateProcessW(
        nullptr, cmdLine, nullptr, nullptr, FALSE,
        DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS,
        nullptr, wDir.empty() ? nullptr : wDir.c_str(),
        &si, &pi);
    if (!ok)
    {
        emitEvent(std::string("CreateProcess failed: ") +
                  std::to_string(GetLastError()));
        m_state.store(State::Error);
        emitState(State::Error);
        m_running.store(false);
        if (m_onStopped) m_onStopped();
        return;
    }
    CloseHandle(pi.hThread);
    m_process = pi.hProcess;
    m_pid     = pi.dwProcessId;
    emitEvent("Process started");

    DEBUG_EVENT ev = {};
    while (m_running.load())
    {
        if (!WaitForDebugEvent(&ev, 100))
        {
            if (m_stopRequested.load()) break;
            continue;
        }

        std::ostringstream os;
        switch (ev.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
            os << "Process started (pid=" << ev.dwProcessId << ")";
            emitEvent(os.str());
            break;

        case EXIT_PROCESS_DEBUG_EVENT:
            os << "Process exited (code=" << ev.u.ExitProcess.dwExitCode << ")";
            emitEvent(os.str());
            m_running.store(false);
            break;

        case CREATE_THREAD_DEBUG_EVENT:
        case EXIT_THREAD_DEBUG_EVENT:
        case LOAD_DLL_DEBUG_EVENT:
        case UNLOAD_DLL_DEBUG_EVENT:
        case OUTPUT_DEBUG_STRING_EVENT:
        case RIP_EVENT:
            // Report these as short notes so the user sees that the engine
            // is alive, but do not translate them into state changes.
            os << "Event " << ev.dwDebugEventCode;
            emitEvent(os.str());
            break;

        case EXCEPTION_DEBUG_EVENT:
        {
            DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
            if (code == EXCEPTION_BREAKPOINT)
            {
                m_steppingThread = ev.dwThreadId;
                m_state.store(State::Break);
                emitState(State::Break);
                emitEvent("Breakpoint hit");
                ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId,
                                   DBG_CONTINUE);
                continue; // do not ContinueDebugEvent again below
            }
            else if (code == EXCEPTION_SINGLE_STEP)
            {
                m_state.store(State::Break);
                emitState(State::Break);
                emitEvent("Single-step complete");
                ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId,
                                   DBG_CONTINUE);
                continue;
            }
            os << "Exception 0x" << std::hex << code;
            emitEvent(os.str());
            break;
        }

        default:
            break;
        }

        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
    }

    if (m_process) { CloseHandle(m_process); m_process = nullptr; }
    m_state.store(State::Stopped);
    emitState(State::Stopped);
    if (m_onStopped) m_onStopped();
}

// --- Event handlers ---------------------------------------------------------

void DebugSession::onLoadModule(const std::string& modulePath, DWORD64 baseAddr)
{
    (void)modulePath; (void)baseAddr;
}

void DebugSession::installBreakpoint(Breakpoint& bp)
{
    (void)bp;
}

void DebugSession::uninstallBreakpoint(Breakpoint& bp)
{
    (void)bp;
}

void DebugSession::installPendingBreakpoints() {}
void DebugSession::uninstallAllBreakpoints() {}

void DebugSession::onBreakpointHit(DWORD threadId)
{
    m_steppingThread = threadId;
    collectFramesAndLocals(threadId);
}

void DebugSession::onSingleStepComplete(DWORD threadId)
{
    collectFramesAndLocals(threadId);
}

// --- Stack + locals ---------------------------------------------------------

void DebugSession::collectFramesAndLocals(DWORD threadId)
{
    std::vector<StackFrame> frames;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT, FALSE, threadId);
    if (!thread) return;
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) { CloseHandle(thread); return; }
    CloseHandle(thread);

    HANDLE proc = GetCurrentProcess();
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset    = ctx.Rip;
    sf.AddrPC.Mode      = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode   = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode   = AddrModeFlat;

    int remaining = 32;
    while (remaining-- > 0)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &sf,
                         &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr))
            break;
        if (sf.AddrPC.Offset == 0) break;
        frames.push_back(resolveFrame(sf.AddrPC.Offset));
        if (sf.AddrReturn.Offset == 0) break;
    }
    if (m_onFrames) m_onFrames(frames);

    // Locals are intentionally not enumerated in the MVP. Reading the value
    // of a local requires evaluating its location in the debuggee context,
    // which needs either a full expression evaluator (DIA + custom frame
    // walker) or the Microsoft Debug Engine API.
    std::vector<LocalVar> locals;
    if (!frames.empty())
    {
        LocalVar v;
        v.name  = "<locals>";
        v.type  = "(not implemented in MVP)";
        v.value = "use Microsoft DbgEng for full locals";
        locals.push_back(v);
    }
    if (m_onLocals) m_onLocals(locals);
}

// --- Low-level helpers ------------------------------------------------------

bool DebugSession::readProcessMemory(DWORD64 addr, void* buf, size_t size) const
{
    SIZE_T got = 0;
    return ReadProcessMemory(m_process, reinterpret_cast<LPCVOID>(addr),
                             buf, size, &got) && got == size;
}

bool DebugSession::writeProcessMemory(DWORD64 addr, const void* buf, size_t size) const
{
    SIZE_T written = 0;
    return WriteProcessMemory(m_process, reinterpret_cast<LPVOID>(addr),
                              buf, size, &written) && written == size;
}

bool DebugSession::getThreadContext(DWORD threadId, CONTEXT& ctx) const
{
    HANDLE t = OpenThread(THREAD_GET_CONTEXT, FALSE, threadId);
    if (!t) return false;
    ctx.ContextFlags = CONTEXT_FULL;
    BOOL ok = GetThreadContext(t, &ctx);
    CloseHandle(t);
    return ok != FALSE;
}

bool DebugSession::setThreadContext(DWORD threadId, const CONTEXT& ctx) const
{
    HANDLE t = OpenThread(THREAD_SET_CONTEXT, FALSE, threadId);
    if (!t) return false;
    BOOL ok = SetThreadContext(t, &ctx);
    CloseHandle(t);
    return ok != FALSE;
}

// --- Emit helpers -----------------------------------------------------------

void DebugSession::emitState(State s)
{
    if (m_onState) m_onState(s);
}

void DebugSession::emitEvent(const std::string& text)
{
    if (m_onEvent) m_onEvent(text);
}

void DebugSession::emitLocals()
{
    if (m_onLocals) m_onLocals({});
}

void DebugSession::emitFrames()
{
    if (m_onFrames) m_onFrames({});
}
