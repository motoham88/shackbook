// tx_power_test — logging the power a QSO was actually made at (#23).
//
// Two layers:
//   1. TxPowerTracker's rules, driven with explicit times: peak not average,
//      the two-minute window, tune carriers ignored, servers without trx
//      edges, reset, and log rounding.
//   2. TciClient end to end against a fake TCI server on loopback: that it
//      asks for sensor readings on connect, parses AetherSDR's exact
//      `tx_sensors:` shape (including its extra trailing field), and forgets
//      readings when the connection drops.
//   3. That a reconnect does not leave the link churning (linkStaysUp).

#include "TciClient.h"
#include "TxPowerTracker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

#include <cmath>
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

bool near(std::optional<double> got, double want)
{
    return got && std::fabs(*got - want) < 1e-9;
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

void trackerRules()
{
    std::printf("\n-- TxPowerTracker rules --\n");

    {
        TxPowerTracker t;
        check(!t.recentPeakWatts(0), "nothing measured yet -> no value (caller uses the default)");
    }

    {
        // SSB-like transmission: readings go up and down; the PEAK is logged.
        TxPowerTracker t;
        t.setTransmitting(true, 1000);
        t.addForwardPower(12.0, 1100);
        t.addForwardPower(97.5, 1200);
        t.addForwardPower(40.0, 1300);
        check(near(t.recentPeakWatts(1350), 97.5), "while keyed: the peak so far");
        t.setTransmitting(false, 1400);
        check(near(t.recentPeakWatts(1500), 97.5), "after unkey: the peak, not the last reading");
        check(near(t.recentPeakWatts(1400 + TxPowerTracker::kDefaultWindowMs), 97.5),
              "still used exactly at the end of the 2-minute window");
        check(!t.recentPeakWatts(1400 + TxPowerTracker::kDefaultWindowMs + 1),
              "stale after the window -> no value");
    }

    {
        // A later transmission replaces the earlier one, even at lower power.
        TxPowerTracker t;
        t.setTransmitting(true, 0);   t.addForwardPower(100.0, 10); t.setTransmitting(false, 20);
        t.setTransmitting(true, 1000); t.addForwardPower(5.0, 1010); t.setTransmitting(false, 1020);
        check(near(t.recentPeakWatts(1100), 5.0), "QRP after QRO: the most recent transmission wins");
    }

    {
        // Keyed with no RF (zero / garbage readings) does not replace a real one.
        TxPowerTracker t;
        t.setTransmitting(true, 0);   t.addForwardPower(50.0, 10); t.setTransmitting(false, 20);
        t.setTransmitting(true, 100); t.addForwardPower(0.0, 110);
        t.addForwardPower(std::nan(""), 120); t.setTransmitting(false, 130);
        check(near(t.recentPeakWatts(200), 50.0), "a keyed-but-no-RF transmission is ignored");
    }

    {
        // Tune carriers never become the logged power.
        TxPowerTracker t;
        t.setTransmitting(true, 0); t.addForwardPower(80.0, 10); t.setTransmitting(false, 20);
        t.setTuning(true);
        t.setTransmitting(true, 100); t.addForwardPower(10.0, 110); t.setTransmitting(false, 120);
        t.setTuning(false);
        check(near(t.recentPeakWatts(200), 80.0), "a tune carrier (tune before key) is skipped");

        t.setTransmitting(true, 300); t.addForwardPower(10.0, 310);
        t.setTuning(true);             // tune starts mid-transmission
        t.setTransmitting(false, 320); t.setTuning(false);
        check(near(t.recentPeakWatts(400), 80.0), "a tune that starts mid-transmission taints it");
    }

    {
        // Server with sensor readings but no trx edges.
        TxPowerTracker t;
        t.addForwardPower(30.0, 0);
        t.addForwardPower(45.0, 1000);
        check(near(t.recentPeakWatts(1500), 45.0), "no trx edges: nearby readings form one transmission");
        t.addForwardPower(8.0, 1000 + TxPowerTracker::kOrphanGapMs + 1);
        check(near(t.recentPeakWatts(10000), 8.0), "no trx edges: a gap starts a new transmission");
    }

    {
        TxPowerTracker t;
        t.setTransmitting(true, 0); t.addForwardPower(100.0, 10); t.setTransmitting(false, 20);
        t.reset();
        check(!t.recentPeakWatts(30), "reset forgets everything");
    }

    check(TxPowerTracker::roundForLog(4.96) == 5.0,  "rounding: 4.96 W -> 5.0");
    check(TxPowerTracker::roundForLog(4.94) == 4.9,  "rounding: 4.94 W -> 4.9 (QRP keeps a decimal)");
    check(TxPowerTracker::roundForLog(99.6) == 100.0, "rounding: 99.6 W -> 100");
    check(TxPowerTracker::roundForLog(-3.0) == 0.0,  "rounding: nonsense -> 0");
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
            ++connections;
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
    int     connections = 0;

private:
    QWebSocketServer* m_server{nullptr};
    QWebSocket*       m_sock{nullptr};
    bool              m_ok{false};
};

void clientEndToEnd()
{
    std::printf("\n-- TciClient against a fake TCI server --\n");

    constexpr quint16 kPort = 45831;
    FakeTci server(kPort);
    check(server.ok(), "fake TCI server bound to loopback");
    if (!server.ok()) return;

    TciClient tci;
    bool lastTx = false;
    int  sensorEvents = 0;
    QObject::connect(&tci, &TciClient::transmittingChanged, [&](bool on) { lastTx = on; });
    QObject::connect(&tci, &TciClient::txSensorsReceived, [&](double, double) { ++sensorEvents; });

    tci.connectToServer(QStringLiteral("127.0.0.1"), kPort);
    check(waitFor([&] { return tci.connected() && server.clientConnected(); }),
          "client connects");
    check(waitFor([&] { return server.received.contains(QStringLiteral("tx_sensors_enable:true;")); }),
          "client asks for transmit sensor readings on connect");
    check(!tci.measuredTxPowerW(), "no measurement before any transmission");

    // AetherSDR's exact shape: tx_sensors:trx,mic_dbm,fwd_watts,peak_watts,swr,alc_dbfs
    server.send(QStringLiteral("trx:0,true;"));
    server.send(QStringLiteral("tx_sensors:0,-12.0,4.2,4.2,1.1,-3.0;"));
    server.send(QStringLiteral("tx_sensors:0,-10.0,4.9,4.9,1.2,-2.5;tx_sensors:0,-11.0,3.0,3.0,1.2,-2.0;"));
    check(waitFor([&] { return sensorEvents >= 3 && lastTx; }), "trx and tx_sensors lines are parsed");
    check(tci.transmitting(), "transmitting() follows trx:0,true");
    server.send(QStringLiteral("trx:0,false;"));
    check(waitFor([&] { return !tci.transmitting(); }), "transmitting() follows trx:0,false");
    check(near(tci.measuredTxPowerW(), 4.9), "measured power = peak of the transmission, rounded (4.9 W)");

    // Another receiver's readings must not be taken as TRX 0's.
    server.send(QStringLiteral("trx:1,true;tx_sensors:1,-10.0,500.0,500.0,1.0,0.0;trx:1,false;"));
    waitFor([] { return false; }, 200);
    check(near(tci.measuredTxPowerW(), 4.9), "TRX 1 readings are ignored");

    // A dropped connection forgets the measurement.
    server.dropClient();
    check(waitFor([&] { return !tci.connected(); }), "client notices the drop");
    check(!tci.measuredTxPowerW(), "measurement forgotten after the connection drops");

    tci.disconnectFromServer();
}

// ⭐ The link must stay up. Seen in AetherSDR's log during on-air tests:
// ShackBook dropped and reopened its TCI connection once a second, for a
// whole session. connectToServer() on a live socket aborts it, the abort
// schedules a reconnect, the socket reopens at once, and the timer then
// fires a second later and aborts the healthy connection, forever. Every
// drop makes frequency, mode, TX state and MQTT status flap, and forgets
// the measured TX power.
void linkStaysUp()
{
    std::printf("\n-- reconnecting does not churn the link --\n");

    constexpr quint16 kPort = 45832;
    FakeTci server(kPort);
    check(server.ok(), "fake TCI server bound to loopback");
    if (!server.ok()) return;

    TciClient tci;
    int drops = 0;
    QObject::connect(&tci, &TciClient::connectionChanged, [&](bool up) { if (!up) ++drops; });

    tci.connectToServer(QStringLiteral("127.0.0.1"), kPort);
    check(waitFor([&] { return tci.connected(); }), "client connects");

    // Connect again while connected, as a Settings save or a source switch
    // does. One replacement connection is expected; then it must hold.
    tci.connectToServer(QStringLiteral("127.0.0.1"), kPort);
    check(waitFor([&] { return tci.connected(); }), "and reconnects");
    const int dropsAfterReconnect = drops;
    const int connsAfterReconnect = server.connections;
    waitFor([] { return false; }, 3500);   // past several 1 s backoff periods
    check(tci.connected(), "still connected 3.5 s later");
    check(drops == dropsAfterReconnect, "no further drops after the reconnect");
    check(server.connections == connsAfterReconnect, "and no further connections to the server");

    tci.disconnectFromServer();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    trackerRules();
    clientEndToEnd();
    linkStaysUp();

    if (failures == 0) {
        std::printf("\ntx_power_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "\ntx_power_test: %d failure(s)\n", failures);
    return 1;
}
