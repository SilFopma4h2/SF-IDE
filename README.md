# SF-IDE

A small, native C/C++ IDE for macOS, with the [clil](https://github.com/SilFopma4h2/clil) coding agent built in.
Written in C++20 with GLFW, OpenGL and Dear ImGui. No Electron, no web view, no language server: it starts
instantly and uses no CPU while you are not typing.

```
+-----------+---------------------------------+------------------+
| Explorer  |  main.cpp  ×   util.h           |  clil  ready     |
|           |                                 |  [Explain] [Fix] |
|  src/     |   1  #include <cstdio>          |                  |
|   main.cpp|   2                             |  › explain @src/ |
|   util.h  |   3  int main() { ... }         |  ...             |
|           +---------------------------------+                  |
|           | PROBLEMS  BUILD  TERMINAL  RUN  DEBUG              |
|           | $ cmake --build build                              |
+-----------+----------------------------------------------------+
| ⎇ main   ● 0  ▲ 0   Build succeeded        Ln 12, Col 5  C/C++ |
+----------------------------------------------------------------+
```

## What's in it

**Fixed layout.** Explorer on the left, the editor in the middle, the build / terminal panel below it and clil on
the right. Panels cannot float, undock or pile up; you can only resize them with the dividers or hide them
(⌘1, ⌘J, ⌘I). Panel sizes, theme and font size are remembered.

**Editor**
- Draws only the visible lines, so large files scroll smoothly.
- Syntax highlighting for C, C++, Objective-C and friends, plus CMake / Makefile / shell / Markdown.
- macOS keys: ⌘←/→ line start/end, ⌥←/→ by word, ⌘↑/↓ file start/end, ⌥⌫ / ⌘⌫ delete word / to line start.
- Auto-indent, Tab / ⇧Tab on selections, ⌘/ toggle comment, ⌘D duplicate line, ⌘] / ⌘[ indent.
- Grouped undo / redo (undoing back to the saved text marks the file clean again).
- Find & replace (⌘F, ⌥⌘F), go to line (⌘L), quick open (⌘P), bracket matching.
- UTF-8 throughout; CRLF files are saved as CRLF again.
- Files changed on disk (by clil, git or another editor) reload automatically; if you have unsaved edits you
  get a choice.

**Build, run, debug**
- Detects CMake, Make, Swift and Meson projects; a single `.c` / `.cpp` file builds on its own too.
- ⌘B builds, ⌘R builds and runs, F5 builds and starts the debugger.
- Compiler errors and warnings appear in **Problems**, as squiggles in the editor and as dots in the gutter;
  click one to jump to it.
- Programs run in a real terminal (the **Run** tab), so `scanf` and colours work.
- Debugging uses `lldb` (or `gdb` on Linux) in the **Debug** tab. Click a line number to set a breakpoint;
  the line where the program stopped is highlighted in the editor and follows F10 / F11.
- Build and run commands can be changed per project in **Project Settings** (saved in `.sfide`).

**Terminal.** A real terminal emulator (xterm compatible: 256 / true colour, alternate screen, bracketed paste,
scrollback) running your login shell. ⌃\` toggles it.

**clil, built in.** clil runs in the right-hand panel in the project folder, with its full interface
(approvals, `/models`, `/undo`, history, ...). The IDE adds:
- **live edits**: clil's `edit` / `write` go straight into the editor. The changed lines light up in the open
  tab, your cursor stays where it is, the change is one ⌘Z away, and the file is saved so builds see it.
  clil's `read` sees the editor's text, unsaved edits included;
- **context**: each message to clil says which file you have open, the cursor line and the selected text,
  so "explain this" or "fix the selection" just work;
- buttons and menu items that send the right prompt: *Explain*, *Find bugs*, *Write tests* (for the
  selection or the current file), *Fix build errors* (sends the problem list), *Commit message*;
- right-click in the editor, the explorer or a problem to ask clil about it.

Under the hood SF-IDE listens on a private Unix socket (`/tmp/sf-ide-<uid>/`, only your user can connect)
and passes it to clil as `SFIDE_SOCKET`; see `src/IdeBridge.h` for the small JSON protocol.

The build bundles clil from the `external/clil` submodule, so `SF-IDE.app` contains its own copy.
clil needs [LM Studio](https://lmstudio.ai) with its server running; set the server address, model or other
clil options under **Settings** (⌘,).

## Requirements

- macOS 11 or later (Apple Silicon or Intel)
- Xcode command line tools: `xcode-select --install`
- CMake 3.20+: `brew install cmake`
- GLFW is used from Homebrew when installed (`brew install glfw`), otherwise CMake downloads it.

Linux works too (X11 / Wayland through GLFW): install `cmake`, a C++ compiler and `libglfw3-dev libgl-dev`.

## Build

```sh
git clone --recursive https://github.com/SilFopma4h2/SF-IDE.git
cd SF-IDE
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
open build/SF-IDE.app          # Linux: ./build/SF-IDE
```

Already cloned without `--recursive`? Run `git submodule update --init` first, so clil is bundled.

- Universal binary (Apple Silicon + Intel): add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`.
- Install: drag `build/SF-IDE.app` to `/Applications`.
- Tests: `ctest --test-dir build`.

You can also open a folder or file directly: `open -a SF-IDE ~/code/myproject`, drag it onto the window, or
pass it as an argument.

The app is not signed. The first time, right-click it in Finder and choose **Open**.

## Keys

| Key | Action |
|---|---|
| ⌘P | Quick open |
| ⌘S / ⇧⌘S / ⌥⌘S | Save / save as / save all |
| ⌘W | Close tab (⇧⌘[ / ⇧⌘] switch tabs) |
| ⌘F / ⌥⌘F / ⌘G | Find / replace / find next |
| ⌘L | Go to line |
| ⌘/ | Toggle comment |
| ⌘B / ⌘R / ⇧⌘R | Build / build & run / run without building |
| ⌘. | Stop build, program or debugger |
| F5 / F9 / F10 / F11 | Debug or continue / breakpoint / step over / step into |
| ⌘I | Show clil |
| ⌃\` | Toggle terminal |
| ⌘1 / ⌘J | Toggle explorer / bottom panel |
| ⌘+ / ⌘- / ⌘0 | Text size |
| ⌘, | Settings |

On Linux use Ctrl instead of ⌘.

## Code

```
src/
  main.cpp          GLFW / OpenGL host, fonts (Retina), event-driven main loop
  App.*             layout, menus, commands, glue between editor, build, terminals and clil
  Document.*        text buffer, line index, undo, file I/O, change detection
  EditorView.*      editor drawing and input
  Syntax.*          single-line tokenizer for highlighting
  Vt.*              xterm-compatible terminal emulator
  Pty.*             processes in a pseudo terminal (shell, clil, lldb, your program)
  TerminalView.*    Pty + Vt as a panel
  IdeBridge.*       socket through which clil reads and edits files live in the editor
  BuildRunner.*     background builds, compiler diagnostic parsing, project detection
  FileTree.*        cached project explorer
  Theme.*           dark and light colour themes
external/clil       clil (git submodule), built and bundled into the app
third_party/imgui   Dear ImGui 1.91.9b with the GLFW + OpenGL3 backends
tests/tests.cpp     unit tests for the buffer, terminal, pty, tokenizer and diagnostics
```

Why it is light: the main loop sleeps in `glfwWaitEvents` until there is input or process output, the editor
only tokenises and draws visible lines, the explorer re-reads a folder only when its modification time changes,
and there is no background indexing.

## Not included

No language server / code completion, no plug-ins, no multi-file search, no Git UI. SF-IDE 1.x (Windows,
Direct3D, Win32 debugger) is in the git history.
