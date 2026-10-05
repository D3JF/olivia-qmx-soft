#include "audio_backend.hpp"

#include <portaudio.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace olivia {
namespace {

std::string pa_error(PaError error) {
    return std::string(Pa_GetErrorText(error));
}

}  // namespace

struct PortAudioBackend::Impl {
    static constexpr std::size_t max_callback_frames = 4096;
    static constexpr std::size_t input_block_count = 32;
    static constexpr std::size_t output_sample_count = 262144;
    PaStream* stream = nullptr;
    AudioConfiguration configuration;
    InputCallback input_callback;
    ErrorCallback error_callback;
    std::mutex mutex;
    std::condition_variable condition;
    std::array<std::array<float, max_callback_frames>, input_block_count>
        input_blocks{};
    std::array<std::size_t, input_block_count> input_sizes{};
    std::size_t input_head = 0;
    std::size_t input_tail = 0;
    std::size_t input_count = 0;
    std::array<float, output_sample_count> output_samples{};
    std::size_t output_head = 0;
    std::size_t output_tail = 0;
    std::size_t output_count = 0;
    std::thread input_thread;
    bool running = false;
    bool stopping = false;
    bool initialized = false;

    static int callback(const void* input, void* output,
                        unsigned long frame_count,
                        const PaStreamCallbackTimeInfo*,
                        PaStreamCallbackFlags, void* user_data) {
        auto* state = static_cast<Impl*>(user_data);
        const auto* input_samples = static_cast<const float*>(input);
        auto* output_samples = static_cast<float*>(output);
        const auto frame_count_size = static_cast<std::size_t>(frame_count);

        if (input_samples != nullptr) {
            std::lock_guard lock(state->mutex);
            if (frame_count_size <= max_callback_frames &&
                state->input_count < input_block_count) {
                std::copy_n(input_samples, frame_count_size,
                            state->input_blocks[state->input_tail].data());
                state->input_sizes[state->input_tail] = frame_count_size;
                state->input_tail =
                    (state->input_tail + 1) % input_block_count;
                ++state->input_count;
                state->condition.notify_one();
            }
        }

        if (output_samples != nullptr) {
            std::fill(output_samples, output_samples + frame_count_size, 0.0F);
            std::lock_guard lock(state->mutex);
            const auto copied = std::min(frame_count_size, state->output_count);
            for (std::size_t index = 0; index < copied; ++index) {
                output_samples[index] = state->output_samples[state->output_head];
                state->output_head =
                    (state->output_head + 1) % output_sample_count;
            }
            state->output_count -= copied;
        }
        return paContinue;
    }

    void consume_input() {
        for (;;) {
            std::vector<float> block;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] {
                    return stopping || input_count != 0;
                });
                if (stopping && input_count == 0) {
                    return;
                }
                const auto size = input_sizes[input_head];
                block.assign(input_blocks[input_head].begin(),
                             input_blocks[input_head].begin() +
                                 static_cast<std::ptrdiff_t>(size));
                input_head = (input_head + 1) % input_block_count;
                --input_count;
            }
            if (input_callback) {
                input_callback(block);
            }
        }
    }
};

PortAudioBackend::PortAudioBackend() : impl_(new Impl) {
    const auto error = Pa_Initialize();
    if (error != paNoError) {
        impl_->initialized = false;
    } else {
        impl_->initialized = true;
    }
}

PortAudioBackend::~PortAudioBackend() {
    stop();
    Pa_Terminate();
    delete impl_;
}

std::vector<AudioDevice> PortAudioBackend::devices() const {
    std::vector<AudioDevice> result;
    if (!impl_->initialized) {
        return result;
    }
    const int count = Pa_GetDeviceCount();
    if (count < 0) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const auto* info = Pa_GetDeviceInfo(index);
        if (info == nullptr) {
            continue;
        }
        result.push_back({index, info->name != nullptr ? info->name : "",
                          info->maxInputChannels, info->maxOutputChannels,
                          info->defaultSampleRate});
    }
    return result;
}

int PortAudioBackend::find_qmx_device(
    const std::vector<AudioDevice>& available_devices) {
    for (const auto& device : available_devices) {
        std::string name = device.name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (name.find("qmx") != std::string::npos ||
            name.find("qrp labs") != std::string::npos) {
            return device.index;
        }
    }
    return -1;
}

bool PortAudioBackend::start(const AudioConfiguration& configuration,
                             InputCallback input_callback,
                             ErrorCallback error_callback) {
    stop();
    impl_->configuration = configuration;
    impl_->input_callback = std::move(input_callback);
    impl_->error_callback = std::move(error_callback);
    if (!impl_->initialized) {
        if (impl_->error_callback) {
            impl_->error_callback("PortAudio initialization failed.");
        }
        return false;
    }
    if (configuration.sample_rate <= 0 ||
        configuration.frames_per_buffer <= 0) {
        if (impl_->error_callback) {
            impl_->error_callback("Invalid audio configuration.");
        }
        return false;
    }

    PaStreamParameters input_parameters{};
    PaStreamParameters output_parameters{};
    PaStreamParameters* input = nullptr;
    PaStreamParameters* output = nullptr;
    if (configuration.input_device >= 0) {
        const auto* info = Pa_GetDeviceInfo(configuration.input_device);
        if (info == nullptr || info->maxInputChannels < 1) {
            if (impl_->error_callback) {
                impl_->error_callback("Invalid audio input device.");
            }
            return false;
        }
        input_parameters.device = configuration.input_device;
        input_parameters.channelCount = 1;
        input_parameters.sampleFormat = paFloat32;
        input_parameters.suggestedLatency = info->defaultLowInputLatency;
        input = &input_parameters;
    }
    if (configuration.output_device >= 0) {
        const auto* info = Pa_GetDeviceInfo(configuration.output_device);
        if (info == nullptr || info->maxOutputChannels < 1) {
            if (impl_->error_callback) {
                impl_->error_callback("Invalid audio output device.");
            }
            return false;
        }
        output_parameters.device = configuration.output_device;
        output_parameters.channelCount = 1;
        output_parameters.sampleFormat = paFloat32;
        output_parameters.suggestedLatency = info->defaultLowOutputLatency;
        output = &output_parameters;
    }
    if (input == nullptr && output == nullptr) {
        if (impl_->error_callback) {
            impl_->error_callback("No audio device was selected.");
        }
        return false;
    }

    const auto error = Pa_OpenStream(
        &impl_->stream, input, output, paFloat32,
        static_cast<double>(configuration.sample_rate),
        static_cast<unsigned long>(configuration.frames_per_buffer),
        &Impl::callback, impl_);
    if (error != paNoError) {
        if (impl_->error_callback) {
            impl_->error_callback("Could not open audio stream: " +
                                  pa_error(error));
        }
        impl_->stream = nullptr;
        return false;
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->running = true;
        impl_->stopping = false;
    }
    const auto start_error = Pa_StartStream(impl_->stream);
    if (start_error != paNoError) {
        if (impl_->error_callback) {
            impl_->error_callback("Could not start audio stream: " +
                                  pa_error(start_error));
        }
        Pa_CloseStream(impl_->stream);
        impl_->stream = nullptr;
        return false;
    }
    impl_->input_thread = std::thread(&Impl::consume_input, impl_);
    return true;
}

void PortAudioBackend::stop() {
    if (impl_->stream == nullptr) {
        return;
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        impl_->running = false;
    }
    impl_->condition.notify_all();
    Pa_StopStream(impl_->stream);
    Pa_CloseStream(impl_->stream);
    impl_->stream = nullptr;
    if (impl_->input_thread.joinable()) {
        impl_->input_thread.join();
    }
    std::lock_guard lock(impl_->mutex);
    impl_->input_head = impl_->input_tail = impl_->input_count = 0;
    impl_->output_head = impl_->output_tail = impl_->output_count = 0;
}

bool PortAudioBackend::queue_output(const std::vector<float>& samples) {
    if (samples.empty()) {
        return true;
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running || impl_->stream == nullptr) {
        return false;
    }
    if (samples.size() > Impl::output_sample_count - impl_->output_count) {
        if (impl_->error_callback) {
            impl_->error_callback("Audio output queue is full.");
        }
        return false;
    }
    for (const float sample : samples) {
        impl_->output_samples[impl_->output_tail] = sample;
        impl_->output_tail =
            (impl_->output_tail + 1) % Impl::output_sample_count;
    }
    impl_->output_count += samples.size();
    return true;
}

}  // namespace olivia
