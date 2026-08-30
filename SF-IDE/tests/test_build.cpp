// End-to-end test: drives BuildRunner on a tiny CMake project and verifies
// that the resulting executable is produced and runs.

#include "../src/BuildRunner.h"

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

} // namespace

int main()
{
    const std::string dir = "build_test_project";
    CreateDirectoryA(dir.c_str(), nullptr);

    if (!writeFile(dir + "/CMakeLists.txt",
                   "cmake_minimum_required(VERSION 3.20)\n"
                   "project(SFBuildTest LANGUAGES CXX)\n"
                   "set(CMAKE_CXX_STANDARD 20)\n"
                   "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
                   "add_executable(SFBuildTest main.cpp)\n"))
    {
        std::fprintf(stderr, "cannot write CMakeLists.txt\n");
        return 1;
    }
    if (!writeFile(dir + "/main.cpp",
                   "#include <iostream>\n"
                   "int main() {\n"
                   "    int value = 42;\n"
                   "    std::cout << \"Hello SF-IDE\\n\";\n"
                   "    std::cout << value << \"\\n\";\n"
                   "    return 0;\n"
                   "}\n"))
    {
        std::fprintf(stderr, "cannot write main.cpp\n");
        return 1;
    }

    std::atomic<int> lines{0};
    std::atomic<bool> done{false};
    BuildRunner runner;
    if (!runner.start(dir,
        [&](const std::string& line, bool isError)
        {
            lines.fetch_add(1);
            std::fprintf(isError ? stderr : stdout, "%s\n", line.c_str());
        }))
    {
        std::fprintf(stderr, "BuildRunner::start failed: %s\n",
                     runner.lastError().c_str());
        return 1;
    }

    // Wait up to 5 minutes (configure can be slow on first run).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto s = runner.state();
        if (s == BuildRunner::State::Succeeded ||
            s == BuildRunner::State::Failed    ||
            s == BuildRunner::State::Error)
        {
            done.store(true);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!done.load())
    {
        std::fprintf(stderr, "BuildRunner timed out\n");
        runner.cancel();
        return 1;
    }

    std::fprintf(stdout, "Final state: %d, lines: %d\n",
                 static_cast<int>(runner.state()), lines.load());

    if (runner.state() != BuildRunner::State::Succeeded)
    {
        std::fprintf(stderr, "build did not succeed\n");
        return 1;
    }

    const std::string exe = dir + "/build/Debug/SFBuildTest.exe";
    if (!fileExists(exe))
    {
        std::fprintf(stderr, "expected executable not found: %s\n", exe.c_str());
        return 1;
    }

    // Run it and capture "Hello SF-IDE" + "42". Replace forward slashes with
    // backslashes because system() goes through cmd.exe which only accepts
    // backslash path separators.
    std::string winPath = exe;
    for (char& c : winPath) if (c == '/') c = '\\';
    std::string cmd = "\"" + winPath + "\" > sfbuild_test_out.txt 2>&1";
    std::fprintf(stdout, "running: %s\n", cmd.c_str());
    int rc = system(cmd.c_str());
    std::fprintf(stdout, "exit code: %d\n", rc);

    std::ifstream out("sfbuild_test_out.txt");
    std::string s((std::istreambuf_iterator<char>(out)),
                  std::istreambuf_iterator<char>());
    std::fprintf(stdout, "--- output ---\n%s---\n", s.c_str());
    if (s.find("Hello SF-IDE") == std::string::npos ||
        s.find("42") == std::string::npos)
    {
        std::fprintf(stderr, "executable output did not match expectations\n");
        return 1;
    }

    std::printf("OK\n");
    return 0;
}
