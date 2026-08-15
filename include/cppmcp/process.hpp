#pragma once

#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace cppmcp {

// Cross-platform child process with redirected stdin/stdout. Spawn a server as
// a child process and expose the parent-side handles/file descriptors for I/O.
// Reading/writing is the caller's responsibility (e.g. StdioClientTransport).
class Process {
public:
    Process() = default;
    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Launch `executable` with `args`. `env` entries override (or add to) the
    // parent environment; `working_dir` ("" = inherit) is the child's cwd. On
    // success the parent holds the write end of the child's stdin pipe and the
    // read end of its stdout pipe.
    void spawn(const std::string& executable, std::vector<std::string> args = {},
               const std::map<std::string, std::string>& env = {},
               const std::string& working_dir = {});

#ifdef _WIN32
    HANDLE stdin_write() const { return stdin_write_; }
    HANDLE stdout_read() const { return stdout_read_; }
#else
    int stdin_fd() const { return stdin_fd_; }
    int stdout_fd() const { return stdout_fd_; }
#endif

    // Close the parent's stdin write handle (signals EOF to the child).
    void close_stdin();

#ifdef _WIN32
    // Relinquish ownership of the parent-side overlapped pipe handles so the
    // caller (an asio stream_handle) owns them. After this, terminate() will
    // NOT close them (the stream_handle is responsible).
    void release_stdin() { stdin_write_ = nullptr; }
    void release_stdout() { stdout_read_ = nullptr; }
#endif

    // Wait up to timeout_ms for the child to exit. Returns true if it exited
    // (and was reaped, so terminate() becomes a no-op). exit_code() is valid
    // after a true return.
    bool wait_for_exit(int timeout_ms);
    int exit_code() const;

    // Forcefully terminate the child and close all handles. Idempotent. Never
    // blocks indefinitely (escalates to SIGKILL / TerminateProcess).
    void terminate();

private:
#ifdef _WIN32
    HANDLE child_process_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE owned_null_handle_ = nullptr;  // stderr sink for GUI/service parents
#else
    pid_t child_pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
#endif
    int exit_code_ = -1;
};

} // namespace cppmcp
