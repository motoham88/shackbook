#pragma once

// TciClient — WebSocket client speaking the EESDR TCI protocol.
//
// TCI (Transceiver Control Interface) is the wire protocol used by
// AetherSDR, ExpertSDR2, SunSDR / Expert Electronics radios.  Messages are
// short ASCII text, each terminated with `;`, of the form
//   command:arg1,arg2,...;
// We connect, send `start;` to request event streaming, then watch for
// `vfo:` and `mode:` events on RX 0 / VFO 0 (the primary receiver).
//
// Frequency on the wire is in Hz; we normalise to MHz for the rest of the
// app since that's how operators talk about freqs and how ADIF stores them.
//
// Auto-reconnect: if the socket closes for any reason, we retry with an
// exponential backoff (1, 2, 5, 10, 30 s) until the user explicitly calls
// disconnectFromServer().

#include "TxPowerTracker.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QUrl>

#include <optional>

class QWebSocket;
class QTimer;

namespace ShackBook {

// Settings key holding the operator-set nickname for the radio reached at
// this endpoint. Per host:port so two radios keep separate names; shared here
// because both SettingsDialog (writes) and MainWindow (reads) need the same
// key, and a mismatch would silently lose the nickname.
inline QString tciNicknameKey(const QString& host, const QString& port)
{
    return QStringLiteral("TCI_NICKNAME_%1_%2").arg(host, port);
}

// ADIF mode -> TCI modulation string, as a pure function so the mapping can
// be tested without a radio, a socket, or a TciClient.
//
// `currentMhz` exists only to resolve "SSB", which is not a sideband: below
// 10 MHz it means LSB and above it USB, the same rule an operator applies
// without thinking. Getting that wrong puts the radio on the opposite
// sideband and the DX sounds like nothing at all.
//
// Returns an empty string when the mode is unknown, ambiguous, or when SSB
// cannot be resolved because no frequency is known — every one of which
// means "leave the radio alone" rather than "pick something".
QString tciModulationForAdifMode(const QString& adifMode, double currentMhz);

// CW text -> the complete `cw_macros:0,<text>;` command, as a pure function so
// the bytes that key a transmitter can be pinned without a socket (#32).
//
// This is the transport's last line of defence, not the sanitiser: CwKeyer
// decides what an operator's macro becomes. Here we only refuse what would
// corrupt the TCI stream or the radio's buffer: `;` (it ends the command and
// the rest would be parsed as a NEW command), control characters, anything
// outside printable ASCII, and text that is empty once trimmed. Refusing
// rather than stripping is deliberate: silently sending something other than
// what the caller asked for is how a wrong callsign goes out on the air.
//
// Commas are kept. AetherSDR keys everything after `<trx>,` (so
// `cw_macros:0,CQ,CQ` sends "CQ,CQ"); whether other servers do is CwKeyer's
// call to make.
//
// Leading and trailing whitespace is trimmed (it has no Morse meaning); inner
// text is passed through untouched. Returns an empty string when the text is
// refused.
QString tciCwMacroCommand(const QString& text);

// Lowest and highest speed setCwSpeed() will send, in WPM.
inline constexpr int kTciCwMinWpm = 5;
inline constexpr int kTciCwMaxWpm = 60;
// How long after a speed set the client reads the speed back (ms).
inline constexpr int kTciCwSpeedReadbackMs = 250;


class TciClient : public QObject {
    Q_OBJECT

public:
    explicit TciClient(QObject* parent = nullptr);
    ~TciClient() override;

    // Open ws://host:port and request event streaming.  Cancels any
    // previous connection.  host is an IP or hostname; port is typically
    // 40001 for AetherSDR, 50001 for ExpertSDR2.
    void connectToServer(const QString& host, quint16 port);

    // Permanent disconnect — does not auto-reconnect.
    void disconnectFromServer();

    // One-shot mode for discovery probes: never auto-reconnect, whatever
    // happens. Without this a probe against a dead port retries on a backoff
    // forever, and a scan of a dozen ports leaves a dozen retry timers running
    // behind the app. Set it BEFORE connectToServer().
    void setProbeMode(bool probe) { m_probeMode = probe; }

    bool    connected()             const { return m_connected; }
    double  currentFrequencyMhz()   const { return m_freqMhz; }
    QString currentMode()           const { return m_mode; }
    QString serverProtocolName()    const { return m_protoName; }
    QString serverProtocolVersion() const { return m_protoVersion; }
    QString lastError()             const { return m_lastError; }
    QUrl    currentUrl()            const { return m_url; }

    // The name the server announces for itself via `device:`.
    //
    // ⚠ This is the APPLICATION, not the radio: AetherSDR answers
    // "AetherSDR" whether a Hermes-Lite 2 or a FLEX-6700 is behind it. Two
    // radios driven by the same application are indistinguishable here, which
    // is why a user-set nickname overrides this when attributing a QSO.
    QString deviceName()            const { return m_device; }

    // ── Measured transmit power (#23) ─────────────────────────────────
    //
    // On connect the client asks for transmit sensor readings
    // (`tx_sensors_enable:true;`); a server that does not support them
    // ignores the request. The value is the peak forward power of the most
    // recent transmission, if that is still going or ended within the last
    // two minutes — see TxPowerTracker for the rules. Empty means "nothing
    // trustworthy measured": use the configured default instead.
    //
    // ⚠ It is the RADIO's forward power. With an external amplifier in line
    // that is the drive level, not the power at the antenna.
    std::optional<double> measuredTxPowerW() const;
    bool transmitting() const { return m_transmitting; }

    // ── Tuning ────────────────────────────────────────────────────────
    //
    // Until now this client was read-only: it followed the radio so a QSO
    // could be logged with the right frequency and mode. These SEND, which
    // means a bug here moves somebody's radio mid-QSO rather than merely
    // showing a wrong number. Both are deliberately narrow — RX0/VFO0, no
    // split handling, no TX — and both no-op when not connected rather than
    // queueing, because a tune that lands after the operator has moved on is
    // worse than one that never happened.
    //
    // Return false when nothing was sent, so the caller can say so rather
    // than leaving the operator wondering whether the click registered.
    bool tuneToMhz(double mhz);

    // Best-effort: an unrecognised mode is not sent and returns false,
    // leaving the radio on whatever it had. Spot sources often omit the mode
    // entirely, so failing here must not prevent the frequency change.
    bool setModeString(const QString& adifMode);

    // ── Sending CW (#32) ──────────────────────────────────────────────
    //
    // These KEY THE TRANSMITTER. They are the only TX path in the app, and
    // CwKeyer is meant to be their only caller, so the operator-facing rules
    // (keyer enabled, CW mode, an explicit operator action) live there. This
    // layer only guarantees that what goes on the wire is well formed and
    // that nothing is queued: like tuning, a send while disconnected is
    // dropped, never replayed on reconnect.

    // Send `cw_macros:0,<text>;`. False when nothing was sent: not
    // connected, or the text was refused by tciCwMacroCommand().
    //
    // ⚠ True means the command was written, not that the radio keyed.
    // AetherSDR silently ignores cw_macros on a radio with no radio-side CW
    // keyer; watch transmittingChanged() for evidence it actually went out.
    bool sendCw(const QString& text);

    // Send `cw_macros_stop;`. Deliberately NOT gated on connected() or on
    // whether we think a send is in progress: the radio may still be
    // draining its buffer after we believe it finished, and a stop that
    // arrives when nothing is sending costs nothing. The only refusal is
    // having no open socket to write to. False when nothing was written.
    // Flushed to the socket before returning, so a disconnect straight
    // after cannot strand it in the buffer.
    bool stopCw();

    // Send `cw_macros_speed:<wpm>;`, clamped to kTciCwMinWpm..kTciCwMaxWpm,
    // then read the speed back with a GET shortly afterwards. False when not
    // connected. The result arrives as cwSpeedChanged(); until then
    // cwSpeedWpm() still shows the old value.
    //
    // The read-back is not optional. AetherSDR (v26.10.1) sends a set's
    // notification only to the OTHER clients (TciServer, "broadcast to all
    // other clients"), never to the one that asked, so without the GET the
    // requester never learns the new speed. It is delayed because AetherSDR
    // applies the set on a queued call: a GET in the same burst could read
    // the old value. Several sets in a row share one read-back.
    //
    // ⚠ The usable range is the RADIO's, and narrower on some backends:
    // AetherSDR silently ignores a speed outside its backend's CW-text
    // limits. A read-back that still shows the old speed means the set was
    // not accepted.
    bool setCwSpeed(int wpm);

    // Ask for the current macro speed (`cw_macros_speed;`, a GET). The answer
    // arrives as cwSpeedChanged(). Needed because AetherSDR does not include
    // the speed in its connect burst, so cwSpeedWpm() is 0 until something
    // asks. Not sent automatically on connect: with the keyer off ShackBook
    // sends no cw_ command of any kind. False when not connected.
    bool requestCwSpeed();

    // The macro speed the SERVER last reported (`cw_macros_speed:` echo or
    // event), or 0 when unknown: never what we asked for, so a display built
    // on it shows what the radio is really using.
    int cwSpeedWpm() const { return m_cwSpeedWpm; }


signals:
    void connectionChanged(bool connected);
    void frequencyChanged(double mhz);
    void modeChanged(const QString& mode);
    void serverInfoChanged(const QString& name, const QString& version);
    void deviceNameChanged(const QString& device);
    void transmittingChanged(bool transmitting);
    // Forward power (W) and SWR from a `tx_sensors:` reading on TRX 0.
    void txSensorsReceived(double forwardWatts, double swr);
    // The server reported its CW macro speed (WPM); 0 when it became unknown
    // because the connection dropped.
    void cwSpeedChanged(int wpm);
    // Diagnostic — every line received, after stripping the trailing ';'.
    void rawMessageReceived(const QString& line);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString& message);
    void onErrorOccurred();
    void onReconnectTimeout();

private:
    void send(const QString& cmd);
    void parseLine(const QString& line);
    void scheduleReconnect();
    void cancelReconnect();
    void setConnected(bool c);

    QWebSocket* m_socket{nullptr};
    QTimer*     m_reconnectTimer{nullptr};
    QTimer*     m_cwSpeedReadback{nullptr};   // see setCwSpeed()

    QUrl    m_url;
    bool    m_userInitiatedDisconnect{false};
    bool    m_probeMode{false};
    bool    m_connected{false};
    int     m_reconnectAttempts{0};

    double  m_freqMhz{0.0};
    QString m_mode;
    QString m_protoName;
    QString m_protoVersion;
    QString m_device;
    QString m_lastError;

    int            m_cwSpeedWpm{0};

    bool           m_transmitting{false};
    TxPowerTracker m_txPower;
    QElapsedTimer  m_clock;   // monotonic time for TxPowerTracker
};

} // namespace ShackBook
