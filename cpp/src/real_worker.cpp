#include "real_worker.hpp"

#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace olivia {

RealWorker::RealWorker(RealConfiguration configuration, QObject* parent)
    : QObject(parent),
      configuration_(std::move(configuration)),
      serial_(std::make_unique<SerialPort>()),
      audio_(std::make_unique<PortAudioBackend>()),
      modem_(configuration_.tones, configuration_.bandwidth, 8000) {
    radio_ = std::make_unique<QmxRadio>(
        [this](const std::string& command, std::string& error_message) {
            return send_command(command, error_message);
        });
}

RealWorker::~RealWorker() {
    stop();
}

void RealWorker::start() {
    if (running_) {
        return;
    }
    std::string error_message;
    if (!serial_->open(configuration_.serial_path, 115200, error_message)) {
        emit error(QStringLiteral("Could not open QMX+ serial port: %1")
                       .arg(QString::fromStdString(error_message)));
        return;
    }
    if (!radio_->initialize(error_message)) {
        emit error(QStringLiteral("Could not initialize QMX+: %1")
                       .arg(QString::fromStdString(error_message)));
        serial_->close();
        return;
    }

    AudioConfiguration audio_configuration;
    audio_configuration.input_device = configuration_.audio_input;
    audio_configuration.output_device = configuration_.audio_output;
    audio_configuration.sample_rate = configuration_.hardware_sample_rate;
    audio_configuration.frames_per_buffer = 480;
    if (!audio_->start(
            audio_configuration,
            [this](const std::vector<float>& samples) {
                handle_input(samples);
            },
            [this](const std::string& message) {
                emit error(QString::fromStdString(message));
            })) {
        serial_->close();
        return;
    }

    running_ = true;
    emit connected(true);
    emit status(QStringLiteral("QMX+ connected; receiving"));
}

void RealWorker::stop() {
    if (!running_ && (!serial_ || !serial_->is_open())) {
        return;
    }
    audio_->stop();
    if (serial_->is_open()) {
        std::string error_message;
        if (!radio_->begin_receive(error_message)) {
            emit error(QStringLiteral("Could not return QMX+ to receive: %1")
                           .arg(QString::fromStdString(error_message)));
        }
    }
    serial_->close();
    running_ = false;
    emit connected(false);
}

void RealWorker::transmit(const QString& message) {
    if (!running_) {
        emit error(QStringLiteral("TX failed: QMX+ is not connected."));
        return;
    }
    emit status(QStringLiteral("Transmitting..."));
    std::string error_message;
    if (!radio_->begin_transmit(error_message)) {
        emit error(QStringLiteral("Could not start QMX+ transmit: %1")
                       .arg(QString::fromStdString(error_message)));
        return;
    }

    std::vector<float> modem_samples;
    {
        std::lock_guard lock(modem_mutex_);
        modem_samples = modem_.modulate(message.toLatin1().toStdString());
    }
    const auto hardware_samples = upsample(modem_samples);
    if (!audio_->queue_output(hardware_samples)) {
        emit error(QStringLiteral("Could not queue QMX+ transmit audio."));
        return;
    }

    const auto duration = std::chrono::duration<double>(
        static_cast<double>(hardware_samples.size()) /
        configuration_.hardware_sample_rate);
    std::this_thread::sleep_for(duration);
    if (!radio_->begin_receive(error_message)) {
        emit error(QStringLiteral("Could not return QMX+ to receive: %1")
                       .arg(QString::fromStdString(error_message)));
        return;
    }
    emit status(QStringLiteral("Transmitted: %1").arg(message));
}

void RealWorker::handle_input(const std::vector<float>& samples) {
    if (!running_) {
        return;
    }
    const auto modem_samples = downsample(samples);
    if (modem_samples.empty()) {
        return;
    }
    QMetaObject::invokeMethod(
        this,
        [this, samples = std::move(modem_samples)]() {
            if (!running_) {
                return;
            }
            std::string decoded;
            {
                std::lock_guard lock(modem_mutex_);
                decoded = modem_.demodulate(samples);
            }
            if (!decoded.empty()) {
                emit received(
                    QString::fromLatin1(decoded.data(),
                                        static_cast<int>(decoded.size())));
            }
        },
        Qt::QueuedConnection);
}

bool RealWorker::send_command(const std::string& command,
                              std::string& error_message) {
    return serial_->write(command, error_message);
}

std::vector<float> RealWorker::upsample(
    const std::vector<float>& samples) const {
    constexpr int ratio = 6;
    std::vector<float> result;
    result.reserve(samples.size() * ratio);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const float current = samples[index];
        const float next = index + 1 < samples.size() ? samples[index + 1]
                                                       : current;
        for (int offset = 0; offset < ratio; ++offset) {
            const float amount = static_cast<float>(offset) / ratio;
            result.push_back(current + amount * (next - current));
        }
    }
    return result;
}

std::vector<float> RealWorker::downsample(
    const std::vector<float>& samples) const {
    constexpr int ratio = 6;
    std::vector<float> result;
    result.reserve((samples.size() + ratio - 1) / ratio);
    for (std::size_t index = 0; index < samples.size(); index += ratio) {
        result.push_back(samples[index]);
    }
    return result;
}

}  // namespace olivia
