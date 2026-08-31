# SF-IDE

A minimal native C++ IDE for Windows. Written in C++20 with Win32, Direct3D 11, and Dear ImGui (docking). No Qt, no Electron, no webview, no editor framework — just a hand-rolled text editor, syntax highlighter, build runner, debugger, and integrated shell.

The goal is a small, self-contained IDE that boots fast, edits C/C++ source without fuss, builds it with the local MSVC toolchain, and runs it under a real on-Windows debugger.

## Features

### Editor
- Loads / saves UTF-8 source files, normalises CRLF ↔ LF transparently.
- Line numbers, breakpoint gutter, click-to-toggle breakpoints.
- C/C++ syntax highlighting (keywords, strings, char literals, numbers, preprocessor, line and block comments).
- Selection (click + drag, Shift + arrows, Home / End, PageUp / PageDown, Ctrl+A).
- Editing: typing, Backspace, Delete, Enter, Tab (inserts 4 spaces), copy / cut / paste.
- Per-line undo / redo (Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z).
- Horizontal scroll for long lines; vertical scroll for large files.
- Cursor position is accurate, mapped from mouse coordinates captured during the draw pass.

### File explorer
- Tree view of the project root.
- Click a file to select; double-click to open it in the editor.
- Skips `.git`, `build`, `vcpkg_installed`, and other noise directories.

### Build pipeline
- Configures and builds a CMake project under MSVC.
- Streams stdout / stderr line-by-line into the Output pane.
- Build can be cancelled mid-run.

### Debugger
- Native Win32 debugging API (no GDB, no MinGW).
- INT3 (0xCC) breakpoints, resolved through `SymGetLineFromName64`.
- Continue / Step Over / Step Into / Stop.
- Stack frames and local variables read from the PDB through the DIA SDK.
- Hardware single-step via the EFLAGS TRAP flag.

### Terminal
- Integrated PowerShell pane with redirected stdin / stdout / stderr.
- Type a command, press Enter — the output streams into the same pane.

## Layout

```
+-----------+----------------+------------+
|           |                |            |
| Explorer  |     Editor     |  Debugger  |
|           |                |            |
+-----------+----------------+------------+
|        Output  /  Terminal             |
+--------------------------------------- +
|  status: file · lines · cursor offset  |
+--------------------------------------- +
```

The dock layout is built with `ImGui::DockBuilder`: Explorer on the left, Editor in the centre, Debugger on the right, Output and Terminal as tabs in the bottom strip. The Editor takes the largest area.

## Requirements

- Windows 10 or 11
- Visual Studio 2022 (MSVC, Windows SDK 10.0)
- CMake 3.20+
- vcpkg (manifest mode; `vcpkg.json` is checked in)
- The `cl.exe` / `link.exe` / Windows SDK are picked up at build time by `BuildRunner` via `vswhere` and `VsDevCmd.bat`.

## Build

The repository ships a CMake preset and a vcpkg manifest. From the project root:

```powershell
# Configure (first time only; vcpkg will install imgui automatically)
cmake -B build --preset msvc-debug

# Build
cmake --build build --config Debug
```

The resulting binary is `build/Debug/SF-IDE.exe`. On first launch it will look for a `CMakeLists.txt` up to four levels above the executable and pre-load `src/main.cpp` from that project so the editor is populated immediately.

If you prefer the classic generator flow without presets, point CMake at the vcpkg toolchain file directly:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE=C:/Users/silfo/vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

## Run

```powershell
.\build\Debug\SF-IDE.exe
```

The window starts maximised. DPI is per-monitor aware (V2) so the font, gutter, and dock layout scale correctly on 4K displays.

### Project layout convention

The Explorer and the terminal expect a project root. Either:

- launch the IDE from a directory that contains `CMakeLists.txt`, or
- use **File → Open Folder…** to pick one. The terminal will start in the chosen directory.

The current source file's directory is the default project root for **Build**.

## Tests

Unit tests are gated behind `SFIDE_BUILD_TESTS`:

```powershell
cmake -B build -DSFIDE_BUILD_TESTS=ON
cmake --build build --config Debug
.\build\Debug\test_editor.exe
.\build\Debug\test_editor_main.exe
.\build\Debug\test_debug.exe
.\build\Debug\test_build.exe
```

- `test_editor` — basic TextEdit: load / save, cursor, selection, undo / redo, line math, single-undo-entry-per-edit invariant.
- `test_editor_main` — end-to-end load / edit / undo / save / reload against a realistic C++ file (verifies that all the source survives the round trip).
- `test_debug` — drives a small child process through CreateProcess + WaitForDebugEvent, exercises breakpoint hit, single-step, restart.
- `test_build` — runs the CMake build pipeline against a small `build_test_project` fixture and checks the resulting executable.

All four should print `OK`.

## Keyboard

| Key                     | Action                       |
| ----------------------- | ---------------------------- |
| Arrow keys              | Move cursor (Shift = extend) |
| Home / End              | Move to line start / end     |
| PageUp / PageDown       | Move 20 lines                |
| Click                   | Position cursor              |
| Click on line number    | Toggle breakpoint            |
| Type                    | Insert text                  |
| Backspace / Delete      | Erase backward / forward     |
| Enter                   | New line                     |
| Tab                     | Insert 4 spaces              |
| Ctrl+A                  | Select all                   |
| Ctrl+C / Ctrl+X         | Copy / cut                   |
| Ctrl+V                  | Paste                        |
| Ctrl+Z / Ctrl+Y         | Undo / Redo                  |
| Ctrl+S                  | Save (current file)          |
| Ctrl+O                  | Open file…                   |
| F5 / Shift+F5           | Continue / Stop              |
| F10 / F11               | Step Over / Step Into        |

## Architecture

```
src/
  main.cpp            Win32 + D3D11 + ImGui host, all panels
  Editor.h / .cpp     Plain-text buffer, cursor, selection, undo
  BuildRunner.h/.cpp  CMake + MSVC pipeline (vswhere + VsDevCmd)
  DebugSession.h/.cpp Win32 debug API wrapper, INT3 breakpoints
  Terminal.h/.cpp     PowerShell with redirected pipes
  imgui_backends/     ImGui Win32 + DX11 backends
  app.rc, resource.h  Icon + Windows manifest
tests/
  test_editor.cpp
  test_editor_main.cpp
  test_debug.cpp
  test_build.cpp
```

The text editor stores a single UTF-8 `std::string` buffer with normalised `\n` line endings. Cursor and anchor are character offsets into the buffer. Undo snapshots the whole buffer on each user edit (small files, small IDE — the simplicity is the point). The renderer draws the gutter and code area on one ImDrawList using the dummy's `GetItemRectMin()` as the canvas origin; that makes text alignment independent of ImGui's internal cursor state.

## Non-goals

- No language server, no clangd, no IntelliSense.
- No plug-in system, no extension API.
- No multi-file find / replace, no refactor tools.
- No remote / SSH support.
- No Linux / macOS port.

These are intentional; SF-IDE is meant to be a small, native, debuggable starting point, not a clone of Visual Studio.
