#include "serial_port.hpp"

#include <iostream>

int main() {
    for (const auto& port : olivia::SerialPort::enumerate()) {
        std::cout << port.path << " (" << port.description << ")\n";
    }
    const auto qmx_port = olivia::SerialPort::find_qmx_port();
    std::cout << "QMX+ candidate: "
              << (qmx_port.empty() ? "none" : qmx_port) << '\n';
    return 0;
}
