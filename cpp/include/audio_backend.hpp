#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace olivia {

struct AudioDevice {
    int index = -1;
    std::string name;
    int input_channels = 0;
    int output_channels = 0;
    double default_sample_rate = 0.0;
};

struct AudioConfiguration {
    int input_device = -1;
    int output_device = -1;
    int sample_rate = 8000;
    int frames_per_buffer = 256;
};

class AudioBackend {
public:
    using InputCallback = std::function<void(const std::vector<float>&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    virtual ~AudioBackend() = default;
    virtual std::vector<AudioDevice> devices() const = 0;
    virtual bool start(const AudioConfiguration& configuration,
                       InputCallback input_callback,
                       ErrorCallback error_callback) = 0;
    virtual void stop() = 0;
    virtual bool queue_output(const std::vector<float>& samples) = 0;
};

class PortAudioBackend final : public AudioBackend {
public:
    PortAudioBackend();
    ~PortAudioBackend() override;

    PortAudioBackend(const PortAudioBackend&) = delete;
    PortAudioBackend& operator=(const PortAudioBackend&) = delete;

    std::vector<AudioDevice> devices() const override;
    static int find_qmx_device(const std::vector<AudioDevice>& devices);
    bool start(const AudioConfiguration& configuration,
               InputCallback input_callback,
               ErrorCallback error_callback) override;
    void stop() override;
    bool queue_output(const std::vector<float>& samples) override;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace olivia
