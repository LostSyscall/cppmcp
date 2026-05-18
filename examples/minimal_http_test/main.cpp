#include <cppmcp/http_transport.hpp>
#include <iostream>
#include <fstream>
#include <chrono>
#include <windows.h>

using namespace cppmcp;
using json = nlohmann::json;

static std::ofstream log_file("E:/DevProjects/cpp/cppmcp/build/examples/minimal_http_test/Debug/test_log.txt");

void log(const std::string& msg) {
    log_file << msg << std::endl;
    log_file.flush();
    std::cerr << msg << std::endl;
    std::cerr.flush();
}

// Truly C-style SEH wrapper - no C++ objects at all
extern "C" size_t __cdecl seh_run_one_wrap(asio::io_context* io_ctx_ptr) {
    __try {
        return io_ctx_ptr->run_one();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFF;  // Sentinel value for SEH exception
    }
}

int main() {
    log("[test] main() starting");

    HttpTransportConfig config;
    config.mode = HttpTransportMode::StreamableHttp;
    config.host = "127.0.0.1";
    config.port = 3000;
    config.path = "/mcp";

    asio::io_context io_ctx;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard(io_ctx.get_executor());

    asio::steady_timer timer(io_ctx, std::chrono::seconds(2));
    std::function<void(const asio::error_code&)> timer_cb;
    int tick = 0;
    timer_cb = [&](const asio::error_code& ec) {
        if (ec) { log("[test] timer error: " + ec.message()); return; }
        tick++;
        log("[test] tick " + std::to_string(tick));
        timer.expires_after(std::chrono::seconds(2));
        timer.async_wait(timer_cb);
    };
    timer.async_wait(timer_cb);

    auto transport = std::make_shared<HttpTransport>(config);
    transport->set_io_context(&io_ctx);

    transport->set_message_handler([&](const json& msg) {
        log("[test] message_handler invoked");
        if (msg.contains("id")) {
            json response = json{{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", json::object()}};
            log("[test] calling send_message");
            transport->send_message(response);
            log("[test] send_message returned");
        }
    });

    transport->set_error_handler([](const std::string& err) {
        log("[test] error: " + err);
    });

    transport->start();
    log("[test] Starting server on port 3000...");

    int handler_count = 0;
    while (true) {
        log("[test] calling run_one() #" + std::to_string(handler_count + 1));
        size_t n = seh_run_one_wrap(&io_ctx);
        handler_count++;
        if (n == 0xFFFFFFFF) {
            log("[test] SEH exception caught in run_one()!");
            break;
        }
        log("[test] run_one() returned " + std::to_string(n) + " (total: " + std::to_string(handler_count) + ")");
        if (n == 0) {
            log("[test] run_one() returned 0 - no more work!");
            break;
        }
    }

    log("[test] exiting main(), total: " + std::to_string(handler_count));
    return 0;
}