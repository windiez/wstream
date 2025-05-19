#include "whisper.h"
#include "common-sdl.h"
#include "common.h"
#include "common-whisper.h"
#include "concurrentqueue.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <set>
#include <queue>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <condition_variable>

namespace fs = std::filesystem;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Global flags
std::atomic<bool> is_running(true);

// Thread-safe queue for transcriptions
struct TranscriptionQueue {
    std::queue<std::string> queue;
    std::mutex mutex;
    std::condition_variable cond;

    void push(const std::string& transcription) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(transcription);
        cond.notify_one();
    }

    bool pop(std::string& transcription) {
        std::unique_lock<std::mutex> lock(mutex);
        if (queue.empty()) {
            return false;
        }
        transcription = queue.front();
        queue.pop();
        return true;
    }

    bool wait_and_pop(std::string& transcription, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex);
        if (cond.wait_for(lock, std::chrono::milliseconds(timeout_ms),
            [this] { return !queue.empty() || !is_running.load(); })) {
            if (!is_running.load()) return false;
            transcription = queue.front();
            queue.pop();
        return true;
            }
            return false;
    }
};

moodycamel::ConcurrentQueue<std::string> transcription_queue;

// Shared state for WebSocket server (optimized)
class shared_state {
    std::set<websocket::stream<tcp::socket>*> m_connections;
    std::mutex m_mutex;
    // Pre-allocated buffers
    nlohmann::json m_json_template;

public:
    shared_state() {
        // Initialize the JSON template to avoid repeated construction
        m_json_template["type"] = "transcribe";
    }

    void join(websocket::stream<tcp::socket>* ws) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.insert(ws);
    }

    void leave(websocket::stream<tcp::socket>* ws) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.erase(ws);
    }

    bool is_client_connected() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_connections.empty();
    }

    // Optimized broadcast with pre-allocated JSON
    void broadcast(const std::string& transcription) {
        if (transcription.empty()) return;

        // Copy the template and add the content
        auto json_message = m_json_template;
        json_message["content"] = transcription;
        std::string message = json_message.dump();

        std::vector<websocket::stream<tcp::socket>*> clients;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_connections.empty()) return; // Quick exit if no clients
            clients.assign(m_connections.begin(), m_connections.end());
        }

        // Broadcast to all clients
        for (auto ws : clients) {
            try {
                ws->text(true);
                ws->write(net::buffer(message));
            } catch (const std::exception& e) {
                std::cerr << "WebSocket Broadcast Error: " << e.what() << std::endl;
            }
        }
    }
};

// WebSocket session handler (optimized)
void do_session(tcp::socket socket, std::shared_ptr<shared_state> state) {
    websocket::stream<tcp::socket> ws{std::move(socket)};
    try {
        // Set options for better performance
        ws.auto_fragment(false);
        ws.read_message_max(64 * 1024); // 64K max message size

        // Configure no delay for lower latency
        beast::get_lowest_layer(ws).set_option(
            tcp::no_delay(true));

        ws.accept();
        state->join(&ws);

        while (is_running) {
            beast::flat_buffer buffer;
            ws.read(buffer);

            // Use string_view for better performance
            std::string_view message(
                static_cast<const char*>(buffer.data().data()),
                                     buffer.data().size());

            try {
                // Parse the message as JSON
                nlohmann::json json_message = nlohmann::json::parse(message);

                // Check if the message is a command
                if (json_message["type"] == "reset") {
                    // Handle reset command
                    std::string content = json_message["content"];
                    // Reset implementation
                }
            } catch (const nlohmann::json::exception& e) {
                // Handle JSON parsing error
                std::cerr << "JSON parsing error: " << e.what() << std::endl;
            }
        }
    } catch (beast::system_error const& se) {
        if (se.code() != websocket::error::closed) {
            std::cerr << "WebSocket Error: " << se.code().message() << std::endl;
        }
    } catch (std::exception const& e) {
        std::cerr << "WebSocket Error: " << e.what() << std::endl;
    }

    // Remove the client from the connections set
    state->leave(&ws);
}

// WebSocket server thread
void websocket_server(std::shared_ptr<shared_state> state) {
    try {
        net::io_context ioc;
        tcp::acceptor acceptor{ioc, {tcp::v4(), 8080}};

        // Set options for better connection handling
        acceptor.set_option(tcp::acceptor::reuse_address(true));

        std::cout << "WebSocket server is running on port 8080..." << std::endl;

        while (is_running) {
            tcp::socket socket{ioc};
            acceptor.accept(socket);
            if (!is_running) break;
            std::thread{do_session, std::move(socket), state}.detach();
        }

        ioc.stop();
    } catch (std::exception const& e) {
        std::cerr << "WebSocket Server Error: " << e.what() << std::endl;
    }
}

// Broadcasting thread
void broadcast_thread(std::shared_ptr<shared_state> state, moodycamel::ConcurrentQueue<std::string>& queue) {
    std::string transcription;
    while (is_running) {
        if (queue.try_dequeue(transcription)) {
            state->broadcast(transcription);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Small sleep to avoid busy-waiting
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
    // Reserve some threads for system and other processes
    return std::max(1, hardware_threads - 2);
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
    cparams.gpu_device = 0; // Select specific GPU if multiple are available

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
    //TranscriptionQueue transcription_queue;
    moodycamel::ConcurrentQueue<std::string> transcription_queue;

    // Start the WebSocket server
    auto state = std::make_shared<shared_state>();
    std::thread ws_thread(websocket_server, state);

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

        // Process audio (non-VAD case)
        if (!use_vad) {
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

                // Queue for broadcasting instead of immediate broadcast
                //transcription_queue.push(processed_text);
                transcription_queue.enqueue(processed_text);
            }
        }

        // Optimize memory usage for next iteration
        pcmf32_old.assign(pcmf32.end() - n_samples_keep, pcmf32.end());
    }

    std::cout << "Shutting down..." << std::endl;
    audio.pause();
    SDL_Quit();
    is_running = false;

    // Wait for threads to finish
    if (ws_thread.joinable()) ws_thread.join();
    if (bc_thread.joinable()) bc_thread.join();

    whisper_free(ctx);

    return 0;
}
