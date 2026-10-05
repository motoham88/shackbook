#pragma once

// CwKeyer — F1-F8 CW messages, and every rule about when one may be sent (#32).
//
// This is the ONLY caller of ICwSender, so it is the one place to audit for
// transmit safety. The rules, which are not up for negotiation:
//
//   1. Off by default. While disabled, nothing here reaches the sender: no
//      send, no stop, no speed, not even a speed query. The one exception is
//      the moment of disabling while the radio may still be sending, which
//      sends a stop: turning the keyer off must not leave a message keying.
//   2. Only an explicit operator action sends: sendMacro() is called from a
//      button click or an F-key, never on QSO save, spot click, connect or a
//      timer. Nothing in this class sends on its own.
//   3. stop() always sends a stop while enabled, whether or not a message is
//      thought to be in progress: the radio may still be draining its buffer.
//   4. Only in a CW mode (CW, CWR, CWL, CWU), with a visible reason otherwise.
//   5. No retries and no queueing: a failed send is reported and dropped.
//
// The pieces that decide what text goes out — token expansion, cut numbers,
// sanitising — are free functions so they can be tested without a keyer.

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

class QTimer;

namespace ShackBook {

class ICwSender;

struct CwMacro {
    QString label;   // shown on the button, e.g. "CQ"
    QString text;    // e.g. "CQ {MYCALL} {MYCALL} TEST"
};

// N1MM-style run messages for F1-F8.
QVector<CwMacro> defaultCwMacros();

// What the tokens expand to, read from QuickEntry and settings at the moment
// of sending — never cached, so a message always carries what is on screen.
struct CwContext {
    QString call;    // {CALL}   the station being worked
    QString myCall;  // {MYCALL}
    QString rst;     // {RST}    empty means 599
    QString nr;      // {NR}     sent serial; padded to three digits (1 -> 001)
    QString exch;    // {EXCH}   sent exchange, already resolved by the caller
    QString name;    // {NAME}   operator name
    // {NR} and {EXCH} come from the contest row, so outside contest mode the
    // caller leaves them empty and a message using them is refused with
    // that reason rather than a bare "Nothing in EXCH".
    bool    contest = true;
};

struct CwCutOptions {
    bool cutRst = true;    // 599 -> 5NN
    bool cutNr  = true;    // 001 -> TT1
    bool cutOne = false;   // also 1 -> A
};

// 9 -> N, 0 -> T, and with cutOne 1 -> A. Other characters untouched.
QString cwCutNumbers(const QString& s, bool cutOne);

// Uppercase, keep A-Z 0-9 space and / ? . , = + -, drop everything else.
// `dropped` lists what was removed (each character once), so the operator
// can be told rather than surprised.
struct CwSanitized {
    QString text;
    QString dropped;
};
CwSanitized cwSanitize(const QString& s);

// Expand tokens, apply cut numbers, sanitise. On failure `error` says why in
// words for the operator, and `text` must not be sent: an unknown token
// ("Unknown token {FOO}"), a token with nothing sendable behind it ("Nothing
// in CALL", checked after sanitising, so a call of only non-Morse characters
// counts as nothing), or a message that comes out empty.
struct CwExpansion {
    QString text;
    QString error;
    QString dropped;   // characters sanitising removed, if any
    bool ok() const { return error.isEmpty(); }
};
CwExpansion cwExpandMacro(const QString& macro, const CwContext& ctx,
                          const CwCutOptions& cut);

// CW, CWR (AetherSDR), CWL / CWU (ExpertSDR and others); case-insensitive.
bool isCwMode(const QString& mode);

// How long the radio must stay in receive before a message counts as over.
// Ten dot lengths — longer than a word space (seven) — because transmit can
// drop briefly mid-message (seen between characters, under 200 ms, on a
// FLEX-6500 via AetherSDR). Never under 400 ms. Unknown speed (0) is read
// as 20 WPM.
int cwHangMs(int wpm);

// How long `text` takes to send at `wpm`, by PARIS timing (dot 1, dash 3,
// gap inside a character 1, between characters 3, between words 7 units;
// one unit is 1200 / WPM ms). Characters without Morse count as nothing.
// Unknown speed (0) is read as 20 WPM.
//
// Why the keyer needs it: a quiet trx alone cannot tell a gap from the end of
// a message, and a keyer that wrongly thinks a message is over sends no stop
// on replace, disable or disconnect, so the radio keeps sending what it
// holds. Requiring the Morse estimate to have run out as well means no gap,
// however long, can end a message early.
int cwDurationMs(const QString& text, int wpm);


class CwKeyer : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,
        Sending,    // a message was accepted and the radio is, or will be, keying
        Stopping,   // stop sent; waiting for the radio to come out of transmit
    };

    enum class Result {
        Sent,
        Disabled,
        NotConnected,
        NotCwMode,
        NoSuchMacro,
        BadMacro,     // expansion failed: see lastError()
        Refused,      // the sender wrote nothing
    };

    using ContextProvider = std::function<CwContext()>;

    // `sender` is not owned and must outlive the keyer.
    CwKeyer(ICwSender* sender, ContextProvider context, QObject* parent = nullptr);
    ~CwKeyer() override;

    // Off by default. Disabling while radioMayBeSending() sends a stop
    // (rule 1). Enabling asks the radio for its speed, which the hang and the
    // panel need.
    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    void setMacros(const QVector<CwMacro>& macros) { m_macros = macros; }
    const QVector<CwMacro>& macros() const { return m_macros; }
    void setCutOptions(const CwCutOptions& cut) { m_cut = cut; }

    // F1 is index 0. A send while radioMayBeSending() replaces what is going
    // out (stop, then the new one), as N1MM does: the operator pressing a key
    // means "send THIS now".
    Result sendMacro(int index);

    // Esc / STOP (rule 3). False when no stop could be written (disabled, or
    // no link), with the reason in lastError().
    bool stop();

    // False when disabled or the sender wrote nothing.
    bool setSpeed(int wpm);
    // Ask the radio for its speed again. False when disabled or not written.
    bool requestSpeed();

    State   state() const { return m_state; }
    // Whether a stop is owed before anything that would otherwise leave the
    // radio keying: a new message, disabling, dropping the link. True while
    // a message is in progress, and also while Idle if the radio still
    // reports transmitting: Idle comes from timers and the Morse estimate,
    // not the radio, so a late key, a speed change mid-message or a slow
    // stop can leave it keying after the keyer thinks it is done. False
    // while disabled, when the keyer sends nothing anyway.
    bool    radioMayBeSending() const
    {
        return m_enabled && (m_state != State::Idle || m_transmitting);
    }
    // Why the last send did not go, or a warning about one that did; empty
    // when there is nothing to say.
    QString lastError() const { return m_lastError; }
    // The expanded text of the current message, exactly as sent.
    QString sendingText() const { return m_sendingText; }

    // For tests: shorten the waits. hangOverrideMs > 0 replaces cwHangMs();
    // busyMarginMs >= 0 replaces the margin added to the Morse estimate.
    void setTimings(int noKeyMs, int hangOverrideMs, int stopTimeoutMs, int busyMarginMs = -1);

public slots:
    // Wire to the radio link's transmit and connection state. A connect
    // while enabled asks the radio for its speed.
    void onTransmittingChanged(bool transmitting);
    void onConnectionChanged(bool connected);

signals:
    void stateChanged(ShackBook::CwKeyer::State state);
    void sendingTextChanged(const QString& text);
    // A message was accepted but the radio never reported transmitting:
    // the server may not support CW macros (AetherSDR ignores them silently
    // on a radio with no radio-side keyer).
    void radioDidNotKey();
    // A stop was sent but the radio was still transmitting afterwards.
    void stopNotConfirmed();

private:
    void setState(State s);
    void finish();
    void onNoKeyTimeout();
    void onHangTimeout();
    void onStopTimeout();

    ICwSender*       m_sender;
    ContextProvider  m_context;
    QVector<CwMacro> m_macros;
    CwCutOptions     m_cut;

    bool    m_enabled{false};
    State   m_state{State::Idle};
    bool    m_transmitting{false};
    bool    m_keyed{false};    // the current message has keyed the radio
    QString m_lastError;
    QString m_sendingText;

    QTimer* m_noKeyTimer{nullptr};
    QTimer* m_hangTimer{nullptr};
    QTimer* m_stopTimer{nullptr};
    int     m_hangOverrideMs{0};
    // When the current message should be over by Morse timing, on m_clock.
    // Sending never ends before this, however quiet trx goes.
    qint64  m_busyUntilMs{0};
    int     m_busyMarginMs;
    QElapsedTimer m_clock;
};

} // namespace ShackBook
