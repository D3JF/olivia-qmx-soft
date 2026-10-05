#include "serial_port.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace olivia {
namespace {

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool looks_like_qmx(const SerialPortInfo& port) {
    const auto text = lowercase(port.path + " " + port.description);
    return text.find("qmx") != std::string::npos ||
           text.find("qrp labs") != std::string::npos;
}

#ifdef _WIN32

std::string win_error(DWORD error) {
    char buffer[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, buffer, sizeof(buffer), nullptr);
    return std::string(buffer);
}

#else

speed_t baud_constant(int baud_rate) {
    switch (baud_rate) {
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        default:
            return 0;
    }
}

#endif

}  // namespace

struct SerialPort::Impl {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int file_descriptor = -1;
#endif
};

std::vector<SerialPortInfo> SerialPort::enumerate() {
    std::vector<SerialPortInfo> ports;
#ifdef _WIN32
    for (int index = 1; index <= 256; ++index) {
        ports.push_back(
            {std::string("COM") + std::to_string(index), "Windows COM port"});
    }
#else
    const char* directories[] = {"/dev", "/dev/serial/by-id"};
    for (const char* directory_name : directories) {
        DIR* directory = opendir(directory_name);
        if (directory == nullptr) {
            continue;
        }
        while (const auto* entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            if (directory_name == std::string("/dev") &&
                name.find("ttyUSB") != 0 && name.find("ttyACM") != 0 &&
                name.find("ttyS") != 0) {
                continue;
            }
            ports.push_back({std::string(directory_name) + "/" + name, name});
        }
        closedir(directory);
    }
#endif
    return ports;
}

std::string SerialPort::find_qmx_port() {
    for (const auto& port : enumerate()) {
        if (looks_like_qmx(port)) {
            return port.path;
        }
    }
    return {};
}

SerialPort::SerialPort() : impl_(new Impl) {}

SerialPort::~SerialPort() {
    close();
    delete impl_;
}

bool SerialPort::open(const std::string& path, int baud_rate,
                      std::string& error_message) {
    close();
#ifdef _WIN32
    const std::string device_path =
        path.rfind("COM", 0) == 0 && path.size() > 4
            ? "\\\\.\\" + path
            : path;
    impl_->handle = CreateFileA(device_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        error_message = win_error(GetLastError());
        return false;
    }
    DCB settings{};
    settings.DCBlength = sizeof(settings);
    if (!GetCommState(impl_->handle, &settings)) {
        error_message = win_error(GetLastError());
        close();
        return false;
    }
    settings.BaudRate = baud_rate;
    settings.ByteSize = 8;
    settings.StopBits = ONESTOPBIT;
    settings.Parity = NOPARITY;
    if (!SetCommState(impl_->handle, &settings)) {
        error_message = win_error(GetLastError());
        close();
        return false;
    }
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    SetCommTimeouts(impl_->handle, &timeouts);
    return true;
#else
    const speed_t speed = baud_constant(baud_rate);
    if (speed == 0) {
        error_message = "Unsupported serial baud rate.";
        return false;
    }
    impl_->file_descriptor = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (impl_->file_descriptor < 0) {
        error_message = std::strerror(errno);
        return false;
    }
    termios settings{};
    if (tcgetattr(impl_->file_descriptor, &settings) != 0) {
        error_message = std::strerror(errno);
        close();
        return false;
    }
    cfmakeraw(&settings);
    cfsetispeed(&settings, speed);
    cfsetospeed(&settings, speed);
    settings.c_cflag |= CLOCAL | CREAD;
    settings.c_cflag &= ~CSTOPB;
    settings.c_cflag &= ~CRTSCTS;
    if (tcsetattr(impl_->file_descriptor, TCSANOW, &settings) != 0) {
        error_message = std::strerror(errno);
        close();
        return false;
    }
    return true;
#endif
}

bool SerialPort::write(const std::string& data, std::string& error_message) {
    if (!is_open()) {
        error_message = "Serial port is not open.";
        return false;
    }
#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(impl_->handle, data.data(), static_cast<DWORD>(data.size()),
                   &written, nullptr) ||
        written != data.size()) {
        error_message = win_error(GetLastError());
        return false;
    }
    return true;
#else
    const auto result = ::write(impl_->file_descriptor, data.data(), data.size());
    if (result < 0 || static_cast<std::size_t>(result) != data.size()) {
        error_message = std::strerror(errno);
        return false;
    }
    return true;
#endif
}

void SerialPort::close() {
#ifdef _WIN32
    if (impl_->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->handle);
        impl_->handle = INVALID_HANDLE_VALUE;
    }
#else
    if (impl_->file_descriptor >= 0) {
        ::close(impl_->file_descriptor);
        impl_->file_descriptor = -1;
    }
#endif
}

bool SerialPort::is_open() const noexcept {
#ifdef _WIN32
    return impl_->handle != INVALID_HANDLE_VALUE;
#else
    return impl_->file_descriptor >= 0;
#endif
}

}  // namespace olivia
