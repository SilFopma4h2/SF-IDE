// DebugSession implementation. See DebugSession.h for the design.

#include "DebugSession.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <algorithm>

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

// Normalise a source path so the editor path and the PDB line-table path
// can be compared by basename, case-insensitively, on Windows.
std::string baseNameLower(const std::string& s)
{
    std::string n = s;
    for (auto& c : n) if (c == '\\') c = '/';
    auto p = n.find_last_of('/');
    if (p != std::string::npos) n = n.substr(p + 1);
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n;
}

} // namespace

// --- Construction -----------------------------------------------------------

DebugSession::DebugSession() = default;

DebugSession::~DebugSession()
{
    requestStop();
    // A previous start() may have left a joinable thread; joining in the
    // destructor is the only safe way to release it without terminating.
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

    // Always join a leftover worker thread before starting a new one to
    // avoid std::terminate on std::thread re-assignment.
    if (m_thread.joinable()) m_thread.join();

    m_onEvent   = onEvent;
    m_onState   = onState;
    m_onLocals  = onLocals;
    m_onFrames  = onFrames;
    m_onStopped = onStopped;
    m_stopRequested.store(false);
    m_hardwareSingleStep = false;
    m_pendingStepOverReturn = 0;
    {
        std::lock_guard<std::mutex> lk(m_breakpointsMutex);
        m_breakpoints.clear();
    }
    m_modules.clear();

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
    // Store the original path the user gave us; the worker thread may
    // re-normalise on the fly when resolving against the PDB line table.
    bp.sourceFile = sourceFile;
    bp.sourceLine = line;
    bp.userAdded  = true;

    {
        std::lock_guard<std::mutex> lk(m_breakpointsMutex);
        m_breakpoints.push_back(bp);
    }
    emitEvent("Breakpoint queued at " + sourceFile + ":" +
              std::to_string(line));

    // If the debuggee is already running, try to resolve and install now
    // by operating on the vector element that we just inserted.
    resolveAndInstallAllPending();
}

void DebugSession::removeBreakpoint(const std::string& sourceFile, int line)
{
    std::lock_guard<std::mutex> lk(m_breakpointsMutex);
    auto target = baseNameLower(sourceFile);
    for (auto it = m_breakpoints.begin(); it != m_breakpoints.end(); ++it)
    {
        if (it->sourceLine == line && baseNameLower(it->sourceFile) == target)
        {
            // If installed, restore the original byte before removing.
            if (it->installed && m_process && it->address)
            {
                uint8_t orig = it->savedByte;
                writeProcessMemory(it->address, &orig, 1);
            }
            m_breakpoints.erase(it);
            return;
        }
    }
}

void DebugSession::clearBreakpoints()
{
    uninstallAllBreakpoints();
    std::lock_guard<std::mutex> lk(m_breakpointsMutex);
    m_breakpoints.clear();
}

void DebugSession::toggleBreakpoint(const std::string& sourceFile, int line)
{
    // Add if not present, remove if present. We snapshot the iterator so
    // we can erase without holding the lock across the restore-byte write.
    auto target = baseNameLower(sourceFile);
    std::vector<Breakpoint>::iterator toErase;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(m_breakpointsMutex);
        for (auto it = m_breakpoints.begin(); it != m_breakpoints.end(); ++it)
        {
            if (it->sourceLine == line && baseNameLower(it->sourceFile) == target)
            {
                toErase = it;
                found = true;
                break;
            }
        }
    }
    if (found)
    {
        if (toErase->installed && m_process && toErase->address)
        {
            uint8_t orig = toErase->savedByte;
            writeProcessMemory(toErase->address, &orig, 1);
        }
        std::lock_guard<std::mutex> lk(m_breakpointsMutex);
        m_breakpoints.erase(toErase);
        return;
    }
    addBreakpoint(sourceFile, line);
}

bool DebugSession::hasBreakpoint(const std::string& sourceFile, int line) const
{
    std::lock_guard<std::mutex> lk(m_breakpointsMutex);
    auto target = baseNameLower(sourceFile);
    for (const auto& bp : m_breakpoints)
    {
        if (bp.sourceLine == line && baseNameLower(bp.sourceFile) == target)
            return true;
    }
    return false;
}

void DebugSession::requestContinue()
{
    if (m_state.load() != State::Break) return;
    DWORD pid = m_pid;
    DWORD tid = m_steppingThread;
    if (!pid || !tid) return;
    ContinueDebugEvent(pid, tid, DBG_CONTINUE);
    m_state.store(State::Running);
    emitState(State::Running);
}

void DebugSession::requestStepInto()
{
    if (m_state.load() != State::Break) return;

    // Single-step the current thread: set the TRAP flag so the CPU fires
    // EXCEPTION_SINGLE_STEP after one instruction. This is true Step Into,
    // it will descend into any CALL.
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                          FALSE, m_steppingThread);
    if (!h) { requestContinue(); return; }
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(h, &ctx))
    {
        ctx.EFlags |= 0x100; // TRAP
        SetThreadContext(h, &ctx);
        m_hardwareSingleStep = true;
    }
    CloseHandle(h);
    ContinueDebugEvent(m_pid, m_steppingThread, DBG_CONTINUE);
    m_state.store(State::Stepping);
    emitState(State::Stepping);
}

void DebugSession::requestStepOver()
{
    if (m_state.load() != State::Break) return;

    // MVP Step Over: if the current instruction looks like a near CALL,
    // place a one-shot breakpoint at the return address. Otherwise behave
    // like Step Into.
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                          FALSE, m_steppingThread);
    if (!h) { requestContinue(); return; }
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(h, &ctx)) { CloseHandle(h); requestContinue(); return; }

    uint8_t op = 0;
    readProcessMemory(ctx.Rip, &op, 1);
    bool isCall = (op == 0xE8)            // near call with rel32
               || (op == 0xFF)            // near call r/m (op=FF /2)
               || (op == 0x9A);           // far call
    if (isCall)
    {
        // For 0xE8 the return address is ctx.Rip + 5 (size of the call
        // instruction). The simple MVP works for direct relative calls.
        // For 0xFF / 0x9A the call length varies; we fall back to Step
        // Into in those cases.
        if (op == 0xE8)
        {
            DWORD64 retAddr = ctx.Rip + 5;
            Breakpoint bp;
            bp.sourceFile  = "<step-over>";
            bp.sourceLine  = 0;
            bp.address     = retAddr;
            bp.oneShot     = true;
            installBreakpoint(bp);
            {
                std::lock_guard<std::mutex> lk(m_breakpointsMutex);
                m_breakpoints.push_back(bp);
            }
            m_pendingStepOverReturn = retAddr;
            CloseHandle(h);
            ContinueDebugEvent(m_pid, m_steppingThread, DBG_CONTINUE);
            m_state.store(State::Running);
            emitState(State::Running);
            return;
        }
        // Unhandled call shape: fall through to Step Into.
    }

    ctx.EFlags |= 0x100; // TRAP
    SetThreadContext(h, &ctx);
    m_hardwareSingleStep = true;
    CloseHandle(h);
    ContinueDebugEvent(m_pid, m_steppingThread, DBG_CONTINUE);
    m_state.store(State::Stepping);
    emitState(State::Stepping);
}

void DebugSession::requestStop()
{
    m_stopRequested.store(true);
    HANDLE proc = m_process;
    if (proc) TerminateProcess(proc, 0);
}

// --- Worker thread: minimal debug event pump -------------------------------

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

        switch (ev.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
        {
            std::ostringstream os;
            os << "Process started (pid=" << ev.dwProcessId << ")";
            emitEvent(os.str());
            // Initialise dbghelp for the debugger process and load the
            // main module so its symbols are available for line queries.
            SymInitialize(GetCurrentProcess(), nullptr, FALSE);
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
            std::string mainExe = m_pendingExe;
            SymLoadModule64(GetCurrentProcess(), 0, mainExe.c_str(),
                            nullptr, reinterpret_cast<DWORD64>(
                                ev.u.CreateProcessInfo.lpBaseOfImage), 0);
            m_modules[reinterpret_cast<DWORD64>(
                ev.u.CreateProcessInfo.lpBaseOfImage)] = { mainExe };
            resolveAndInstallAllPending();
            break;
        }

        case EXIT_PROCESS_DEBUG_EVENT:
        {
            std::ostringstream os;
            os << "Process exited (code=" << ev.u.ExitProcess.dwExitCode << ")";
            emitEvent(os.str());
            m_running.store(false);
            break;
        }

        case LOAD_DLL_DEBUG_EVENT:
        {
            DWORD64 base = reinterpret_cast<DWORD64>(
                ev.u.LoadDll.lpBaseOfDll);
            char modulePath[MAX_PATH] = {};
            if (GetFinalPathNameByHandleA(ev.u.LoadDll.hFile, modulePath,
                                          MAX_PATH, FILE_NAME_NORMALIZED))
            {
                SymLoadModule64(GetCurrentProcess(), 0, modulePath, nullptr,
                                base, 0);
                m_modules[base] = { modulePath };
            }
            resolveAndInstallAllPending();
            break;
        }

        case UNLOAD_DLL_DEBUG_EVENT:
        {
            DWORD64 base = reinterpret_cast<DWORD64>(
                ev.u.UnloadDll.lpBaseOfDll);
            m_modules.erase(base);
            SymUnloadModule64(GetCurrentProcess(), base);
            break;
        }

        case CREATE_THREAD_DEBUG_EVENT:
        case EXIT_THREAD_DEBUG_EVENT:
        case OUTPUT_DEBUG_STRING_EVENT:
        case RIP_EVENT:
            break;

        case EXCEPTION_DEBUG_EVENT:
        {
            DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
            if (code == EXCEPTION_BREAKPOINT)
            {
                onBreakpointHit(ev.dwThreadId);
                // onBreakpointHit is responsible for continuing the thread.
                continue;
            }
            if (code == EXCEPTION_SINGLE_STEP)
            {
                onSingleStepComplete(ev.dwThreadId);
                continue;
            }
            std::ostringstream os;
            os << "Exception 0x" << std::hex << code;
            emitEvent(os.str());
            break;
        }

        default:
            break;
        }

        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
    }

    uninstallAllBreakpoints();
    if (m_process) { CloseHandle(m_process); m_process = nullptr; }
    m_state.store(State::Stopped);
    emitState(State::Stopped);
    if (m_onStopped) m_onStopped();
}

// --- Breakpoint hit / step complete ---------------------------------------

void DebugSession::onBreakpointHit(DWORD threadId)
{
    m_steppingThread = threadId;

    // Read the thread context so we can inspect the instruction pointer
    // and adjust it if we hit a user breakpoint.
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                          FALSE, threadId);
    if (!h) return;
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(h, &ctx)) { CloseHandle(h); return; }

    DWORD64 hitAddr = ctx.Rip - 1; // RIP points after the 0xCC byte

    Breakpoint* userBp = findBreakpointAt(hitAddr, /*requireUser*/ true);
    if (userBp)
    {
        // Restore the original byte, step over the original instruction,
        // and re-install the breakpoint so future hits still work.
        uint8_t orig = userBp->savedByte;
        writeProcessMemory(userBp->address, &orig, 1);
        ctx.Rip = userBp->address;     // rewind past the patched byte
        ctx.EFlags |= 0x100;           // single-step
        SetThreadContext(h, &ctx);
        userBp->installed = false;     // temporarily uninstalled
        m_hardwareSingleStep = true;
        CloseHandle(h);
        collectFramesAndLocals(threadId);
        m_state.store(State::Break);
        emitState(State::Break);
        {
            std::ostringstream a;
            a << "Breakpoint hit at 0x" << std::hex << userBp->address << std::dec;
            emitEvent(a.str());
        }
        ContinueDebugEvent(m_pid, threadId, DBG_CONTINUE);
        return;
    }

    // One-shot step-over breakpoint: clear it and stop.
    Breakpoint* oneShot = findBreakpointAt(hitAddr, /*requireUser*/ false);
    if (oneShot && oneShot->oneShot)
    {
        uint8_t orig = oneShot->savedByte;
        writeProcessMemory(oneShot->address, &orig, 1);
        {
            std::lock_guard<std::mutex> lk(m_breakpointsMutex);
            m_breakpoints.erase(
                m_breakpoints.begin() +
                (oneShot - m_breakpoints.data()));
        }
        ctx.Rip = oneShot->address;
        SetThreadContext(h, &ctx);
        CloseHandle(h);
        collectFramesAndLocals(threadId);
        m_state.store(State::Break);
        emitState(State::Break);
        emitEvent("Step over complete");
        ContinueDebugEvent(m_pid, threadId, DBG_CONTINUE);
        return;
    }

    // An unexpected breakpoint (e.g. the debuggee's own INT3 or the
    // initial debug attach trap). Just stop without restoring any byte.
    CloseHandle(h);
    collectFramesAndLocals(threadId);
    m_state.store(State::Break);
    emitState(State::Break);
    emitEvent("Breakpoint (unexpected)");
    ContinueDebugEvent(m_pid, threadId, DBG_CONTINUE);
}

void DebugSession::onSingleStepComplete(DWORD threadId)
{
    m_steppingThread = threadId;
    m_hardwareSingleStep = false;
    // Re-install any user breakpoints we temporarily uninstalled so the
    // next execution can hit them again.
    reinstallUserBreakpoints();
    collectFramesAndLocals(threadId);
    m_state.store(State::Break);
    emitState(State::Break);
    emitEvent("Single-step complete");
    ContinueDebugEvent(m_pid, threadId, DBG_CONTINUE);
}

// --- Breakpoint resolution and installation --------------------------------

void DebugSession::resolveAndInstallAllPending()
{
    // For every queued breakpoint that has no address yet, try to resolve
    // it against the loaded modules, then install if resolved.
    std::vector<Breakpoint> snapshot;
    {
        std::lock_guard<std::mutex> lk(m_breakpointsMutex);
        snapshot = m_breakpoints;
    }
    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        if (snapshot[i].address == 0 && !snapshot[i].oneShot)
        {
            tryResolveBreakpoint(snapshot[i]);
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_breakpointsMutex);
        for (size_t i = 0; i < m_breakpoints.size() && i < snapshot.size(); ++i)
        {
            // Apply resolved info back into the real vector element.
            m_breakpoints[i].address   = snapshot[i].address;
            m_breakpoints[i].installed = snapshot[i].installed;
        }
    }
    // Install anything that resolved but is not yet installed.
    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        if (snapshot[i].address && !snapshot[i].installed && !snapshot[i].oneShot)
        {
            installBreakpoint(snapshot[i]);
            {
                std::lock_guard<std::mutex> lk(m_breakpointsMutex);
                if (i < m_breakpoints.size())
                    m_breakpoints[i].installed = snapshot[i].installed;
            }
        }
    }
}

void DebugSession::tryResolveBreakpoint(Breakpoint& bp)
{
    if (bp.address) return;
    if (bp.sourceFile.empty() || bp.sourceLine <= 0) return;

    auto wanted = baseNameLower(bp.sourceFile);
    for (const auto& kv : m_modules)
    {
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        LONG dispLong = 0;
        // IMPORTANT: the third argument of SymGetLineFromName64 is the
        // *source file* name (basename usually), not the module path.
        // Passing the module path here is a common bug; we use the
        // source file the user gave us instead.
        if (SymGetLineFromName64(GetCurrentProcess(), nullptr,
                                 const_cast<char*>(bp.sourceFile.c_str()),
                                 static_cast<DWORD>(bp.sourceLine),
                                 &dispLong, &line))
        {
            if (line.Address != 0)
            {
                bp.address = line.Address;
                std::ostringstream os;
                os << "Breakpoint resolved at 0x" << std::hex << bp.address
                   << " (" << bp.sourceFile << ":" << std::dec << bp.sourceLine
                   << " in " << kv.second.fileName << ")";
                emitEvent(os.str());
                return;
            }
        }
    }
}

void DebugSession::installBreakpoint(Breakpoint& bp)
{
    if (bp.address == 0 || bp.installed) return;
    if (!m_process) return;
    uint8_t orig = 0;
    if (!readProcessMemory(bp.address, &orig, 1)) return;
    bp.savedByte = orig;
    uint8_t cc = 0xCC;
    if (!writeProcessMemory(bp.address, &cc, 1)) return;
    bp.installed = true;
}

void DebugSession::uninstallBreakpoint(Breakpoint& bp)
{
    if (bp.address == 0 || !bp.installed) return;
    if (!m_process) return;
    writeProcessMemory(bp.address, &bp.savedByte, 1);
    bp.installed = false;
}

void DebugSession::installPendingBreakpoints()
{
    std::lock_guard<std::mutex> lk(m_breakpointsMutex);
    for (auto& bp : m_breakpoints) installBreakpoint(bp);
}

void DebugSession::uninstallAllBreakpoints()
{
    std::lock_guard<std::mutex> lk(m_breakpointsMutex);
    for (auto& bp : m_breakpoints) uninstallBreakpoint(bp);
}

void DebugSession::reinstallUserBreakpoints()
{
    std::lock_guard<std::mutex> lk(m_breakpointsMutex);
    for (auto& bp : m_breakpoints)
    {
        if (!bp.userAdded) continue;
        if (bp.address && !bp.installed)
            installBreakpoint(bp);
    }
}

Breakpoint* DebugSession::findBreakpointAt(DWORD64 addr, bool requireUser)
{
    std::lock_guard<std::mutex> lk(m_breakpointsMutex);
    for (auto& bp : m_breakpoints)
    {
        if (bp.address == addr)
        {
            if (requireUser && !bp.userAdded) continue;
            return &bp;
        }
    }
    return nullptr;
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
    HANDLE proc = m_process;
    if (!proc) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(addr),
                             buf, size, &got) && got == size;
}

bool DebugSession::writeProcessMemory(DWORD64 addr, const void* buf, size_t size) const
{
    HANDLE proc = m_process;
    if (!proc) return false;
    SIZE_T written = 0;
    return WriteProcessMemory(proc, reinterpret_cast<LPVOID>(addr),
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
