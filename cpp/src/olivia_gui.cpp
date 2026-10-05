#include "olivia_modem.hpp"
#include "serial_port.hpp"
#ifdef OLIVIA_HAS_PORTAUDIO
#include "audio_backend.hpp"
#endif

#include <QApplication>
#include <QButtonGroup>
#include <QCommandLineParser>
#include <QDataStream>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <map>
#include <optional>
#include <unordered_map>

namespace {

constexpr int sample_rate = 8000;
constexpr int default_port = 45800;
constexpr int packet_samples = 2048;
constexpr int receive_buffer_bytes = 4 * 1024 * 1024;
constexpr int spectrum_input_size = 1024;
constexpr int spectrum_fft_size = 4096;
constexpr int center_frequency = 1500;
constexpr int waterfall_row_height = 3;
constexpr double pi = 3.14159265358979323846;

struct Configuration {
    int tones = 8;
    int bandwidth = 250;
};

struct RealDeviceSelection {
    QString serial_path;
    int audio_input = -1;
    int audio_output = -1;
};

class SpectrumWidget final : public QWidget {
public:
    explicit SpectrumWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(280);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        waterfall_ = QImage(768, 360, QImage::Format_RGB32);
        for (int index = 0; index < spectrum_input_size; ++index) {
            window_[index] =
                0.5 -
                0.5 * std::cos(2.0 * pi * index / spectrum_input_size);
        }
        render_timer_.setInterval(32);
        connect(&render_timer_, &QTimer::timeout, this,
                &SpectrumWidget::render_pending);
        render_timer_.start();
        clear();
    }

    void set_samples(const QVector<float>& samples) {
        pending_samples_ += samples;
    }

    void clear() {
        pending_samples_.clear();
        waterfall_.fill(QColor(QStringLiteral("#101820")));
        update();
    }

    void set_mode(int bandwidth) {
        bandwidth_ = bandwidth;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(QStringLiteral("#101820")));
        painter.setRenderHint(QPainter::Antialiasing);

        const QRect plot = rect().adjusted(78, 24, -18, -38);
        painter.setPen(QColor(QStringLiteral("#52606d")));
        painter.drawRect(plot);
        if (plot.width() < 2 || plot.height() < 2) {
            return;
        }
        painter.drawImage(plot, waterfall_);

        painter.setPen(QColor(QStringLiteral("#d0d7de")));
        painter.drawText(6, 16, QStringLiteral("TX waterfall"));
        painter.drawText(plot.left(), height() - 10,
                         QStringLiteral("%1 Hz").arg(min_frequency()));
        painter.drawText(plot.center().x() - 24, height() - 10,
                         QStringLiteral("%1 Hz").arg(center_frequency));
        painter.drawText(plot.right() - 58, height() - 10,
                         QStringLiteral("%1 Hz").arg(max_frequency()));
        painter.drawText(plot.center().x() - 48, height() - 24,
                         QStringLiteral("Frequency (Hz)"));
        painter.drawText(18, plot.top() + 12, QStringLiteral("old"));
        painter.drawText(18, plot.bottom(), QStringLiteral("now"));
        painter.save();
        painter.translate(12, plot.center().y());
        painter.rotate(-90);
        painter.drawText(0, 0, QStringLiteral("Time"));
        painter.restore();
        painter.drawText(plot.right() - 105, height() - 10,
                         QStringLiteral("BW %1 Hz").arg(bandwidth_));
    }

private:
    void render_pending() {
        constexpr int rows_per_frame = 1;
        int rows_rendered = 0;
        while (pending_samples_.size() >= spectrum_input_size &&
               rows_rendered < rows_per_frame) {
            append_spectrum_row(pending_samples_.constData(),
                                spectrum_input_size);
            pending_samples_.remove(0, spectrum_hop);
            ++rows_rendered;
        }
        if (rows_rendered > 0) {
            update();
        }
    }

    void append_spectrum_row(const float* samples, int count) {
        std::array<std::complex<double>, spectrum_fft_size> spectrum{};
        for (int index = 0; index < count; ++index) {
            spectrum[index] = samples[index] * window_[index];
        }
        for (int length = 2; length <= spectrum_fft_size; length *= 2) {
            const double angle = -2.0 * pi / static_cast<double>(length);
            const auto stage_factor = std::polar(1.0, angle);
            for (int start = 0; start < spectrum_fft_size; start += length) {
                auto factor = std::complex<double>(1.0, 0.0);
                for (int index = 0; index < length / 2; ++index) {
                    const auto even = spectrum[start + index];
                    const auto odd = spectrum[start + index + length / 2] *
                                     factor;
                    spectrum[start + index] = even + odd;
                    spectrum[start + index + length / 2] = even - odd;
                    factor *= stage_factor;
                }
            }
        }
        const int row_bytes = waterfall_.bytesPerLine();
        const int shift_rows =
            std::min(waterfall_row_height, waterfall_.height());
        std::memmove(waterfall_.scanLine(0),
                     waterfall_.scanLine(shift_rows),
                     static_cast<std::size_t>(row_bytes) *
                         (waterfall_.height() - shift_rows));
        std::array<QRgb, 768> colors{};
        for (int x = 0; x < waterfall_.width(); ++x) {
            const double frequency =
                min_frequency() +
                static_cast<double>(x) * frequency_span() /
                    (waterfall_.width() - 1);
            const int bin = std::clamp(
                static_cast<int>(std::round(
                    frequency * spectrum_fft_size / sample_rate)),
                0, spectrum_fft_size / 2 - 1);
            const double magnitude =
                std::abs(spectrum[bin]) / spectrum_input_size;
            const double decibels = 20.0 * std::log10(magnitude + 1.0e-9);
            const double normalized =
                std::clamp((decibels + 65.0) / 55.0, 0.0, 1.0);
            const double level = std::pow(normalized, 0.7);
            const QColor color = waterfall_color(level);
            colors[static_cast<std::size_t>(x)] = color.rgb();
        }
        for (int row_index = waterfall_.height() - shift_rows;
             row_index < waterfall_.height(); ++row_index) {
            auto* row =
                reinterpret_cast<QRgb*>(waterfall_.scanLine(row_index));
            std::copy(colors.begin(), colors.begin() + waterfall_.width(),
                      row);
        }
    }

    QColor waterfall_color(double level) const {
        static constexpr std::array<QColor, 7> jet = {
            QColor(0, 0, 128),   QColor(0, 0, 255),   QColor(0, 255, 255),
            QColor(0, 128, 0),  QColor(255, 255, 0), QColor(255, 0, 0),
            QColor(128, 0, 0)};
        const double scaled =
            std::clamp(level, 0.0, 1.0) * (jet.size() - 1);
        const int lower = std::min(static_cast<int>(scaled),
                                   static_cast<int>(jet.size() - 2));
        const double amount = scaled - lower;
        const auto& first = jet[lower];
        const auto& second = jet[lower + 1];
        return QColor(
            static_cast<int>(first.red() +
                             amount * (second.red() - first.red())),
            static_cast<int>(first.green() +
                             amount * (second.green() - first.green())),
            static_cast<int>(first.blue() +
                             amount * (second.blue() - first.blue())));
    }

    int min_frequency() const {
        return std::max(0, center_frequency - (3 * bandwidth_) / 4);
    }

    int max_frequency() const {
        return std::min(sample_rate / 2,
                        center_frequency + (3 * bandwidth_) / 4);
    }

    int frequency_span() const { return max_frequency() - min_frequency(); }

    static constexpr int spectrum_hop = 256;
    QVector<float> pending_samples_;
    QImage waterfall_;
    std::array<double, spectrum_input_size> window_{};
    QTimer render_timer_;
    int bandwidth_ = 250;
};

class SimulatorWorker final : public QObject {
    Q_OBJECT

public:
    explicit SimulatorWorker(char station, int channel_port)
        : station_(station),
          channel_port_(channel_port),
          socket_(nullptr),
          running_(false),
          modem_(8, 250, sample_rate),
          transaction_id_(0) {}

public slots:
    void start() {
        if (socket_ != nullptr) {
            return;
        }
        socket_ = new QUdpSocket(this);
        socket_->setSocketOption(
            QAbstractSocket::ReceiveBufferSizeSocketOption,
            receive_buffer_bytes);
        if (!socket_->bind(QHostAddress::LocalHost, listen_port())) {
            emit error(QStringLiteral("Could not bind UDP port %1: %2")
                           .arg(listen_port())
                           .arg(socket_->errorString()));
            socket_->deleteLater();
            socket_ = nullptr;
            emit connected(false);
            return;
        }
        connect(socket_, &QUdpSocket::readyRead, this,
                &SimulatorWorker::read_pending_datagrams);
        running_ = true;
        emit status(QStringLiteral("Simulator station %1; listening on UDP %2")
                        .arg(QChar(station_))
                        .arg(listen_port()));
        emit connected(true);
    }

    void stop() {
        running_ = false;
        packets_.clear();
        packet_totals_.clear();
        next_sequences_.clear();
        if (socket_ != nullptr) {
            socket_->close();
            socket_->deleteLater();
            socket_ = nullptr;
        }
        emit connected(false);
    }

    void transmit(const QString& message, int tones, int bandwidth) {
        if (!running_ || socket_ == nullptr) {
            emit error(QStringLiteral("TX failed: simulator is not connected."));
            return;
        }
        emit status(QStringLiteral("Transmitting..."));
        configure_modem(tones, bandwidth);
        const auto samples = modem_.modulate(message.toLatin1().toStdString());
        ++transaction_id_;
        const int total_packets =
            static_cast<int>((samples.size() + packet_samples - 1) /
                             packet_samples);
        for (int sequence = 0; sequence < total_packets; ++sequence) {
            const int start = sequence * packet_samples;
            const int count = std::min(
                packet_samples, static_cast<int>(samples.size()) - start);
            QByteArray packet;
            QDataStream stream(&packet, QIODevice::WriteOnly);
            stream.setByteOrder(QDataStream::BigEndian);
            stream.writeRawData("OLIV", 4);
            stream << static_cast<quint32>(transaction_id_)
                   << static_cast<quint16>(sequence)
                   << static_cast<quint16>(total_packets);
            packet.append(reinterpret_cast<const char*>(samples.data() + start),
                          count * static_cast<int>(sizeof(float)));
            QVector<float> audio_chunk(count);
            std::copy(samples.begin() + start, samples.begin() + start + count,
                      audio_chunk.begin());
            emit audio_samples(audio_chunk);
            socket_->writeDatagram(packet, QHostAddress::LocalHost,
                                   target_port());
            if (sequence + 1 < total_packets) {
                QThread::msleep(
                    static_cast<unsigned long>(1000.0 * count / sample_rate));
            }

        }
        emit status(QStringLiteral("Transmitted: %1").arg(message));
    }

    void configure(int tones, int bandwidth) {
        configure_modem(tones, bandwidth);
    }

signals:
    void connected(bool connected);
    void received(const QString& message);
    void audio_samples(const QVector<float>& samples);
    void status(const QString& message);
    void error(const QString& message);

private slots:
    void read_pending_datagrams() {
        while (socket_ != nullptr && socket_->hasPendingDatagrams()) {
            QByteArray packet;
            packet.resize(static_cast<int>(socket_->pendingDatagramSize()));
            socket_->readDatagram(packet.data(), packet.size());
            if (packet.size() < 12 || packet.left(4) != "OLIV") {
                continue;
            }
            QDataStream stream(packet);
            stream.setByteOrder(QDataStream::BigEndian);
            char magic[4];
            quint32 transaction = 0;
            quint16 sequence = 0;
            quint16 total = 0;
            stream.readRawData(magic, 4);
            stream >> transaction >> sequence >> total;
            const int payload_size = packet.size() - 12;
            if (total == 0 || sequence >= total || payload_size <= 0 ||
                payload_size % static_cast<int>(sizeof(float)) != 0) {
                continue;
            }

            auto& transaction_packets = packets_[transaction];
            transaction_packets[sequence] = packet.mid(12);
            packet_totals_[transaction] = total;
            auto& next_sequence = next_sequences_[transaction];
            while (transaction_packets.find(next_sequence) !=
                   transaction_packets.end()) {
                const auto payload = transaction_packets[next_sequence];
                transaction_packets.erase(next_sequence);
                decode_payload(payload);
                ++next_sequence;
            }
            if (next_sequence == packet_totals_[transaction]) {
                packets_.erase(transaction);
                packet_totals_.erase(transaction);
                next_sequences_.erase(transaction);
            }
        }
    }

private:
    void configure_modem(int tones, int bandwidth) {
        if (modem_.tones() == tones && modem_.bandwidth() == bandwidth) {
            return;
        }
        modem_ = olivia::OliviaModem(tones, bandwidth, sample_rate);
        packets_.clear();
        packet_totals_.clear();
        next_sequences_.clear();
    }

    void decode_payload(const QByteArray& payload) {
        if (payload.isEmpty() ||
            payload.size() % static_cast<int>(sizeof(float)) != 0) {
            return;
        }
        std::vector<float> samples(payload.size() / sizeof(float));
        std::memcpy(samples.data(), payload.constData(),
                    static_cast<std::size_t>(payload.size()));
        const auto decoded = modem_.demodulate(samples);
        if (!decoded.empty()) {
            emit received(QString::fromLatin1(
                decoded.data(), static_cast<int>(decoded.size())));
        }
    }

    int listen_port() const {
        return channel_port_ + (station_ == 'A' ? 1 : 2);
    }

    int target_port() const {
        return channel_port_ + (station_ == 'A' ? 2 : 1);
    }

    char station_;
    int channel_port_;
    QUdpSocket* socket_;
    bool running_;
    olivia::OliviaModem modem_;
    quint32 transaction_id_;
    std::unordered_map<quint32, std::map<quint16, QByteArray>>
        packets_;
    std::unordered_map<quint32, quint16> packet_totals_;
    std::unordered_map<quint32, quint16> next_sequences_;
};

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(char station, int channel_port)
        : station_(station),
          channel_port_(channel_port),
          thread_(new QThread(this)),
          worker_(new SimulatorWorker(station, channel_port)) {
        worker_->moveToThread(thread_);
        setWindowTitle(QStringLiteral("Olivia MFSK - QMX+ (Station %1)")
                           .arg(QChar(station)));
        resize(720, 600);

        station_label_ =
            new QLabel(QStringLiteral("Station %1").arg(QChar(station)));
        connection_button_ = new QPushButton(QStringLiteral("Reconnect"));
        transmit_button_ = new QPushButton(QStringLiteral("TRANSMIT"));
        clear_button_ = new QPushButton(QStringLiteral("Clear received"));
        received_text_ = new QPlainTextEdit;
        received_text_->setReadOnly(true);
        received_text_->setPlaceholderText(QStringLiteral("Received text"));
        outgoing_text_ = new QPlainTextEdit;
        outgoing_text_->setPlaceholderText(
            QStringLiteral("Message to transmit"));
        waterfall_ = new SpectrumWidget;

        auto* central = new QWidget;
        auto* layout = new QVBoxLayout(central);
        layout->addWidget(station_label_);
        layout->addWidget(connection_button_);
        layout->addWidget(create_radio_group(
            QStringLiteral("Tones"), {2, 4, 8, 16, 32, 64, 128, 256},
            tone_group_, 8));
        layout->addWidget(create_radio_group(
            QStringLiteral("Bandwidth (Hz)"), {125, 250, 500, 1000, 2000},
            bandwidth_group_, 250));
        layout->addWidget(waterfall_);
        layout->addWidget(received_text_, 3);
        layout->addWidget(outgoing_text_, 1);
        layout->addWidget(transmit_button_);
        layout->addWidget(clear_button_);
        setCentralWidget(central);
        statusBar()->showMessage(QStringLiteral("Starting..."));
        transmit_button_->setEnabled(false);

        connect(connection_button_, &QPushButton::clicked, this,
                &MainWindow::toggle_connection);
        connect(transmit_button_, &QPushButton::clicked, this,
                &MainWindow::transmit);
        connect(clear_button_, &QPushButton::clicked, received_text_,
                &QPlainTextEdit::clear);
        connect(thread_, &QThread::started, worker_, &SimulatorWorker::start);
        connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
        connect(this, &MainWindow::start_requested, worker_,
                &SimulatorWorker::start);
        connect(this, &MainWindow::stop_requested, worker_,
                &SimulatorWorker::stop);
        connect(this, &MainWindow::configuration_requested, worker_,
                &SimulatorWorker::configure);
        connect(tone_group_, &QButtonGroup::idClicked, this,
                &MainWindow::update_configuration);
        connect(bandwidth_group_, &QButtonGroup::idClicked, this,
                &MainWindow::update_configuration);
        connect(worker_, &SimulatorWorker::connected, this,
                &MainWindow::connection_changed);
        connect(worker_, &SimulatorWorker::received, this,
                &MainWindow::show_received);
        connect(worker_, &SimulatorWorker::audio_samples, waterfall_,
                &SpectrumWidget::set_samples);
        connect(worker_, &SimulatorWorker::status, this,
                [this](const QString& message) {
                    statusBar()->showMessage(message);
                });
        connect(worker_, &SimulatorWorker::error, this,
                [this](const QString& message) {
                    statusBar()->showMessage(message);
                });
        connect(this, &MainWindow::transmit_requested, worker_,
                &SimulatorWorker::transmit);
        emit configuration_requested(tone_group_->checkedId(),
                                     bandwidth_group_->checkedId());
        waterfall_->set_mode(bandwidth_group_->checkedId());
        thread_->start();
    }

    ~MainWindow() override {
        if (QThread::currentThread() == worker_->thread()) {
            worker_->stop();
        } else if (thread_->isRunning()) {
            QMetaObject::invokeMethod(worker_, "stop",
                                      Qt::BlockingQueuedConnection);
        }
        thread_->quit();
        thread_->wait();
        worker_ = nullptr;
    }

signals:
    void transmit_requested(const QString& message, int tones, int bandwidth);
    void configuration_requested(int tones, int bandwidth);
    void start_requested();
    void stop_requested();

private:
    QGroupBox* create_radio_group(const QString& title,
                                  const QList<int>& values,
                                  QButtonGroup*& group,
                                  int default_value) {
        auto* box = new QGroupBox(title);
        auto* layout = new QHBoxLayout(box);
        group = new QButtonGroup(box);
        for (const int value : values) {
            auto* button = new QRadioButton(QString::number(value));
            group->addButton(button, value);
            layout->addWidget(button);
            if (value == default_value) {
                button->setChecked(true);
            }
        }
        return box;
    }

    void toggle_connection() {
        if (connection_button_->text() == QStringLiteral("Disconnect")) {
            emit stop_requested();
        } else {
            emit start_requested();
        }
    }

    void transmit() {
        const auto message =
            outgoing_text_->toPlainText().replace(QStringLiteral("\\n"),
                                                  QStringLiteral("\n"));
        outgoing_text_->clear();
        if (!message.trimmed().isEmpty()) {
            waterfall_->clear();
            emit transmit_requested(message, tone_group_->checkedId(),
                                    bandwidth_group_->checkedId());
        }
    }

    void update_configuration() {
        emit configuration_requested(tone_group_->checkedId(),
                                     bandwidth_group_->checkedId());
        waterfall_->set_mode(bandwidth_group_->checkedId());
    }

    void connection_changed(bool connected) {
        connection_button_->setText(connected ? QStringLiteral("Disconnect")
                                              : QStringLiteral("Reconnect"));
        transmit_button_->setEnabled(connected);
    }

    void show_received(const QString& message) {
        received_text_->moveCursor(QTextCursor::End);
        received_text_->insertPlainText(message);
    }

    char station_;
    int channel_port_;
    QThread* thread_;
    SimulatorWorker* worker_;
    QLabel* station_label_;
    SpectrumWidget* waterfall_;
    QPushButton* connection_button_;
    QPushButton* transmit_button_;
    QPlainTextEdit* received_text_;
    QPlainTextEdit* outgoing_text_;
    QPushButton* clear_button_;
    QButtonGroup* tone_group_ = nullptr;
    QButtonGroup* bandwidth_group_ = nullptr;
};

std::optional<char> choose_start_mode(bool& real_mode) {
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Olivia QMX+ setup"));
    dialog.setMinimumWidth(360);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        QStringLiteral("Choose how you want to use Olivia:")));

    auto* mode_group = new QGroupBox(QStringLiteral("Mode"));
    auto* mode_layout = new QVBoxLayout(mode_group);
    auto* test_mode = new QRadioButton(QStringLiteral("Test mode (simulator)"));
    auto* real_mode_button =
        new QRadioButton(QStringLiteral("Real mode (QMX+ radio)"));
    test_mode->setChecked(true);
    mode_layout->addWidget(test_mode);
    mode_layout->addWidget(real_mode_button);
    layout->addWidget(mode_group);

    auto* station_group = new QGroupBox(QStringLiteral("Test station"));
    auto* station_layout = new QHBoxLayout(station_group);
    auto* station_a = new QRadioButton(QStringLiteral("Station A"));
    auto* station_b = new QRadioButton(QStringLiteral("Station B"));
    station_a->setChecked(true);
    station_layout->addWidget(station_a);
    station_layout->addWidget(station_b);
    layout->addWidget(station_group);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
    QObject::connect(real_mode_button, &QRadioButton::toggled, station_group,
                     &QWidget::setDisabled);

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }

    if (real_mode_button->isChecked()) {
        real_mode = true;
        return std::nullopt;
    }
    return station_b->isChecked() ? 'B' : 'A';
}

std::optional<RealDeviceSelection> choose_real_devices() {
#ifndef OLIVIA_HAS_PORTAUDIO
    QMessageBox::warning(
        nullptr, QStringLiteral("Real mode unavailable"),
        QStringLiteral(
            "This build does not include PortAudio. Install PortAudio and "
            "rebuild with OLIVIA_AUDIO enabled."));
    return std::nullopt;
#else
    const auto serial_devices = olivia::SerialPort::enumerate();
    olivia::PortAudioBackend audio;
    const auto audio_devices = audio.devices();
    if (serial_devices.empty()) {
        QMessageBox::warning(
            nullptr, QStringLiteral("No serial devices"),
            QStringLiteral("No serial ports were found for the QMX+ control "
                           "connection."));
        return std::nullopt;
    }

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Select QMX+ devices"));
    dialog.setMinimumWidth(520);
    auto* layout = new QFormLayout(&dialog);
    auto* serial_combo = new QComboBox(&dialog);
    auto* input_combo = new QComboBox(&dialog);
    auto* output_combo = new QComboBox(&dialog);

    const auto qmx_serial = olivia::SerialPort::find_qmx_port();
    int serial_index = 0;
    for (std::size_t index = 0; index < serial_devices.size(); ++index) {
        const auto& device = serial_devices[index];
        serial_combo->addItem(
            QString::fromStdString(device.path + " (" + device.description + ")"),
            QString::fromStdString(device.path));
        if (!qmx_serial.empty() && device.path == qmx_serial) {
            serial_index = static_cast<int>(index);
        }
    }
    serial_combo->setCurrentIndex(serial_index);

    const int qmx_audio =
        olivia::PortAudioBackend::find_qmx_device(audio_devices);
    int input_index = -1;
    int output_index = -1;
    for (const auto& device : audio_devices) {
        if (device.input_channels > 0) {
            input_combo->addItem(QString::fromStdString(device.name),
                                 device.index);
            if (device.index == qmx_audio) {
                input_index = input_combo->count() - 1;
            }
        }
        if (device.output_channels > 0) {
            output_combo->addItem(QString::fromStdString(device.name),
                                  device.index);
            if (device.index == qmx_audio) {
                output_index = output_combo->count() - 1;
            }
        }
    }
    if (input_combo->count() == 0 || output_combo->count() == 0) {
        QMessageBox::warning(
            nullptr, QStringLiteral("No audio devices"),
            QStringLiteral("A mono input and output device are required for "
                           "QMX+ audio."));
        return std::nullopt;
    }
    if (input_index >= 0) {
        input_combo->setCurrentIndex(input_index);
    }
    if (output_index >= 0) {
        output_combo->setCurrentIndex(output_index);
    }

    layout->addRow(QStringLiteral("Serial control:"), serial_combo);
    layout->addRow(QStringLiteral("Audio input:"), input_combo);
    layout->addRow(QStringLiteral("Audio output:"), output_combo);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return RealDeviceSelection{
        serial_combo->currentData().toString(),
        input_combo->currentData().toInt(),
        output_combo->currentData().toInt()};
#endif
}

}  // namespace

#include "olivia_gui.moc"

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    qRegisterMetaType<QVector<float>>("QVector<float>");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Olivia Qt simulator for QMX+"));
    parser.addHelpOption();
    QCommandLineOption station_option(
        {"s", "station"}, "Simulator station (A or B; default A).", "station",
        QString());
    QCommandLineOption port_option(
        {"p", "channel-port"}, "Shared simulator channel port.", "port",
        QString::number(default_port));
    parser.addOption(station_option);
    parser.addOption(port_option);
    parser.process(application);
    bool port_ok = false;
    const int port = parser.value(port_option).toInt(&port_ok);
    if (!port_ok || port < 1 || port > 65532) {
        parser.showHelp(1);
    }
    char station = '\0';
    const auto station_value = parser.value(station_option).toUpper();
    if (!station_value.isEmpty()) {
        if (station_value != QStringLiteral("A") &&
            station_value != QStringLiteral("B")) {
            parser.showHelp(1);
        }
        station = station_value.at(0).toLatin1();
    } else {
        bool real_mode = false;
        const auto selected_station = choose_start_mode(real_mode);
        if (real_mode) {
            const auto selection = choose_real_devices();
            if (selection.has_value()) {
                QMessageBox::information(
                    nullptr, QStringLiteral("Devices selected"),
                    QStringLiteral("Serial: %1\nAudio input: %2\nAudio output: %3\n"
                                   "Live Real-mode streaming is the next step.")
                        .arg(selection->serial_path)
                        .arg(selection->audio_input)
                        .arg(selection->audio_output));
            }
            return 0;
        }
        if (!selected_station.has_value()) {
            return 0;
        }
        station = selected_station.value();
    }
    MainWindow window(station, port);
    window.show();
    return application.exec();
}
