#include <gtest/gtest.h>

#include "test_helpers.hpp"

#include <cppmcp/local_pipe_transport.hpp>

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>

using namespace cppmcp;
using namespace cppmcp::testing;
using json = nlohmann::json;

// --- LocalPipeClient: connects to named pipe or Unix socket ---
class LocalPipeClient {
public:
    LocalPipeClient(const std::string& pipe_name)
        : pipe_name_(pipe_name) {
#ifdef _WIN32
        // nothing yet — connect() will create stream_handle
#else
        socket_ = std::make_unique<asio::local::stream_protocol::socket>(io_ctx_);
#endif
        // Run io_context in a background thread for async operations
        work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            io_ctx_.get_executor());
        io_thread_ = std::thread([this]() { io_ctx_.run(); });
    }

    ~LocalPipeClient() {
        close();
        work_guard_->reset();
        io_ctx_.stop();
        if (io_thread_.joinable()) io_thread_.join();
    }

    void connect() {
#ifdef _WIN32
        std::string path = R"(\\.\pipe\)" + pipe_name_;
        HANDLE h = INVALID_HANDLE_VALUE;
        for (int i = 0; i < 100; ++i) {
            h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE) break;
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                WaitNamedPipeA(path.c_str(), 1000);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (h == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to connect to named pipe: " + pipe_name_);
        }
        stream_handle_ = std::make_unique<asio::windows::stream_handle>(io_ctx_, h);
#else
        std::string path = "/tmp/" + pipe_name_ + ".sock";
        asio::local::stream_protocol::endpoint ep(path);
        socket_->connect(ep);
#endif
        start_read();
    }

    void send_message(const json& msg) {
        auto data_ptr = std::make_shared<std::string>(msg.dump() + "\n");
#ifdef _WIN32
        asio::post(io_ctx_, [this, data_ptr]() {
            bool was_empty = write_queue_.empty();
            write_queue_.push_back(std::move(*data_ptr));
            if (was_empty) start_write();
        });
#else
        asio::post(io_ctx_, [this, data_ptr]() {
            bool was_empty = write_queue_.empty();
            write_queue_.push_back(std::move(*data_ptr));
            if (was_empty) start_write();
        });
#endif
    }

    // Write an arbitrary byte blob (e.g. several newline-delimited messages
    // batched into one write) to exercise framing behavior.
    void send_raw(const std::string& data) {
        auto data_ptr = std::make_shared<std::string>(data);
        asio::post(io_ctx_, [this, data_ptr]() {
            bool was_empty = write_queue_.empty();
            write_queue_.push_back(std::move(*data_ptr));
            if (was_empty) start_write();
        });
    }

    // Wait for a response by id (generous default: CI runners have slow I/O)
    json read_response(int expected_id, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::lock_guard<std::mutex> lock(responses_mutex_);
            for (auto it = responses_.begin(); it != responses_.end(); ++it) {
                if (it->contains("id") && it->at("id") == expected_id) {
                    json resp = std::move(*it);
                    responses_.erase(it);
                    return resp;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("LocalPipeClient: no response with id " + std::to_string(expected_id));
    }

    // Wait for any response
    json read_any_response(std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::lock_guard<std::mutex> lock(responses_mutex_);
            if (!responses_.empty()) {
                json resp = std::move(responses_.front());
                responses_.pop_front();
                return resp;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("LocalPipeClient: no response received");
    }

    void close() {
#ifdef _WIN32
        if (stream_handle_) {
            asio::post(io_ctx_, [this]() {
                asio::error_code ec;
                stream_handle_->close(ec);
            });
        }
#else
        if (socket_) {
            asio::post(io_ctx_, [this]() {
                asio::error_code ec;
                socket_->close(ec);
            });
        }
#endif
    }

private:
    void start_read() {
#ifdef _WIN32
        asio::async_read_until(*stream_handle_, read_buf_, '\n',
            [this](const asio::error_code& ec, std::size_t bytes_transferred) {
                if (ec) {
                    std::cerr << "[pipe_client] read error: " << ec.message() << std::endl;
                    return;
                }
                handle_read(bytes_transferred);
                start_read();
            });
#else
        asio::async_read_until(*socket_, read_buf_, '\n',
            [this](const asio::error_code& ec, std::size_t bytes_transferred) {
                if (ec) {
                    std::cerr << "[pipe_client] read error: " << ec.message() << std::endl;
                    return;
                }
                handle_read(bytes_transferred);
                start_read();
            });
#endif
    }

    void handle_read(std::size_t bytes_transferred) {
        std::string line;
        line.reserve(bytes_transferred);
        auto bufs = read_buf_.data();
        for (auto it = asio::buffers_begin(bufs);
             it != asio::buffers_begin(bufs) + bytes_transferred; ++it) {
            line += *it;
        }
        read_buf_.consume(bytes_transferred);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (!line.empty()) {
            try {
                json resp = json::parse(line);
                std::lock_guard<std::mutex> lock(responses_mutex_);
                responses_.push_back(std::move(resp));
            } catch (const json::parse_error&) {}
        }
    }

    void start_write() {
        if (write_queue_.empty()) return;
        auto data_ptr = std::make_shared<std::string>(std::move(write_queue_.front()));
        write_queue_.pop_front();
#ifdef _WIN32
        asio::async_write(*stream_handle_, asio::buffer(*data_ptr),
            [this, data_ptr](const asio::error_code& ec, std::size_t) {
                if (ec) {
                    std::cerr << "[pipe_client] write error: " << ec.message() << std::endl;
                    return;
                }
                if (!write_queue_.empty()) start_write();
            });
#else
        asio::async_write(*socket_, asio::buffer(*data_ptr),
            [this, data_ptr](const asio::error_code& ec, std::size_t) {
                if (ec) {
                    std::cerr << "[pipe_client] write error: " << ec.message() << std::endl;
                    return;
                }
                if (!write_queue_.empty()) start_write();
            });
#endif
    }

    asio::io_context io_ctx_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    std::thread io_thread_;
    std::string pipe_name_;
    asio::streambuf read_buf_;
    std::deque<std::string> write_queue_;
    std::deque<json> responses_;
    std::mutex responses_mutex_;

#ifdef _WIN32
    std::unique_ptr<asio::windows::stream_handle> stream_handle_;
#else
    std::unique_ptr<asio::local::stream_protocol::socket> socket_;
#endif
};

// --- Test Fixture ---
class LocalPipeTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = create_test_server_with_tools();
        pipe_name_ = get_unique_pipe_name();

        LocalPipeConfig config;
        config.pipe_name = pipe_name_;
        config.mode = PipeMode::SingleClient;

        auto transport = std::make_shared<LocalPipeTransport>(config);
        server_->connect(transport);
        transport_ = transport;

        server_thread_ = std::make_unique<ServerThread>(*server_);
        server_thread_->wait_until_ready();

        // Wait specifically for the transport to be running (pipe created on Windows)
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (transport_->is_running()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        server_thread_->stop();
#ifndef _WIN32
        // Clean up Unix socket file
        std::string sock_path = "/tmp/" + pipe_name_ + ".sock";
        std::remove(sock_path.c_str());
#endif
    }

    std::shared_ptr<McpServer> server_;
    std::shared_ptr<LocalPipeTransport> transport_;
    std::unique_ptr<ServerThread> server_thread_;
    std::string pipe_name_;
};

TEST_F(LocalPipeTest, ConnectAndInitialize) {
    LocalPipeClient client(pipe_name_);
    client.connect();

    client.send_message(make_initialize_request(1));
    auto resp = client.read_response(1);

    EXPECT_EQ(resp["jsonrpc"], "2.0");
    EXPECT_EQ(resp["id"], 1);
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"]["protocolVersion"], "2025-03-26");
    EXPECT_EQ(resp["result"]["serverInfo"]["name"], "test_server");

    client.close();
}

TEST_F(LocalPipeTest, PingAfterInit) {
    LocalPipeClient client(pipe_name_);
    client.connect();

    client.send_message(make_initialize_request(1));
    auto init_resp = client.read_response(1);
    EXPECT_EQ(init_resp["id"], 1);

    client.send_message(make_initialized_notification());

    client.send_message(make_ping_request(2));
    auto ping_resp = client.read_response(2);

    EXPECT_EQ(ping_resp["id"], 2);
    EXPECT_TRUE(ping_resp.contains("result"));

    client.close();
}

TEST_F(LocalPipeTest, ToolsCall) {
    LocalPipeClient client(pipe_name_);
    client.connect();

    client.send_message(make_initialize_request(1));
    auto init_resp = client.read_response(1);

    client.send_message(make_initialized_notification());

    client.send_message(make_tools_call_request(3, "echo", json{{"message", "hello pipe"}}));
    auto call_resp = client.read_response(3);

    EXPECT_EQ(call_resp["id"], 3);
    EXPECT_EQ(call_resp["result"]["content"][0]["text"], "hello pipe");
    EXPECT_FALSE(call_resp["result"].value("isError", false));

    client.close();
}

TEST_F(LocalPipeTest, MultipleSequentialRequests) {
    LocalPipeClient client(pipe_name_);
    client.connect();

    client.send_message(make_initialize_request(1));
    auto r1 = client.read_response(1);
    EXPECT_EQ(r1["id"], 1);

    client.send_message(make_initialized_notification());

    client.send_message(make_tools_list_request(2));
    auto r2 = client.read_response(2);
    EXPECT_EQ(r2["id"], 2);
    EXPECT_TRUE(r2["result"]["tools"].is_array());

    client.send_message(make_tools_call_request(3, "echo", json{{"message", "test1"}}));
    auto r3 = client.read_response(3);
    EXPECT_EQ(r3["result"]["content"][0]["text"], "test1");

    client.send_message(make_tools_call_request(4, "add", json{{"a", 10}, {"b", 20}}));
    auto r4 = client.read_response(4);
    EXPECT_EQ(r4["id"], 4);

    client.send_message(make_ping_request(5));
    auto r5 = client.read_response(5);
    EXPECT_TRUE(r5.contains("result"));

    client.close();
}

#ifdef _WIN32
// Regression (#10): on Windows the named pipe is now byte-stream + line
// framed. Two newline-delimited messages written in a single write must both
// be parsed and answered (previously the whole chunk was treated as one JSON
// and failed to parse).
TEST_F(LocalPipeTest, BatchedMessagesInOneWrite) {
    LocalPipeClient client(pipe_name_);
    client.connect();

    std::string batch = make_initialize_request(1).dump() + "\n" +
                        make_ping_request(2).dump() + "\n";
    client.send_raw(batch);

    auto r1 = client.read_response(1);
    EXPECT_EQ(r1["id"], 1);

    auto r2 = client.read_response(2);
    EXPECT_EQ(r2["id"], 2);  // present (error body is fine — we only check framing)

    client.close();
}
#endif