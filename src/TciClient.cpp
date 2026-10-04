#include "TciClient.h"

#include <QHash>

#include <QWebSocket>
#include <QTimer>
#include <QStringList>
#include <QDebug>

namespace ShackBook {

namespace {

// Backoff schedule (seconds) for auto-reconnect.  Last value repeats.
constexpr int kBackoff[] = { 1, 2, 5, 10, 30 };

int backoffSeconds(int attempt)
{
    const int n = static_cast<int>(sizeof(kBackoff) / sizeof(kBackoff[0]));
    if (attempt < 0) attempt = 0;
    return kBackoff[attempt < n ? attempt : n - 1];
}

} // namespace

TciClient::TciClient(QObject* parent)
    : QObject(parent),
      m_socket(new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this)),
      m_reconnectTimer(new QTimer(this))
{
    m_reconnectTimer->setSingleShot(true);
    m_clock.start();

    connect(m_socket, &QWebSocket::connected,        this, &TciClient::onConnected);
    connect(m_socket, &QWebSocket::disconnected,     this, &TciClient::onDisconnected);
    connect(m_socket, &QWebSocket::textMessageReceived,
            this, &TciClient::onTextMessage);
    // QWebSocket grew errorOccurred in Qt 6.5; distro Qt 6.4 (Ubuntu 24.04)
    // still has only the overloaded error() signal.  Either way we only need
    // a notification, not the specific code.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(m_socket, &QWebSocket::errorOccurred,
            this, &TciClient::onErrorOccurred);
#else
    connect(m_socket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &TciClient::onErrorOccurred);
#endif

    connect(m_reconnectTimer, &QTimer::timeout,
            this, &TciClient::onReconnectTimeout);
}

TciClient::~TciClient()
{
    cancelReconnect();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
}

void TciClient::connectToServer(const QString& host, quint16 port)
{
    m_userInitiatedDisconnect = false;
    m_reconnectAttempts = 0;

    QUrl url;
    url.setScheme("ws");
    url.setHost(host);
    url.setPort(port);
    m_url = url;

    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
    // The abort above reports a drop, which schedules a reconnect. This IS
    // the reconnect, so that timer must not survive to abort it a second
    // later (and the next one, and the next: see onConnected).
    cancelReconnect();
    m_socket->open(m_url);
}

void TciClient::disconnectFromServer()
{
    m_userInitiatedDisconnect = true;
    cancelReconnect();
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->close();
    }
    setConnected(false);
}

void TciClient::send(const QString& cmd)
{
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->sendTextMessage(cmd);
    }
}

void TciClient::onConnected()
{
    // A reconnect still pending from before this connection would fire on a
    // healthy link: onReconnectTimeout() aborts and reopens, the abort
    // schedules another, and the link churns once a second for good. Seen
    // in AetherSDR's log as a "TCP drop" every ~990 ms (#32 on-air tests).
    cancelReconnect();
    m_reconnectAttempts = 0;
    setConnected(true);
    // TCI dialect varies between servers — sending `start;` is enough for
    // AetherSDR, ExpertSDR2, and SunSDR-mb1.  Servers ignore unknown
    // commands.  We do NOT also send `iq_start;` etc — we don't want IQ.
    send("start;");
    // Transmit sensor readings (forward power, SWR) for logging the power a
    // QSO was actually made at (#23). Opt-in on AetherSDR; ignored by servers
    // that do not have them.
    send("tx_sensors_enable:true;");
}

void TciClient::onDisconnected()
{
    // Forget the announced device. Without this a QSO logged after switching
    // to a different radio would be stamped with the PREVIOUS radio's name —
    // a wrong attribution is worse than a blank one.
    if (!m_device.isEmpty()) {
        m_device.clear();
        emit deviceNameChanged(m_device);
    }
    // Readings from before the drop may belong to a different radio or a
    // long-gone transmission; never log them against a new contact.
    m_txPower.reset();
    // The next server may run at a different speed; show "unknown" until it
    // says, rather than the old radio's number.
    if (m_cwSpeedWpm != 0) {
        m_cwSpeedWpm = 0;
        emit cwSpeedChanged(0);
    }
    if (m_transmitting) {
        m_transmitting = false;
        emit transmittingChanged(false);
    }
    setConnected(false);
    if (!m_userInitiatedDisconnect) {
        scheduleReconnect();
    }
}

void TciClient::onErrorOccurred()
{
    if (m_socket) m_lastError = m_socket->errorString();
}

void TciClient::onTextMessage(const QString& message)
{
    // Some TCI servers concatenate multiple commands per WebSocket frame
    // (`vfo:0,0,14250000;mode:0,USB;`); split on ';' to handle both forms.
    const QStringList lines = message.split(';', Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        emit rawMessageReceived(line);
        parseLine(line);
    }
}

bool TciClient::tuneToMhz(double mhz)
{
    // Refuse rather than send nonsense. A spot with a missing or malformed
    // frequency reaches here as 0 or negative, and "tune to 0 Hz" is not a
    // request any radio should be asked to interpret. The upper bound is
    // deliberately generous — this guards against garbage, not against
    // microwave operators.
    if (!m_connected || !(mhz > 0.0) || mhz > 100000.0)
        return false;

    // vfo:rx,vfo,freqHz — the same shape parseLine() reads back, so a
    // successful tune produces a frequencyChanged() echo from the radio
    // rather than us assuming it worked.
    const qint64 hz = static_cast<qint64>(mhz * 1.0e6 + 0.5);
    send(QStringLiteral("vfo:0,0,%1;").arg(hz));
    return true;
}

QString tciModulationForAdifMode(const QString& adifMode, double currentMhz)
{
    // Only modes with an unambiguous mapping are listed. A spot saying
    // "DIGITAL" or "DATA" could be any of a dozen things, and choosing one
    // for the operator is worse than leaving the radio where it is.
    static const QHash<QString, QString> kMap = {
        {QStringLiteral("CW"),    QStringLiteral("cw")},
        {QStringLiteral("USB"),   QStringLiteral("usb")},
        {QStringLiteral("LSB"),   QStringLiteral("lsb")},
        {QStringLiteral("AM"),    QStringLiteral("am")},
        {QStringLiteral("FM"),    QStringLiteral("nfm")},
        // RTTY is conventionally LSB-side; the rest of the data modes are
        // upper-side by convention on every band they are used on, so these
        // stay fixed rather than following the sideband rule above.
        {QStringLiteral("RTTY"),  QStringLiteral("digl")},
        {QStringLiteral("FT8"),   QStringLiteral("digu")},
        {QStringLiteral("FT4"),   QStringLiteral("digu")},
        {QStringLiteral("JT65"),  QStringLiteral("digu")},
        {QStringLiteral("JS8"),   QStringLiteral("digu")},
        {QStringLiteral("PSK"),   QStringLiteral("digu")},
        {QStringLiteral("PSK31"), QStringLiteral("digu")},
    };

    const QString key = adifMode.trimmed().toUpper();
    if (key.isEmpty()) return {};

    // Both SSB and the digital-voice modes need a sideband, and they must
    // agree — a spot that tunes USB for voice but DIGL for FreeDV on the same
    // band would be incoherent. One helper, used by both.
    //
    // 60 m is the exception that matters: it sits below 10 MHz but is USB by
    // convention (and by regulation in most countries). A plain "< 10 MHz
    // means LSB" rule puts the radio on the wrong sideband for the whole band.
    const auto upperSideband = [](double mhz) -> bool {
        // 5.25 is the WRC-15 band edge (5.2515), NOT the 5.06 that
        // LogbookModel::bandFromFreqMhz uses for the same band. The two are
        // deliberately different: the band table is a wide ADIF-style bucket
        // that only has to name a QSO's band, while this decides what sideband
        // to put a transmitter on. Nothing between 5.06 and 5.25 is a 60 m
        // amateur allocation anywhere.
        if (mhz >= 5.25 && mhz <= 5.45) return true;   // 60 m: USB despite the frequency
        return mhz >= 10.0;
    };

    // SSB is not a sideband. Resolve it by frequency, or not at all.
    // N3FJP sends a SCORING BUCKET (PH / CW / DG) alongside the real mode,
    // and a spot forwarded from a logger often carries only the bucket. "PH"
    // is phone, which is the same question SSB asks - resolve it by frequency.
    // "DG" is deliberately absent: it covers FT8, RTTY, PSK and a dozen others
    // that need different sidebands and different software, so it is exactly
    // the ambiguous case this function refuses rather than guesses at. "CW"
    // needs no special case; it is already in the map and means one thing.
    //
    // Without this a phone spot from a logger matched nothing, setModeString()
    // returned false, and the radio stayed on whatever mode it was already in
    // while the frequency moved - tuned to the DX, listening in the wrong mode.
    if (key == QLatin1String("SSB") || key == QLatin1String("PH")
        || key == QLatin1String("PHONE")) {
        if (!(currentMhz > 0.0)) return {};
        return upperSideband(currentMhz) ? QStringLiteral("usb")
                                         : QStringLiteral("lsb");
    }

    // ── Digital voice ─────────────────────────────────────────────────
    //
    // FreeDV — including RADE (Radio Autoencoder), its current generation —
    // is not a mode the radio knows about. The rig is a dumb SSB pipe on an
    // ordinary HF frequency and the codec runs in software on top, so the
    // right thing to send is a DATA sideband, exactly as AetherSDR does for
    // its own RADE pipeline (MainWindow_DigitalModes.cpp: "RADE requires
    // DIGU or DIGL").
    //
    // This is why these belong here rather than being refused as ambiguous:
    // unlike a bare "DIGITAL", a FreeDV spot has an unambiguous answer.
    if (key == QLatin1String("FREEDV") || key == QLatin1String("RADE")
        || key == QLatin1String("DIGITALVOICE") || key == QLatin1String("DV")) {
        if (!(currentMhz > 0.0)) return {};
        return upperSideband(currentMhz) ? QStringLiteral("digu")
                                         : QStringLiteral("digl");
    }

    const auto it = kMap.constFind(key);
    return it == kMap.constEnd() ? QString{} : *it;
}

bool TciClient::setModeString(const QString& adifMode)
{
    if (!m_connected) return false;
    const QString mod = tciModulationForAdifMode(adifMode, m_freqMhz);
    if (mod.isEmpty()) return false;   // unknown: leave the radio alone
    send(QStringLiteral("modulation:0,%1;").arg(mod));
    return true;
}

QString tciCwMacroCommand(const QString& text)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return {};
    for (const QChar c : t) {
        const char16_t u = c.unicode();
        // Printable ASCII only: control characters and anything beyond 0x7E
        // have no Morse meaning and no agreed handling on any TCI server.
        if (u < 0x20 || u > 0x7E) return {};
        // ';' terminates a TCI command; the remainder would be read as a
        // fresh command of whatever it happened to spell.
        if (u == u';') return {};
    }
    return QStringLiteral("cw_macros:0,%1;").arg(t);
}

bool TciClient::sendCw(const QString& text)
{
    if (!m_connected) return false;
    const QString cmd = tciCwMacroCommand(text);
    if (cmd.isEmpty()) return false;
    send(cmd);
    return true;
}

bool TciClient::stopCw()
{
    // Checks the socket, not m_connected: see the header. send() would
    // silently drop it otherwise, and the caller deserves to know.
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return false;
    send(QStringLiteral("cw_macros_stop;"));
    return true;
}

bool TciClient::setCwSpeed(int wpm)
{
    if (!m_connected) return false;
    const int clamped = qBound(kTciCwMinWpm, wpm, kTciCwMaxWpm);
    send(QStringLiteral("cw_macros_speed:%1;").arg(clamped));
    return true;
}

bool TciClient::requestCwSpeed()
{
    if (!m_connected) return false;
    send(QStringLiteral("cw_macros_speed;"));
    return true;
}


void TciClient::parseLine(const QString& line)
{
    // Format: command:arg1,arg2,arg3
    const int colon = line.indexOf(':');
    QString cmd  = colon < 0 ? line : line.left(colon);
    QString rest = colon < 0 ? QString{} : line.mid(colon + 1);
    cmd = cmd.trimmed().toLower();
    const QStringList args = rest.split(',', Qt::KeepEmptyParts);

    if (cmd == "vfo" && args.size() >= 3) {
        // vfo:rx,vfo,freqHz — track RX 0 / VFO 0 only.
        if (args[0].trimmed() == "0" && args[1].trimmed() == "0") {
            const qint64 hz = args[2].trimmed().toLongLong();
            const double mhz = hz / 1.0e6;
            if (qAbs(mhz - m_freqMhz) > 1.0e-9) {
                m_freqMhz = mhz;
                emit frequencyChanged(m_freqMhz);
            }
        }
    } else if ((cmd == "mode" || cmd == "modulation") && args.size() >= 2) {
        // mode:rx,modeString — track RX 0 only.
        // AetherSDR sends `modulation:` instead of `mode:` (with the same
        // arg shape and a lowercase mode string like "lsb"); accept both
        // so a single TciClient works against AetherSDR, ExpertSDR2, and
        // SunSDR-mb1 without per-server branching.
        if (args[0].trimmed() == "0") {
            const QString m = args[1].trimmed().toUpper();
            if (m != m_mode) {
                m_mode = m;
                emit modeChanged(m_mode);
            }
        }
    } else if (cmd == "protocol" && args.size() >= 2) {
        m_protoName    = args[0].trimmed();
        m_protoVersion = args[1].trimmed();
        emit serverInfoChanged(m_protoName, m_protoVersion);
    } else if (cmd == "device" && args.size() >= 1) {
        // device:<name> — announced once on connect. Identifies the SERVER
        // APPLICATION (e.g. "AetherSDR"), not the radio behind it.
        const QString d = args[0].trimmed();
        if (!d.isEmpty() && d != m_device) {
            m_device = d;
            emit deviceNameChanged(m_device);
        }
    } else if (cmd == "if" && args.size() >= 3) {
        // Some servers send `if:` instead of `vfo:` for the RX freq.
        if (args[0].trimmed() == "0" && args[1].trimmed() == "0") {
            const qint64 hz = args[2].trimmed().toLongLong();
            const double mhz = hz / 1.0e6;
            // `if` is a fallback — only honour it if we have nothing yet.
            if (m_freqMhz <= 0.0 && mhz > 0.0) {
                m_freqMhz = mhz;
                emit frequencyChanged(m_freqMhz);
            }
        }
    } else if (cmd == "trx" && args.size() >= 2) {
        // trx:rx,true|false — transmit state of TRX 0.
        if (args[0].trimmed() == "0") {
            const bool on = args[1].trimmed().compare(QLatin1String("true"),
                                                      Qt::CaseInsensitive) == 0;
            m_txPower.setTransmitting(on, m_clock.elapsed());
            if (on != m_transmitting) {
                m_transmitting = on;
                emit transmittingChanged(on);
            }
        }
    } else if (cmd == "tune" && args.size() >= 2) {
        // tune:rx,true|false — a tune carrier, whose power must not be logged
        // as a QSO's. ExpertSDR sends this; AetherSDR (as of 2026-09) does
        // not, so on AetherSDR a tune looks like any other transmission.
        if (args[0].trimmed() == "0") {
            m_txPower.setTuning(args[1].trimmed().compare(QLatin1String("true"),
                                                          Qt::CaseInsensitive) == 0);
        }
    } else if ((cmd == "tx_sensors" || cmd == "tx_sensor") && args.size() >= 5) {
        // tx_sensors:trx,mic_dbm,fwd_watts,peak_watts,swr[,alc_dbfs...]
        // Read by position; AetherSDR appends extra fields after swr. Both
        // spellings are accepted because TCI documents and servers differ.
        if (args[0].trimmed() == "0") {
            bool okFwd = false, okSwr = false;
            const double fwd = args[2].trimmed().toDouble(&okFwd);
            const double swr = args[4].trimmed().toDouble(&okSwr);
            if (okFwd) {
                m_txPower.addForwardPower(fwd, m_clock.elapsed());
                emit txSensorsReceived(fwd, okSwr ? swr : 0.0);
            }
        }
    } else if (cmd == "cw_macros_speed" && args.size() >= 1) {
        // cw_macros_speed:wpm — the answer to requestCwSpeed() and the echo
        // of setCwSpeed(). The speed is global (spec shape has no TRX), but
        // read the LAST argument so a `cw_macros_speed:0,25;` form also
        // means 25, the same rule AetherSDR applies to what it receives
        // (TciProtocol::cmdCwMacrosSpeed).
        bool ok = false;
        const int wpm = args.last().trimmed().toInt(&ok);
        if (ok && wpm > 0 && wpm != m_cwSpeedWpm) {
            m_cwSpeedWpm = wpm;
            emit cwSpeedChanged(m_cwSpeedWpm);
        }
    }
    // Other events (drive, rit, xit, sql, ...) intentionally ignored.
}

std::optional<double> TciClient::measuredTxPowerW() const
{
    const auto w = m_txPower.recentPeakWatts(m_clock.elapsed());
    if (!w) return std::nullopt;
    return TxPowerTracker::roundForLog(*w);
}

void TciClient::scheduleReconnect()
{
    if (m_userInitiatedDisconnect) return;
    if (m_probeMode) return;   // discovery probes are one-shot by design
    const int secs = backoffSeconds(m_reconnectAttempts);
    ++m_reconnectAttempts;
    m_reconnectTimer->start(secs * 1000);
}

void TciClient::cancelReconnect()
{
    if (m_reconnectTimer) m_reconnectTimer->stop();
}

void TciClient::onReconnectTimeout()
{
    if (m_userInitiatedDisconnect || m_url.isEmpty()) return;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
    // The abort above reports a drop, which schedules a reconnect. This IS
    // the reconnect, so that timer must not survive to abort it a second
    // later (and the next one, and the next: see onConnected).
    cancelReconnect();
    m_socket->open(m_url);
}

void TciClient::setConnected(bool c)
{
    if (m_connected == c) return;
    m_connected = c;
    emit connectionChanged(c);
}

} // namespace ShackBook
