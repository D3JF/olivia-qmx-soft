#include "qmx_radio.hpp"

#include <utility>

namespace olivia {

QmxRadio::QmxRadio(CommandWriter command_writer)
    : command_writer_(std::move(command_writer)) {}

bool QmxRadio::initialize(std::string& error_message) {
    return send_command("FA00007040000;", error_message) &&
           send_command("MD2;", error_message);
}

bool QmxRadio::begin_transmit(std::string& error_message) {
    return send_command("TX;", error_message);
}

bool QmxRadio::begin_receive(std::string& error_message) {
    return send_command("RX;", error_message);
}

bool QmxRadio::send_command(const char* command, std::string& error_message) {
    if (!command_writer_) {
        error_message = "QMX+ command writer is not configured.";
        return false;
    }
    return command_writer_(command, error_message);
}

}  // namespace olivia
