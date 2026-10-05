#include "CwKeyer.h"

#include "CwSender.h"

#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

namespace ShackBook {

// Off by default. QT_LOGGING_RULES="shackbook.cw.debug=true" turns it on:
// every send, stop, transmit edge and state change, for diagnosing on air.
Q_LOGGING_CATEGORY(lcCw, "shackbook.cw", QtInfoMsg)

namespace {
const char* stateName(CwKeyer::State s)
{
    switch (s) {
    case CwKeyer::State::Idle:     return "Idle";
    case CwKeyer::State::Sending:  return "Sending";
    case CwKeyer::State::Stopping: return "Stopping";
    }
    return "?";
}
} // namespace

namespace {

// How long after a send the radio has to report transmitting before the
// operator is told it did not key. The FLEX-6500 via AetherSDR keys in under
// 100 ms; 1.5 s leaves room for a slow link without leaving them wondering.
constexpr int kNoKeyMs = 1500;

// How long after a stop the radio may still report transmitting before the
// operator is told. The FLEX stops within ~90 ms; a radio that must finish
// text already in its own buffer can take several seconds.
constexpr int kStopTimeoutMs = 8000;

constexpr int kMinHangMs = 400;

// Added to the Morse-timing estimate of a message: the link and radio
// latency before it keys (~100 ms measured) and the break-in hold after it.
constexpr int kBusyMarginMs = 1000;

// Morse for what cwSanitize() lets through, '.' dot and '-' dash.
const char* morseFor(QChar c)
{
    switch (c.unicode()) {
    case u'A': return ".-";    case u'B': return "-...";  case u'C': return "-.-.";
    case u'D': return "-..";   case u'E': return ".";     case u'F': return "..-.";
    case u'G': return "--.";   case u'H': return "....";  case u'I': return "..";
    case u'J': return ".---";  case u'K': return "-.-";   case u'L': return ".-..";
    case u'M': return "--";    case u'N': return "-.";    case u'O': return "---";
    case u'P': return ".--.";  case u'Q': return "--.-";  case u'R': return ".-.";
    case u'S': return "...";   case u'T': return "-";     case u'U': return "..-";
    case u'V': return "...-";  case u'W': return ".--";   case u'X': return "-..-";
    case u'Y': return "-.--";  case u'Z': return "--..";
    case u'0': return "-----"; case u'1': return ".----"; case u'2': return "..---";
    case u'3': return "...--"; case u'4': return "....-"; case u'5': return ".....";
    case u'6': return "-...."; case u'7': return "--..."; case u'8': return "---..";
    case u'9': return "----.";
    case u'/': return "-..-."; case u'?': return "..--.."; case u'.': return ".-.-.-";
    case u',': return "--..--"; case u'=': return "-...-"; case u'+': return ".-.-.";
    case u'-': return "-....-";
    default:   return nullptr;
    }
}

} // namespace

QVector<CwMacro> defaultCwMacros()
{
    return {
        {QStringLiteral("CQ"),       QStringLiteral("CQ {MYCALL} {MYCALL} TEST")},
        {QStringLiteral("Exch"),     QStringLiteral("{RST} {EXCH}")},
        {QStringLiteral("TU"),       QStringLiteral("TU {MYCALL}")},
        {QStringLiteral("My call"),  QStringLiteral("{MYCALL}")},
        {QStringLiteral("His call"), QStringLiteral("{CALL}")},
        {QStringLiteral("Again"),    QStringLiteral("AGN?")},
        {QStringLiteral("?"),        QStringLiteral("?")},
        {QStringLiteral("Nr?"),      QStringLiteral("NR?")},
    };
}

QString cwCutNumbers(const QString& s, bool cutOne)
{
    QString out = s;
    for (QChar& c : out) {
        if (c == u'9')                 c = u'N';
        else if (c == u'0')            c = u'T';
        else if (cutOne && c == u'1')  c = u'A';
    }
    return out;
}

CwSanitized cwSanitize(const QString& s)
{
    static const QString kPunct = QStringLiteral("/?.,=+- ");
    CwSanitized r;
    r.text.reserve(s.size());
    for (QChar c : s.toUpper()) {
        // Line breaks and tabs from a multi-line macro editor are word gaps,
        // not mistakes, so they become spaces without a warning.
        if (c == u'\n' || c == u'\r' || c == u'\t') c = u' ';
        const char16_t u = c.unicode();
        const bool keep = (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9')
                       || kPunct.contains(c);
        if (keep) r.text += c;
        else if (!r.dropped.contains(c)) r.dropped += c;
    }
    r.text = r.text.trimmed();
    return r;
}

CwExpansion cwExpandMacro(const QString& macro, const CwContext& ctx,
                          const CwCutOptions& cut)
{
    CwExpansion r;
    QString raw;
    int i = 0;
    while (i < macro.size()) {
        const QChar c = macro.at(i);
        if (c != u'{') { raw += c; ++i; continue; }

        const int close = macro.indexOf(u'}', i + 1);
        if (close < 0) {
            r.error = QStringLiteral("Unclosed { in the message");
            return r;
        }
        const QString name = macro.mid(i + 1, close - i - 1).trimmed().toUpper();
        i = close + 1;

        // An unknown token is refused, not passed through: sanitising would
        // strip the braces and key the token's NAME on the air.
        const bool known = name == QLatin1String("CALL") || name == QLatin1String("MYCALL")
                        || name == QLatin1String("RST")  || name == QLatin1String("NR")
                        || name == QLatin1String("EXCH") || name == QLatin1String("NAME");
        if (!known) {
            r.error = QStringLiteral("Unknown token {%1}").arg(name);
            return r;
        }

        QString value;
        if      (name == QLatin1String("CALL"))   value = ctx.call;
        else if (name == QLatin1String("MYCALL")) value = ctx.myCall;
        else if (name == QLatin1String("RST"))    value = ctx.rst;
        else if (name == QLatin1String("NR"))     value = ctx.nr;
        else if (name == QLatin1String("EXCH"))   value = ctx.exch;
        else                                      value = ctx.name;

        // Sanitised before the emptiness check, so a value of only non-Morse
        // characters counts as empty rather than vanishing from the message.
        const CwSanitized v = cwSanitize(value);
        value = v.text;
        for (const QChar d : v.dropped)
            if (!r.dropped.contains(d)) r.dropped += d;

        if (name == QLatin1String("RST")) {
            if (value.isEmpty()) value = QStringLiteral("599");
            if (cut.cutRst) value = cwCutNumbers(value, cut.cutOne);
        } else if (name == QLatin1String("NR")) {
            // Serials go out as three digits, as contest loggers send them:
            // 1 -> 001, which cut numbers then make TT1.
            static const QRegularExpression kDigits(QStringLiteral("^[0-9]{1,2}$"));
            if (kDigits.match(value).hasMatch())
                value = value.rightJustified(3, u'0');
            if (cut.cutNr) value = cwCutNumbers(value, cut.cutOne);
        }

        // A token with nothing behind it is refused: "TU {CALL}" with no call
        // entered would otherwise send a bare "TU" to nobody in particular.
        if (value.isEmpty()) {
            const bool contestOnly = name == QLatin1String("NR") || name == QLatin1String("EXCH");
            r.error = contestOnly && !ctx.contest
                ? QStringLiteral("Nothing in %1: it is filled only in contest mode").arg(name)
                : QStringLiteral("Nothing in %1").arg(name);
            return r;
        }
        raw += value;
    }

    const CwSanitized clean = cwSanitize(raw);
    for (const QChar d : clean.dropped)
        if (!r.dropped.contains(d)) r.dropped += d;
    if (clean.text.isEmpty()) {
        r.error = QStringLiteral("The message is empty");
        return r;
    }
    r.text = clean.text;
    return r;
}

bool isCwMode(const QString& mode)
{
    const QString m = mode.trimmed().toUpper();
    return m == QLatin1String("CW")  || m == QLatin1String("CWR")
        || m == QLatin1String("CWL") || m == QLatin1String("CWU");
}

int cwHangMs(int wpm)
{
    const int w = wpm > 0 ? wpm : 20;
    // PARIS timing: one dot is 1200 / WPM milliseconds.
    return std::max(kMinHangMs, 10 * 1200 / w);
}

int cwDurationMs(const QString& text, int wpm)
{
    const int w = wpm > 0 ? wpm : 20;
    int units = 0;
    bool inWord = false;    // a character has been sent in the current word
    bool gapOwed = false;   // a word space is pending before the next character
    for (const QChar c : text.toUpper()) {
        if (c == u' ') { if (inWord) gapOwed = true; continue; }
        const char* m = morseFor(c);
        if (!m) continue;
        if (gapOwed)     units += 7;
        else if (inWord) units += 3;
        for (const char* p = m; *p; ++p) {
            if (p != m) units += 1;            // gap inside the character
            units += (*p == '.') ? 1 : 3;
        }
        inWord = true;
        gapOwed = false;
    }
    return units * 1200 / w;
}


CwKeyer::CwKeyer(ICwSender* sender, ContextProvider context, QObject* parent)
    : QObject(parent)
    , m_sender(sender)
    , m_context(std::move(context))
    , m_macros(defaultCwMacros())
    , m_busyMarginMs(kBusyMarginMs)
{
    m_noKeyTimer = new QTimer(this);
    m_noKeyTimer->setSingleShot(true);
    m_noKeyTimer->setInterval(kNoKeyMs);
    connect(m_noKeyTimer, &QTimer::timeout, this, &CwKeyer::onNoKeyTimeout);

    m_hangTimer = new QTimer(this);
    m_hangTimer->setSingleShot(true);
    connect(m_hangTimer, &QTimer::timeout, this, &CwKeyer::onHangTimeout);

    m_clock.start();

    m_stopTimer = new QTimer(this);
    m_stopTimer->setSingleShot(true);
    m_stopTimer->setInterval(kStopTimeoutMs);
    connect(m_stopTimer, &QTimer::timeout, this, &CwKeyer::onStopTimeout);
}

CwKeyer::~CwKeyer() = default;

void CwKeyer::setTimings(int noKeyMs, int hangOverrideMs, int stopTimeoutMs, int busyMarginMs)
{
    m_noKeyTimer->setInterval(noKeyMs);
    m_hangOverrideMs = hangOverrideMs;
    m_stopTimer->setInterval(stopTimeoutMs);
    if (busyMarginMs >= 0) m_busyMarginMs = busyMarginMs;
}

void CwKeyer::setEnabled(bool on)
{
    if (on == m_enabled) return;
    // Turning the keyer off must not leave a message keying: the one stop
    // sent while disabling, and the last thing this class sends until it is
    // enabled again.
    if (!on && radioMayBeSending() && m_sender)
        m_sender->stopCwText();
    m_enabled = on;
    if (!on) { finish(); return; }
    // The hang is timed from the radio's speed, and AetherSDR does not
    // report it until asked. A read, so it does not break "nothing CW is
    // sent until the operator asks": and only now the keyer is on.
    if (m_sender && m_sender->cwConnected())
        m_sender->requestCwTextSpeed();
}

CwKeyer::Result CwKeyer::sendMacro(int index)
{
    m_lastError.clear();
    if (!m_enabled) {
        m_lastError = QStringLiteral("The CW keyer is off in Settings");
        return Result::Disabled;
    }
    if (index < 0 || index >= m_macros.size()) {
        m_lastError = QStringLiteral("No F%1 message").arg(index + 1);
        return Result::NoSuchMacro;
    }
    if (!m_sender || !m_sender->cwConnected()) {
        m_lastError = QStringLiteral("Not connected to the radio");
        return Result::NotConnected;
    }
    const QString mode = m_sender->cwMode();
    if (!isCwMode(mode)) {
        m_lastError = mode.isEmpty()
            ? QStringLiteral("The radio has not reported its mode")
            : QStringLiteral("The radio is in %1, not CW").arg(mode);
        return Result::NotCwMode;
    }

    // A message cleared in Settings: name the key, since an empty-message
    // refusal alone would not say which one or where to fix it.
    if (m_macros.at(index).text.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("F%1 has no message (Settings → CW keyer)").arg(index + 1);
        return Result::BadMacro;
    }

    const CwExpansion x = cwExpandMacro(m_macros.at(index).text,
                                        m_context ? m_context() : CwContext{},
                                        m_cut);
    if (!x.ok()) {
        m_lastError = x.error;
        return Result::BadMacro;
    }

    // Replace a message in progress rather than appending to it.
    qCDebug(lcCw) << "sendMacro F" << index + 1 << "state" << stateName(m_state)
                  << "transmitting" << m_transmitting << "keyed" << m_keyed;
    if (radioMayBeSending()) {
        const bool ok = m_sender->stopCwText();
        qCDebug(lcCw) << "  replace: stop written" << ok;
    }

    const bool sent = m_sender->sendCwText(x.text);
    qCDebug(lcCw) << "  send" << x.text << "written" << sent;
    if (!sent) {
        m_lastError = QStringLiteral("The radio link did not take the message");
        finish();
        return Result::Refused;
    }

    if (!x.dropped.isEmpty())
        m_lastError = QStringLiteral("Not sent (no Morse for them): %1").arg(x.dropped);

    const int estimateMs = cwDurationMs(x.text, m_sender->cwTextSpeed());
    m_busyUntilMs = m_clock.elapsed() + estimateMs + m_busyMarginMs;
    qCDebug(lcCw) << "  estimated" << estimateMs << "ms at" << m_sender->cwTextSpeed() << "wpm";

    // Already transmitting — the usual case for a replace, since with a
    // break-in delay the radio never leaves transmit between the stop and
    // the new message — counts as keyed. Waiting for a fresh transmitting
    // edge that will never come would report "didn't key" and drop to Idle
    // with the radio still sending.
    m_keyed = m_transmitting;
    m_hangTimer->stop();
    m_stopTimer->stop();
    if (m_keyed) m_noKeyTimer->stop();
    else         m_noKeyTimer->start();
    m_sendingText = x.text;
    emit sendingTextChanged(m_sendingText);
    setState(State::Sending);
    return Result::Sent;
}

bool CwKeyer::stop()
{
    if (!m_enabled || !m_sender) return false;
    // Always sent (rule 3): the radio may be draining a buffer we think is
    // empty, and a stop that arrives when nothing is sending costs nothing.
    const bool ok = m_sender->stopCwText();
    qCDebug(lcCw) << "stop: state" << stateName(m_state) << "transmitting" << m_transmitting
                  << "written" << ok;
    if (!ok) m_lastError = QStringLiteral("Stop not sent: no link to the radio");

    m_noKeyTimer->stop();
    m_hangTimer->stop();
    if (m_state == State::Idle) return ok;
    if (m_transmitting) {
        m_stopTimer->start();
        setState(State::Stopping);
    } else {
        finish();
    }
    return ok;
}

bool CwKeyer::setSpeed(int wpm)
{
    if (!m_enabled || !m_sender) return false;
    return m_sender->setCwTextSpeed(wpm);
}

bool CwKeyer::requestSpeed()
{
    if (!m_enabled || !m_sender) return false;
    return m_sender->requestCwTextSpeed();
}

void CwKeyer::onTransmittingChanged(bool transmitting)
{
    qCDebug(lcCw) << "trx" << transmitting << "in" << stateName(m_state) << "keyed" << m_keyed;
    m_transmitting = transmitting;
    switch (m_state) {
    case State::Idle:
        break;
    case State::Sending:
        if (transmitting) {
            m_keyed = true;
            m_noKeyTimer->stop();
            m_hangTimer->stop();
        } else if (m_keyed) {
            const int wpm = m_sender ? m_sender->cwTextSpeed() : 0;
            m_hangTimer->start(m_hangOverrideMs > 0 ? m_hangOverrideMs : cwHangMs(wpm));
        }
        break;
    case State::Stopping:
        if (!transmitting) finish();
        break;
    }
}

void CwKeyer::onConnectionChanged(bool connected)
{
    if (!connected) {
        m_transmitting = false;
        finish();
        return;
    }
    if (m_enabled && m_sender)
        m_sender->requestCwTextSpeed();
}

void CwKeyer::onNoKeyTimeout()
{
    qCDebug(lcCw) << "no-key timeout in" << stateName(m_state) << "keyed" << m_keyed;
    if (m_state != State::Sending || m_keyed) return;
    m_lastError = QStringLiteral("The radio didn't key. Check the TCI server supports CW macros");
    finish();
    emit radioDidNotKey();
}

void CwKeyer::onHangTimeout()
{
    qCDebug(lcCw) << "hang timeout in" << stateName(m_state) << "transmitting" << m_transmitting;
    if (m_state != State::Sending || m_transmitting) return;
    // Quiet for the hang, but Morse timing says the message is not over yet:
    // a long gap in trx, not the end. Wait out the rest, then look again.
    const qint64 left = m_busyUntilMs - m_clock.elapsed();
    if (left > 0) {
        qCDebug(lcCw) << "  not over by Morse timing:" << left << "ms left";
        m_hangTimer->start(int(left));
        return;
    }
    finish();
}

void CwKeyer::onStopTimeout()
{
    if (m_state != State::Stopping) return;
    m_lastError = QStringLiteral("The radio was still transmitting after STOP");
    finish();
    emit stopNotConfirmed();
}

void CwKeyer::finish()
{
    m_noKeyTimer->stop();
    m_hangTimer->stop();
    m_stopTimer->stop();
    m_keyed = false;
    if (!m_sendingText.isEmpty()) {
        m_sendingText.clear();
        emit sendingTextChanged(m_sendingText);
    }
    setState(State::Idle);
}

void CwKeyer::setState(State s)
{
    if (s == m_state) return;
    qCDebug(lcCw) << "state" << stateName(m_state) << "->" << stateName(s);
    m_state = s;
    emit stateChanged(m_state);
}

} // namespace ShackBook
