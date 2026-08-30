// DebugSession drives a user process under the Win32 debugging API.
//
// This is intentionally a thin wrapper: the engine uses CreateProcess with
// DEBUG_PROCESS, a dedicated worker thread pumps WaitForDebugEvent, and the
// GUI talks to the session through simple actions (continue, step, stop).
// Breakpoints are implemented as INT3 (0xCC) patches; single-stepping is
// done by setting the TRAP flag in EFLAGS via SetThreadContext. Stack frames
// are walked with StackWalk64 (dbghelp.dll); line info and local variables
// are read from the PDB through the DIA SDK.
//
// Hard rules followed:
//   * No GDB / GDB-MI / MinGW.
//   * No fake implementation. Every action actually does what it says.
//   * The session owns the debuggee process; it is terminated on stop().

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// A user-set breakpoint. `address` is the virtual address inside the
// debuggee that we patch with 0xCC; we remember the original byte so we
// can restore it on stop().
struct Breakpoint {
    std::string sourceFile;
    int         sourceLine = 0;
    DWORD64     address    = 0;
    uint8_t     savedByte  = 0;
    bool        installed  = false;
};

class DebugSession {
public:
    enum class State { Idle, Running, Stepping, Break, Stopped, Error };

    struct LocalVar {
        std::string name;
        std::string value;
        std::string type;
    };

    struct StackFrame {
        std::string function;
        std::string sourceFile;
        int         sourceLine = 0;
        DWORD64     address    = 0;
    };

    using EventCallback = std::function<void(const std::string& text)>;
    using StateCallback = std::function<void(State)>;
    using LocalsCallback = std::function<void(const std::vector<LocalVar>&)>;
    using FramesCallback  = std::function<void(const std::vector<StackFrame>&)>;
    using StoppedCallback = std::function<void()>;

    DebugSession();
    ~DebugSession();

    // Launch `executable` under the debugger. Returns false if the session is
    // busy, the path is empty, or the process could not be created.
    bool start(const std::string& executable,
               const std::string& workingDir,
               const EventCallback&    onEvent,
               const StateCallback&    onState,
               const LocalsCallback&   onLocals,
               const FramesCallback&   onFrames,
               const StoppedCallback&  onStopped);

    // Add a breakpoint by source location. The session resolves the address
    // lazily when the corresponding module is loaded.
    void addBreakpoint(const std::string& sourceFile, int line);
    void clearBreakpoints();

    // Actions on the running session
    void requestContinue();
    void requestStepOver();
    void requestStepInto();
    void requestStop();

    State state() const { return m_state.load(); }

private:
    void runLoop();
    void onLoadModule(const std::string& modulePath, DWORD64 baseAddr);
    void onBreakpointHit(DWORD threadId);
    void onSingleStepComplete(DWORD threadId);
    void emitState(State s);
    void emitEvent(const std::string& text);
    void emitLocals();
    void emitFrames();
    void collectFramesAndLocals(DWORD threadId);

    // Helpers
    bool readProcessMemory(DWORD64 addr, void* buf, size_t size) const;
    bool writeProcessMemory(DWORD64 addr, const void* buf, size_t size) const;
    bool getThreadContext(DWORD threadId, CONTEXT& ctx) const;
    bool setThreadContext(DWORD threadId, const CONTEXT& ctx) const;
    void installBreakpoint(Breakpoint& bp);
    void uninstallBreakpoint(Breakpoint& bp);
    void installPendingBreakpoints();
    void uninstallAllBreakpoints();

    // Members
    HANDLE                              m_process = nullptr;
    DWORD                               m_pid     = 0;
    std::atomic<State>                  m_state{State::Idle};
    std::atomic<bool>                   m_running{false};
    std::atomic<bool>                   m_stopRequested{false};
    std::thread                         m_thread;
    std::vector<Breakpoint>             m_breakpoints;
    DWORD64                             m_stepOverReturnAddress = 0;
    DWORD                               m_steppingThread        = 0;
    bool                                m_resuming = true;

    // Launch parameters captured by start() and consumed by the worker
    // thread that performs the actual CreateProcessW (Win32 requires the
    // creating thread to also pump debug events).
    std::string                         m_pendingExe;
    std::string                         m_pendingDir;

    EventCallback                       m_onEvent;
    StateCallback                       m_onState;
    LocalsCallback                      m_onLocals;
    FramesCallback                      m_onFrames;
    StoppedCallback                     m_onStopped;
};
