#pragma once

#include <functional>
#include <string>

namespace olivia {

class QmxRadio {
public:
    using CommandWriter =
        std::function<bool(const std::string&, std::string&)>;

    explicit QmxRadio(CommandWriter command_writer);

    bool initialize(std::string& error_message);
    bool begin_transmit(std::string& error_message);
    bool begin_receive(std::string& error_message);

private:
    bool send_command(const char* command, std::string& error_message);

    CommandWriter command_writer_;
};

}  // namespace olivia
