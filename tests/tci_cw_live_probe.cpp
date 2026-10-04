// Drive TciClient's CW commands against a REAL TCI server and time what it does.
//
// Not a unit test — a bench tool. cw_send_test.cpp pins the bytes against a
// fake written alongside the client, so a shared wrong assumption would pass
// both. This points the real client at AetherSDR or a TCI bridge and reports
// what actually comes back — the answers the CW keyer (#32) is built on:
//
//   - does the server send trx:0,true when a CW macro keys the radio, and how
//     soon? (the keyer's "the radio didn't key" hint depends on it)
//   - how long from cw_macros_stop to trx:0,false?
//   - does a stop with nothing sending stay quiet?
//   - what speed does the server echo for an in-range and an out-of-range set?
//
//   tci_cw_live_probe <host> <port>               read-only (the default)
//   tci_cw_live_probe <host> <port> --transmit    keys the transmitter
//
// Read-only mode sends nothing but `start;` and the speed query
// `cw_macros_speed;`.
//
// ⚠ --transmit PUTS A CARRIER ON THE AIR, on whatever frequency the radio is
// on. It refuses unless the radio reports a CW mode, and counts down five
// seconds first; Ctrl-C stops it. Use a dummy load or low power on a clear
// frequency. The radio's speed is put back at the end.

#include "TciClient.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <cstdio>
#include <functional>

using namespace ShackBook;

namespace {

QElapsedTimer g_clock;

void say(const char* fmt, const QString& s = {})
{
    std::printf("%7lld ms  ", static_cast<long long>(g_clock.elapsed()));
    std::printf(fmt, qPrintable(s));
    std::printf("\n");
    std::fflush(stdout);
}

bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return done();
}

void pause(int ms) { waitFor([] { return false; }, ms); }

bool isCwMode(const QString& m)
{
    return m == QLatin1String("CW") || m == QLatin1String("CWL")
        || m == QLatin1String("CWU");
}

// Send `text`, then report how long until the radio keys and unkeys. With
// stopAfterMs >= 0, send a stop that long after it keys, and time that.
void timedSend(TciClient& tci, const QString& text, int stopAfterMs)
{
    say("> cw_macros:0,%s;", text);
    QElapsedTimer t;
    t.start();
    if (!tci.sendCw(text)) { say("!! sendCw refused or not connected"); return; }

    if (!waitFor([&] { return tci.transmitting(); }, 3000)) {
        say("!! no trx:0,true within 3 s: the radio did not key, or the server does not report it");
        return;
    }
    say("== keyed %s ms after the send", QString::number(t.elapsed()));

    if (stopAfterMs >= 0) {
        pause(stopAfterMs);
        say("> cw_macros_stop;");
        QElapsedTimer s;
        s.start();
        tci.stopCw();
        if (waitFor([&] { return !tci.transmitting(); }, 30000))
            say("== unkeyed %s ms after the stop", QString::number(s.elapsed()));
        else
            say("!! still transmitting 30 s after the stop");
        return;
    }

    if (waitFor([&] { return !tci.transmitting(); }, 30000))
        say("== unkeyed %s ms after the send", QString::number(t.elapsed()));
    else
        say("!! still transmitting 30 s after the send");
}

// Ask for `wpm`, report what the server echoed, if anything.
void speedStep(TciClient& tci, int wpm)
{
    int echoed = -1;
    auto c = QObject::connect(&tci, &TciClient::cwSpeedChanged,
                              [&](int w) { echoed = w; });
    say("> setCwSpeed(%s)", QString::number(wpm));
    tci.setCwSpeed(wpm);
    waitFor([&] { return echoed >= 0; }, 1500);
    QObject::disconnect(c);
    if (echoed >= 0) say("== server reports %s wpm", QString::number(echoed));
    else             say("== no echo within 1.5 s (unchanged, or refused silently); cwSpeedWpm() = %s",
                         QString::number(tci.cwSpeedWpm()));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    g_clock.start();

    if (argc < 3) {
        std::fprintf(stderr, "usage: tci_cw_live_probe <host> <port> [--transmit]\n");
        return 2;
    }
    const QString host = QString::fromUtf8(argv[1]);
    const quint16 port = quint16(QString::fromUtf8(argv[2]).toUInt());
    const bool transmit = argc > 3 && QByteArray(argv[3]) == "--transmit";

    TciClient tci;
    QObject::connect(&tci, &TciClient::rawMessageReceived, [](const QString& l) {
        say("< %s;", l);
    });

    tci.connectToServer(host, port);
    if (!waitFor([&] { return tci.connected(); }, 5000)) {
        say("!! could not connect: %s", tci.lastError());
        return 1;
    }
    pause(1500);   // let the connect burst land: device, mode, vfo, trx
    say("== device \"%s\"", tci.deviceName());
    say("== mode %s", tci.currentMode().isEmpty() ? QStringLiteral("(not reported)") : tci.currentMode());
    say("== frequency %s MHz", QString::number(tci.currentFrequencyMhz(), 'f', 6));
    say("== transmitting %s", tci.transmitting() ? QStringLiteral("YES") : QStringLiteral("no"));

    say("> cw_macros_speed;");
    tci.requestCwSpeed();
    waitFor([&] { return tci.cwSpeedWpm() > 0; }, 1500);
    const int originalWpm = tci.cwSpeedWpm();
    say("== speed %s", originalWpm > 0 ? QString::number(originalWpm) + " wpm"
                                       : QStringLiteral("(no reply to the GET)"));

    if (!transmit) {
        pause(1000);
        say("== read-only run done. Re-run with --transmit to key the radio.");
        tci.disconnectFromServer();
        return 0;
    }

    if (!isCwMode(tci.currentMode())) {
        say("!! refusing to transmit: the radio is in %s, not CW", tci.currentMode());
        tci.disconnectFromServer();
        return 1;
    }
    if (tci.transmitting()) {
        say("!! refusing to transmit: the radio is already transmitting");
        tci.disconnectFromServer();
        return 1;
    }

    std::printf("\n⚠  ABOUT TO TRANSMIT CW on %.6f MHz (%s). Ctrl-C now to abort.\n",
                tci.currentFrequencyMhz(), qPrintable(tci.currentMode()));
    for (int i = 5; i > 0; --i) {
        std::printf("   %d...\n", i);
        std::fflush(stdout);
        pause(1000);
        if (!tci.connected()) { say("!! connection lost; not transmitting"); return 1; }
    }

    say("-- 1. a short message, left to finish");
    timedSend(tci, QStringLiteral("TEST"), -1);
    pause(1500);

    say("-- 2. a long message, stopped 1.5 s after it keys");
    timedSend(tci, QStringLiteral("TEST TEST TEST TEST TEST TEST TEST TEST TEST TEST"), 1500);
    pause(1500);

    say("-- 3. a stop with nothing sending must not key the radio");
    say("> cw_macros_stop;");
    tci.stopCw();
    if (waitFor([&] { return tci.transmitting(); }, 1500))
        say("!! the radio KEYED after an idle stop");
    else
        say("== stayed in receive");

    say("-- 4. speed: in range, then the client's ceiling (60 wpm)");
    speedStep(tci, 22);
    speedStep(tci, 60);
    if (originalWpm > 0) {
        say("-- restoring the original speed");
        speedStep(tci, originalWpm);
    }

    pause(500);
    say("== done");
    tci.disconnectFromServer();
    return 0;
}
