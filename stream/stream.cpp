#include "whisper.h"
#include "common-sdl.h"
#include "common.h"
#include "common-whisper.h"
#include "concurrentqueue.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <set>
#include <queue>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <condition_variable>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <regex>
#include <memory>
#include <cstring>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Security configuration
// ---------------------------------------------------------------------------
// Maximum number of concurrent WebSocket client sessions. Prevents resource
// exhaustion from connection floods (one detached thread per client).
static constexpr size_t WSTREAM_MAX_CLIENTS = 64;

// Maximum size in bytes of a transcript file that will be returned over the
// WebSocket. Prevents memory exhaustion from a client requesting a huge file.
static constexpr uintmax_t WSTREAM_MAX_TRANSCRIPT_BYTES = 4 * 1024 * 1024;

// Allow-list for WebSocket Origin header. Empty string means "no Origin
// header" (non-browser clients). Add the origins of any trusted web front-ends
// here; everything else is rejected to block Cross-Site WebSocket Hijacking.
static const std::set<std::string> WSTREAM_ALLOWED_ORIGINS = {
    "",                        // native / CLI clients
    "http://localhost",
    "http://localhost:8080",
    "http://127.0.0.1",
    "http://127.0.0.1:8080",
};

// Read the admin control token from the environment at startup. Returns an
// empty string if unset, in which case the control interface is disabled.
static std::string load_control_token() {
    const char* t = std::getenv("WSTREAM_CONTROL_TOKEN");
    return t ? std::string(t) : std::string();
}

// Constant-time string comparison to avoid leaking token bytes via timing.
static bool secure_compare(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char r = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        r |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return r == 0;
}

// Strict validator for hostnames / IPv4 / IPv6 literals used by the
// diagnostic command. Character set is restricted to [A-Za-z0-9.:-] and the
// first character must not be '-' so the value cannot be interpreted as an
// option switch by the spawned ping binary (argv injection). This is
// defence-in-depth; the primary fix is that input never reaches a shell.
static bool is_safe_host(const std::string& h) {
    static const std::regex re(R"(^[A-Za-z0-9.:][A-Za-z0-9.\-:]{0,252}$)");
    return std::regex_match(h, re);
}
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Transcription logger - persists transcriptions to file asynchronously
// Uses lock-free queue to avoid blocking the main transcription thread
class TranscriptionLogger {
private:
    fs::path m_log_path;
    moodycamel::ConcurrentQueue<std::string> m_queue;
    std::vector<std::string> m_buffer;
    std::thread m_writer_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
    std::condition_variable m_cond;
    std::mutex m_cond_mutex;
    std::atomic<bool> m_new_data{false};
    size_t m_flush_threshold;

    static fs::path get_log_directory() {
        const char* home = std::getenv("HOME");
        if (!home) {
            home = std::getenv("USERPROFILE");
        }
        if (!home) {
            return fs::path{};
        }
        return fs::path(home) / ".wstream";
    }

    static std::string generate_filename() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_now{};
        localtime_r(&time_t_now, &tm_now);

        std::ostringstream oss;
        oss << std::put_time(&tm_now, "%d%m%Y_%H%M%S") << ".log";
        return oss.str();
    }

    void flush_buffer_to_file() {
        if (m_buffer.empty()) {
            return;
        }

        std::ofstream file(m_log_path, std::ios::app);
        if (!file.is_open()) {
            std::cerr << "TranscriptionLogger: Failed to open log file" << std::endl;
            return;
        }

        for (const auto& entry : m_buffer) {
            file << entry << '\n';
        }
        file.flush();
        m_buffer.clear();
    }

    void writer_loop() {
        std::string transcription;
        m_buffer.reserve(m_flush_threshold);

        while (m_running.load()) {
            // Wait for data or shutdown signal
            {
                std::unique_lock<std::mutex> lock(m_cond_mutex);
                m_cond.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return m_new_data.load() || !m_running.load();
                });
                m_new_data.store(false);
            }

            // Drain queue into buffer
            while (m_queue.try_dequeue(transcription)) {
                m_buffer.push_back(std::move(transcription));

                if (m_buffer.size() >= m_flush_threshold) {
                    flush_buffer_to_file();
                }
            }
        }

        // Final drain on shutdown
        while (m_queue.try_dequeue(transcription)) {
            m_buffer.push_back(std::move(transcription));
        }
        flush_buffer_to_file();
    }

public:
    explicit TranscriptionLogger(size_t flush_threshold = 10)
        : m_flush_threshold(flush_threshold) {

        fs::path log_dir = get_log_directory();
        if (log_dir.empty()) {
            std::cerr << "TranscriptionLogger: Could not determine home directory" << std::endl;
            return;
        }

        std::error_code ec;
        if (!fs::exists(log_dir)) {
            if (!fs::create_directories(log_dir, ec)) {
                std::cerr << "TranscriptionLogger: Failed to create directory "
                          << log_dir << ": " << ec.message() << std::endl;
                return;
            }
        }

        m_log_path = log_dir / generate_filename();
        m_initialized.store(true);
        m_running.store(true);
        m_writer_thread = std::thread(&TranscriptionLogger::writer_loop, this);

        std::cout << "TranscriptionLogger: Logging to " << m_log_path << std::endl;
    }

    ~TranscriptionLogger() {
        stop();
    }

    TranscriptionLogger(const TranscriptionLogger&) = delete;
    TranscriptionLogger& operator=(const TranscriptionLogger&) = delete;
    TranscriptionLogger(TranscriptionLogger&&) = delete;
    TranscriptionLogger& operator=(TranscriptionLogger&&) = delete;

    // Lock-free, non-blocking log call
    void log(const std::string& transcription) {
        if (!m_initialized.load() || transcription.empty()) {
            return;
        }

        m_queue.enqueue(transcription);
        {
            std::lock_guard<std::mutex> lock(m_cond_mutex);
            m_new_data.store(true);
        }
        m_cond.notify_one();
    }

    void stop() {
        if (!m_running.exchange(false)) {
            return; // Already stopped
        }
        m_cond.notify_one();
        if (m_writer_thread.joinable()) {
            m_writer_thread.join();
        }
    }

    bool is_initialized() const {
        return m_initialized.load();
    }

    fs::path get_log_path() const {
        return m_log_path;
    }
};

// Global flags
std::atomic<bool> is_running(true);

// Lock-free concurrent queue for transcriptions
class TranscriptionQueue {
private:
    moodycamel::ConcurrentQueue<std::string> queue;
    std::atomic<bool> new_data{false};
    std::condition_variable cond;
    std::mutex mutex;

public:
    void push(const std::string& transcription) {
        queue.enqueue(transcription);
        {
            std::lock_guard<std::mutex> lock(mutex);
            new_data.store(true);
        }
        cond.notify_one();
    }

    bool pop(std::string& transcription) {
        return queue.try_dequeue(transcription);
    }

    bool wait_and_pop(std::string& transcription, int timeout_ms) {
        // First try a quick non-blocking dequeue
        if (queue.try_dequeue(transcription)) {
            return true;
        }

        // If empty, wait for notification or timeout
        std::unique_lock<std::mutex> lock(mutex);
        if (cond.wait_for(lock, std::chrono::milliseconds(timeout_ms),
            [this] { return new_data.load() || !is_running.load(); })) {
            new_data.store(false);
        if (!is_running.load()) return false;
        return queue.try_dequeue(transcription);
            }
            return false;
    }
};

// Shared state for WebSocket server.
//
// Lifetime / thread-safety model:
//
//   - Each session owns its websocket::stream as a stack object.
//   - On join(), we create a heap-allocated client_handle containing a
//     write-mutex and an "alive" flag, and store a shared_ptr to it.
//   - broadcast() takes a snapshot of the handle list *outside* m_mutex to
//     avoid serialising all writes, then for each handle acquires the
//     per-client write_mtx and checks 'alive' before touching ws.
//   - leave() flips 'alive' to false *under write_mtx* and then removes the
//     entry under m_mutex. Because broadcast() holds write_mtx while it
//     dereferences ws, and leave() holds write_mtx while clearing 'alive',
//     broadcast() can never see alive==true with a destroyed ws. This closes
//     the use-after-free window that existed when broadcast() dereferenced
//     raw ws* pointers after releasing m_mutex.
class shared_state {
public:
    struct client_handle {
        websocket::stream<tcp::socket>* ws;
        std::mutex                      write_mtx;
        bool                            alive = true;   // guarded by write_mtx
    };

private:
    std::vector<std::shared_ptr<client_handle>> m_connections;
    std::mutex     m_mutex;
    nlohmann::json m_json_template;

public:
    shared_state() {
        m_json_template["type"] = "transcribe";
    }

    // Attempt to register a new client. Returns nullptr if the server is at
    // capacity; otherwise returns the handle the session must use for all
    // writes it performs on its own stream.
    std::shared_ptr<client_handle> join(websocket::stream<tcp::socket>* ws) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_connections.size() >= WSTREAM_MAX_CLIENTS) {
            return nullptr;
        }
        auto h = std::make_shared<client_handle>();
        h->ws = ws;
        m_connections.push_back(h);
        return h;
    }

    // Mark the client dead and remove it from the broadcast list. The caller
    // must invoke this *before* its websocket::stream is destroyed.
    void leave(const std::shared_ptr<client_handle>& h) {
        if (!h) return;
        {
            // Flip alive under write_mtx so that any in-flight broadcast()
            // either completes its write first or observes alive==false.
            std::lock_guard<std::mutex> wlock(h->write_mtx);
            h->alive = false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.erase(
            std::remove(m_connections.begin(), m_connections.end(), h),
            m_connections.end());
    }

    bool is_client_connected() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_connections.empty();
    }

    size_t client_count() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_connections.size();
    }

    // Close all active WebSocket connections (for clean shutdown).
    void close_all() {
        std::vector<std::shared_ptr<client_handle>> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot = m_connections;
        }
        for (auto& h : snapshot) {
            std::lock_guard<std::mutex> wlock(h->write_mtx);
            if (!h->alive) continue;
            try {
                beast::get_lowest_layer(*h->ws).cancel();
            } catch (...) {}
        }
    }

    void broadcast(const std::string& transcription) {
        if (transcription.empty()) return;

        auto json_message = m_json_template;
        json_message["content"] = transcription;
        std::string message = json_message.dump();

        std::vector<std::shared_ptr<client_handle>> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_connections.empty()) return;
            snapshot = m_connections;
        }

        for (auto& h : snapshot) {
            try {
                std::lock_guard<std::mutex> wlock(h->write_mtx);
                if (!h->alive) continue;        // session already tearing down
                h->ws->text(true);
                h->ws->write(net::buffer(message));
            } catch (const std::exception& e) {
                std::cerr << "WebSocket Broadcast Error: " << e.what() << std::endl;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Diagnostic helper: run ping without invoking a shell.
// Uses posix_spawnp + pipe so that user-supplied data is passed as an argv
// element, not interpolated into a command string. Combined with the
// is_safe_host() allow-list this removes the command-injection vector.
// ---------------------------------------------------------------------------
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

static std::string run_ping(const std::string& host) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return "diagnostic: pipe() failed";
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    // argv array - host is passed as a single argument, no shell involved.
    char arg0[] = "ping";
    char arg1[] = "-c";
    char arg2[] = "1";
    char arg3[] = "-W";
    char arg4[] = "1";
    std::vector<char> host_buf(host.begin(), host.end());
    host_buf.push_back('\0');
    char* argv[] = { arg0, arg1, arg2, arg3, arg4, host_buf.data(), nullptr };

    pid_t pid;
    int rc = posix_spawnp(&pid, "ping", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);

    if (rc != 0) {
        close(pipefd[0]);
        return "diagnostic: failed to spawn ping";
    }

    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 8192) break;   // cap output
    }
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    return out;
}

// ---------------------------------------------------------------------------
// Transcript retrieval helper. Performs canonical-path containment check so
// that the resolved file is guaranteed to live inside ~/.wstream/ and cannot
// be escaped via "..", absolute paths, or symlinks.
// ---------------------------------------------------------------------------
static bool read_transcript_file(const std::string& filename,
                                 std::string&       out_contents,
                                 std::string&       out_error) {
    // Reject obviously malicious input early.
    if (filename.empty() ||
        filename.find('/')  != std::string::npos ||
        filename.find('\\') != std::string::npos ||
        filename.find('\0') != std::string::npos ||
        filename.find("..") != std::string::npos) {
        out_error = "invalid filename";
        return false;
    }

    const char* home = std::getenv("HOME");
    if (!home) {
        out_error = "server misconfiguration";
        return false;
    }

    std::error_code ec;
    fs::path log_dir   = fs::weakly_canonical(fs::path(home) / ".wstream", ec);
    if (ec) { out_error = "log directory unavailable"; return false; }

    fs::path candidate = fs::weakly_canonical(log_dir / filename, ec);
    if (ec) { out_error = "file not found"; return false; }

    // Containment check: resolved path must start with the log directory.
    auto dir_str  = log_dir.string();
    auto file_str = candidate.string();
    if (file_str.compare(0, dir_str.size(), dir_str) != 0 ||
        (file_str.size() > dir_str.size() &&
         file_str[dir_str.size()] != fs::path::preferred_separator)) {
        out_error = "access denied";
        return false;
    }

    if (!fs::is_regular_file(candidate, ec) || ec) {
        out_error = "file not found";
        return false;
    }

    uintmax_t sz = fs::file_size(candidate, ec);
    if (ec || sz > WSTREAM_MAX_TRANSCRIPT_BYTES) {
        out_error = "file too large";
        return false;
    }

    std::ifstream f(candidate, std::ios::binary);
    if (!f) { out_error = "read failed"; return false; }
    out_contents.assign((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return true;
}

// Helper to send a JSON response using the session's write mutex.
static void send_json(websocket::stream<tcp::socket>& ws,
                      std::mutex&                     write_mtx,
                      const nlohmann::json&           resp) {
    std::string payload = resp.dump();
    std::lock_guard<std::mutex> lock(write_mtx);
    ws.text(true);
    ws.write(net::buffer(payload));
}

// WebSocket session handler
void do_session(tcp::socket socket, std::shared_ptr<shared_state> state) {
    static const std::string control_token = load_control_token();

    websocket::stream<tcp::socket> ws{std::move(socket)};
    std::shared_ptr<shared_state::client_handle> handle;

    try {
        ws.auto_fragment(false);
        ws.read_message_max(64 * 1024);
        beast::get_lowest_layer(ws).set_option(tcp::no_delay(true));

        // Perform the handshake manually so we can inspect the Origin header
        // before accepting. Rejecting unknown origins stops Cross-Site
        // WebSocket Hijacking from malicious web pages.
        beast::flat_buffer hbuf;
        beast::http::request<beast::http::string_body> req;
        beast::http::read(beast::get_lowest_layer(ws), hbuf, req);

        std::string origin(req[beast::http::field::origin]);
        if (WSTREAM_ALLOWED_ORIGINS.find(origin) == WSTREAM_ALLOWED_ORIGINS.end()) {
            beast::http::response<beast::http::string_body> res{
                beast::http::status::forbidden, req.version()};
            res.set(beast::http::field::content_type, "text/plain");
            res.body() = "Forbidden origin";
            res.prepare_payload();
            beast::http::write(beast::get_lowest_layer(ws), res);
            return;
        }

        ws.accept(req);

        handle = state->join(&ws);
        if (!handle) {
            // Server at capacity - close politely.
            ws.close(websocket::close_code::try_again_later);
            return;
        }
        std::mutex& write_mtx = handle->write_mtx;

        while (is_running) {
            beast::flat_buffer buffer;
            ws.read(buffer);

            std::string_view message(
                static_cast<const char*>(buffer.data().data()),
                buffer.data().size());

            nlohmann::json json_message;
            try {
                json_message = nlohmann::json::parse(message);
            } catch (const nlohmann::json::exception&) {
                nlohmann::json err;
                err["type"]  = "error";
                err["error"] = "malformed JSON";
                send_json(ws, write_mtx, err);
                continue;
            }
            if (!json_message.is_object()) continue;

            // nlohmann::json::value("k", default) throws type_error if the
            // key exists with an incompatible type (e.g. {"type":[]}). Wrap
            // all field extraction so a malformed-but-valid-JSON message
            // cannot tear down the session.
            try {

            const std::string type = json_message.value("type", "");

            if (type == "reset") {
                // Reset implementation (no-op for now).

            } else if (type == "diagnostic") {
                // Privileged: requires control token.
                if (control_token.empty() ||
                    !secure_compare(json_message.value("token", ""), control_token)) {
                    nlohmann::json err;
                    err["type"]  = "error";
                    err["error"] = "unauthorised";
                    send_json(ws, write_mtx, err);
                    continue;
                }
                std::string host = json_message.value("host", "localhost");
                if (!is_safe_host(host)) {
                    nlohmann::json err;
                    err["type"]  = "error";
                    err["error"] = "invalid host";
                    send_json(ws, write_mtx, err);
                    continue;
                }
                nlohmann::json resp;
                resp["type"]   = "diagnostic_result";
                resp["output"] = run_ping(host);
                send_json(ws, write_mtx, resp);

            } else if (type == "fetch_transcript") {
                std::string filename = json_message.value("file", "");
                std::string contents, errmsg;
                nlohmann::json resp;
                if (read_transcript_file(filename, contents, errmsg)) {
                    resp["type"]    = "transcript_content";
                    resp["file"]    = filename;
                    resp["content"] = contents;
                } else {
                    resp["type"]  = "error";
                    resp["error"] = errmsg;
                }
                send_json(ws, write_mtx, resp);

            } else if (type == "inject") {
                // Privileged: arbitrary broadcast to every client. Without a
                // token gate any client could spoof transcription output.
                if (control_token.empty() ||
                    !secure_compare(json_message.value("token", ""), control_token)) {
                    nlohmann::json err;
                    err["type"]  = "error";
                    err["error"] = "unauthorised";
                    send_json(ws, write_mtx, err);
                    continue;
                }
                std::string text = json_message.value("text", "");
                if (!text.empty() && text.size() <= 4096) {
                    state->broadcast(text);
                }

            } else if (type == "control") {
                std::string token  = json_message.value("token", "");
                std::string action = json_message.value("action", "");
                if (control_token.empty() ||
                    !secure_compare(token, control_token)) {
                    nlohmann::json err;
                    err["type"]  = "error";
                    err["error"] = "unauthorised";
                    send_json(ws, write_mtx, err);
                    continue;
                }
                if (action == "shutdown") {
                    is_running.store(false);
                } else if (action == "status") {
                    nlohmann::json resp;
                    resp["type"]    = "status";
                    resp["running"] = is_running.load();
                    resp["clients"] = state->client_count();
                    send_json(ws, write_mtx, resp);
                }
            }

            } catch (const nlohmann::json::exception&) {
                nlohmann::json err;
                err["type"]  = "error";
                err["error"] = "invalid field type";
                send_json(ws, write_mtx, err);
            }
        }
    } catch (beast::system_error const& se) {
        if (se.code() != websocket::error::closed) {
            std::cerr << "WebSocket Error: " << se.code().message() << std::endl;
        }
    } catch (std::exception const& e) {
        std::cerr << "WebSocket Error: " << e.what() << std::endl;
    }

    // Mark dead and deregister *before* ws is destroyed so that any in-flight
    // broadcast() sees alive==false and skips this stream.
    state->leave(handle);
}

// WebSocket server thread
//
// By default the server binds to the loopback interface only. Set the
// environment variable WSTREAM_BIND (e.g. "0.0.0.0") to expose it on other
// interfaces, and WSTREAM_PORT to change the port. Binding to loopback by
// default prevents unauthenticated access from the local network.
void websocket_server(std::shared_ptr<shared_state> state, net::io_context& ioc) {
    try {
        const char* bind_env = std::getenv("WSTREAM_BIND");
        const char* port_env = std::getenv("WSTREAM_PORT");
        std::string bind_addr = bind_env ? bind_env : "127.0.0.1";
        unsigned short port = 8080;
        if (port_env) {
            try { port = static_cast<unsigned short>(std::stoi(port_env)); }
            catch (...) { port = 8080; }
        }

        tcp::endpoint ep{net::ip::make_address(bind_addr), port};
        tcp::acceptor acceptor{ioc, ep};

        // Set options for better connection handling
        acceptor.set_option(tcp::acceptor::reuse_address(true));

        std::cout << "WebSocket server listening on "
                  << bind_addr << ":" << port << std::endl;

        std::function<void()> do_accept;
        do_accept = [&] {
            acceptor.async_accept([&](beast::error_code ec, tcp::socket socket) {
                if (ec) return; // Acceptor was cancelled or error
                std::thread{do_session, std::move(socket), state}.detach();
                do_accept();
            });
        };
        do_accept();

        ioc.run();
    } catch (std::exception const& e) {
        std::cerr << "WebSocket Server Error: " << e.what() << std::endl;
    }
}

// Broadcasting thread
void broadcast_thread(std::shared_ptr<shared_state> state, TranscriptionQueue& queue) {
    std::string transcription;
    while (is_running) {
        if (queue.wait_and_pop(transcription, 100)) {
            state->broadcast(transcription);
        }
    }
}

// Optimized function to remove bracketed or parenthesised text
void remove_bracketed_text(std::string& text) {
    // Reserve result space to avoid reallocations
    std::string result;
    result.reserve(text.size());

    bool in_bracket = false;
    bool in_paren = false;

    for (char c : text) {
        if (!in_bracket && !in_paren) {
            if (c == '[') {
                in_bracket = true;
                continue;
            }
            if (c == '(') {
                in_paren = true;
                continue;
            }
            result.push_back(c);
        } else {
            if (in_bracket && c == ']') {
                in_bracket = false;
                continue;
            }
            if (in_paren && c == ')') {
                in_paren = false;
                continue;
            }
        }
    }

    text = std::move(result);
}

// Optimized function to trim leading and trailing whitespace
void lrtrim(std::string &s) {
    // Trim leading whitespace
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));

    // Trim trailing whitespace
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// Text processing worker
void process_text(const std::string& input, std::string& output) {
    output = input;
    remove_bracketed_text(output);
    lrtrim(output);
}

// Function to select optimal thread count
int get_optimal_thread_count() {
    int hardware_threads = std::thread::hardware_concurrency();
    // For GPU-accelerated inference, use fewer threads to avoid
    // CPU/GPU contention and memory bandwidth saturation
    // Optimal: ~1 thread per 2 cores for hybrid CPU/GPU workloads
    return std::min(6, std::max(1, hardware_threads / 4));
}

int main(int argc, char* argv[]) {
    // Initialize whisper context with optimized parameters
    std::string model_path = "models/ggml-small.en-q5_1.bin";

    // Check if a custom path was provided
    if (argc > 1) {
        std::string user_path = argv[1];

        // Validate the path
        if (fs::exists(user_path)) {
            model_path = user_path;
        } else {
            std::cerr << "Warning: Provided model path '" << user_path << "' does not exist. Falling back to default.\n";
        }
    }

    // Optimize context parameters
    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;
    // Set compute capabilities if using CUDA
    // cparams.gpu_device = 0; // Select specific GPU if multiple are available

    struct whisper_context* ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx) {
        std::cerr << "Failed to initialize Whisper context.\n";
        return 1;
    }

    const int step_ms = 3000;
    const int length_ms = 10000;
    const int keep_ms = 200;

    const int n_samples_30s  = (1e-3*30000.0)*WHISPER_SAMPLE_RATE;
    const int n_samples_len  = (1e-3*length_ms)*WHISPER_SAMPLE_RATE;
    const int n_samples_step = (1e-3*step_ms)*WHISPER_SAMPLE_RATE;
    const int n_samples_keep = (1e-3*keep_ms)*WHISPER_SAMPLE_RATE;

    // Pre-allocate vectors to avoid reallocations
    std::vector<float> pcmf32(n_samples_30s, 0.0f);
    std::vector<float> pcmf32_new(n_samples_30s, 0.0f);
    std::vector<float> pcmf32_old;
    pcmf32_old.reserve(n_samples_keep);

    // Initialize audio capture
    audio_async audio(length_ms);

    if (!audio.init(-1, WHISPER_SAMPLE_RATE)) {
        std::cerr << "Failed to initialize audio capture.\n";
        return 1;
    }
    audio.resume();

    // Set up transcription queue
    TranscriptionQueue transcription_queue;

    // Set up transcription logger (flushes every 10 transcriptions or on exit)
    TranscriptionLogger transcription_logger(10);

    // Start the WebSocket server
    auto state = std::make_shared<shared_state>();
    net::io_context ws_ioc;
    std::thread ws_thread(websocket_server, state, std::ref(ws_ioc));

    // Start the broadcasting thread
    std::thread bc_thread(broadcast_thread, state, std::ref(transcription_queue));

    const bool use_vad = false;
    auto t_last = std::chrono::high_resolution_clock::now();

    // Prepare Whisper parameters once (avoid recreating each time)
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.no_context = true;
    wparams.language = "en";
    wparams.n_threads = get_optimal_thread_count();
    wparams.temperature = 0.0f;
    wparams.single_segment = !use_vad;
    wparams.max_tokens = 16;
    wparams.audio_ctx = 0;

    while (is_running) {
        is_running = sdl_poll_events();
        if (!is_running) {
            break;
        }

        // Process audio
        if (!use_vad) {
            // Non-VAD case: process audio in fixed-step chunks
            while (true) {
                is_running = sdl_poll_events();
                if (!is_running) {
                    break;
                }
                audio.get(step_ms, pcmf32_new);

                if ((int) pcmf32_new.size() > 2*n_samples_step) {
                    std::cerr << "WARNING: cannot process audio fast enough, dropping audio..." << std::endl;
                    audio.clear();
                    continue;
                }

                if ((int) pcmf32_new.size() >= n_samples_step) {
                    audio.clear();
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            const int n_samples_new = pcmf32_new.size();
            const int n_samples_take = std::min((int) pcmf32_old.size(), std::max(0, n_samples_keep + n_samples_len - n_samples_new));

            pcmf32.resize(n_samples_new + n_samples_take);

            for (int i = 0; i < n_samples_take; i++) {
                pcmf32[i] = pcmf32_old[pcmf32_old.size() - n_samples_take + i];
            }

            memcpy(pcmf32.data() + n_samples_take, pcmf32_new.data(), n_samples_new*sizeof(float));
            pcmf32_old = pcmf32;
        } else {
            const auto t_now  = std::chrono::high_resolution_clock::now();
            const auto t_diff = std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last).count();

            if (t_diff < 2000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                continue;
            }

            audio.get(2000, pcmf32_new);

            if (::vad_simple(pcmf32_new, WHISPER_SAMPLE_RATE, 1000, 0.85f, 100.0f, false)) {
                audio.get(length_ms, pcmf32);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                continue;
            }

            t_last = t_now;
        }

        if (pcmf32.empty()) continue;

        // Run inference
        if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) != 0) {
            std::cerr << "Failed to process audio.\n";
            break;
        }

        // Get the transcription
        const int n_segments = whisper_full_n_segments(ctx);
        if (n_segments > 0) {
            // Pre-allocate with estimated size
            std::string current_transcription;
            current_transcription.reserve(n_segments * 64); // Assumes average segment is ~64 chars

            for (int i = 0; i < n_segments; ++i) {
                const char* text = whisper_full_get_segment_text(ctx, i);
                if (text) {
                    current_transcription += text;
                }
            }

            // Process text in a separate thread or use a thread pool
            std::string processed_text;
            process_text(current_transcription, processed_text);

            // Skip if the transcription is empty after cleaning
            if (!processed_text.empty()) {
                std::cout << processed_text << std::endl;

                // Log transcription to file
                transcription_logger.log(processed_text);

                // Queue for broadcasting instead of immediate broadcast
                transcription_queue.push(processed_text);
            }
        }

        // Optimize memory usage for next iteration
        pcmf32_old.assign(pcmf32.end() - n_samples_keep, pcmf32.end());
    }

    std::cout << "Shutting down..." << std::endl;

    audio.pause();
    SDL_Quit();
    is_running = false;

    // Stop WebSocket: close active sessions and cancel the acceptor
    state->close_all();
    ws_ioc.stop();

    // Wait for threads to finish
    if (ws_thread.joinable()) ws_thread.join();
    if (bc_thread.joinable()) bc_thread.join();

    // Stop logger thread and flush remaining transcriptions
    transcription_logger.stop();

    whisper_free(ctx);

    return 0;
}
