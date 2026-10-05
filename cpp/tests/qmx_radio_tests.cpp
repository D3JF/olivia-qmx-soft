#include "qmx_radio.hpp"
#include "serial_port.hpp"

#include <cassert>
#include <string>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

void test_command_order() {
    std::vector<std::string> commands;
    olivia::QmxRadio radio(
        [&commands](const std::string& command, std::string&) {
            commands.push_back(command);
            return true;
        });

    std::string error;
    assert(radio.initialize(error));
    assert(radio.begin_transmit(error));
    assert(radio.begin_receive(error));
    assert((commands == std::vector<std::string>{
                           "FA00007040000;", "MD2;", "TX;", "RX;"}));
}

void test_command_failure_stops_sequence() {
    std::vector<std::string> commands;
    olivia::QmxRadio radio(
        [&commands](const std::string& command, std::string& error) {
            commands.push_back(command);
            if (command == "FA00007040000;") {
                error = "serial write failed";
                return false;
            }
            return true;
        });

    std::string error;
    assert(!radio.initialize(error));
    assert(error == "serial write failed");
    assert((commands == std::vector<std::string>{"FA00007040000;"}));
}

#ifndef _WIN32
void test_serial_byte_io() {
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    assert(master >= 0);
    assert(fcntl(master, F_SETFL, O_NONBLOCK) == 0);
    assert(grantpt(master) == 0);
    assert(unlockpt(master) == 0);
    const char* slave_path = ptsname(master);
    assert(slave_path != nullptr);

    olivia::SerialPort port;
    std::string error;
    assert(port.open(slave_path, 115200, error));
    assert(port.is_open());

    termios settings{};
    assert(tcgetattr(master, &settings) == 0);
    assert(cfgetispeed(&settings) == B115200);
    assert(cfgetospeed(&settings) == B115200);

    const std::string command = "TX;";
    assert(port.write(command, error));

    std::string received;
    char buffer[16] = {};
    for (int attempt = 0; attempt < 20 && received.size() < command.size();
         ++attempt) {
        const auto count = ::read(master, buffer, sizeof(buffer));
        if (count > 0) {
            received.append(buffer, static_cast<std::size_t>(count));
        } else {
            assert(errno == EAGAIN || errno == EWOULDBLOCK ||
                   errno == EIO || errno == 0);
            usleep(1000);
        }
    }
    assert(received == command);

    port.close();
    assert(!port.is_open());
    ::close(master);
}
#endif

}  // namespace

int main() {
    test_command_order();
    test_command_failure_stops_sequence();
#ifndef _WIN32
    test_serial_byte_io();
#endif
}
