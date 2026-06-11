#include "psp_debug_socket.h"
#include "psp_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

static constexpr uint32_t PSP_ADDR_MASK  = 0x07FFFFFFU;
static constexpr size_t   MAX_READ_BYTES = 65536;

static std::atomic<bool>  g_debug_running{false};
static std::thread        g_debug_thread;
static int                g_server_fd = -1;

// Static rdram pointer and size set by psp_debug_socket_start.
static uint8_t* g_rdram      = nullptr;
static size_t   g_rdram_size = 0;

// Injected button overlay (declared extern in psp_runtime.h).
// Set by the B command below; consumed by the sceCtrl HLE.
std::atomic<uint32_t> g_injected_buttons{0};
std::atomic<int64_t>  g_injected_buttons_deadline_ms{0};

/// Handle one connected client: read lines, serve R commands.
/// Returns when the client disconnects or the server is shutting down.
static void handle_client(int client_fd) {
    char line[128];
    size_t pos = 0;

    while (g_debug_running.load(std::memory_order_relaxed)) {
        // Read one byte at a time until '\n'
        char ch;
        ssize_t n = ::recv(client_fd, &ch, 1, 0);
        if (n <= 0) {
            break;
        }
        if (ch == '\n' || ch == '\r') {
            if (pos == 0) continue;
            line[pos] = '\0';
            pos = 0;

            // Protocol (one command per line):
            //   R <hex_addr> <decimal_size>  -- read PSP memory, replies
            //                                   with <size> raw bytes
            //   B <hex_mask> <decimal_ms>    -- inject PSP button mask for
            //                                   <ms> milliseconds (no reply)
            if (line[0] == 'B' && line[1] == ' ') {
                char* end = nullptr;
                unsigned long mask = std::strtoul(line + 2, &end, 16);
                unsigned long hold_ms = 0;
                if (end && *end == ' ') {
                    hold_ms = std::strtoul(end + 1, nullptr, 10);
                }
                // Clamp: bounds how long an injected press can stick
                // and keeps the deadline math overflow-free.
                if (hold_ms > 60000) {
                    hold_ms = 60000;
                }
                if (hold_ms > 0) {
                    auto now_ms = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            std::chrono::steady_clock::now()
                                .time_since_epoch()).count();
                    g_injected_buttons.store(
                        static_cast<uint32_t>(mask),
                        std::memory_order_relaxed);
                    g_injected_buttons_deadline_ms.store(
                        now_ms + static_cast<int64_t>(hold_ms),
                        std::memory_order_relaxed);
                }
                continue;
            }

            unsigned long addr_raw = 0;
            unsigned long read_size = 0;
            if (line[0] == 'R' && line[1] == ' ') {
                char* end = nullptr;
                addr_raw  = std::strtoul(line + 2, &end, 16);
                if (end && *end == ' ') {
                    read_size = std::strtoul(end + 1, nullptr, 10);
                }
            }

            if (read_size == 0 || read_size > MAX_READ_BYTES) {
                // Out-of-spec request: send nothing and continue
                continue;
            }

            uint32_t masked = static_cast<uint32_t>(addr_raw) & PSP_ADDR_MASK;
            // Clamp to buffer bounds; send zeros for out-of-range portion
            static uint8_t zero_buf[MAX_READ_BYTES];
            const uint8_t* src;
            if (static_cast<size_t>(masked) + read_size <= g_rdram_size) {
                src = g_rdram + masked;
            } else if (static_cast<size_t>(masked) >= g_rdram_size) {
                std::memset(zero_buf, 0, read_size);
                src = zero_buf;
            } else {
                // Partially in-range: copy valid portion, zero the rest
                size_t valid = g_rdram_size - static_cast<size_t>(masked);
                std::memcpy(zero_buf, g_rdram + masked, valid);
                std::memset(zero_buf + valid, 0, read_size - valid);
                src = zero_buf;
            }

            // Send raw bytes
            size_t sent = 0;
            while (sent < read_size) {
                ssize_t w = ::send(client_fd,
                                   src + sent,
                                   read_size - sent, 0);
                if (w <= 0) goto client_done;
                sent += static_cast<size_t>(w);
            }
        } else {
            if (pos < sizeof(line) - 1) {
                line[pos++] = ch;
            }
        }
    }
client_done:
    ::close(client_fd);
}

/// Main loop for the background server thread.
static void server_loop(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::fprintf(stderr,
            "[DEBUG] Failed to create debug socket\n");
        return;
    }
    g_server_fd = fd;

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr,
            "[DEBUG] Failed to bind debug socket on port %d\n", port);
        ::close(fd);
        g_server_fd = -1;
        return;
    }

    if (::listen(fd, 1) < 0) {
        std::fprintf(stderr, "[DEBUG] Failed to listen on debug socket\n");
        ::close(fd);
        g_server_fd = -1;
        return;
    }

    std::fprintf(stderr,
        "[DEBUG] Debug socket listening on port %d\n", port);

    while (g_debug_running.load(std::memory_order_relaxed)) {
        // Use select() with 1-second timeout so we can check g_debug_running
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv{1, 0};

        int ready = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(
            fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) continue;

        handle_client(client_fd);
        // client socket is closed inside handle_client
    }

    ::close(fd);
    g_server_fd = -1;
}

void psp_debug_socket_start(uint8_t* rdram, size_t rdram_size, int port) {
    g_rdram      = rdram;
    g_rdram_size = rdram_size;
    g_debug_running.store(true, std::memory_order_relaxed);
    g_debug_thread = std::thread(server_loop, port);
}

void psp_debug_socket_stop() {
    if (!g_debug_running.load(std::memory_order_relaxed)) return;
    g_debug_running.store(false, std::memory_order_relaxed);
    // Wake the accept() select() by shutting down the server socket
    if (g_server_fd >= 0) {
        ::shutdown(g_server_fd, SHUT_RDWR);
    }
    if (g_debug_thread.joinable()) {
        g_debug_thread.join();
    }
}
