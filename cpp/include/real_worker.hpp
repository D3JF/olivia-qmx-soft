#pragma once

#include "audio_backend.hpp"
#include "olivia_modem.hpp"
#include "qmx_radio.hpp"
#include "serial_port.hpp"

#include <QObject>
#include <QString>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace olivia {

struct RealConfiguration {
    std::string serial_path;
    int audio_input = -1;
    int audio_output = -1;
    int tones = 8;
    int bandwidth = 250;
    int hardware_sample_rate = 48000;
};

class RealWorker final : public QObject {
    Q_OBJECT

public:
    explicit RealWorker(RealConfiguration configuration,
                        QObject* parent = nullptr);
    ~RealWorker() override;

public slots:
    void start();
    void stop();
    void transmit(const QString& message);

signals:
    void connected(bool connected);
    void received(const QString& message);
    void status(const QString& message);
    void error(const QString& message);

private:
    void handle_input(const std::vector<float>& samples);
    bool send_command(const std::string& command, std::string& error_message);
    std::vector<float> upsample(const std::vector<float>& samples) const;
    std::vector<float> downsample(const std::vector<float>& samples) const;

    RealConfiguration configuration_;
    std::unique_ptr<SerialPort> serial_;
    std::unique_ptr<PortAudioBackend> audio_;
    std::unique_ptr<QmxRadio> radio_;
    OliviaModem modem_;
    std::mutex modem_mutex_;
    bool running_ = false;
};

}  // namespace olivia
