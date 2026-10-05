#include "audio_backend.hpp"

#include <iostream>

int main() {
    const olivia::PortAudioBackend audio;
    const auto devices = audio.devices();
    const int qmx_device = olivia::PortAudioBackend::find_qmx_device(devices);
    for (const auto& device : devices) {
        std::cout << device.index << ": " << device.name << " (input "
                  << device.input_channels << ", output "
                  << device.output_channels << ", default rate "
                  << device.default_sample_rate << ")\n";
    }
    if (qmx_device >= 0) {
        std::cout << "QMX+ candidate: " << qmx_device << '\n';
    } else {
        std::cout << "QMX+ candidate: none\n";
    }
    return 0;
}
