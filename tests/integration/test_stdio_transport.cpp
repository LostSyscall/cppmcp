#include <gtest/gtest.h>

#include "test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace cppmcp::testing;
using json = nlohmann::json;

// --- StdioProcess: cross-platform child process with redirected stdin/stdout ---
class StdioProcess {
public:
    StdioProcess() = default;
    ~StdioProcess() { terminate(); }

    void spawn(const std::string& executable_path) {
#ifdef _WIN32
        // Create pipes for stdin/stdout
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE stdin_read, stdin_write;
        CreatePipe(&stdin_read, &stdin_write, &sa, 0);
        SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

        HANDLE stdout_read, stdout_write;
        CreatePipe(&stdout_read, &stdout_write, &sa, 0);
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = stdin_read;
        si.hStdOutput = stdout_write;
        si.dwFlags |= STARTF_USESTDHANDLES;

        ZeroMemory(&pi, sizeof(pi));

        BOOL success = CreateProcessA(
            nullptr,
            const_cast<LPSTR>(executable_path.c_str()),
            nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW,
            nullptr, nullptr,
            &si, &pi
        );

        // Close handles we don't need
        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        CloseHandle(pi.hThread);

        if (!success) {
            CloseHandle(stdin_write);
            CloseHandle(stdout_read);
            throw std::runtime_error("Failed to create process: " + executable_path);
        }

        child_process_ = pi.hProcess;
        child_pid_ = pi.dwProcessId;
        stdin_write_ = stdin_write;
        stdout_read_ = stdout_read;
#else
        // Create pipes
        int stdin_pipe[2];
        int stdout_pipe[2];
        pipe(stdin_pipe);
        pipe(stdout_pipe);

        child_pid_ = fork();
        if (child_pid_ == 0) {
            // Child process
            close(stdin_pipe[1]);
            close(stdout_pipe[0]);
            dup2(stdin_pipe[0], STDIN_FILENO);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            close(stdin_pipe[0]);
            close(stdout_pipe[1]);
            execl(executable_path.c_str(), executable_path.c_str(), nullptr);
            _exit(1);
        }

        // Parent process
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        stdin_fd_ = stdin_pipe[1];
        stdout_fd_ = stdout_pipe[0];
#endif
    }

    void write_line(const std::string& line) {
        std::string data = line + "\n";
#ifdef _WIN32
        DWORD written;
        WriteFile(stdin_write_, data.c_str(), static_cast<DWORD>(data.size()), &written, nullptr);
#else
        write(stdin_fd_, data.c_str(), data.size());
#endif
    }

    std::string read_line(std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
#ifdef _WIN32
        // Read from stdout pipe with timeout
        char buf[4096];
        std::string result;
        DWORD available = 0;

        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &available, nullptr);
            if (available > 0) {
                DWORD read_bytes = 0;
                ReadFile(stdout_read_, buf, sizeof(buf) - 1, &read_bytes, nullptr);
                result.append(buf, read_bytes);
                auto nl = result.find('\n');
                if (nl != std::string::npos) {
                    return result.substr(0, nl);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("StdioProcess: read timeout");
#else
        // Use poll or select with timeout
        fd_set fds;
        struct timeval tv;
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

        std::string result;
        while (true) {
            FD_ZERO(&fds);
            FD_SET(stdout_fd_, &fds);
            int ret = select(stdout_fd_ + 1, &fds, nullptr, nullptr, &tv);
            if (ret <= 0) throw std::runtime_error("StdioProcess: read timeout");

            char buf[4096];
            ssize_t n = read(stdout_fd_, buf, sizeof(buf) - 1);
            if (n <= 0) throw std::runtime_error("StdioProcess: EOF or error");
            result.append(buf, n);
            auto nl = result.find('\n');
            if (nl != std::string::npos) {
                return result.substr(0, nl);
            }
        }
#endif
    }

    json read_json_response(std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::string line = read_line(timeout);
        // Strip trailing \r if present
        while (!line.empty() && line.back() == '\r') line.pop_back();
        return json::parse(line);
    }

    void close_stdin() {
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

    // Wait (up to timeout_ms) for the child to exit on its own. Returns true
    // if it exited; on success the handle is reaped so terminate() is a no-op.
    bool wait_for_exit(int timeout_ms) {
#ifdef _WIN32
        if (!child_process_) return true;
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

    void terminate() {
#ifdef _WIN32
        if (stdin_write_) CloseHandle(stdin_write_);
        if (stdout_read_) CloseHandle(stdout_read_);
        if (child_process_) {
            TerminateProcess(child_process_, 0);
            WaitForSingleObject(child_process_, 3000);
            CloseHandle(child_process_);
            child_process_ = nullptr;
        }
#else
        if (stdin_fd_ >= 0) close(stdin_fd_);
        if (stdout_fd_ >= 0) close(stdout_fd_);
        if (child_pid_ > 0) {
            kill(child_pid_, SIGTERM);
            int status;
            waitpid(child_pid_, &status, 0);
            child_pid_ = -1;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE child_process_ = nullptr;
    DWORD child_pid_ = 0;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
#else
    pid_t child_pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
#endif
};

// --- Test Fixture ---
class StdioTransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Find the stdio server executable
        stdio_server_path_ = find_stdio_server_executable();
        process_ = std::make_unique<StdioProcess>();
        process_->spawn(stdio_server_path_);
        // Give the server a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        process_->terminate();
    }

    std::string find_stdio_server_executable() {
#ifdef CPPMCP_STDIO_SERVER_PATH
        return CPPMCP_STDIO_SERVER_PATH;
#else
        // Fallback: search common locations
        std::vector<std::string> candidates;
#ifdef _WIN32
        candidates.push_back("cppmcp_test_stdio_server.exe");
        candidates.push_back("./cppmcp_test_stdio_server.exe");
#else
        candidates.push_back("cppmcp_test_stdio_server");
        candidates.push_back("./cppmcp_test_stdio_server");
#endif
        for (const auto& path : candidates) {
            if (std::ifstream(path).good()) return path;
        }
#ifdef _WIN32
        return "cppmcp_test_stdio_server.exe";
#else
        return "cppmcp_test_stdio_server";
#endif
#endif
    }

    std::unique_ptr<StdioProcess> process_;
    std::string stdio_server_path_;
};

TEST_F(StdioTransportTest, InitializeAndPing) {
    process_->write_line(make_initialize_request(1).dump());
    auto init_resp = process_->read_json_response();

    EXPECT_EQ(init_resp["jsonrpc"], "2.0");
    EXPECT_EQ(init_resp["id"], 1);
    EXPECT_TRUE(init_resp.contains("result"));
    EXPECT_EQ(init_resp["result"]["protocolVersion"], "2025-03-26");
    EXPECT_EQ(init_resp["result"]["serverInfo"]["name"], "test_stdio_server");

    process_->write_line(make_initialized_notification().dump());

    process_->write_line(make_ping_request(2).dump());
    auto ping_resp = process_->read_json_response();
    EXPECT_EQ(ping_resp["id"], 2);
    EXPECT_TRUE(ping_resp.contains("result"));
}

TEST_F(StdioTransportTest, ToolsCall) {
    process_->write_line(make_initialize_request(1).dump());
    auto init_resp = process_->read_json_response();

    process_->write_line(make_initialized_notification().dump());

    process_->write_line(make_tools_call_request(3, "echo", json{{"message", "stdio test"}}).dump());
    auto call_resp = process_->read_json_response();

    EXPECT_EQ(call_resp["id"], 3);
    EXPECT_EQ(call_resp["result"]["content"][0]["text"], "stdio test");
    EXPECT_FALSE(call_resp["result"]["isError"].get<bool>());
}

TEST_F(StdioTransportTest, ToolsList) {
    process_->write_line(make_initialize_request(1).dump());
    auto init_resp = process_->read_json_response();

    process_->write_line(make_initialized_notification().dump());

    process_->write_line(make_tools_list_request(2).dump());
    auto list_resp = process_->read_json_response();

    EXPECT_EQ(list_resp["id"], 2);
    EXPECT_TRUE(list_resp["result"]["tools"].is_array());
    EXPECT_GE(list_resp["result"]["tools"].size(), 2);
}

TEST_F(StdioTransportTest, MalformedJson) {
    process_->write_line("this is not json");
    auto error_resp = process_->read_json_response();

    EXPECT_TRUE(error_resp.contains("error"));
    EXPECT_EQ(error_resp["error"]["code"], -32700); // PARSE_ERROR
}

// Regression (#13): closing stdin must make the server shut down. Previously
// the io_context work_guard was never released on EOF, so the process hung.
TEST_F(StdioTransportTest, ExitsOnStdinEof) {
    process_->write_line(make_initialize_request(1).dump());
    auto init_resp = process_->read_json_response();
    ASSERT_EQ(init_resp["id"], 1);

    process_->close_stdin();
    bool exited = process_->wait_for_exit(5000);
    EXPECT_TRUE(exited);
}