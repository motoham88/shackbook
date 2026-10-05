// cw_panel_test — the CW keyer's settings and panel (#32).
//
//   1. Settings: off unless explicitly "1", defaults for anything missing,
//      and a save/load round trip.
//   2. The panel, headless (offscreen platform), against a recording fake
//      sender: buttons blocked with a reason until the radio is connected
//      and in CW, a click sends exactly the expanded message and shows it,
//      STOP stops (and says so when it could not), −/+ step the speed, a
//      keyer that is off sends nothing.
//   3. Esc through the application-wide filter stops once per key press,
//      including when the press would go on to a dialog.

#include "CwKeyer.h"
#include "CwKeyerPanel.h"
#include "CwKeyerSettings.h"
#include "CwSender.h"

#include <QApplication>
#include <QDialog>
#include <QHash>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QWindow>

#include <cstdio>

using namespace ShackBook;

namespace {

int failures = 0;

void check(bool cond, const char* what)
{
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
}

class FakeSender : public ICwSender {
public:
    bool    connected = true;
    QString mode      = QStringLiteral("CW");
    QStringList log;

    bool    cwConnected() const override { return connected; }
    QString cwMode() const override      { return mode; }
    bool    sendCwText(const QString& t) override { log << QStringLiteral("send:") + t; return true; }
    bool    stopCwText() override        { log << QStringLiteral("stop"); return socketOpen; }
    bool    setCwTextSpeed(int w) override { log << QStringLiteral("speed:%1").arg(w); return true; }
    int     cwTextSpeed() const override { return 24; }
    bool    socketOpen = true;   // false: a stop has nowhere to go
    bool    requestCwTextSpeed() override { log << QStringLiteral("speed?"); return true; }
};

CwContext station()
{
    CwContext c;
    c.call = QStringLiteral("G0JKN");
    c.myCall = QStringLiteral("KX3H");
    return c;
}

void settings()
{
    std::printf("\n-- settings --\n");

    QHash<QString, QString> store;
    auto get = [&](const QString& k, const QString& d) { return store.value(k, d); };
    auto set = [&](const QString& k, const QString& v) { store.insert(k, v); };

    CwKeyerConfig fresh = loadCwKeyerConfig(get);
    check(!fresh.enabled, "a log with no CW settings has the keyer OFF");
    check(fresh.macros.size() == 8 && fresh.macros[0].text == QStringLiteral("CQ {MYCALL} {MYCALL} TEST"),
          "and the default messages");
    check(fresh.cut.cutRst && fresh.cut.cutNr && !fresh.cut.cutOne, "cut numbers on for RST and NR, 1 -> A off");

    for (const char* v : {"", "true", "yes", "on", "2", " 1"}) {
        store.insert(QStringLiteral("CW_KEYER_ENABLED"), QString::fromLatin1(v));
        if (loadCwKeyerConfig(get).enabled) {
            std::printf("FAIL  CW_KEYER_ENABLED=\"%s\" turned the keyer on\n", v);
            ++failures;
        }
    }
    std::printf("PASS  only an exact \"1\" turns the keyer on\n");

    CwKeyerConfig cfg = fresh;
    cfg.enabled = true;
    cfg.macros[0] = {QStringLiteral("Run"), QStringLiteral("CQ CWT {MYCALL}")};
    cfg.macros[5] = {QString(), QStringLiteral("{NAME} {NR}")};
    cfg.cut.cutOne = true;
    cfg.name = QStringLiteral("TONY");
    store.clear();
    saveCwKeyerConfig(cfg, set);
    const CwKeyerConfig back = loadCwKeyerConfig(get);
    check(back.enabled, "enabled round-trips");
    check(back.macros[0].label == QStringLiteral("Run") && back.macros[0].text == QStringLiteral("CQ CWT {MYCALL}"),
          "a custom message round-trips");
    check(back.macros[5].label.isEmpty() && back.macros[5].text == QStringLiteral("{NAME} {NR}"),
          "a custom message with no label stays unlabelled");
    check(back.macros[2].text == QStringLiteral("TU {MYCALL}"), "untouched messages keep their defaults");
    check(back.cut.cutOne && back.name == QStringLiteral("TONY"), "cut options and name round-trip");

    store.insert(QStringLiteral("CW_F3_TEXT"), QString());
    check(loadCwKeyerConfig(get).macros[2].text == QStringLiteral("TU {MYCALL}"),
          "a cleared message falls back to its default, not an empty button");
}

void panel()
{
    std::printf("\n-- the panel --\n");

    FakeSender s;
    CwKeyer k(&s, station);
    CwKeyerPanel p(&k);

    k.setEnabled(true);
    s.log.clear();

    p.setRadioState(false, QString(), true);
    check(!p.macroButton(0)->isEnabled(), "not connected: buttons disabled");
    check(p.statusText() == QStringLiteral("Not connected to the radio"), "with the reason");

    p.setRadioState(true, QStringLiteral("USB"), true);
    check(!p.macroButton(0)->isEnabled() && p.statusText() == QStringLiteral("The radio is in USB, not CW"),
          "USB: disabled, with the reason");

    p.setRadioState(true, QStringLiteral("CW"), false);
    check(!p.macroButton(0)->isEnabled() && p.statusText().contains(QStringLiteral("TCI")),
          "a rigctld log: disabled, says a TCI link is needed");

    p.setRadioState(true, QStringLiteral("CW"), true);
    check(p.macroButton(0)->isEnabled(), "connected and in CW: enabled");
    check(p.macroButton(2)->text() == QStringLiteral("F3 TU"), "buttons are labelled F<n> <label>");
    check(p.macroButton(0)->focusPolicy() == Qt::NoFocus && p.stopButton()->focusPolicy() == Qt::NoFocus,
          "buttons never take focus from the call field");

    p.macroButton(2)->click();
    check(s.log == QStringList{QStringLiteral("send:TU KX3H")}, "clicking F3 sends exactly its message");
    check(p.statusText().contains(QStringLiteral("SENDING")) && p.statusText().contains(QStringLiteral("TU KX3H")),
          "and the panel shows what is going out");

    k.onTransmittingChanged(true);
    p.stopButton()->click();
    check(s.log.last() == QStringLiteral("stop"), "STOP sends stop");
    k.onTransmittingChanged(false);
    check(p.statusText() == QStringLiteral("Stopped"), "and says so once the radio unkeys");
    s.log.clear();

    p.stopButton()->click();
    check(s.log == QStringList{QStringLiteral("stop")}, "STOP with nothing sending still sends stop");
    s.log.clear();

    // An empty token: nothing sent, and the operator is told why.
    CwKeyer noCall(&s, [] { CwContext c = station(); c.call.clear(); return c; });
    CwKeyerPanel p2(&noCall);
    noCall.setEnabled(true);
    p2.setRadioState(true, QStringLiteral("CW"), true);
    s.log.clear();
    p2.macroButton(4)->click();
    check(s.log.isEmpty() && p2.statusText() == QStringLiteral("Nothing in CALL"),
          "F5 with no call: nothing sent, 'Nothing in CALL' shown");

    // ⭐ Esc with no link: the stop is not silently lost (#34 review).
    s.socketOpen = false;
    p.setRadioState(false, QString(), true);
    s.log.clear();
    p.stopNow();
    check(s.log == QStringList{QStringLiteral("stop")}
          && p.statusText() == QStringLiteral("Stop not sent: no link to the radio"),
          "STOP with no link: tried, and says it was not sent");
    s.socketOpen = true;
    p.setRadioState(true, QStringLiteral("CW"), true);
    check(p.statusText() == QStringLiteral("Ready"), "the no-link message clears once the link is back");

    // Speed: −/+ step from what was last asked, so quick clicks add up
    // rather than all asking for the reported speed ± 1 (#34 review).
    s.log.clear();
    p.setSpeed(24);
    p.fasterButton()->click();
    p.fasterButton()->click();
    p.fasterButton()->click();
    check(s.log == QStringList({QStringLiteral("speed:25"), QStringLiteral("speed:26"),
                                QStringLiteral("speed:27")}),
          "three quick + clicks ask for 25, 26, 27");
    p.setSpeed(27);
    s.log.clear();
    p.slowerButton()->click();
    check(s.log == QStringList{QStringLiteral("speed:26")}, "after the radio answers, steps start from its speed");
    p.setSpeed(60);
    s.log.clear();
    p.fasterButton()->click();
    check(s.log.isEmpty(), "no step past the client's 60 wpm ceiling");

    // The speed never arrived: −/+ stay usable and ask for it again.
    p.setSpeed(0);
    s.log.clear();
    check(p.fasterButton()->isEnabled(), "speed unknown: −/+ still enabled");
    p.fasterButton()->click();
    check(s.log == QStringList{QStringLiteral("speed?")}, "and a click asks the radio for its speed");
    check(p.statusText().contains(QStringLiteral("Asking")), "saying so");
    p.setSpeed(24);
    check(p.statusText() == QStringLiteral("Ready"), "until the radio answers");

    check(!p.slowerButton()->accessibleName().isEmpty() && !p.fasterButton()->accessibleName().isEmpty(),
          "−/+ have accessible names for screen readers");
    s.log.clear();

    // The keyer off: the panel cannot send anything, even when asked.
    k.setEnabled(false);
    s.log.clear();
    p.trigger(0);
    p.stopNow();
    check(s.log.isEmpty(), "with the keyer off, trigger and stop send nothing");
}

void escape()
{
    std::printf("\n-- Esc --\n");

    FakeSender s;
    CwKeyer k(&s, station);
    CwKeyerPanel p(&k);
    k.setEnabled(true);
    p.setRadioState(true, QStringLiteral("CW"), true);
    CwStopKeyFilter filter(&p);
    qApp->installEventFilter(&filter);

    QWidget main;
    auto* edit = new QLineEdit(&main);
    main.show();
    edit->setFocus();
    QCoreApplication::processEvents();

    p.trigger(0);
    k.onTransmittingChanged(true);
    s.log.clear();

    // As the platform delivers a key: to the window, which passes it on to
    // the focus widget — two trips past the application filter.
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(main.windowHandle(), &esc);
    check(s.log == QStringList{QStringLiteral("stop")}, "Esc in the main window stops, exactly once");
    k.onTransmittingChanged(false);
    s.log.clear();

    // ⭐ A modal dialog has its own window, where the main window's
    // shortcuts cannot reach. The application filter still does.
    QDialog dlg;
    auto* dlgEdit = new QLineEdit(&dlg);
    dlg.show();
    dlgEdit->setFocus();
    QCoreApplication::processEvents();
    QKeyEvent esc2(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(dlg.windowHandle(), &esc2);
    check(s.log == QStringList{QStringLiteral("stop")}, "Esc in a dialog stops too");
    s.log.clear();

    QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, QString(), /*autorep*/ true);
    QCoreApplication::sendEvent(main.windowHandle(), &repeat);
    check(s.log.isEmpty(), "auto-repeat of a held Esc does not flood stops");

    QKeyEvent other(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QCoreApplication::sendEvent(main.windowHandle(), &other);
    check(s.log.isEmpty(), "other keys do nothing");

    qApp->removeEventFilter(&filter);
    QKeyEvent esc3(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(main.windowHandle(), &esc3);
    check(s.log.isEmpty(), "with the filter removed (keyer off), Esc is just Esc");
}

} // namespace

int main(int argc, char** argv)
{
    // Headless: CI has no display.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    settings();
    panel();
    escape();

    if (failures == 0) {
        std::printf("\ncw_panel_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "\ncw_panel_test: %d failure(s)\n", failures);
    return 1;
}
