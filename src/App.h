// App - the IDE: fixed panel layout, menus, commands and the glue between
// the editor, the build, the terminals and clil.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "BuildRunner.h"
#include "Document.h"
#include "EditorView.h"
#include "FileTree.h"
#include "IdeBridge.h"
#include "TerminalView.h"

struct ImFont;

struct Settings {
    float uiFontSize = 13.5f;
    float editorFontSize = 13.0f;
    bool  dark = true;
    std::string clilPath;      // empty: bundled / PATH
    std::string clilArgs;      // extra arguments, e.g. "-m qwen3-coder"
    std::string lmHost;        // LMSTUDIO_HOST for clil, empty = default
    int   winW = 1440, winH = 900;
    float leftW = 250, rightW = 460, bottomH = 250;
    bool  showLeft = true, showRight = true, showBottom = true;
    bool  saveBeforeBuild = true;
    std::vector<std::string> recent;
    std::string lastProject;

    void load();
    void save() const;
};

class App {
public:
    App();
    ~App();

    Settings settings;
    ImFont* uiFont = nullptr;
    ImFont* monoFont = nullptr;
    bool fontsDirty = false;      // main loop rebuilds the font atlas
    bool quitRequested = false;
    bool quitConfirmed = false;

    void openFolder(const std::string& path);
    void openFile(const std::string& path, int line = 0, int col = 0);
    void openPath(const std::string& path);   // file or folder (drag & drop, argv)

    void frame();          // draw one frame
    void tick();           // periodic work (~1 s): file changes, tree, git branch
    void pump();           // drain process output; returns quickly
    bool busy() const;     // something is running that updates by itself
    void requestQuit();
    void saveSettings();

private:
    // Layout & panels
    void drawMenuBar();
    void drawToolbarButtons();
    void drawExplorer(float w, float h);
    void drawEditorArea(float w, float h);
    void drawWelcome();
    void drawFindBar(Document& doc);
    void drawBottomPanel(float w, float h);
    void drawProblems();
    void drawOutput();
    void drawAssistant(float w, float h);
    void drawStatusBar(float w, float h);
    void drawPopups();
    void drawQuickOpen();
    void drawSettings();
    void drawLanguagePicker();
    bool splitter(const char* id, bool vertical, float x, float y, float len, float& value, float sign,
                  float minV, float maxV);
    void panelHeader(const char* title);

    // Commands
    void handleShortcuts();
    Document* activeDoc();
    void newFile();
    bool saveDoc(Document& doc, bool forceDialog = false);
    void saveAll();
    void closeDoc(int index, bool force = false);
    void build(int then, bool currentFile = false);
    bool projectMode() const;                 // a project build / run command applies
    void runCurrentFile() { build(AfterRun, true); }
    void debugCurrentFile();
    void startDebug();                        // F5: project or current file, whichever applies
    void debugPython(const std::string& file);
    std::string expandTemplate(const std::string& tpl, const std::string& file) const;
    void collectRunProblems();
    void debugCommand(const char* lldbCmd, const char* pdbCmd);
    void runProgram();
    void debugProgram();
    void stopAll();
    void startShell(const std::string& cwd = {});
    void startClil();
    void ensureClil();
    void sendToClil(const std::string& prompt, bool submit);
    void askClilAboutSelection(const char* verb);
    void askClilFixProblems();
    void applyDiagnostics();
    void followDebugger();
    void handleBridge(IdeBridge::Request& r);
    Document* findDoc(const std::string& path);
    void loadProjectConfig();
    void saveProjectConfig();
    std::string resolvedRunCommand() const;
    std::string findClil() const;
    void addRecent(const std::string& path);
    void refreshGitBranch();
    std::string rel(const std::string& path) const;
    void showMessage(const std::string& title, const std::string& text);
    void handleTreeAction(const FileTree::Action& a);

    // State
    std::string m_root;
    FileTree m_tree;
    std::vector<std::unique_ptr<Document>> m_docs;
    std::vector<Document*> m_conflicts;   // changed on disk while they have unsaved edits
    int m_active = -1;
    int m_selectTab = -1;          // tab to select on the next frame
    EditorView m_editor;

    BuildRunner m_build;
    std::vector<BuildRunner::Line> m_buildLines;
    std::vector<Problem> m_problems;
    bool m_buildScroll = false;
    enum After { AfterNone, AfterRun, AfterDebug } m_after = AfterNone;
    std::string m_lastExe;
    ProjectCommands m_cmds;        // detected
    std::string m_buildCmd, m_runCmd, m_runArgs;   // project config (.sfide), empty = detected
    bool m_singleFileBuild = false;
    std::string m_fileRunCmd;      // run command for the current-file workflow
    std::string m_fileRunDir;
    bool m_debugPdb = false;       // the Debug tab runs pdb (Python) rather than lldb / gdb
    bool m_runWasRunning = false;
    bool m_openLangPicker = false;

    TerminalView m_shell, m_clil, m_run, m_debug;
    IdeBridge m_bridge;   // clil reads / edits files through the editor
    enum BottomTab { TabProblems, TabOutput, TabTerminal, TabRun, TabDebug } m_bottomTab = TabTerminal;
    int m_selectBottomTab = -1;

    // Find bar
    bool m_findOpen = false, m_replaceOpen = false, m_focusFind = false;
    char m_findBuf[256] = {};
    char m_replaceBuf[256] = {};

    // Popups
    bool m_openGoto = false, m_openQuick = false, m_openSettings = false, m_openAbout = false;
    char m_gotoBuf[32] = {};
    char m_quickBuf[256] = {};
    int  m_quickSel = 0;
    std::vector<std::string> m_quickFiles;
    std::string m_pendingClose;    // path of a dirty doc waiting for "save?" answer
    int  m_pendingCloseIndex = -1;
    bool m_openCloseConfirm = false;
    bool m_openQuitConfirm = false;
    std::string m_messageTitle, m_messageText;
    bool m_openMessage = false;
    // Name prompt (new file / folder / rename / path entry when there is no native dialog)
    enum NamePrompt { PromptNone, PromptNewFile, PromptNewFolder, PromptRename, PromptOpenFolder, PromptOpenFile, PromptSaveAs } m_prompt = PromptNone;
    std::string m_promptBase;
    char m_promptBuf[1024] = {};
    bool m_openPrompt = false;
    std::string m_deletePath;
    bool m_openDelete = false;
    Document* m_saveAsDoc = nullptr;
    // Settings editor buffers
    char m_setClilPath[512] = {}, m_setClilArgs[512] = {}, m_setHost[128] = {};
    char m_setBuild[1024] = {}, m_setRun[1024] = {}, m_setArgs[512] = {};

    std::string m_gitBranch;
    std::string m_status;          // transient status-bar message
    double m_statusUntil = 0;
    bool m_clilStarted = false;
    std::string m_clilMissingReason;
};
