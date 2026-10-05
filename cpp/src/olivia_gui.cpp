#include "olivia_modem.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCommandLineParser>
#include <QDataStream>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
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
#include <cstdint>
#include <cstring>
#include <memory>

namespace {

constexpr int sample_rate = 8000;
constexpr int default_port = 45800;
constexpr int packet_samples = 2048;

struct Configuration {
    int tones = 8;
    int bandwidth = 250;
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
        if (socket_ != nullptr) {
            socket_->close();
            socket_->deleteLater();
            socket_ = nullptr;
        }
        emit connected(false);
    }

    void transmit(const QString& message) {
        if (!running_ || socket_ == nullptr) {
            emit error(QStringLiteral("TX failed: simulator is not connected."));
            return;
        }
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
            socket_->writeDatagram(packet, QHostAddress::LocalHost,
                                   target_port());
        }
        emit status(QStringLiteral("Transmitted: %1").arg(message));
    }

signals:
    void connected(bool connected);
    void received(const QString& message);
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
            Q_UNUSED(transaction);
            Q_UNUSED(sequence);
            Q_UNUSED(total);
            const int payload_size = packet.size() - 12;
            if (payload_size <= 0 ||
                payload_size % static_cast<int>(sizeof(float)) != 0) {
                continue;
            }
            std::vector<float> samples(payload_size / sizeof(float));
            std::memcpy(samples.data(), packet.constData() + 12,
                        static_cast<std::size_t>(payload_size));
            const auto decoded = modem_.demodulate(samples);
            if (!decoded.empty()) {
                emit received(QString::fromLatin1(decoded.data(),
                                                  static_cast<int>(decoded.size())));
            }
        }
    }

private:
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
};

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(char station, int channel_port)
        : station_(station),
          channel_port_(channel_port),
          thread_(new QThread(this)),
          worker_(new SimulatorWorker(station, channel_port)) {
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
        waterfall_ = new QLabel(
            QStringLiteral("TX waterfall: 1375-1625 Hz\n"
                           "Native C++ spectrum display will be added with "
                           "the audio backend."));
        waterfall_->setMinimumHeight(90);
        waterfall_->setStyleSheet(
            QStringLiteral("QLabel { background: #101820; color: #d0d7de; "
                            "padding: 12px; }"));

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
        connect(this, &MainWindow::start_requested, worker_,
                &SimulatorWorker::start);
        connect(this, &MainWindow::stop_requested, worker_,
                &SimulatorWorker::stop);
        connect(worker_, &SimulatorWorker::connected, this,
                &MainWindow::connection_changed);
        connect(worker_, &SimulatorWorker::received, this,
                &MainWindow::show_received);
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
        thread_->start();
    }

    ~MainWindow() override {
        QMetaObject::invokeMethod(worker_, "stop",
                                  Qt::BlockingQueuedConnection);
        thread_->quit();
        thread_->wait();
        delete worker_;
    }

signals:
    void transmit_requested(const QString& message);
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
            emit transmit_requested(message);
        }
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
    QLabel* waterfall_;
    QPushButton* connection_button_;
    QPushButton* transmit_button_;
    QPlainTextEdit* received_text_;
    QPlainTextEdit* outgoing_text_;
    QPushButton* clear_button_;
    QButtonGroup* tone_group_ = nullptr;
    QButtonGroup* bandwidth_group_ = nullptr;
};

}  // namespace

#include "olivia_gui.moc"

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Olivia Qt simulator for QMX+"));
    parser.addHelpOption();
    QCommandLineOption station_option(
        {"s", "station"}, "Simulator station (A or B; default A).", "station",
        QStringLiteral("A"));
    QCommandLineOption port_option(
        {"p", "channel-port"}, "Shared simulator channel port.", "port",
        QString::number(default_port));
    parser.addOption(station_option);
    parser.addOption(port_option);
    parser.process(application);
    const auto station_value = parser.value(station_option).toUpper();
    if (station_value != QStringLiteral("A") &&
        station_value != QStringLiteral("B")) {
        parser.showHelp(1);
    }
    bool port_ok = false;
    const int port = parser.value(port_option).toInt(&port_ok);
    if (!port_ok || port < 1 || port > 65532) {
        parser.showHelp(1);
    }
    MainWindow window(station_value.at(0).toLatin1(), port);
    window.show();
    return application.exec();
}
