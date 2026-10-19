// IdeUtil - small helpers shared by the IDE (not named Util.h: on macOS
// that would shadow <util.h>, which declares forkpty)
// paths, shell quoting, finding programs.

#pragma once

#include <string>
#include <vector>

std::string shellQuote(const std::string& s);
std::string homeDir();
std::string configDir();                       // ~/Library/Application Support/SF-IDE or ~/.config/sf-ide
std::string executableDir();                   // directory of the running binary
std::string findProgram(const std::string& name, const std::vector<std::string>& extraDirs = {});
std::string relativePath(const std::string& path, const std::string& base);
std::string userShell();
bool        readTextFile(const std::string& path, std::string& out);
bool        writeTextFile(const std::string& path, const std::string& data);
// Runs a command and returns its stdout (first `limit` bytes). Used for
// native dialogs (osascript / zenity) and `git` queries.
std::string runCapture(const std::string& command, int* exitCode = nullptr, size_t limit = 1 << 20);
