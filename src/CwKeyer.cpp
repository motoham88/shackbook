#include "CwKeyer.h"

#include "CwSender.h"

#include <QTimer>

#include <algorithm>

namespace ShackBook {

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
        QString value;
        if      (name == QLatin1String("CALL"))   value = ctx.call.trimmed();
        else if (name == QLatin1String("MYCALL")) value = ctx.myCall.trimmed();
        else if (name == QLatin1String("RST")) {
            value = ctx.rst.trimmed();
            if (value.isEmpty()) value = QStringLiteral("599");
            if (cut.cutRst) value = cwCutNumbers(value, cut.cutOne);
        }
        else if (name == QLatin1String("NR")) {
            value = ctx.nr.trimmed();
            if (cut.cutNr) value = cwCutNumbers(value, cut.cutOne);
        }
        else if (name == QLatin1String("EXCH"))   value = ctx.exch.trimmed();
        else if (name == QLatin1String("NAME"))   value = ctx.name.trimmed();
        else {
            r.error = QStringLiteral("Unknown token {%1}").arg(name);
            return r;
        }

        // A token with nothing behind it is refused: "TU {CALL}" with no call
        // entered would otherwise send a bare "TU" to nobody in particular.
        if (value.isEmpty()) {
            r.error = QStringLiteral("Nothing in %1").arg(name);
            return r;
        }
        raw += value;
    }

    const CwSanitized clean = cwSanitize(raw);
    r.dropped = clean.dropped;
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


CwKeyer::CwKeyer(ICwSender* sender, ContextProvider context, QObject* parent)
    : QObject(parent)
    , m_sender(sender)
    , m_context(std::move(context))
    , m_macros(defaultCwMacros())
{
    m_noKeyTimer = new QTimer(this);
    m_noKeyTimer->setSingleShot(true);
    m_noKeyTimer->setInterval(kNoKeyMs);
    connect(m_noKeyTimer, &QTimer::timeout, this, &CwKeyer::onNoKeyTimeout);

    m_hangTimer = new QTimer(this);
    m_hangTimer->setSingleShot(true);
    connect(m_hangTimer, &QTimer::timeout, this, &CwKeyer::onHangTimeout);

    m_stopTimer = new QTimer(this);
    m_stopTimer->setSingleShot(true);
    m_stopTimer->setInterval(kStopTimeoutMs);
    connect(m_stopTimer, &QTimer::timeout, this, &CwKeyer::onStopTimeout);
}

CwKeyer::~CwKeyer() = default;

void CwKeyer::setTimings(int noKeyMs, int hangOverrideMs, int stopTimeoutMs)
{
    m_noKeyTimer->setInterval(noKeyMs);
    m_hangOverrideMs = hangOverrideMs;
    m_stopTimer->setInterval(stopTimeoutMs);
}

void CwKeyer::setEnabled(bool on)
{
    if (on == m_enabled) return;
    // Turning the keyer off must not leave a message keying: the one stop
    // sent while disabling, and the last thing this class sends until it is
    // enabled again.
    if (!on && m_state != State::Idle && m_sender)
        m_sender->stopCwText();
    m_enabled = on;
    if (!on) finish();
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

    const CwExpansion x = cwExpandMacro(m_macros.at(index).text,
                                        m_context ? m_context() : CwContext{},
                                        m_cut);
    if (!x.ok()) {
        m_lastError = x.error;
        return Result::BadMacro;
    }

    // Replace a message in progress rather than appending to it.
    if (m_state != State::Idle)
        m_sender->stopCwText();

    if (!m_sender->sendCwText(x.text)) {
        m_lastError = QStringLiteral("The radio link did not take the message");
        finish();
        return Result::Refused;
    }

    if (!x.dropped.isEmpty())
        m_lastError = QStringLiteral("Not sent (no Morse for them): %1").arg(x.dropped);

    m_keyed = false;
    m_hangTimer->stop();
    m_stopTimer->stop();
    m_noKeyTimer->start();
    m_sendingText = x.text;
    emit sendingTextChanged(m_sendingText);
    setState(State::Sending);
    return Result::Sent;
}

void CwKeyer::stop()
{
    if (!m_enabled || !m_sender) return;
    // Always sent (rule 3): the radio may be draining a buffer we think is
    // empty, and a stop that arrives when nothing is sending costs nothing.
    m_sender->stopCwText();

    m_noKeyTimer->stop();
    m_hangTimer->stop();
    if (m_state == State::Idle) return;
    if (m_transmitting) {
        m_stopTimer->start();
        setState(State::Stopping);
    } else {
        finish();
    }
}

bool CwKeyer::setSpeed(int wpm)
{
    if (!m_enabled || !m_sender) return false;
    return m_sender->setCwTextSpeed(wpm);
}

void CwKeyer::onTransmittingChanged(bool transmitting)
{
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
    }
}

void CwKeyer::onNoKeyTimeout()
{
    if (m_state != State::Sending || m_keyed) return;
    m_lastError = QStringLiteral("The radio didn't key. Check the TCI server supports CW macros");
    finish();
    emit radioDidNotKey();
}

void CwKeyer::onHangTimeout()
{
    if (m_state == State::Sending && !m_transmitting) finish();
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
    m_state = s;
    emit stateChanged(m_state);
}

} // namespace ShackBook
