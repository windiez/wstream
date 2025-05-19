#include "whisper.h"
#include "common-sdl.h"
#include "common.h"
#include "common-whisper.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <set>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>

namespace fs = std::filesystem;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Global flag for pause/resume
std::atomic<bool> is_running(true);

// Store last transcription
std::string last_transcription;

// Shared state for WebSocket server
class shared_state {
    std::set<websocket::stream<tcp::socket>*> m_connections;
    std::mutex m_mutex;

public:
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

    void broadcast(const std::string& message) {
        std::vector<websocket::stream<tcp::socket>*> clients;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            clients.assign(m_connections.begin(), m_connections.end());
        }

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

// WebSocket session handler
void do_session(tcp::socket socket, std::shared_ptr<shared_state> state) {
    websocket::stream<tcp::socket> ws{std::move(socket)};
    try {
        ws.accept();
        state->join(&ws);

        while (is_running) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            // Convert the message to a string
            std::string message = beast::buffers_to_string(buffer.data());

            // Parse the message as JSON
            nlohmann::json json_message = nlohmann::json::parse(message);

            // Check if the message is a prompt
            if (json_message["type"] == "reset") {
                // Handle reset command
                std::string content = json_message["content"];
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

// WebSocket server
void websocket_server(std::shared_ptr<shared_state> state) {
    try {
        net::io_context ioc;
        tcp::acceptor acceptor{ioc, {tcp::v4(), 8080}};

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

// Function to remove bracketed or parenthesised text
void remove_bracketed_text(std::string& text) {
    char* read = text.data();
    char* write = text.data();
    bool in_bracket = false;
    bool in_paren = false;

    while (*read) {
        if (!in_bracket && !in_paren) {
            if (*read == '[') {
                in_bracket = true;
                read++;
                continue;
            }
            if (*read == '(') {
                in_paren = true;
                read++;
                continue;
            }
            *write++ = *read++;
        } else {
            if (in_bracket && *read == ']') {
                in_bracket = false;
                read++;
                continue;
            }
            if (in_paren && *read == ')') {
                in_paren = false;
                read++;
                continue;
            }
            read++;
        }
    }
    *write = '\0';
    text.resize(write - text.data());
}

// Function to trim leading and trailing whitespace
void lrtrim(std::string &s) {
    const char* whitespace = " \t\n\r\f\v";
    size_t start = s.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        s.clear();
        return;
    }
    size_t end = s.find_last_not_of(whitespace);
    s.erase(end + 1);
    s.erase(0, start);
}

int main(int argc, char* argv[]) {
    // Initialize whisper context
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

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;

    struct whisper_context* ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx) {
        std::cerr << "Failed to initialize Whisper context.\n";
        return 1;
    }

    const int step_ms = 3000;     // Changed from 10000 to match the example
    const int length_ms = 10000;  // Changed from 12000 to match the example
    const int keep_ms = 200;      // Changed from 1000 to match the example

    const int n_samples_30s  = (1e-3*30000.0)*WHISPER_SAMPLE_RATE;
    const int n_samples_len  = (1e-3*length_ms)*WHISPER_SAMPLE_RATE;
    const int n_samples_step = (1e-3*step_ms)*WHISPER_SAMPLE_RATE;
    const int n_samples_keep = (1e-3*keep_ms)*WHISPER_SAMPLE_RATE;

    std::vector<float> pcmf32(n_samples_30s, 0.0f);
    std::vector<float> pcmf32_new(n_samples_30s, 0.0f);
    std::vector<float> pcmf32_old;

    // Initialize audio capture
    audio_async audio(length_ms);

    if (!audio.init(-1, WHISPER_SAMPLE_RATE)) {
        std::cerr << "Failed to initialize audio capture.\n";
        return 1;
    }
    audio.resume();

    // Start the WebSocket server
    auto state = std::make_shared<shared_state>();
    std::thread ws_thread(websocket_server, state);

    const bool use_vad = false;
    auto t_last = std::chrono::high_resolution_clock::now();
    const auto t_start = t_last;

    while (is_running) {
        is_running = sdl_poll_events();
        if (!is_running) {
            break;
        }

        // Process new audio using VAD when appropriate
        if (!use_vad) {
            // Original continuous processing logic
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
            // New VAD-based processing from the example
            const auto t_now = std::chrono::high_resolution_clock::now();
            const auto t_diff = std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last).count();

            if (t_diff < 2000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            audio.get(2000, pcmf32_new);

            // Only process audio if speech is detected
            const float vad_thold = 0.6f;
            const float freq_thold = 100.0f;
            if (::vad_simple(pcmf32_new, WHISPER_SAMPLE_RATE, 1000, vad_thold, freq_thold, false)) {
                audio.get(length_ms, pcmf32);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            t_last = t_now;
        }

        if (pcmf32.empty()) continue;

        // Run inference
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_progress = false;
        wparams.print_realtime = false;
        wparams.no_context = true;
        wparams.language = "en";
        // Updated to match example's thread count
        wparams.n_threads = std::min(4, (int32_t) std::thread::hardware_concurrency());
        wparams.temperature = 0.0f;
        wparams.single_segment = !use_vad;
        wparams.max_tokens = 32;
        wparams.audio_ctx = 0;

        if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) != 0) {
            std::cerr << "Failed to process audio.\n";
            break;
        }

        // Get the latest transcription
        const int n_segments = whisper_full_n_segments(ctx);
        if (n_segments > 0) {
            std::string current_transcription;
            for (int i = 0; i < n_segments; ++i) {
                const char* text = whisper_full_get_segment_text(ctx, i);
                if (text) {
                    current_transcription += text;
                }
            }

            // Move text processing to a separate thread or do it more efficiently
            // For now, just continue with the original approach
            remove_bracketed_text(current_transcription);
            lrtrim(current_transcription);

            // Skip if the transcription is empty after cleaning
            if (current_transcription.empty()) {
                continue;
            }

            std::cout << current_transcription << std::endl;

            nlohmann::json transcribe_message = {
                {"type", "transcribe"},
                {"content", current_transcription}
            };

            // Broadcast to WebSocket clients
            state->broadcast(transcribe_message.dump());

            last_transcription = current_transcription;
        }

        // Keep part of the audio for next iteration
        pcmf32_old = std::vector<float>(pcmf32.end() - n_samples_keep, pcmf32.end());
    }

    std::cout << "Shutting down..." << std::endl;
    audio.pause();
    SDL_Quit();
    is_running = false;
    if (ws_thread.joinable()) ws_thread.join();

    whisper_free(ctx);

    return 0;
}
