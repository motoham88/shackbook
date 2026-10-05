// cw_keyer_test — what the CW keyer sends, and every rule about when (#32).
//
// CwKeyer is the only caller of the radio link's CW commands, so this is
// where the transmit-safety rules are pinned. A fake sender records every
// call; the assertions are on that log, so "nothing was sent" means exactly
// that.
//
//   1. The text: token expansion, cut numbers, sanitising, and the refusals
//      (unknown token, empty token, empty message).
//   2. The gates: disabled, not connected, not CW, no such macro.
//   3. The state machine against real timers: the "didn't key" hint, a hang
//      that survives transmit dropping between characters (seen on a
//      FLEX-6500), stop when idle, stop mid-message, replace, disable
//      mid-message, and a dropped connection.
//
// Not covered here, because CwKeyer has no path to it: "saving a QSO sends
// nothing". That is MainWindow's to prove when the panel is wired in.

#include "CwKeyer.h"
#include "CwSender.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStringList>

#include <cstdio>
#include <functional>

using namespace ShackBook;

namespace {

int failures = 0;

void check(bool cond, const char* what)
{
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
}

void checkEq(const QString& got, const QString& want, const char* what)
{
    const bool ok = got == want;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        std::printf("      got  \"%s\"\n      want \"%s\"\n", qPrintable(got), qPrintable(want));
        ++failures;
    }
}

bool waitFor(const std::function<bool()>& done, int ms = 2000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return done();
}

void pause(int ms) { waitFor([] { return false; }, ms); }

class FakeSender : public ICwSender {
public:
    bool    connected = true;
    QString mode      = QStringLiteral("CW");
    int     speed     = 24;
    bool    accept    = true;
    QStringList log;   // every call, in order: "send:TEXT", "stop", "speed:N"

    bool    cwConnected() const override { return connected; }
    QString cwMode() const override      { return mode; }
    bool    sendCwText(const QString& t) override { log << QStringLiteral("send:") + t; return accept; }
    bool    stopCwText() override        { log << QStringLiteral("stop"); return true; }
    bool    setCwTextSpeed(int w) override { log << QStringLiteral("speed:%1").arg(w); return true; }
    int     cwTextSpeed() const override { return speed; }
};

CwContext station()
{
    CwContext c;
    c.call   = QStringLiteral("g0jkn");
    c.myCall = QStringLiteral("KX3H");
    c.rst    = QStringLiteral("599");
    c.nr     = QStringLiteral("001");
    c.exch   = QStringLiteral("5NN MA");
    c.name   = QStringLiteral("TONY");
    return c;
}

QString expand(const char* macro, const CwContext& c = station(), CwCutOptions cut = {})
{
    const CwExpansion x = cwExpandMacro(QString::fromUtf8(macro), c, cut);
    return x.ok() ? x.text : QStringLiteral("ERROR: ") + x.error;
}

void text()
{
    std::printf("\n-- the text --\n");

    checkEq(cwCutNumbers(QStringLiteral("599"), false), QStringLiteral("5NN"), "cut: 599 -> 5NN");
    checkEq(cwCutNumbers(QStringLiteral("001"), false), QStringLiteral("TT1"), "cut: 001 -> TT1");
    checkEq(cwCutNumbers(QStringLiteral("001"), true),  QStringLiteral("TTA"), "cut with 1 -> A: 001 -> TTA");
    checkEq(cwCutNumbers(QStringLiteral("K5"), false),  QStringLiteral("K5"),  "cut leaves other characters alone");

    // The defaults expand as an N1MM user expects.
    checkEq(expand("CQ {MYCALL} {MYCALL} TEST"), QStringLiteral("CQ KX3H KX3H TEST"), "F1 CQ");
    checkEq(expand("{RST} {EXCH}"),              QStringLiteral("5NN 5NN MA"),        "F2 exchange: RST cut, EXCH as given");
    checkEq(expand("TU {MYCALL}"),               QStringLiteral("TU KX3H"),           "F3 TU");
    checkEq(expand("{CALL}"),                    QStringLiteral("G0JKN"),             "F5 his call, uppercased");
    checkEq(expand("{CALL} {NR}"),               QStringLiteral("G0JKN TT1"),         "NR is cut");
    checkEq(expand("{name}"),                    QStringLiteral("TONY"),              "token names are case-insensitive");

    CwCutOptions noCut;
    noCut.cutRst = noCut.cutNr = false;
    checkEq(expand("{RST} {NR}", station(), noCut), QStringLiteral("599 001"), "cut numbers can be turned off");

    CwContext noRst = station();
    noRst.rst.clear();
    checkEq(expand("{RST}", noRst), QStringLiteral("5NN"), "an empty RST means 599");

    // ⭐ Refusals: each of these would put something wrong on the air.
    CwContext noCall = station();
    noCall.call.clear();
    checkEq(expand("TU {CALL}", noCall), QStringLiteral("ERROR: Nothing in CALL"),
            "an empty token refuses the whole message");
    checkEq(expand("CQ {FOO}"), QStringLiteral("ERROR: Unknown token {FOO}"),
            "an unknown token is refused, not keyed as its name");
    checkEq(expand("CQ {MYCALL"), QStringLiteral("ERROR: Unclosed { in the message"),
            "an unclosed brace is refused");
    checkEq(expand("   "), QStringLiteral("ERROR: The message is empty"), "a blank message is refused");
    checkEq(expand(";;"),  QStringLiteral("ERROR: The message is empty"), "a message of nothing sendable is refused");

    // Sanitising: ';' would end the TCI command; others have no Morse.
    const CwExpansion x = cwExpandMacro(QStringLiteral("tu; 73 é!"), station(), {});
    checkEq(x.text, QStringLiteral("TU 73"), "sanitising keeps what has Morse, drops the rest");
    checkEq(x.dropped, QStringLiteral(";É!"), "and reports what it dropped");
    checkEq(expand("CQ,CQ = + / . - ?"), QStringLiteral("CQ,CQ = + / . - ?"), "CW punctuation is kept");
    checkEq(expand("CQ\nTEST"), QStringLiteral("CQ TEST"), "a line break is a word gap");

    check(isCwMode(QStringLiteral("CW")) && isCwMode(QStringLiteral("CWR"))
       && isCwMode(QStringLiteral("cwl")) && isCwMode(QStringLiteral("CWU")),
          "CW, CWR (AetherSDR), CWL/CWU (ExpertSDR and others) are CW modes");
    check(!isCwMode(QStringLiteral("USB")) && !isCwMode(QStringLiteral("DIGU"))
       && !isCwMode(QString()), "USB, DIGU and unknown are not");

    check(cwHangMs(24) == 500, "hang at 24 wpm is ten dots (500 ms)");
    check(cwHangMs(10) == 1200, "hang at 10 wpm covers a word space (1200 ms)");
    check(cwHangMs(60) == 400, "hang never drops under 400 ms");
    check(cwHangMs(0) == 600, "unknown speed reads as 20 wpm");
}

void gates()
{
    std::printf("\n-- the gates --\n");

    // ⭐ Off by default, and while off NOTHING reaches the sender.
    {
        FakeSender s;
        CwKeyer k(&s, station);
        check(!k.isEnabled(), "the keyer is off by default");
        for (int i = -1; i <= 8; ++i) k.sendMacro(i);
        k.stop();
        k.setSpeed(30);
        k.onTransmittingChanged(true);
        k.onTransmittingChanged(false);
        k.onConnectionChanged(false);
        check(s.log.isEmpty(), "while disabled: no send, no stop, no speed, for any input");
        check(k.sendMacro(0) == CwKeyer::Result::Disabled, "and a send says why");
    }

    FakeSender s;
    CwKeyer k(&s, station);
    k.setEnabled(true);

    s.connected = false;
    check(k.sendMacro(0) == CwKeyer::Result::NotConnected && s.log.isEmpty(),
          "not connected: refused, nothing sent");
    s.connected = true;

    s.mode = QStringLiteral("USB");
    check(k.sendMacro(0) == CwKeyer::Result::NotCwMode && s.log.isEmpty(),
          "USB: refused, nothing sent");
    checkEq(k.lastError(), QStringLiteral("The radio is in USB, not CW"), "with the reason");
    s.mode.clear();
    check(k.sendMacro(0) == CwKeyer::Result::NotCwMode, "mode unknown: refused");
    s.mode = QStringLiteral("CWR");
    check(k.sendMacro(0) == CwKeyer::Result::Sent, "CWR: sent");
    k.stop();
    s.log.clear();
    s.mode = QStringLiteral("CW");

    check(k.sendMacro(8) == CwKeyer::Result::NoSuchMacro && s.log.isEmpty(), "F9 does not exist");

    CwKeyer noCall(&s, [] { CwContext c = station(); c.call.clear(); return c; });
    noCall.setEnabled(true);
    check(noCall.sendMacro(4) == CwKeyer::Result::BadMacro && s.log.isEmpty(),
          "F5 with no call entered: refused, nothing sent");
    checkEq(noCall.lastError(), QStringLiteral("Nothing in CALL"), "with the reason");

    s.accept = false;
    check(k.sendMacro(0) == CwKeyer::Result::Refused, "a sender that writes nothing: reported");
    check(k.state() == CwKeyer::State::Idle, "and the keyer is not left Sending");
    s.accept = true;
    s.log.clear();

    // The context is read at the moment of sending, not cached.
    QString liveCall = QStringLiteral("W1AW");
    CwKeyer live(&s, [&] { CwContext c = station(); c.call = liveCall; return c; });
    live.setEnabled(true);
    live.sendMacro(4);
    liveCall = QStringLiteral("K1ABC");
    live.stop();
    live.sendMacro(4);
    check(s.log.contains(QStringLiteral("send:W1AW")) && s.log.contains(QStringLiteral("send:K1ABC")),
          "each send reads the call as it is now");
}

void stateMachine()
{
    std::printf("\n-- sending, stopping, and the radio's transmit state --\n");

    FakeSender s;
    CwKeyer k(&s, station);
    k.setTimings(/*noKeyMs*/ 150, /*hangOverrideMs*/ 120, /*stopTimeoutMs*/ 300);
    k.setEnabled(true);
    int didNotKey = 0, notConfirmed = 0;
    QObject::connect(&k, &CwKeyer::radioDidNotKey, [&] { ++didNotKey; });
    QObject::connect(&k, &CwKeyer::stopNotConfirmed, [&] { ++notConfirmed; });

    // ⭐ Esc with nothing sending still sends a stop.
    k.stop();
    check(s.log == QStringList{QStringLiteral("stop")}, "stop while idle still sends stop");
    s.log.clear();

    // A message that keys, drops out between characters, and finishes.
    check(k.sendMacro(2) == CwKeyer::Result::Sent, "F3 sent");
    check(s.log == QStringList{QStringLiteral("send:TU KX3H")}, "exactly the expanded text");
    checkEq(k.sendingText(), QStringLiteral("TU KX3H"), "the panel can show what is going out");
    check(k.state() == CwKeyer::State::Sending, "Sending");
    k.onTransmittingChanged(true);
    k.onTransmittingChanged(false);   // a gap between characters
    pause(60);
    k.onTransmittingChanged(true);    // back before the hang ran out
    check(k.state() == CwKeyer::State::Sending, "a gap shorter than the hang is still Sending");
    k.onTransmittingChanged(false);
    check(waitFor([&] { return k.state() == CwKeyer::State::Idle; }), "Idle once the hang runs out");
    check(k.sendingText().isEmpty(), "and the sending text is cleared");
    check(didNotKey == 0, "no 'didn't key' for a message that keyed");
    s.log.clear();

    // The radio never keys.
    k.sendMacro(0);
    check(waitFor([&] { return didNotKey == 1; }), "'didn't key' when the radio never transmits");
    check(k.state() == CwKeyer::State::Idle, "and the keyer is Idle");
    s.log.clear();

    // ⭐ Stop mid-message.
    k.sendMacro(0);
    k.onTransmittingChanged(true);
    k.stop();
    check(s.log.last() == QStringLiteral("stop"), "stop mid-message sends stop");
    check(k.state() == CwKeyer::State::Stopping, "Stopping until the radio unkeys");
    k.onTransmittingChanged(false);
    check(k.state() == CwKeyer::State::Idle, "Idle as soon as it does: no hang after a stop");
    s.log.clear();

    // A stop the radio does not honour is reported.
    k.sendMacro(0);
    k.onTransmittingChanged(true);
    k.stop();
    check(waitFor([&] { return notConfirmed == 1; }), "a radio still transmitting after STOP is reported");
    k.onTransmittingChanged(false);
    s.log.clear();

    // ⭐ A new message during one replaces it.
    k.sendMacro(0);
    k.onTransmittingChanged(true);
    k.sendMacro(2);
    check(s.log == QStringList({QStringLiteral("send:CQ KX3H KX3H TEST"), QStringLiteral("stop"),
                                QStringLiteral("send:TU KX3H")}),
          "a second message stops the first, then sends");
    k.onTransmittingChanged(false);
    waitFor([&] { return k.state() == CwKeyer::State::Idle; });
    s.log.clear();

    // ⭐ Disabling mid-message stops it, and then nothing more goes out.
    k.sendMacro(0);
    k.onTransmittingChanged(true);
    k.setEnabled(false);
    check(s.log == QStringList({QStringLiteral("send:CQ KX3H KX3H TEST"), QStringLiteral("stop")}),
          "disabling mid-message sends one stop");
    check(k.state() == CwKeyer::State::Idle, "and the keyer is Idle");
    s.log.clear();
    k.stop();
    k.sendMacro(0);
    check(s.log.isEmpty(), "after disabling, nothing is sent");
    k.setEnabled(true);
    k.onTransmittingChanged(false);
    s.log.clear();

    // Disabling while idle sends nothing.
    k.setEnabled(false);
    check(s.log.isEmpty(), "disabling while idle sends nothing");
    k.setEnabled(true);

    // A dropped connection ends the message without sending anything.
    k.sendMacro(0);
    k.onTransmittingChanged(true);
    s.log.clear();
    k.onConnectionChanged(false);
    check(k.state() == CwKeyer::State::Idle && s.log.isEmpty(),
          "a dropped connection: Idle, nothing sent, nothing queued");

    // Speed passes through while enabled.
    check(k.setSpeed(28) && s.log.last() == QStringLiteral("speed:28"), "speed passes through");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    text();
    gates();
    stateMachine();

    if (failures == 0) {
        std::printf("\ncw_keyer_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "\ncw_keyer_test: %d failure(s)\n", failures);
    return 1;
}
