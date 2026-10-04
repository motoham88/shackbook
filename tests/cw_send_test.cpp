// cw_send_test — the TCI commands that key a transmitter (#32).
//
// ShackBook's first TX path. Everything before it followed the radio or, at
// most, retuned it; a bug here puts a carrier on the air. Two layers:
//
//   1. tciCwMacroCommand(), the pure formatter: the exact bytes, commas kept,
//      and refusal (not stripping) of anything that would corrupt the TCI
//      stream — above all `;`, which would end the command and let the rest
//      of the text be parsed as a new one.
//
//   2. TciClient against a fake TCI server on loopback, asserting on the
//      exact bytes received: nothing CW is sent on connect, nothing while
//      disconnected, nothing queued for a reconnect, stop is sent even when
//      no send is in progress, speed is clamped, and the server's speed echo
//      is what cwSpeedWpm() reports.

#include "TciClient.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QWebSocket>
#include <QWebSocketServer>

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

void checkCmd(const char* text, const char* want)
{
    const QString got = tciCwMacroCommand(QString::fromLatin1(text));
    const bool ok = (got == QString::fromLatin1(want));
    std::printf("%s  \"%s\" -> %s\n", ok ? "PASS" : "FAIL", text,
                *want ? want : "(refused)");
    if (!ok) {
        std::printf("      got \"%s\"\n", qPrintable(got));
        ++failures;
    }
}

// Run the event loop until `done` returns true or `ms` passes.
bool waitFor(const std::function<bool()>& done, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

void settle() { waitFor([] { return false; }, 200); }

void formatter()
{
    std::printf("\n-- tciCwMacroCommand --\n");

    checkCmd("CQ TEST G0JKN", "cw_macros:0,CQ TEST G0JKN;");
    checkCmd("5NN 001",       "cw_macros:0,5NN 001;");
    checkCmd("  TU  ",        "cw_macros:0,TU;");          // trimmed, inner text untouched
    checkCmd("CQ,CQ",         "cw_macros:0,CQ,CQ;");       // AetherSDR keys commas
    checkCmd("NR? AGN? = + / . -", "cw_macros:0,NR? AGN? = + / . -;");
    checkCmd("tu",            "cw_macros:0,tu;");          // case is CwKeyer's business

    // ⭐ `;` ends a TCI command. "TU;trx:0,true" would otherwise send "TU"
    // and then hand the server a second, unrelated command.
    checkCmd("TU;trx:0,true", "");
    checkCmd(";",             "");
    checkCmd("TU;",           "");

    // Empty, and characters with no Morse meaning or agreed TCI handling.
    checkCmd("",              "");
    checkCmd("   ",           "");
    checkCmd("TU\nCQ",        "");
    checkCmd("TU\rCQ",        "");
    checkCmd("TU\tCQ",        "");
    check(tciCwMacroCommand(QStringLiteral("G0JKN É")).isEmpty(),
          "non-ASCII text is refused");
    check(tciCwMacroCommand(QStringLiteral("CQ\u007F")).isEmpty(),
          "DEL is refused");
}

void disconnectedGuards()
{
    std::printf("\n-- a client with no connection --\n");

    TciClient tci;
    check(!tci.sendCw(QStringLiteral("CQ")), "sendCw refuses while disconnected");
    check(!tci.stopCw(),                    "stopCw reports nothing written with no socket");
    check(!tci.setCwSpeed(25),              "setCwSpeed refuses while disconnected");
    check(!tci.requestCwSpeed(),            "requestCwSpeed refuses while disconnected");
    check(tci.cwSpeedWpm() == 0,            "speed is unknown before any server reports it");
}

// A TCI server that records what the client sends and lets the test push lines.
class FakeTci : public QObject {
public:
    explicit FakeTci(quint16 port)
    {
        m_server = new QWebSocketServer(QStringLiteral("fake-tci"),
                                        QWebSocketServer::NonSecureMode, this);
        m_ok = m_server->listen(QHostAddress::LocalHost, port);
        connect(m_server, &QWebSocketServer::newConnection, this, [this]() {
            m_sock = m_server->nextPendingConnection();
            connect(m_sock, &QWebSocket::textMessageReceived, this,
                    [this](const QString& msg) { received += msg; });
        });
    }
    bool ok() const { return m_ok; }
    bool clientConnected() const { return m_sock != nullptr; }
    void send(const QString& s) { if (m_sock) m_sock->sendTextMessage(s); }
    void dropClient() { if (m_sock) { m_sock->close(); m_sock = nullptr; } }

    QString received;

private:
    QWebSocketServer* m_server{nullptr};
    QWebSocket*       m_sock{nullptr};
    bool              m_ok{false};
};

void clientEndToEnd()
{
    std::printf("\n-- TciClient against a fake TCI server --\n");

    constexpr quint16 kPort = 45841;
    FakeTci server(kPort);
    check(server.ok(), "fake TCI server bound to loopback");
    if (!server.ok()) return;

    TciClient tci;
    int lastSpeed = -1;
    QObject::connect(&tci, &TciClient::cwSpeedChanged, [&](int wpm) { lastSpeed = wpm; });

    tci.connectToServer(QStringLiteral("127.0.0.1"), kPort);
    check(waitFor([&] { return tci.connected() && server.clientConnected(); }),
          "client connects");
    check(waitFor([&] { return server.received.contains(QStringLiteral("start;")); }),
          "client sends start;");
    settle();

    // ⭐ Connecting must never key anything, nor set the radio's speed.
    check(!server.received.contains(QStringLiteral("cw_")),
          "nothing CW is sent on connect");

    // ── sendCw: exact bytes ───────────────────────────────────────────
    server.received.clear();
    check(tci.sendCw(QStringLiteral("CQ TEST G0JKN")), "sendCw reports sent");
    check(waitFor([&] { return !server.received.isEmpty(); }), "server received it");
    check(server.received == QStringLiteral("cw_macros:0,CQ TEST G0JKN;"),
          "sendCw sends exactly cw_macros:0,<text>;");

    server.received.clear();
    check(tci.sendCw(QStringLiteral("CQ,CQ")), "text with commas is sent");
    waitFor([&] { return !server.received.isEmpty(); });
    check(server.received == QStringLiteral("cw_macros:0,CQ,CQ;"), "commas reach the wire intact");

    // ⭐ Refused text sends nothing at all, not a truncated half.
    server.received.clear();
    check(!tci.sendCw(QStringLiteral("TU;trx:0,true")), "text containing ';' is refused");
    check(!tci.sendCw(QStringLiteral("   ")),           "blank text is refused");
    settle();
    check(server.received.isEmpty(), "a refused send writes nothing");

    // ── stopCw: sent with nothing in progress ─────────────────────────
    server.received.clear();
    check(tci.stopCw(), "stopCw reports sent with no send in progress");
    waitFor([&] { return !server.received.isEmpty(); });
    check(server.received == QStringLiteral("cw_macros_stop;"), "stopCw sends exactly cw_macros_stop;");

    // ── setCwSpeed: clamped ───────────────────────────────────────────
    server.received.clear();
    check(tci.setCwSpeed(25), "setCwSpeed reports sent");
    tci.setCwSpeed(1);
    tci.setCwSpeed(200);
    check(waitFor([&] { return server.received.count(QLatin1Char(';')) >= 3; }), "three speed commands arrive");
    check(server.received == QStringLiteral("cw_macros_speed:25;cw_macros_speed:5;cw_macros_speed:60;"),
          "speed sent as cw_macros_speed:<wpm>; and clamped to 5..60");

    // ── The server's word on speed, not ours ──────────────────────────
    check(tci.cwSpeedWpm() == 0, "asking for a speed does not change cwSpeedWpm()");
    server.send(QStringLiteral("cw_macros_speed:28;"));
    check(waitFor([&] { return tci.cwSpeedWpm() == 28; }), "cw_macros_speed echo is parsed");
    check(lastSpeed == 28, "cwSpeedChanged carries the server's speed");
    server.send(QStringLiteral("cw_macros_speed:0,32;"));
    check(waitFor([&] { return tci.cwSpeedWpm() == 32; }),
          "a cw_macros_speed:<trx>,<wpm> form is read from its last argument");

    server.received.clear();
    check(tci.requestCwSpeed(), "requestCwSpeed reports sent");
    waitFor([&] { return !server.received.isEmpty(); });
    check(server.received == QStringLiteral("cw_macros_speed;"),
          "requestCwSpeed sends the bare GET cw_macros_speed;");
    server.send(QStringLiteral("cw_macros_speed:garbage;cw_macros_speed:0;"));
    settle();
    check(tci.cwSpeedWpm() == 32, "a malformed or zero speed is ignored");

    // ── ⭐ A drop: nothing sent, nothing queued for the reconnect ───────
    server.dropClient();
    check(waitFor([&] { return !tci.connected(); }), "client notices the drop");
    check(tci.cwSpeedWpm() == 0 && lastSpeed == 0, "speed becomes unknown after the drop");
    check(!tci.sendCw(QStringLiteral("TU")), "sendCw refuses after the drop");
    check(!tci.setCwSpeed(30),              "setCwSpeed refuses after the drop");

    server.received.clear();
    check(waitFor([&] { return tci.connected() && server.clientConnected(); }, 5000),
          "client auto-reconnects");
    waitFor([&] { return server.received.contains(QStringLiteral("start;")); });
    settle();
    check(!server.received.contains(QStringLiteral("cw_")),
          "nothing refused while down is replayed on reconnect");

    tci.disconnectFromServer();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    formatter();
    disconnectedGuards();
    clientEndToEnd();

    if (failures == 0) {
        std::printf("\ncw_send_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "\ncw_send_test: %d failure(s)\n", failures);
    return 1;
}
