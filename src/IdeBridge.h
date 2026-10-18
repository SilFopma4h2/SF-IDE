// IdeBridge - lets clil read and edit files through the editor.
//
// SF-IDE listens on a private Unix socket (in /tmp/sf-ide-<uid>, mode 0700)
// and passes its path to clil as SFIDE_SOCKET. Each connection carries one
// JSON request line and gets one JSON reply line:
//
//   {"op":"read","path":"/abs/file"}           -> {"ok":true,"open":true,"text":"..."}
//   {"op":"write","path":"/abs/file","text":…} -> {"ok":true} or {"ok":false,"error":"..."}
//   {"op":"state"}                              -> what the user has open (see App)
//
// A background thread accepts connections; requests are handed to the GUI
// thread, which owns the documents, through next() / reply().

#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

class IdeBridge {
public:
    struct Request {
        int fd = -1;
        std::string op, path, text;
    };

    ~IdeBridge();
    bool start();
    void stop();
    const std::string& socketPath() const { return m_path; }

    bool next(Request& out);                              // GUI thread
    static void reply(Request& r, const std::string& json);  // sends and closes

    // JSON helpers for building replies.
    static std::string quote(const std::string& s);
    // Parses a flat JSON object of string values; other value types are skipped.
    static bool parseRequest(const std::string& line, Request& out);

private:
    void acceptLoop();

    int m_listen = -1;
    std::string m_path;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::mutex m_mutex;
    std::deque<Request> m_queue;
};
