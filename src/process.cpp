#include "cppmcp/process.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace cppmcp {

Process::~Process() {
    terminate();
}

void Process::spawn(const std::string& executable, std::vector<std::string> args) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;

    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0) ||
        !SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("Process: CreatePipe(stdin) failed");
    }
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        throw std::runtime_error("Process: CreatePipe(stdout) failed");
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.dwFlags |= STARTF_USESTDHANDLES;
    ZeroMemory(&pi, sizeof(pi));

    std::string cmdline = "\"" + executable + "\"";
    for (const auto& a : args) {
        cmdline += " \"";
        cmdline += a;
        cmdline += "\"";
    }
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr,
        cmd_buf.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

    if (!ok) {
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        throw std::runtime_error("Process: CreateProcess failed: " + executable);
    }

    CloseHandle(pi.hThread);
    child_process_ = pi.hProcess;
    stdin_write_ = stdin_write;
    stdout_read_ = stdout_read;
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

bool Process::wait_for_exit(int timeout_ms) {
#ifdef _WIN32
    if (!child_process_) {
        return true;
    }
    DWORD r = WaitForSingleObject(child_process_, static_cast<DWORD>(timeout_ms));
    if (r == WAIT_OBJECT_0) {
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
#else
    if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; }
    if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        int status;
        waitpid(child_pid_, &status, 0);
        child_pid_ = -1;
    }
#endif
}

} // namespace cppmcp
