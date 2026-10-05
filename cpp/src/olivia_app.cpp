#include "olivia_modem.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <utility>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_handle = SOCKET;
constexpr socket_handle invalid_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle = int;
constexpr socket_handle invalid_socket = -1;
#endif

namespace {

constexpr int sample_rate = 8000;
constexpr int default_channel_port = 45800;
constexpr std::size_t packet_samples = 2048;
constexpr std::size_t packet_header_bytes = 12;

void close_socket(socket_handle socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

void initialize_sockets() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("WSAStartup failed.");
    }
#endif
}

void cleanup_sockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

std::uint16_t read_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
}

std::uint32_t read_u32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

void write_u16(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 8));
    data.push_back(static_cast<std::uint8_t>(value));
}

void write_u32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 24));
    data.push_back(static_cast<std::uint8_t>(value >> 16));
    data.push_back(static_cast<std::uint8_t>(value >> 8));
    data.push_back(static_cast<std::uint8_t>(value));
}

std::string normalize_message(std::string message) {
    std::size_t position = 0;
    while ((position = message.find("\\n", position)) != std::string::npos) {
        message.replace(position, 2, "\n");
        ++position;
    }
    return message;
}

struct Options {
    char station = '\0';
    int channel_port = default_channel_port;
    std::string message;
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program
              << " --station A|B [--channel-port PORT] [--message TEXT]\n"
                 "\n"
                 "Run two instances with the same channel port to exchange\n"
                 "Olivia 8/250 messages over localhost UDP.\n"
                 "Without --message, type messages at the prompt. Use \\n for\n"
                 "line breaks and Ctrl+C to stop.\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (argument == "--station" && index + 1 < argc) {
            options.station = argv[++index][0];
        } else if (argument == "--channel-port" && index + 1 < argc) {
            options.channel_port = std::stoi(argv[++index]);
        } else if (argument == "--message" && index + 1 < argc) {
            options.message = argv[++index];
        } else {
            throw std::invalid_argument("Unknown or incomplete argument: " +
                                        argument);
        }
    }
    if (options.station != 'A' && options.station != 'B') {
        throw std::invalid_argument("--station must be A or B.");
    }
    if (options.channel_port < 1 || options.channel_port > 65532) {
        throw std::invalid_argument("--channel-port must be between 1 and 65532.");
    }
    return options;
}

class Simulator {
public:
    explicit Simulator(Options options)
        : options_(std::move(options)),
          modem_(8, 250, sample_rate),
          socket_(invalid_socket),
          running_(true),
          transaction_id_(0) {}

    ~Simulator() {
        stop_receiver();
    }

    void run() {
        initialize_sockets();
        try {
            open_socket();
            receiver_ = std::thread(&Simulator::receive_loop, this);
            print("Station " + std::string(1, options_.station) +
                  " listening on UDP " + std::to_string(listen_port()) +
                  "; type a message and press Enter.");
            if (!options_.message.empty()) {
                transmit(normalize_message(options_.message));
            }
            if (options_.message.empty()) {
                std::string line;
                while (running_ && std::getline(std::cin, line)) {
                    line = normalize_message(line);
                    if (!line.empty()) {
                        transmit(line);
                    }
                }
            }
            stop_receiver();
        } catch (...) {
            stop_receiver();
            cleanup_sockets();
            throw;
        }
        cleanup_sockets();
    }

private:
    int listen_port() const {
        return options_.channel_port + (options_.station == 'A' ? 1 : 2);
    }

    int target_port() const {
        return options_.channel_port + (options_.station == 'A' ? 2 : 1);
    }

    void print(const std::string& message) {
        std::lock_guard<std::mutex> lock(output_mutex_);
        std::cout << message << '\n' << std::flush;
    }

    void stop_receiver() {
        running_ = false;
        if (socket_ != invalid_socket) {
            close_socket(socket_);
            socket_ = invalid_socket;
        }
        if (receiver_.joinable()) {
            receiver_.join();
        }
    }

    void open_socket() {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalid_socket) {
            throw std::runtime_error("Could not create UDP socket.");
        }
        int receive_buffer = 4 * 1024 * 1024;
        setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&receive_buffer),
                   sizeof(receive_buffer));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(listen_port()));
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) < 0) {
            close_socket(socket_);
            socket_ = invalid_socket;
            throw std::runtime_error(
                "Could not bind UDP port " + std::to_string(listen_port()) +
                ". Close the other instance or choose another channel.");
        }
    }

    void transmit(const std::string& message) {
        const auto samples = modem_.modulate(message);
        ++transaction_id_;
        const std::size_t total_packets =
            (samples.size() + packet_samples - 1) / packet_samples;
        if (total_packets > 65535) {
            throw std::runtime_error("Message is too large for simulator packet format.");
        }

        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        destination.sin_port = htons(static_cast<std::uint16_t>(target_port()));
        for (std::size_t sequence = 0; sequence < total_packets; ++sequence) {
            const std::size_t start = sequence * packet_samples;
            const std::size_t count =
                std::min(packet_samples, samples.size() - start);
            std::vector<std::uint8_t> packet;
            packet.reserve(packet_header_bytes + count * sizeof(float));
            packet.insert(packet.end(), {'O', 'L', 'I', 'V'});
            write_u32(packet, transaction_id_);
            write_u16(packet, static_cast<std::uint16_t>(sequence));
            write_u16(packet, static_cast<std::uint16_t>(total_packets));
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(
                samples.data() + static_cast<std::ptrdiff_t>(start));
            packet.insert(packet.end(), bytes, bytes + count * sizeof(float));
            const auto sent = sendto(
                socket_, reinterpret_cast<const char*>(packet.data()),
                static_cast<int>(packet.size()), 0,
                reinterpret_cast<const sockaddr*>(&destination),
                sizeof(destination));
            if (sent < 0 || sent != static_cast<int>(packet.size())) {
                throw std::runtime_error("Could not send simulator UDP packet.");
            }
            std::this_thread::sleep_for(std::chrono::duration<double>(
                static_cast<double>(count) / sample_rate));
        }
        print("Transmitted: " + message);
    }

    void receive_loop() {
        std::vector<std::uint8_t> packet(65536);
        while (running_) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(socket_, &read_set);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 200000;
            const auto ready = select(
#ifdef _WIN32
                0,
#else
                socket_ + 1,
#endif
                &read_set, nullptr, nullptr, &timeout);
            if (!running_) {
                return;
            }
            if (ready <= 0) {
                continue;
            }
            sockaddr_in sender{};
#ifdef _WIN32
            int sender_size = sizeof(sender);
#else
            socklen_t sender_size = sizeof(sender);
#endif
            const auto received = recvfrom(
                socket_, reinterpret_cast<char*>(packet.data()),
                static_cast<int>(packet.size()), 0,
                reinterpret_cast<sockaddr*>(&sender), &sender_size);
            if (received < 0) {
                if (running_) {
                    print("Receive error.");
                }
                return;
            }
            if (received < static_cast<int>(packet_header_bytes) ||
                std::memcmp(packet.data(), "OLIV", 4) != 0) {
                continue;
            }
            const auto total = read_u16(packet.data() + 10);
            const auto sequence = read_u16(packet.data() + 8);
            if (total == 0 || sequence >= total) {
                continue;
            }
            // The application sends complete ordered packets on localhost.
            // Each packet is fed incrementally, preserving codec stream state.
            const auto payload_bytes = received - packet_header_bytes;
            if (payload_bytes % sizeof(float) != 0) {
                continue;
            }
            std::vector<float> samples(payload_bytes / sizeof(float));
            std::memcpy(samples.data(), packet.data() + packet_header_bytes,
                        payload_bytes);
            const auto decoded = modem_.demodulate(samples);
            if (!decoded.empty()) {
                print("Received: " + decoded);
            }
        }
    }

    Options options_;
    olivia::OliviaModem modem_;
    socket_handle socket_;
    std::atomic<bool> running_;
    std::thread receiver_;
    std::uint32_t transaction_id_;
    std::mutex output_mutex_;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        Simulator simulator(options);
        simulator.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
