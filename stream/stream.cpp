#include "whisper.h"
#include "common-sdl.h"
#include <iostream>
#include <set>
#include <termios.h>
#include <thread>
#include <atomic>
#include <string>
#include <unistd.h>
#include <vector>
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

void remove_bracketed_text(char* text) {
    char *read = text, *write = text;
    bool in_bracket = false, in_paren = false;

    while (*read) {
        if (!in_bracket && !in_paren) {
            if (*read == '[')      { in_bracket = true; read++; continue; }
            if (*read == '(')      { in_paren = true;   read++; continue; }
            *write++ = *read++;
        } else {
            if (in_bracket && *read == ']') { in_bracket = false; read++; continue; }
            if (in_paren   && *read == ')') { in_paren = false;   read++; continue; }
            read++;
        }
    }
    *write = '\0';  // Null-terminate
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

void lrtrim(char* s) {
    if (!s || !*s) return;  // Handle NULL/empty

    // Trim leading whitespace
    char* start = s;
    while (*start && isspace((unsigned char)*start)) start++;

    // Trim trailing whitespace
    char* end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';

    // Shift back if needed
    if (start != s) memmove(s, start, end - start + 1);
}

// Calculate Levenshtein distance between two strings
float levenshtein_distance(const std::string& s1, const std::string& s2) {
    const size_t len1 = s1.size();
    const size_t len2 = s2.size();
    std::vector<std::vector<size_t>> d(len1 + 1, std::vector<size_t>(len2 + 1));

    for (size_t i = 0; i <= len1; ++i) d[i][0] = i;
    for (size_t j = 0; j <= len2; ++j) d[0][j] = j;

    for (size_t i = 1; i <= len1; ++i) {
        for (size_t j = 1; j <= len2; ++j) {
            size_t cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1,      // Deletion
                                d[i][j - 1] + 1,      // Insertion
                                d[i - 1][j - 1] + cost}); // Substitution
        }
    }

    return static_cast<float>(d[len1][len2]);
}

// Helper function to find minimum of three values
static size_t min3(size_t a, size_t b, size_t c) {
    if (a < b) return (a < c) ? a : c;
    return (b < c) ? b : c;
}

float levenshtein_distance(const char* s1, const char* s2) {
    size_t len1 = strlen(s1), len2 = strlen(s2);

    // Handle empty strings
    if (len1 == 0) return (float)len2;
    if (len2 == 0) return (float)len1;

    // Allocate two rows
    size_t* prev_row = (size_t*)malloc((len2 + 1) * sizeof(size_t));
    size_t* curr_row = (size_t*)malloc((len2 + 1) * sizeof(size_t));

    // Check for allocation failure
    if (!prev_row || !curr_row) {
        free(prev_row);
        free(curr_row);
        return -1.0f;  // Error indicator
    }

    // Initialize first row
    for (size_t j = 0; j <= len2; j++) {
        prev_row[j] = j;
    }

    // Compute distance
    for (size_t i = 1; i <= len1; i++) {
        curr_row[0] = i;
        for (size_t j = 1; j <= len2; j++) {
            size_t cost = (s1[i-1] == s2[j-1]) ? 0 : 1;
            curr_row[j] = min3(
                curr_row[j-1] + 1,      // Insertion
                prev_row[j] + 1,        // Deletion
                prev_row[j-1] + cost    // Substitution
                );
        }

        // Swap rows for next iteration
        size_t* tmp = prev_row;
        prev_row = curr_row;
        curr_row = tmp;
    }

    float result = (float)prev_row[len2];

    // Clean up
    free(prev_row);
    free(curr_row);

    return result;
}

// Calculate similarity score (0 = identical, 1 = completely different)
float string_similarity(const std::string& s1, const std::string& s2) {
    if (s1.empty() || s2.empty()) return 1.0f;
    float max_len = static_cast<float>(std::max(s1.size(), s2.size()));
    return levenshtein_distance(s1, s2) / max_len;
}

float string_similarity(const char* s1, const char* s2) {
    if (!s1 || !s2 || !*s1 || !*s2) return 1.0f;  // Handle empty strings
    size_t len1 = strlen(s1), len2 = strlen(s2);
    float max_len = (float)(len1 > len2 ? len1 : len2);
    return levenshtein_distance(s1, s2) / max_len;
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
    cparams.flash_attn = false;

    struct whisper_context* ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx) {
        std::cerr << "Failed to initialize Whisper context.\n";
        return 1;
    }

    const int step_ms = 10000;
    const int length_ms = 12000;
    const int keep_ms = 1000;
    const int n_samples_30s  = (1e-3*30000.0)*WHISPER_SAMPLE_RATE;
    const int n_samples_len  = (1e-3*length_ms)*WHISPER_SAMPLE_RATE;
    const int n_samples_step = (1e-3*step_ms)*WHISPER_SAMPLE_RATE;
    const int n_samples_keep = (1e-3*keep_ms)*WHISPER_SAMPLE_RATE;
    std::vector<float> pcmf32(n_samples_30s, 0.0f);
    std::vector<float> pcmf32_new(n_samples_30s, 0.0f);
    std::vector<float> pcmf32_old;

    // Initialize audio capture
    audio_async audio(15000);

    if (!audio.init(-1, WHISPER_SAMPLE_RATE)) {
        std::cerr << "Failed to initialize audio capture.\n";
        return 1;
    }
    audio.resume();

    // Start the WebSocket server
    auto state = std::make_shared<shared_state>();
    std::thread ws_thread(websocket_server, state);

    while (is_running) {
        is_running = sdl_poll_events();
        if (!is_running) {
            break;
        }

        // Capture audio
        while (true) {
            // handle Ctrl + C
            is_running = sdl_poll_events();
            if (!is_running) {
                break;
            }
            audio.get(step_ms, pcmf32_new);

            if ((int) pcmf32_new.size() > 2*n_samples_step) {
                fprintf(stderr, "\n\n%s: WARNING: cannot process audio fast enough, dropping audio ...\n\n", __func__);
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
        // take up to params.length_ms audio from previous iteration
        const int n_samples_take = std::min((int) pcmf32_old.size(), std::max(0, n_samples_keep + n_samples_len - n_samples_new));

        pcmf32.resize(n_samples_new + n_samples_take);

        for (int i = 0; i < n_samples_take; i++) {
            pcmf32[i] = pcmf32_old[pcmf32_old.size() - n_samples_take + i];
        }

        memcpy(pcmf32.data() + n_samples_take, pcmf32_new.data(), n_samples_new*sizeof(float));

        pcmf32_old = pcmf32;

        if (pcmf32.empty()) continue;

        // Run inference only if speech is detected
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_progress = false;
        wparams.print_realtime = false;
        wparams.no_context = true; // Disable context carryover
        wparams.language = "en";
        wparams.max_tokens = 32;
        wparams.no_timestamps = true;
        wparams.n_threads = std::min(static_cast<int32_t>(std::thread::hardware_concurrency()/2), 8);
        wparams.temperature = 0.0f;
        wparams.greedy.best_of = 1;
        wparams.single_segment = true;
        wparams.audio_ctx = 0;
        wparams.prompt_tokens = nullptr;
        wparams.prompt_n_tokens = 0;

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

            // Remove partial bracketed text (e.g., [inaudible], [ Background Conversations ])
            remove_bracketed_text(current_transcription);

            // Trim leading and trailing whitespace
            lrtrim(current_transcription);

            // Skip if the transcription is empty after cleaning
            if (current_transcription.empty()) {
                continue;
            }

            if (last_transcription.empty() ||
                string_similarity(last_transcription, current_transcription) > 0.1f) {

                std::cout << current_transcription << std::endl; // Print the new content

                nlohmann::json transcribe_message = {
                    {"type", "transcribe"},
                    {"content", current_transcription}
                };

                // Broadcast the new content to WebSocket clients
                state->broadcast(transcribe_message.dump());

                last_transcription = current_transcription;
            }
        }

        // keep part of the audio for next iteration to try to mitigate word boundary issues
        pcmf32_old = std::vector<float>(pcmf32.end() - n_samples_keep, pcmf32.end());
    }

    std::cout << "CTRL-C again to exit..." << std::endl;
    audio.pause();
    SDL_Quit();
    is_running = false;
    if (ws_thread.joinable()) ws_thread.join();

    whisper_free(ctx);

    return 0;
}
