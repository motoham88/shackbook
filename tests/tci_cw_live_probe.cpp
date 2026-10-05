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
//   tci_cw_live_probe <host> <port> --stop-tests  keys it: does a stop survive
//                                                 being followed AT ONCE by a new
//                                                 message, or by a disconnect?
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

// How long trx must stay false before a message counts as finished. AetherSDR
// on a FLEX with a short CWX break-in delay drops trx between characters, so
// trx:0,false alone does not mean the message is over.
constexpr int kHangMs = 700;

// Wait until the radio has been out of transmit for kHangMs, counting the
// keyed stretches on the way. Returns false on timeout.
bool waitForMessageEnd(TciClient& tci, int timeoutMs, int& segments, qint64& lastUnkeyMs,
                       const QElapsedTimer& since)
{
    bool wasTx = tci.transmitting();
    QElapsedTimer quiet;
    quiet.start();
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        const bool tx = tci.transmitting();
        if (tx && !wasTx) ++segments;
        if (!tx && wasTx) { quiet.restart(); lastUnkeyMs = since.elapsed(); }
        if (tx) quiet.restart();
        wasTx = tx;
        if (!tx && quiet.elapsed() >= kHangMs) return true;
    }
    return false;
}

// Send `text`, then report how long until the radio keys, how many times it
// keyed, and when it last unkeyed. With stopAfterMs >= 0, send a stop that
// long after it first keys, and time from the stop.
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

    int segments = 1;
    qint64 lastUnkey = -1;
    if (stopAfterMs >= 0) {
        pause(stopAfterMs);
        say("> cw_macros_stop;");
        QElapsedTimer s;
        s.start();
        tci.stopCw();
        segments = 0;
        if (waitForMessageEnd(tci, 30000, segments, lastUnkey, s))
            say("== %s", QStringLiteral("last unkey %1 ms after the stop; keyed %2 more time(s) after it")
                             .arg(lastUnkey < 0 ? 0 : lastUnkey).arg(segments));
        else
            say("!! still keying 30 s after the stop");
        return;
    }

    if (waitForMessageEnd(tci, 30000, segments, lastUnkey, t))
        say("== %s", QStringLiteral("message over: last unkey %1 ms after the send, keyed %2 time(s)")
                         .arg(lastUnkey).arg(segments));
    else
        say("!! still keying 30 s after the send");
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

// --stop-tests. A stop sent on its own ends a message within ~90 ms on a
// FLEX via AetherSDR, yet in ShackBook a stop followed at once by a new
// message (F-key replace) or by a disconnect let the message run to the end.
// Each case here sends a long message, stops 1.5 s in, then does the follow-up
// either at once or after a pause, and times the radio through a SECOND
// connection that stays up — the only way to see what the radio does after
// the first one has gone.
void runStopTests(TciClient& tci, const QString& host, quint16 port)
{
    const QString longText = QStringLiteral("TEST TEST TEST TEST TEST TEST TEST TEST TEST TEST");

    TciClient obs;
    obs.connectToServer(host, port);
    if (!waitFor([&] { return obs.connected(); }, 5000)) {
        say("!! observer could not connect");
        return;
    }
    pause(1500);
    QObject::connect(&obs, &TciClient::transmittingChanged, [](bool on) {
        say(on ? "   (observer) trx:0,true" : "   (observer) trx:0,false");
    });

    auto run = [&](const char* label, int pauseMs, bool disconnect) {
        say("-- %s", QString::fromLatin1(label));
        if (!tci.connected()) {
            tci.connectToServer(host, port);
            if (!waitFor([&] { return tci.connected() && isCwMode(tci.currentMode()); }, 8000)) {
                say("!! could not reconnect; skipping");
                return;
            }
            pause(1000);
        }
        say("> cw_macros:0,%s;", longText);
        tci.sendCw(longText);
        if (!waitFor([&] { return obs.transmitting(); }, 3000)) {
            say("!! the radio did not key; skipping");
            return;
        }
        pause(1500);

        QElapsedTimer t;
        t.start();
        // Timed from the stop, including any unkey during the pause below —
        // which is exactly when an honoured stop shows up.
        qint64 unkeyDuringPause = -1;
        auto c = QObject::connect(&obs, &TciClient::transmittingChanged, [&](bool on) {
            if (!on) unkeyDuringPause = t.elapsed();
        });
        say("> cw_macros_stop;");
        tci.stopCw();
        if (pauseMs > 0) pause(pauseMs);
        QObject::disconnect(c);
        if (disconnect) {
            say("> (disconnect)");
            tci.disconnectFromServer();
        } else {
            say("> cw_macros:0,TU;");
            tci.sendCw(QStringLiteral("TU"));
        }

        int segments = 0;
        qint64 lastUnkey = -1;
        if (!waitForMessageEnd(obs, 30000, segments, lastUnkey, t)) {
            say("!! still keying 30 s after the stop");
            return;
        }
        if (lastUnkey < 0) lastUnkey = unkeyDuringPause;
        // The long message has ~10 s left at 24 wpm when the stop goes.
        // "TU" alone is under a second.
        const bool honoured = lastUnkey >= 0 && lastUnkey < (disconnect ? 1500 : 3000);
        say("== %s", QStringLiteral("radio quiet: last unkey %1 ms after the stop  =>  %2")
                         .arg(lastUnkey)
                         .arg(honoured ? QStringLiteral("STOP HONOURED")
                                       : QStringLiteral("STOP IGNORED (message ran on)")));
        pause(1500);
    };

    run("5a. stop, then a new message AT ONCE (F-key replace)", 0, false);
    run("5b. stop, 250 ms, then a new message", 250, false);
    run("6a. stop, then disconnect AT ONCE", 0, true);
    run("6b. stop, 250 ms, then disconnect", 250, true);

    if (!tci.connected()) {
        tci.connectToServer(host, port);
        waitFor([&] { return tci.connected(); }, 5000);
    }
    obs.disconnectFromServer();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    g_clock.start();

    if (argc < 3) {
        std::fprintf(stderr, "usage: tci_cw_live_probe <host> <port> [--transmit | --stop-tests]\n");
        return 2;
    }
    const QString host = QString::fromUtf8(argv[1]);
    const quint16 port = quint16(QString::fromUtf8(argv[2]).toUInt());
    const bool stopTests = argc > 3 && QByteArray(argv[3]) == "--stop-tests";
    const bool transmit  = stopTests || (argc > 3 && QByteArray(argv[3]) == "--transmit");

    TciClient tci;
    // The stop tests print only the observer's trx and the results.
    if (!stopTests) {
        QObject::connect(&tci, &TciClient::rawMessageReceived, [](const QString& l) {
            say("< %s;", l);
        });
    }

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

    if (stopTests) {
        runStopTests(tci, host, port);
        say("== done");
        tci.disconnectFromServer();
        return 0;
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
