#include "olivia_modem.hpp"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    olivia::OliviaModem modem(8, 250, 8000);
    const std::string message = "Hello, station B!";
    const auto waveform = modem.modulate(message);
    assert(!waveform.empty());

    std::string decoded;
    for (std::size_t offset = 0; offset < waveform.size(); offset += 137) {
        const auto end = std::min(waveform.size(), offset + 137);
        decoded += modem.demodulate(
            std::vector<float>(waveform.begin() + offset,
                               waveform.begin() + end));
    }
    assert(decoded == message);

    bool rejected = false;
    try {
        olivia::OliviaModem invalid(3, 250, 8000);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    std::cout << "Olivia C++ codec tests passed\n";
}
