#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace olivia {

struct SerialPortInfo {
    std::string path;
    std::string description;
};

class SerialPort {
public:
    static std::vector<SerialPortInfo> enumerate();
    static std::string find_qmx_port();

    SerialPort();
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool open(const std::string& path, int baud_rate,
              std::string& error_message);
    bool write(const std::string& data, std::string& error_message);
    void close();
    bool is_open() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace olivia
