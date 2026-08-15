#include "cppmcp/process.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace cppmcp {

Process::~Process() {
    terminate();
}

#ifdef _WIN32
namespace {
// Generate a unique named-pipe path. Anonymous pipes created by CreatePipe are
// not overlapped-capable, so asio cannot drive them. We instead create named
// pipes with FILE_FLAG_OVERLAPPED on the parent side (the end we hand to asio)
// and connect the child side via a synchronous, inheritable CreateFile handle.
std::string unique_pipe_name(const char* suffix) {
    static std::atomic<unsigned long long> counter{0};
    unsigned long long n = counter.fetch_add(1, std::memory_order_relaxed);
    return std::string(R"(\\.\pipe\cppmcp-)") + suffix + "-" +
           std::to_string(GetCurrentProcessId()) + "-" + std::to_string(n);
}

// Create an overlapped named-pipe server end + a synchronous inheritable client
// end. Returns the server (parent, overlapped) handle in `server` and the
// client (child, sync, inheritable) handle in `client`. `inbound=true` means
// data flows child->parent (server is read end, client is write end).
bool make_overlapped_pair(bool inbound, HANDLE& server, HANDLE& client) {
    std::string name = unique_pipe_name(inbound ? "out" : "in");
    DWORD access = inbound ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND;
    server = CreateNamedPipeA(
        name.c_str(),
        access | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        64 * 1024, 64 * 1024, 0, nullptr);
    if (server == INVALID_HANDLE_VALUE) {
        return false;
    }
    // Arm an overlapped ConnectNamedPipe so the connection completes when the
    // child side opens its end. ERROR_PIPE_CONNECTED (already connected) is fine.
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) { CloseHandle(server); return false; }
    BOOL connected = ConnectNamedPipe(server, &ov);
    DWORD err = connected ? 0 : GetLastError();
    if (!connected && err != ERROR_IO_PENDING && err != ERROR_PIPE_CONNECTED) {
        CloseHandle(ov.hEvent); CloseHandle(server); return false;
    }
    // Child end: synchronous (no FILE_FLAG_OVERLAPPED), inheritable.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    DWORD child_access = inbound ? GENERIC_WRITE : GENERIC_READ;
    client = CreateFileA(name.c_str(), child_access, 0, &sa, OPEN_EXISTING, 0, nullptr);
    if (client == INVALID_HANDLE_VALUE) {
        // Could be ERROR_PIPE_BUSY if the server is still arming; retry once.
        DWORD le = GetLastError();
        if (le == ERROR_PIPE_BUSY) {
            WaitNamedPipeA(name.c_str(), 1000);
            client = CreateFileA(name.c_str(), child_access, 0, &sa, OPEN_EXISTING, 0, nullptr);
        }
        if (client == INVALID_HANDLE_VALUE) {
            CancelIoEx(server, &ov);
            WaitForSingleObjectEx(ov.hEvent, 100, FALSE);
            CloseHandle(ov.hEvent); CloseHandle(server); return false;
        }
    }
    // Wait for the ConnectNamedPipe to finish (it completes once client connected).
    if (err == ERROR_IO_PENDING) {
        WaitForSingleObjectEx(ov.hEvent, 1000, FALSE);
    }
    CloseHandle(ov.hEvent);
    return true;
}

// Windows command-line quoting per the argv parsing rules documented at
// <CreateProcess>: backslashes are literal unless they precede a quote; a
// quoted argument escapes embedded quotes with backslashes.
std::string quote_windows_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;  // no special chars: no quoting needed
    }
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            backslashes = 0;
            out += c;
        }
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

// Build an environment block ("KEY=VALUE\0...\0\0") = parent env + overrides.
// Returns an empty vector on failure, in which case the child inherits.
std::vector<char> build_environment(const std::map<std::string, std::string>& overrides) {
    if (overrides.empty()) {
        return {};
    }
    std::map<std::string, std::string> env;
    LPCH parent = GetEnvironmentStringsA();
    if (parent) {
        for (LPCH p = parent; *p; ) {
            std::string entry(p);
            auto eq = entry.find('=');
            if (eq != std::string::npos && eq > 0) {
                env[entry.substr(0, eq)] = entry.substr(eq + 1);
            }
            p += entry.size() + 1;
        }
        FreeEnvironmentStringsA(parent);
    }
    for (const auto& [k, v] : overrides) {
        env[k] = v;
    }
    std::vector<char> block;
    for (const auto& [k, v] : env) {
        std::string entry = k + "=" + v;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back('\0');
    }
    block.push_back('\0');
    return block;
}
}  // namespace
#endif

void Process::spawn(const std::string& executable, std::vector<std::string> args,
                    const std::map<std::string, std::string>& env, const std::string& working_dir) {
#ifdef _WIN32
    // Parent ends are overlapped (for asio); child ends are synchronous + inheritable.
    HANDLE stdin_server = nullptr;   // parent writes here (OUTBOUND)
    HANDLE stdin_client = nullptr;   // child reads from here
    HANDLE stdout_server = nullptr;  // parent reads here (INBOUND)
    HANDLE stdout_client = nullptr;  // child writes to here

    if (!make_overlapped_pair(false /*outbound*/, stdin_server, stdin_client) ||
        !make_overlapped_pair(true  /*inbound*/, stdout_server, stdout_client)) {
        if (stdin_server) CloseHandle(stdin_server);
        if (stdin_client) CloseHandle(stdin_client);
        throw std::runtime_error("Process: overlapped pipe creation failed");
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    // GUI/service parents may have no console: NUL keeps the child's stderr
    // writes from failing (spec-recommended diagnostic channel survives).
    HANDLE err_handle = GetStdHandle(STD_ERROR_HANDLE);
    if (!err_handle || err_handle == INVALID_HANDLE_VALUE) {
        err_handle = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        owned_null_handle_ = err_handle;
    }
    si.hStdError = err_handle;
    si.hStdInput = stdin_client;
    si.hStdOutput = stdout_client;
    si.dwFlags |= STARTF_USESTDHANDLES;
    ZeroMemory(&pi, sizeof(pi));

    std::string cmdline = quote_windows_arg(executable);
    for (const auto& a : args) {
        cmdline += " ";
        cmdline += quote_windows_arg(a);
    }
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');

    std::vector<char> env_block = build_environment(env);
    const char* cwd = working_dir.empty() ? nullptr : working_dir.c_str();

    BOOL ok = CreateProcessA(
        nullptr,
        cmd_buf.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        env_block.empty() ? nullptr : env_block.data(),
        cwd,
        &si, &pi);

    // Child ends are owned by the child now; close our copies.
    CloseHandle(stdin_client);
    CloseHandle(stdout_client);

    if (!ok) {
        CloseHandle(stdin_server);
        CloseHandle(stdout_server);
        if (owned_null_handle_) { CloseHandle(owned_null_handle_); owned_null_handle_ = nullptr; }
        throw std::runtime_error("Process: CreateProcess failed: " + executable);
    }

    CloseHandle(pi.hThread);
    child_process_ = pi.hProcess;
    stdin_write_ = stdin_server;   // overlapped, parent side
    stdout_read_ = stdout_server;  // overlapped, parent side
#else
    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        throw std::runtime_error("Process: pipe() failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("Process: fork() failed");
    }
    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) {
            _exit(127);
        }
        if (!env.empty()) {
            for (const auto& [k, v] : env) {
                setenv(k.c_str(), v.c_str(), 1);
            }
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(executable.c_str(), argv.data());
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    child_pid_ = pid;
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
#endif
}

void Process::close_stdin() {
#ifdef _WIN32
    if (stdin_write_) {
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
#else
    if (stdin_fd_ >= 0) {
        close(stdin_fd_);
        stdin_fd_ = -1;
    }
#endif
}

int Process::exit_code() const {
    return exit_code_;
}

bool Process::wait_for_exit(int timeout_ms) {
#ifdef _WIN32
    if (!child_process_) {
        return true;
    }
    DWORD r = WaitForSingleObject(child_process_, static_cast<DWORD>(timeout_ms));
    if (r == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(child_process_, &code);
        exit_code_ = static_cast<int>(code);
        CloseHandle(child_process_);
        child_process_ = nullptr;
        return true;
    }
    return false;
#else
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status;
        pid_t r = waitpid(child_pid_, &status, WNOHANG);
        if (r == child_pid_) {
            exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            child_pid_ = -1;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
#endif
}

void Process::terminate() {
#ifdef _WIN32
    if (stdin_write_) { CloseHandle(stdin_write_); stdin_write_ = nullptr; }
    if (stdout_read_) { CloseHandle(stdout_read_); stdout_read_ = nullptr; }
    if (child_process_) {
        TerminateProcess(child_process_, 0);
        WaitForSingleObject(child_process_, 3000);
        CloseHandle(child_process_);
        child_process_ = nullptr;
    }
    if (owned_null_handle_) { CloseHandle(owned_null_handle_); owned_null_handle_ = nullptr; }
#else
    if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; }
    if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        // Bounded grace: escalate to SIGKILL when the child ignores SIGTERM,
        // so terminate() can never block the caller indefinitely.
        for (int i = 0; i < 50; ++i) {
            int status;
            pid_t r = waitpid(child_pid_, &status, WNOHANG);
            if (r == child_pid_) {
                child_pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(child_pid_, SIGKILL);
        int status;
        waitpid(child_pid_, &status, 0);
        child_pid_ = -1;
    }
#endif
}

} // namespace cppmcp
