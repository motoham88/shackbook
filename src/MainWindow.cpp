#include "MainWindow.h"

#include "AwardsDialog.h"
#include "QsoPartyDialog.h"
#include "LogbookModel.h"
#include "TciClient.h"
#include "RigctldClient.h"
#include "TciDiscovery.h"
#include "Version.h"
#include "server/WsjtxAdifReceiver.h"
#include "EditDialog.h"
#include "RigctldSupervisor.h"
#include "SettingsDialog.h"
#include "SpotIndex.h"
#include "DxClusterClient.h"
#include "PotaClient.h"
#include "CallsignLookup.h"
#include "SectionMapDialog.h"
#include "GridMapDialog.h"
#include "SessionMapDialog.h"
#include "LotwDialog.h"
#include "AprsActivityDialog.h"
#include "AetherSettingsReader.h"
#include "MqttPublisher.h"
#include "ShackStatus.h"
#include "CwKeyer.h"
#include "CwKeyerPanel.h"
#include "CwKeyerSettings.h"
#include "CwSender.h"

#include <QApplication>
#include <QDialog>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QTextCursor>
#include <QTimer>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QInputDialog>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSettings>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QSysInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace ShackBook {

namespace {

// ── Stylesheets ─────────────────────────────────────────────────────────
constexpr const char* kHeaderCallStyle =
    "QLabel { color: #00d8ef; font-size: 16px; font-weight: bold; "
    "font-family: Consolas, 'Cascadia Mono', monospace; }";

constexpr const char* kHeaderValueStyle =
    "QLabel { color: #ffd400; font-size: 14px; font-weight: bold; "
    "font-family: Consolas, 'Cascadia Mono', monospace; }";

constexpr const char* kHeaderLabelStyle =
    "QLabel { color: #6b8099; font-size: 9px; font-weight: bold; "
    "letter-spacing: 0.08em; }";

constexpr const char* kCallEditStyle =
    "QLineEdit { background: #0b1220; border: 1px solid #1c2a40; border-radius: 3px; "
    "padding: 5px 8px; font-size: 16px; font-weight: bold; color: #ffd400; "
    "font-family: Consolas, 'Cascadia Mono', monospace; letter-spacing: 0.05em; }"
    "QLineEdit:focus { border: 1px solid #00d8ef; }";

constexpr const char* kEditStyle =
    "QLineEdit { background: #0b1220; border: 1px solid #1c2a40; border-radius: 3px; "
    "padding: 4px 6px; font-size: 12px; color: #dde6f0; }"
    "QLineEdit:focus { border: 1px solid #00d8ef; }";

constexpr const char* kSaveBtnStyle =
    "QPushButton { background: #003040; border: 1px solid #00d8ef; border-radius: 3px; "
    "padding: 6px 16px; font-size: 12px; font-weight: bold; color: #00d8ef; }"
    "QPushButton:hover { background: #00475e; }"
    "QPushButton:disabled { background: #0b1220; border: 1px solid #1c2a40; color: #3a4a60; }";

constexpr const char* kDupBadgeIdle =
    "QLabel { color: transparent; font-size: 11px; font-weight: bold; "
    "letter-spacing: 0.1em; }";
constexpr const char* kDupBadgeHit =
    "QLabel { color: #ff5050; font-size: 11px; font-weight: bold; "
    "letter-spacing: 0.1em; }";

constexpr const char* kTciDotConnected =
    "QLabel { color: #4cff7c; font-size: 14px; }";
constexpr const char* kTciDotDisconnected =
    "QLabel { color: #6b8099; font-size: 14px; }";

QString askSavePath(QWidget* parent, const QString& title, const QString& filter,
                    const QString& defaultName)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QFileDialog::getSaveFileName(parent, title, dir + "/" + defaultName, filter);
}

// ── Multi-log store ────────────────────────────────────────────────────
// One SQLite file per operator callsign — shackbook-<CALL>.sqlite in the
// app-data dir.  The pre-multi-log single shackbook.sqlite is adopted
// (renamed) for the first callsign entered after upgrading.

QString logsDirPath()
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir{}.mkpath(dir);
    return dir;
}

QString callToLogFile(QString call)
{
    call = call.toUpper();
    call.replace(QRegularExpression(QStringLiteral("[^A-Z0-9]")),
                 QStringLiteral("_"));          // G0JKN/P -> G0JKN_P

    const QString base = logsDirPath() + QStringLiteral("/");
    const QString current = base + QStringLiteral("shackbook-") + call
                          + QStringLiteral(".sqlite");

    // Prefer the file that already EXISTS. A log created before the
    // ShackLog -> ShackBook rename is still named "shacklog-<CALL>.sqlite",
    // and the migration copies it across under that name. Always returning
    // the new-style path would quietly open a fresh empty database beside
    // the operator's real one, which looks exactly like data loss.
    if (!QFile::exists(current)) {
        const QString legacy = base + QStringLiteral("shacklog-") + call
                             + QStringLiteral(".sqlite");
        if (QFile::exists(legacy)) return legacy;
    }
    return current;   // new log, or one already migrated by name
}

QStringList existingLogCalls()
{
    // BOTH prefixes, deliberately. Logs created before the ShackLog ->
    // ShackBook rename are named "shacklog-<CALL>.sqlite", and the migration
    // copies them across under those names rather than renaming the
    // operator's files behind their back. Searching only the new prefix made
    // every existing logbook invisible even though it had been carried over
    // correctly - the data was there, the picker just could not see it.
    //
    // The old prefix stays supported indefinitely: these are the operator's
    // files, and no amount of tidiness justifies silently renaming somebody's
    // logbook.
    QSet<QString> seen;
    QStringList calls;
    const QDir dir(logsDirPath());
    for (const QString& prefix : {QStringLiteral("shackbook-"),
                                  QStringLiteral("shacklog-")}) {
        const auto files = dir.entryList({prefix + QStringLiteral("*.sqlite")},
                                         QDir::Files, QDir::Name);
        for (const QString& f : files) {
            QString call = f;
            call.remove(0, prefix.size());
            call.chop(QStringLiteral(".sqlite").size());
            if (!call.isEmpty() && !seen.contains(call)) {
                seen.insert(call);
                calls << call;
            }
        }
    }
    calls.sort();
    return calls;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_model(new LogbookModel(this)),
      m_tci(new TciClient(this)),
      m_rigctld(new RigctldClient(this)),
      m_rigctldSupervisor(new RigctldSupervisor(this)),
      m_spotIndex(new SpotIndex(this)),
      m_dxc(new DxClusterClient(this)),
      m_pota(new PotaClient(this)),
      m_spotPurgeTimer(new QTimer(this))
{
    setWindowTitle(windowTitleFor({}));
    resize(1200, 700);

    if (!chooseAndOpenLog(/*startup*/ true)) {
        QMessageBox::critical(this, "ShackBook",
                              QString("Could not open logbook database:\n%1")
                                  .arg(m_model->errorString()));
    }

    // WSJT-X direct ingest (solo-operating mode): listen for the Secondary
    // UDP Server ADIF datagrams and log them straight into whichever log is
    // open.  Point WSJT-X's Reporting tab at 127.0.0.1:1100.  (FD laptops
    // point at shack-hub's server instead — same wire, different listener.)
    m_wsjtx = new Server::WsjtxAdifReceiver(m_model, this);
    if (!m_wsjtx->start(1100))
        qWarning() << "WSJT-X UDP listener failed to bind 1100 — "
                      "direct logging from WSJT-X disabled";

    // Callsign-lookup chain: cty.dat ships inside the binary as a resource;
    // online providers are configured from settings (Lookup tab).
    m_cty.load(QStringLiteral(":/data/cty.dat"));
    if (!m_cty.isLoaded())
        qWarning() << "cty.dat resource failed to load — offline country "
                      "lookup disabled";
    m_lookup = new CallsignLookup(this);
    m_lookupDebounce = new QTimer(this);
    m_lookupDebounce->setSingleShot(true);
    m_lookupDebounce->setInterval(700);
    connect(m_lookupDebounce, &QTimer::timeout, this, [this] {
        if (m_callEdit) startCallsignLookup(m_callEdit->text());
    });
    connect(m_lookup, &CallsignLookup::result, this,
            [this](const CallsignLookup::Result& r) {
        if (r.call != m_lookupFillCall) return;     // call changed — stale
        if (!r.ok) {
            if (!r.error.isEmpty())
                statusBar()->showMessage(
                    QString("%1 lookup failed: %2").arg(r.source, r.error), 5000);
            return;
        }
        if (m_lookupFill.name.isEmpty())       m_lookupFill.name       = r.name;
        if (m_lookupFill.qth.isEmpty())        m_lookupFill.qth        = r.qth;
        if (m_lookupFill.state.isEmpty())      m_lookupFill.state      = r.state;
        if (m_lookupFill.cnty.isEmpty())       m_lookupFill.cnty       = r.county;
        if (m_lookupFill.country.isEmpty())    m_lookupFill.country    = r.country;
        if (m_lookupFill.gridsquare.isEmpty()) m_lookupFill.gridsquare = r.grid;
        if (!m_lookupFill.dxcc && r.dxcc)      m_lookupFill.dxcc       = r.dxcc;
        QStringList bits;
        if (!r.name.isEmpty()) bits << r.name;
        if (!r.qth.isEmpty())  bits << r.qth;
        if (!r.state.isEmpty()) bits << r.state;
        if (!bits.isEmpty())
            statusBar()->showMessage(
                QString("%1: %2 — fills on save").arg(r.source, bits.join(", ")),
                6000);
    });

    buildMenus();
    buildUI();

    connect(m_model, &LogbookModel::qsoAdded,    this, &MainWindow::onLogbookChanged);
    connect(m_model, &LogbookModel::qsoUpdated,  this, &MainWindow::onLogbookChanged);
    connect(m_model, &LogbookModel::qsoDeleted,  this, &MainWindow::onLogbookChanged);
    connect(m_model, &LogbookModel::settingChanged,
            this, &MainWindow::onContextSettingsChanged);

    connect(m_tci, &TciClient::connectionChanged, this, &MainWindow::onTciConnectionChanged);
    // `device:` lands just after the connection comes up, so the status bar
    // needs a second refresh once the name is known.
    connect(m_tci, &TciClient::deviceNameChanged, this, [this](const QString&) {
        refreshStatusBar();
    });
    connect(m_tci, &TciClient::frequencyChanged,  this, &MainWindow::onTciFrequencyChanged);
    connect(m_tci, &TciClient::modeChanged,       this, &MainWindow::onTciModeChanged);

    // Shack status to MQTT (#24). Transmit state only comes from TCI.
    m_mqtt = new MqttPublisher(this);
    m_mqtt->setThrottle(QString::fromLatin1(ShackStatus::kFrequency), 500);
    connect(m_tci, &TciClient::transmittingChanged, this, [this](bool on) {
        m_mqtt->publishState(QString::fromLatin1(ShackStatus::kTransmitting),
                             ShackStatus::onOff(on));
    });
    connect(m_model, &LogbookModel::qsoAdded, this,
            [this](qint64 id) { publishQsoStatus(id); });
    connect(m_model, &LogbookModel::qsoDeleted, this, [this]() { publishQsoStatus(); });
    m_mqttDayTimer = new QTimer(this);
    m_mqttDayTimer->setInterval(60 * 1000);
    connect(m_mqttDayTimer, &QTimer::timeout, this, [this]() { publishQsoStatus(); });

    // rigctld feeds the SAME handlers. Frequency and mode are frequency and
    // mode whatever carried them, so the rest of the app never learns which
    // kind of radio is attached.
    // A rigctld that dies takes the radio link with it. The old design
    // could not report that, because it never knew the process existed.
    connect(m_rigctldSupervisor, &RigctldSupervisor::exitedUnexpectedly,
            this, [this](const QString& detail) {
                statusBar()->showMessage(
                    tr("rigctld stopped: %1").arg(detail), 10000);
            });
    connect(m_rigctld, &RigctldClient::connectionChanged,
            this, &MainWindow::onTciConnectionChanged);
    connect(m_rigctld, &RigctldClient::frequencyChanged,
            this, &MainWindow::onTciFrequencyChanged);
    connect(m_rigctld, &RigctldClient::modeChanged,
            this, &MainWindow::onTciModeChanged);
    connect(m_rigctld, &RigctldClient::modelNameChanged, this, [this](const QString&) {
        refreshStatusBar();
    });

    connect(m_dxc, &DxClusterClient::connectionChanged,
            this, &MainWindow::onClusterConnectionChanged);
    connect(m_dxc, &DxClusterClient::spotReceived,
            this, &MainWindow::onClusterSpotReceived);
    connect(m_dxc, &DxClusterClient::rawLine,
            this, &MainWindow::onClusterRawLine);
    connect(m_pota, &PotaClient::spotReceived,
            this, &MainWindow::onPotaSpotReceived);
    connect(m_pota, &PotaClient::pollCompleted,
            this, &MainWindow::onPotaPollCompleted);

    connect(m_dxc, &DxClusterClient::loginRejected, this,
            [this](const QString& reason) {
                if (m_dxcLog) {
                    m_dxcLog->appendPlainText(
                        QString("[%1] LOGIN REJECTED: %2 — auto-reconnect stopped. "
                                "Try a different Login suffix in Settings → DX Cluster.")
                            .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                            .arg(reason));
                }
                statusBar()->showMessage(
                    QString("Cluster rejected login (%1) — see DX Cluster Log").arg(reason),
                    8000);
                refreshStatusBar();
            });

    connect(m_dxc, &DxClusterClient::duplicateLoginKick, this,
            [this](const QString& msg) {
                if (m_dxcLog) {
                    m_dxcLog->appendPlainText(
                        QString("[%1] DUPLICATE LOGIN KICK: %2 — another "
                                "program logged in with our callsign-SSID "
                                "(second ShackBook / laptop / logger?). "
                                "Retrying slowly; change the Login suffix in "
                                "Settings → DX Cluster on one of them.")
                            .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                            .arg(msg));
                }
                statusBar()->showMessage(
                    "Cluster: another client logged in with our callsign — "
                    "see DX Cluster Log", 10000);
            });

    // Hidden, lazy-shown diagnostic buffer for the cluster traffic.
    // Populated continuously so it has history when first opened.
    m_dxcLog = new QPlainTextEdit;
    m_dxcLog->setReadOnly(true);
    m_dxcLog->setMaximumBlockCount(20000);
    m_dxcLog->setStyleSheet(
        "QPlainTextEdit { background-color: #050a14; color: #dde6f0; "
        "font-family: Consolas, 'Cascadia Mono', monospace; font-size: 11px; "
        "border: 1px solid #1c2a40; }");
    m_dxcLog->setLineWrapMode(QPlainTextEdit::NoWrap);

    // Seed with where we expect to find AetherSDR's settings, so we know
    // immediately if the path lookup is wrong.
    const auto detected = AetherSettingsReader::readDxClusterConfig();
    m_dxcLog->appendPlainText(QString("[boot] AetherSDR settings path: %1")
                                  .arg(detected.sourcePath));
    if (detected.found) {
        m_dxcLog->appendPlainText(QString("[boot] detected %1:%2 as %3 (auto=%4)")
                                      .arg(detected.host)
                                      .arg(detected.port)
                                      .arg(detected.callsign)
                                      .arg(detected.autoConnect ? "yes" : "no"));
    } else {
        m_dxcLog->appendPlainText("[boot] AetherSDR settings file NOT readable / no DxCluster keys");
    }

    // Drop any spot older than its lifetime once a minute so the index
    // doesn't grow without bound during a long session.
    m_spotPurgeTimer->setInterval(60 * 1000);
    connect(m_spotPurgeTimer, &QTimer::timeout, this, &MainWindow::purgeStaleSpots);
    m_spotPurgeTimer->start();

    refreshHeader();
    refreshContestUI();
    refreshTable();
    populateBandFilter();
    populateModeFilter();
    populateContestFilter();
    refreshStatusBar();
    refreshLinkLabel();

    applyAutoConnectFromSettings();
    applyClusterConfigFromSettings();
    applyPotaConfigFromSettings();
    applyLookupConfigFromSettings();
    applyMqttConfigFromSettings();

    buildCwKeyer();
    applyCwKeyerFromSettings();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    stopCwBeforeLinkGoes();
    QMainWindow::closeEvent(event);
}

void MainWindow::stopCwBeforeLinkGoes()
{
    if (!m_cwKeyer || m_cwKeyer->state() == CwKeyer::State::Idle) return;
    const bool wasTransmitting = m_tci->transmitting();
    m_cwPanel->stopNow();

    // Measured on a FLEX-6500 via AetherSDR v26.10.1: a stop followed AT
    // ONCE by a disconnect is lost and the message runs to the end (16 s);
    // the same stop with 250 ms before the disconnect ends it in ~90 ms.
    // So hold the link until the radio has unkeyed, which proves the stop
    // was carried out, or 250 ms if it was not transmitting to begin with.
    // Capped, and user input is held off meanwhile so nothing else can
    // start in the middle of a disconnect.
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 600) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
        if (wasTransmitting ? !m_tci->transmitting() : t.elapsed() >= 250) break;
    }
}

MainWindow::~MainWindow()
{
    // Closing ShackBook mid-message must not leave the radio keying: turning
    // the keyer off sends a stop if, and only if, a message is in progress.
    // The TCI client is still alive here; children are deleted after this.
    if (m_cwKeyer) m_cwKeyer->setEnabled(false);
}

// ── CW keyer (#32) ─────────────────────────────────────────────────────

void MainWindow::buildCwKeyer()
{
    m_cwSender = std::make_unique<TciCwSender>(m_tci);
    m_cwKeyer  = new CwKeyer(m_cwSender.get(), [this] { return cwContext(); }, this);
    m_cwPanel  = new CwKeyerPanel(m_cwKeyer, this);
    addDockWidget(Qt::BottomDockWidgetArea, m_cwPanel);
    m_cwPanel->hide();
    m_cwStopFilter = new CwStopKeyFilter(m_cwPanel, this);

    connect(m_tci, &TciClient::transmittingChanged, m_cwKeyer, &CwKeyer::onTransmittingChanged);
    connect(m_tci, &TciClient::connectionChanged,   m_cwKeyer, &CwKeyer::onConnectionChanged);
    connect(m_tci, &TciClient::cwSpeedChanged,      m_cwPanel, &CwKeyerPanel::setSpeed);
    connect(m_tci, &TciClient::connectionChanged, this, [this](bool) { refreshCwPanelRadioState(); });
    connect(m_tci, &TciClient::modeChanged,       this, [this](const QString&) { refreshCwPanelRadioState(); });
}

void MainWindow::applyCwKeyerFromSettings()
{
    if (!m_cwKeyer || !m_model) return;
    const CwKeyerConfig cfg = loadCwKeyerConfig(
        [this](const QString& k, const QString& d) { return m_model->settingValue(k, d); });

    m_cwKeyer->setMacros(cfg.macros);
    m_cwKeyer->setCutOptions(cfg.cut);
    m_cwPanel->refreshLabels();

    // Disabling sends a stop if a message is in progress; enabling asks the
    // radio for its speed. Neither happens when nothing changed.
    m_cwKeyer->setEnabled(cfg.enabled);
    m_cwPanel->setVisible(cfg.enabled);

    if (cfg.enabled && m_cwShortcuts.isEmpty()) {
        // Window-scoped: F1-F8 send only while a ShackBook main-window widget
        // has focus. No auto-repeat: holding F1 must not send CQ over and
        // over, each one replacing the last.
        for (int i = 0; i < 8; ++i) {
            auto* sc = new QShortcut(QKeySequence(Qt::Key_F1 + i), this);
            sc->setContext(Qt::WindowShortcut);
            sc->setAutoRepeat(false);
            connect(sc, &QShortcut::activated, this, [this, i] { m_cwPanel->trigger(i); });
            m_cwShortcuts << sc;
        }
        qApp->installEventFilter(m_cwStopFilter);
    } else if (!cfg.enabled && !m_cwShortcuts.isEmpty()) {
        qDeleteAll(m_cwShortcuts);
        m_cwShortcuts.clear();
        qApp->removeEventFilter(m_cwStopFilter);
    }
    refreshCwPanelRadioState();
}

void MainWindow::refreshCwPanelRadioState()
{
    if (!m_cwPanel) return;
    m_cwPanel->setRadioState(m_tci->connected(), m_tci->currentMode(), !usingRigctld());
    m_cwPanel->setSpeed(m_tci->cwSpeedWpm());
}

CwContext MainWindow::cwContext() const
{
    CwContext c;
    if (m_callEdit)      c.call   = m_callEdit->text();
    if (m_model)         c.myCall = m_model->myCall();
    if (m_rstSentEdit)   c.rst    = m_rstSentEdit->text();
    if (m_stxEdit)       c.nr     = m_stxEdit->text();
    if (m_stxStringEdit) c.exch   = m_stxStringEdit->text();
    if (m_model)         c.name   = m_model->settingValue(QStringLiteral("CW_NAME"));
    return c;
}

// ── Menus ──────────────────────────────────────────────────────────────

void MainWindow::buildMenus()
{
    auto* fileMenu = menuBar()->addMenu("&File");
    m_actSwitchLog = fileMenu->addAction("Switch &Operator / Log…", this, &MainWindow::onSwitchLog);
    fileMenu->addSeparator();
    m_actImportAdif = fileMenu->addAction("&Import ADIF…", this, &MainWindow::onImportAdif);
    fileMenu->addSeparator();
    m_actExportAdif = fileMenu->addAction("Export &ADIF…", this, &MainWindow::onExportAdif);
    m_actExportCab  = fileMenu->addAction("Export &Cabrillo…", this, &MainWindow::onExportCabrillo);
    fileMenu->addSeparator();
    m_actQuit = fileMenu->addAction("&Quit", this, &QWidget::close);
    m_actQuit->setShortcut(QKeySequence::Quit);

    auto* editMenu = menuBar()->addMenu("&Edit");
    m_actNew    = editMenu->addAction("&New QSO…",    this, &MainWindow::onNewQso);
    m_actNew->setShortcut(QKeySequence::New);
    m_actEdit   = editMenu->addAction("&Edit QSO…",   this, &MainWindow::onEditQso);
    m_actDelete = editMenu->addAction("&Delete QSO",  this, &MainWindow::onDeleteQso);

    auto* toolsMenu = menuBar()->addMenu("&Tools");
    m_actSettings      = toolsMenu->addAction("&Settings…", this, &MainWindow::onSettings);
    m_actAwards        = toolsMenu->addAction("&Awards…", this, &MainWindow::onShowAwards);
    toolsMenu->addAction("&QSO Party…", this, &MainWindow::onShowQsoParty);
    m_actLotw          = toolsMenu->addAction("&LoTW Upload…", this, &MainWindow::onShowLotw);
    toolsMenu->addSeparator();
    m_actFindRadios    = toolsMenu->addAction("&Find radios…",   this, &MainWindow::onFindRadios);
    // NOT "Connect TCI". onConnectTci() routes to whichever source is
    // configured, so naming it after one protocol left an operator using
    // Hamlib with no menu item that looked like it applied to them. The
    // text is refreshed with the link label. #14.
    m_actConnectTci    = toolsMenu->addAction("&Connect radio",    this, &MainWindow::onConnectTci);
    m_actDisconnectTci = toolsMenu->addAction("&Disconnect radio", this, &MainWindow::onDisconnectTci);
    toolsMenu->addSeparator();
    m_actAprsActivity  = toolsMenu->addAction("&APRS Activity…", this, &MainWindow::onShowAprsActivity);
    m_actDxcLog        = toolsMenu->addAction("DX Cluster &Log…", this, &MainWindow::onShowClusterLog);
    m_actSpotIndex     = toolsMenu->addAction("Show Spot &Index…", this, &MainWindow::onShowSpotIndex);

    auto* mapsMenu = menuBar()->addMenu("&Maps");
    m_actSectionMap = mapsMenu->addAction("&Section Map (ARRL/RAC)…",
                                          this, &MainWindow::onShowSectionMap);
    mapsMenu->addAction("&Grid Map (Maidenhead)…",
                        this, &MainWindow::onShowGridMap);
    mapsMenu->addAction("Se&ssion Map…", this, &MainWindow::onShowSessionMap);
    m_actHowFarMap  = mapsMenu->addAction("&How far? (PSK Reporter)…",
                                          this, &MainWindow::onHowFar);

    auto* helpMenu = menuBar()->addMenu("&Help");
    m_actAbout = helpMenu->addAction("&About ShackBook", this, &MainWindow::onAbout);
}

void MainWindow::onShowSectionMap()
{
    if (!m_model) return;
    if (!m_sectionMap) {
        m_sectionMap = new SectionMapDialog(m_model, this);
        m_sectionMap->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_sectionMap, &QObject::destroyed, this,
                [this]() { m_sectionMap = nullptr; });
    }
    m_sectionMap->show();
    m_sectionMap->raise();
    m_sectionMap->activateWindow();
}

void MainWindow::onShowGridMap()
{
    if (!m_model) return;
    if (!m_gridMap) {
        m_gridMap = new GridMapDialog(m_model, this);
        m_gridMap->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_gridMap, &QObject::destroyed, this,
                [this]() { m_gridMap = nullptr; });
        // Click-to-filter: the square lands in the ordinary filter box —
        // visible, editable, and cleared the same way as any hand-typed
        // filter (the text search already covers gridsquare).
        connect(m_gridMap, &GridMapDialog::filterRequested,
                this, [this](const QString& square) {
                    if (m_filterText) m_filterText->setText(square);
                });
    }
    m_gridMap->show();
    m_gridMap->raise();
    m_gridMap->activateWindow();
}

void MainWindow::onShowSessionMap()
{
    if (!m_model) return;
    if (!m_sessionMap) {
        m_sessionMap = new SessionMapDialog(m_model, this);
        m_sessionMap->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_sessionMap, &QObject::destroyed, this,
                [this]() { m_sessionMap = nullptr; });
    }
    // Re-derive on every open: sessions are computed from the log, so a QSO
    // logged since the dialog was last shown belongs in the current session.
    m_sessionMap->refresh();
    m_sessionMap->show();
    m_sessionMap->raise();
    m_sessionMap->activateWindow();
}

void MainWindow::onShowAprsActivity()
{
    if (!m_model) return;
    if (!m_aprsActivity) {
        m_aprsActivity = new AprsActivityDialog(m_model, this);
        m_aprsActivity->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_aprsActivity, &QObject::destroyed, this,
                [this]() { m_aprsActivity = nullptr; });
    }
    m_aprsActivity->show();
    m_aprsActivity->raise();
    m_aprsActivity->activateWindow();
}

// ── Layout ─────────────────────────────────────────────────────────────

void MainWindow::buildUI()
{
    auto* central = new QWidget;
    setCentralWidget(central);

    auto* main = new QVBoxLayout(central);
    main->setContentsMargins(8, 6, 8, 6);
    main->setSpacing(6);

    // ── Header strip ────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(14);

        m_myCallLabel = new QLabel("(no call)");
        m_myCallLabel->setStyleSheet(kHeaderCallStyle);

        // Fixed minimum widths so labels don't resize as content changes.
        // Without these the freq label grows by ~10 px when going from
        // "14.250 MHz" to "144.250 MHz" and the whole row shifts left/right.
        // Right-aligned within their fixed box for numeric stability.
        auto* freqBlock = new QVBoxLayout;
        freqBlock->setSpacing(0);
        auto* fL = new QLabel("FREQ"); fL->setStyleSheet(kHeaderLabelStyle);
        m_freqLabel = new QLabel("—");
        m_freqLabel->setStyleSheet(kHeaderValueStyle);
        m_freqLabel->setMinimumWidth(120);
        m_freqLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        freqBlock->addWidget(fL); freqBlock->addWidget(m_freqLabel);

        auto* bandBlock = new QVBoxLayout;
        bandBlock->setSpacing(0);
        auto* bL = new QLabel("BAND"); bL->setStyleSheet(kHeaderLabelStyle);
        m_bandLabel = new QLabel("—");
        m_bandLabel->setStyleSheet(kHeaderValueStyle);
        m_bandLabel->setMinimumWidth(60);
        bandBlock->addWidget(bL); bandBlock->addWidget(m_bandLabel);

        auto* modeBlock = new QVBoxLayout;
        modeBlock->setSpacing(0);
        auto* mL = new QLabel("MODE"); mL->setStyleSheet(kHeaderLabelStyle);
        m_modeLabel = new QLabel("—");
        m_modeLabel->setStyleSheet(kHeaderValueStyle);
        m_modeLabel->setMinimumWidth(90);
        modeBlock->addWidget(mL); modeBlock->addWidget(m_modeLabel);

        m_tciDot    = new QLabel("●");
        m_tciDot->setStyleSheet(kTciDotDisconnected);
        // Text is set by refreshLinkLabel() once the configured source is
        // known -- a literal here cannot be right, because it has to name
        // TCI or CAT depending on a setting. #14.
        m_tciStatus = new QLabel;
        m_tciStatus->setStyleSheet(kHeaderLabelStyle);
        // "TCI offline" is wider than "TCI live" — pin the width so the
        // dot doesn't slide left when we go online.
        m_tciStatus->setMinimumWidth(70);
        m_tciStatus->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        row->addWidget(m_myCallLabel);
        row->addSpacing(20);
        row->addLayout(freqBlock);
        row->addLayout(bandBlock);
        row->addLayout(modeBlock);
        row->addStretch();
        row->addWidget(m_tciDot);
        row->addWidget(m_tciStatus);
        main->addLayout(row);
    }

    // separator
    {
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet("color: #1c2a40;");
        main->addWidget(sep);
    }

    // ── Quick entry row ────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(8);

        auto* lc = new QLabel("CALL"); lc->setStyleSheet(kHeaderLabelStyle);
        m_callEdit = new QLineEdit;
        m_callEdit->setStyleSheet(kCallEditStyle);
        m_callEdit->setPlaceholderText("DX call");
        m_callEdit->setMinimumWidth(180);
        m_callEdit->setValidator(new QRegularExpressionValidator(
            QRegularExpression{"^[A-Za-z0-9/\\-]{0,16}$"}, this));
        connect(m_callEdit, &QLineEdit::textEdited, this, &MainWindow::onCallEdited);
        connect(m_callEdit, &QLineEdit::returnPressed, this, &MainWindow::onSaveQso);

        auto* lrs = new QLabel("RST→"); lrs->setStyleSheet(kHeaderLabelStyle);
        m_rstSentEdit = new QLineEdit("59");
        m_rstSentEdit->setStyleSheet(kEditStyle);
        m_rstSentEdit->setMaximumWidth(56);
        m_rstSentEdit->setMaxLength(4);

        auto* lrr = new QLabel("←RST"); lrr->setStyleSheet(kHeaderLabelStyle);
        m_rstRcvdEdit = new QLineEdit("59");
        m_rstRcvdEdit->setStyleSheet(kEditStyle);
        m_rstRcvdEdit->setMaximumWidth(56);
        m_rstRcvdEdit->setMaxLength(4);

        auto* lc2 = new QLabel("COMMENT"); lc2->setStyleSheet(kHeaderLabelStyle);
        m_commentEdit = new QLineEdit;
        m_commentEdit->setStyleSheet(kEditStyle);
        m_commentEdit->setPlaceholderText("comment / exchange");
        // If the operator types in the comment field, mark it as
        // user-owned so future auto-fills don't stomp on their text.
        connect(m_commentEdit, &QLineEdit::textEdited, this,
                [this](const QString& t) {
                    if (t.trimmed() != m_lastAutofilledComment)
                        m_lastAutofilledComment.clear();
                });

        m_dupBadge = new QLabel("DUPE");
        m_dupBadge->setStyleSheet(kDupBadgeIdle);

        m_saveBtn = new QPushButton("SAVE QSO  ↵");
        m_saveBtn->setStyleSheet(kSaveBtnStyle);
        m_saveBtn->setEnabled(false);
        connect(m_saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveQso);

        row->addWidget(lc);
        row->addWidget(m_callEdit);
        row->addSpacing(8);
        row->addWidget(lrs);  row->addWidget(m_rstSentEdit);
        row->addWidget(lrr);  row->addWidget(m_rstRcvdEdit);
        row->addSpacing(8);
        row->addWidget(lc2);
        row->addWidget(m_commentEdit, 1);
        row->addWidget(m_dupBadge);
        row->addWidget(m_saveBtn);
        main->addLayout(row);
    }

    // ── Contest sub-row (visibility tied to contestMode) ───────────────
    {
        m_contestFrame = new QFrame;
        m_contestFrame->setStyleSheet(
            "QFrame { background: #0d1e30; border: 1px solid #1c2a40; "
            "border-radius: 3px; }");
        auto* row = new QHBoxLayout(m_contestFrame);
        row->setContentsMargins(8, 4, 8, 4);
        row->setSpacing(6);

        m_contestIdLabel = new QLabel("(no contest)");
        m_contestIdLabel->setStyleSheet(
            "QLabel { color: #ffaa00; font-size: 11px; font-weight: bold; "
            "letter-spacing: 0.08em; border: none; }");

        auto* lstx = new QLabel("STX"); lstx->setStyleSheet("QLabel { border: none; color: #6b8099; font-size: 9px; font-weight: bold; }");
        m_stxEdit = new QLineEdit;
        m_stxEdit->setStyleSheet(kEditStyle);
        m_stxEdit->setMaximumWidth(60);
        m_stxEdit->setValidator(new QIntValidator(0, 999999, this));

        auto* lstxs = new QLabel("STX exch"); lstxs->setStyleSheet("QLabel { border: none; color: #6b8099; font-size: 9px; font-weight: bold; }");
        m_stxStringEdit = new QLineEdit;
        m_stxStringEdit->setStyleSheet(kEditStyle);
        m_stxStringEdit->setMaximumWidth(120);

        auto* lsrx = new QLabel("SRX"); lsrx->setStyleSheet("QLabel { border: none; color: #6b8099; font-size: 9px; font-weight: bold; }");
        m_srxEdit = new QLineEdit;
        m_srxEdit->setStyleSheet(kEditStyle);
        m_srxEdit->setMaximumWidth(60);
        m_srxEdit->setValidator(new QIntValidator(0, 999999, this));

        auto* lsrxs = new QLabel("SRX exch"); lsrxs->setStyleSheet("QLabel { border: none; color: #6b8099; font-size: 9px; font-weight: bold; }");
        m_srxStringEdit = new QLineEdit;
        m_srxStringEdit->setStyleSheet(kEditStyle);
        m_srxStringEdit->setMaximumWidth(120);

        row->addWidget(m_contestIdLabel);
        row->addSpacing(12);
        row->addWidget(lstx);  row->addWidget(m_stxEdit);
        row->addWidget(lstxs); row->addWidget(m_stxStringEdit);
        row->addSpacing(12);
        row->addWidget(lsrx);  row->addWidget(m_srxEdit);
        row->addWidget(lsrxs); row->addWidget(m_srxStringEdit);
        row->addStretch();

        m_contestFrame->hide();
        main->addWidget(m_contestFrame);
    }

    // separator
    {
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet("color: #1c2a40;");
        main->addWidget(sep);
    }

    // ── Filter row ─────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        m_filterText = new QLineEdit;
        m_filterText->setPlaceholderText("Filter by call / name / QTH / grid / comment");
        m_filterText->setStyleSheet(kEditStyle);
        connect(m_filterText, &QLineEdit::textChanged, this, &MainWindow::onFilterChanged);

        m_filterBand    = new QComboBox; m_filterBand->setMinimumWidth(80);
        m_filterMode    = new QComboBox; m_filterMode->setMinimumWidth(80);
        m_filterContest = new QComboBox; m_filterContest->setMinimumWidth(160);
        connect(m_filterBand,    &QComboBox::currentIndexChanged, this, &MainWindow::onFilterChanged);
        connect(m_filterMode,    &QComboBox::currentIndexChanged, this, &MainWindow::onFilterChanged);
        connect(m_filterContest, &QComboBox::currentIndexChanged, this, &MainWindow::onFilterChanged);

        m_resetBtn = new QPushButton("Reset");
        connect(m_resetBtn, &QPushButton::clicked, this, &MainWindow::onResetFilter);

        row->addWidget(m_filterText, 1);
        row->addWidget(m_filterBand);
        row->addWidget(m_filterMode);
        row->addWidget(m_filterContest);
        row->addWidget(m_resetBtn);
        main->addLayout(row);
    }

    // ── Table ──────────────────────────────────────────────────────────
    {
        m_table = new QTableWidget;
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setAlternatingRowColors(true);
        m_table->verticalHeader()->setVisible(false);
        m_table->setColumnCount(13);
        m_table->setHorizontalHeaderLabels({
            "Date", "Time", "Call", "Band", "Mode", "Freq",
            "RST→", "←RST", "Name", "QTH", "Grid", "Contest", "Comment"
        });
        m_table->horizontalHeader()->setStretchLastSection(true);
        connect(m_table, &QTableWidget::doubleClicked, this, &MainWindow::onEditQso);
        main->addWidget(m_table, 1);
    }

    // ── Action row ─────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        m_countLabel = new QLabel("0 QSOs");
        m_howFarBtn = new QPushButton("How far?");
        m_howFarBtn->setToolTip(
            "Open the PSK Reporter map filtered to stations that heard YOUR "
            "signal in the last 15 minutes, on the current band/mode.");
        m_newBtn    = new QPushButton("New QSO…");
        m_editBtn   = new QPushButton("Edit…");
        m_deleteBtn = new QPushButton("Delete");
        connect(m_howFarBtn, &QPushButton::clicked, this, &MainWindow::onHowFar);
        connect(m_newBtn,    &QPushButton::clicked, this, &MainWindow::onNewQso);
        connect(m_editBtn,   &QPushButton::clicked, this, &MainWindow::onEditQso);
        connect(m_deleteBtn, &QPushButton::clicked, this, &MainWindow::onDeleteQso);
        row->addWidget(m_countLabel);
        row->addWidget(m_howFarBtn);
        row->addStretch();
        row->addWidget(m_newBtn);
        row->addWidget(m_editBtn);
        row->addWidget(m_deleteBtn);
        main->addLayout(row);
    }

    // ── Status bar ─────────────────────────────────────────────────────
    m_sbTci = new QLabel("TCI: not connected");
    m_sbDxc = new QLabel("DXC: off");
    m_sbDb  = new QLabel("DB: —");
    {
        auto* sbWsjtx = new QLabel(
            (m_wsjtx && m_wsjtx->isListening())
                ? QStringLiteral("WSJT-X: udp/%1").arg(m_wsjtx->port())
                : QStringLiteral("WSJT-X: off"));
        sbWsjtx->setToolTip("WSJT-X Secondary UDP Server target for direct "
                            "logging into the open log (Reporting tab → "
                            "127.0.0.1:1100).");
        statusBar()->addPermanentWidget(sbWsjtx);
    }
    statusBar()->addPermanentWidget(m_sbTci);
    statusBar()->addPermanentWidget(m_sbDxc);
    statusBar()->addPermanentWidget(m_sbDb);
}

// ── Header / quick-entry refresh ──────────────────────────────────────────

void MainWindow::refreshHeader()
{
    const QString call = m_model ? m_model->myCall() : QString{};
    m_myCallLabel->setText(call.isEmpty() ? "(no call)" : call);

    m_freqLabel->setText(m_curFreqMhz > 0.0
                         ? QString::number(m_curFreqMhz, 'f', 3) + " MHz"
                         : "—");
    m_bandLabel->setText(m_curBand.isEmpty() ? "—" : m_curBand);
    QString modeText = m_curMode.isEmpty() ? "—" : m_curMode;
    if (!m_curSubmode.isEmpty()) modeText += "/" + m_curSubmode;
    m_modeLabel->setText(modeText);
}

void MainWindow::refreshQuickEntry()
{
    refreshDupBadge();
    m_saveBtn->setEnabled(!m_callEdit->text().trimmed().isEmpty());
}

void MainWindow::refreshContestUI()
{
    if (!m_model) return;
    const bool on = m_model->contestMode();
    m_contestFrame->setVisible(on);
    if (on) {
        const QString id = m_model->contestId();
        m_contestIdLabel->setText(id.isEmpty() ? "CONTEST" : id);
        const int next = m_model->settingValue("CONTEST_STX_NEXT", "1").toInt();
        m_stxEdit->setText(QString::number(next > 0 ? next : 1));
    }
    refreshHeader();
}

// ── Quick-entry handlers ──────────────────────────────────────────────────

void MainWindow::onCallEdited(const QString& text)
{
    const QString upper = text.toUpper();
    if (upper != text) {
        const int cursor = m_callEdit->cursorPosition();
        QSignalBlocker b{m_callEdit};
        m_callEdit->setText(upper);
        m_callEdit->setCursorPosition(cursor);
    }
    // The operator just edited the call manually — relinquish ownership
    // of the field so future spot-click auto-fill doesn't overwrite it.
    if (upper.trimmed() != m_lastAutofilledCall) {
        m_lastAutofilledCall.clear();
    }
    m_saveBtn->setEnabled(!upper.trimmed().isEmpty());
    refreshDupBadge();
    // Lookup is debounced so we query once per call, not per keystroke.
    if (m_lookupDebounce) m_lookupDebounce->start();
}

void MainWindow::refreshDupBadge()
{
    if (!m_model) return;
    const QString call = m_callEdit->text().trimmed().toUpper();
    if (call.isEmpty() || m_curBand.isEmpty() || m_curMode.isEmpty()) {
        m_dupBadge->setStyleSheet(kDupBadgeIdle);
        return;
    }
    if (m_model->isDuplicate(call, m_curBand, m_curMode)) {
        m_dupBadge->setStyleSheet(kDupBadgeHit);
    } else {
        m_dupBadge->setStyleSheet(kDupBadgeIdle);
    }
}

void MainWindow::onSaveQso()
{
    if (!m_model) return;
    const QString call = m_callEdit->text().trimmed().toUpper();
    if (call.isEmpty()) return;

    Qso q;
    q.call = call;

    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    q.qsoDate = nowUtc.toString("yyyyMMdd");
    q.timeOn  = nowUtc.toString("HHmmss");
    q.band    = m_curBand;
    q.freq    = m_curFreqMhz;
    q.mode    = m_curMode;
    q.submode = m_curSubmode;
    q.rstSent = m_rstSentEdit->text().trimmed();
    q.rstRcvd = m_rstRcvdEdit->text().trimmed();
    q.comment = m_commentEdit->text().trimmed();
    q.myCall       = m_model->myCall();
    q.myGridsquare = m_model->myGridsquare();
    q.myState      = m_model->myState();
    q.txPwr        = txPowerForNewQso();
    q.station      = currentRadioName();   // blank unless TCI is connected

    if (m_model->contestMode()) {
        q.contestId = m_model->contestId();
        q.stx        = m_stxEdit->text().toInt();
        q.srx        = m_srxEdit->text().toInt();
        q.stxString  = m_stxStringEdit->text().trimmed();
        q.srxString  = m_srxStringEdit->text().trimmed();
    }

    // Merge pending lookup results (worked-before / cty.dat / online) into
    // fields the quick-entry row has no widgets for.  Guarded by call match
    // so a late result for a previous call can never leak into this QSO.
    if (m_lookupFillCall == call) {
        q.name       = m_lookupFill.name;
        q.qth        = m_lookupFill.qth;
        q.gridsquare = m_lookupFill.gridsquare;
        q.state      = m_lookupFill.state;
        q.cnty       = m_lookupFill.cnty;
        q.country    = m_lookupFill.country;
        q.cont       = m_lookupFill.cont;
        q.dxcc       = m_lookupFill.dxcc;
        q.cqz        = m_lookupFill.cqz;
        q.ituz       = m_lookupFill.ituz;
    }

    if (!m_model->insertQso(q)) {
        QMessageBox::critical(this, "Save failed", m_model->errorString());
        return;
    }

    m_callEdit->clear();
    m_commentEdit->clear();
    m_srxEdit->clear();
    m_srxStringEdit->clear();
    // Slate is clean post-save — drop any "we own this field" markers
    // so the next freq change will autofill afresh.
    m_lastAutofilledCall.clear();
    m_lastAutofilledComment.clear();
    m_lookupFill = Qso{};
    m_lookupFillCall.clear();
    if (m_model->contestMode()) {
        const int next = m_stxEdit->text().toInt() + 1;
        m_stxEdit->setText(QString::number(next));
        m_model->setSetting("CONTEST_STX_NEXT", QString::number(next));
    }
    m_saveBtn->setEnabled(false);
    refreshDupBadge();
    m_callEdit->setFocus();
}

// ── Filter / table ───────────────────────────────────────────────────────

void MainWindow::onFilterChanged() { refreshTable(); }

void MainWindow::onResetFilter()
{
    if (m_filterText)    m_filterText->clear();
    if (m_filterBand)    m_filterBand->setCurrentIndex(0);
    if (m_filterMode)    m_filterMode->setCurrentIndex(0);
    if (m_filterContest) m_filterContest->setCurrentIndex(0);
    refreshTable();
}

void MainWindow::onLogbookChanged()
{
    refreshTable();
    populateBandFilter();
    populateModeFilter();
    populateContestFilter();
    refreshDupBadge();
}

void MainWindow::onContextSettingsChanged()
{
    refreshHeader();
    refreshContestUI();
    refreshStatusBar();
}

void MainWindow::populateBandFilter()
{
    if (!m_model) return;
    QSet<QString> bands;
    for (const auto& q : m_model->queryQsos()) if (!q.band.isEmpty()) bands.insert(q.band);
    QSignalBlocker b{m_filterBand};
    m_filterBand->clear();
    m_filterBand->addItem("(any band)", "");
    QStringList sorted(bands.begin(), bands.end());
    sorted.sort();
    for (const auto& v : sorted) m_filterBand->addItem(v, v);
}

void MainWindow::populateModeFilter()
{
    if (!m_model) return;
    QSet<QString> modes;
    for (const auto& q : m_model->queryQsos()) if (!q.mode.isEmpty()) modes.insert(q.mode);
    QSignalBlocker b{m_filterMode};
    m_filterMode->clear();
    m_filterMode->addItem("(any mode)", "");
    QStringList sorted(modes.begin(), modes.end());
    sorted.sort();
    for (const auto& v : sorted) m_filterMode->addItem(v, v);
}

void MainWindow::populateContestFilter()
{
    if (!m_model) return;
    QSet<QString> contests;
    for (const auto& q : m_model->queryQsos()) if (!q.contestId.isEmpty()) contests.insert(q.contestId);
    QSignalBlocker b{m_filterContest};
    m_filterContest->clear();
    m_filterContest->addItem("(any)", "");
    m_filterContest->addItem("non-contest only", "<NONE>");
    QStringList sorted(contests.begin(), contests.end());
    sorted.sort();
    for (const auto& v : sorted) m_filterContest->addItem(v, v);
}

void MainWindow::refreshTable()
{
    if (!m_model || !m_table) return;
    LogbookFilter f;
    f.text      = m_filterText    ? m_filterText->text().trimmed() : QString{};
    f.band      = m_filterBand    ? m_filterBand->currentData().toString()    : QString{};
    f.mode      = m_filterMode    ? m_filterMode->currentData().toString()    : QString{};
    f.contestId = m_filterContest ? m_filterContest->currentData().toString() : QString{};

    const QVector<Qso> rows = m_model->queryQsos(f);
    m_table->setRowCount(rows.size());

    auto fmtDate = [](const QString& d) -> QString {
        if (d.size() == 8) return d.left(4) + "-" + d.mid(4, 2) + "-" + d.mid(6, 2);
        return d;
    };
    auto fmtTime = [](const QString& t) -> QString {
        if (t.size() >= 4) return t.left(2) + ":" + t.mid(2, 2);
        return t;
    };

    for (int i = 0; i < rows.size(); ++i) {
        const Qso& q = rows[i];
        auto setItem = [&](int col, const QString& text) {
            auto* it = new QTableWidgetItem(text);
            it->setData(Qt::UserRole, QVariant::fromValue<qint64>(q.id));
            m_table->setItem(i, col, it);
        };
        setItem(0,  fmtDate(q.qsoDate));
        setItem(1,  fmtTime(q.timeOn));
        setItem(2,  q.call);
        setItem(3,  q.band);
        setItem(4,  q.submode.isEmpty() ? q.mode : q.mode + "/" + q.submode);
        setItem(5,  q.freq > 0.0 ? QString::number(q.freq, 'f', 3) : QString{});
        setItem(6,  q.rstSent);
        setItem(7,  q.rstRcvd);
        setItem(8,  q.name);
        setItem(9,  q.qth);
        setItem(10, q.gridsquare);
        setItem(11, q.contestId);
        setItem(12, q.comment);
    }
    m_table->resizeColumnsToContents();
    m_countLabel->setText(QString("%1 QSO%2")
                              .arg(rows.size())
                              .arg(rows.size() == 1 ? "" : "s"));
}

qint64 MainWindow::selectedQsoId() const
{
    if (!m_table) return -1;
    const auto sel = m_table->selectedItems();
    if (sel.isEmpty()) return -1;
    auto* it = m_table->item(sel.first()->row(), 0);
    return it ? it->data(Qt::UserRole).toLongLong() : -1;
}

// ── Action slots ──────────────────────────────────────────────────────────

void MainWindow::onNewQso()
{
    if (!m_model) return;
    Qso q;
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    q.qsoDate = nowUtc.toString("yyyyMMdd");
    q.timeOn  = nowUtc.toString("HHmmss");
    q.band    = m_curBand;
    q.freq    = m_curFreqMhz;
    q.mode    = m_curMode;
    q.submode = m_curSubmode;
    q.rstSent = "59";
    q.rstRcvd = "59";
    q.myCall       = m_model->myCall();
    q.myGridsquare = m_model->myGridsquare();
    q.myState      = m_model->myState();
    q.txPwr        = txPowerForNewQso();   // prefilled; the dialog can change it
    q.station      = currentRadioName();   // blank unless TCI is connected
    EditDialog dlg(m_model, q, this);
    dlg.exec();
}

void MainWindow::onEditQso()
{
    const qint64 id = selectedQsoId();
    if (id < 0) return;
    bool ok = false;
    Qso q = m_model->getQso(id, &ok);
    if (!ok) return;
    EditDialog dlg(m_model, q, this);
    dlg.exec();
}

void MainWindow::onDeleteQso()
{
    const qint64 id = selectedQsoId();
    if (id < 0) return;
    bool ok = false;
    Qso q = m_model->getQso(id, &ok);
    if (!ok) return;
    const auto reply = QMessageBox::question(
        this, "Delete QSO",
        QString("Delete QSO with %1 on %2 %3?").arg(q.call, q.qsoDate, q.timeOn));
    if (reply != QMessageBox::Yes) return;
    if (!m_model->deleteQso(id)) {
        QMessageBox::critical(this, "Delete failed", m_model->errorString());
    }
}

// ONE place that decides what the link is called, so the header and the
// status bar cannot disagree.
//
// This used to live inside onTciConnectionChanged(), which only fires on a
// connection CHANGE -- so before the first attempt the label kept its
// construction-time literal "TCI offline", even when the configured source
// was rigctld and TCI was not in use at all. The operator saw a link named
// after the protocol they were not using, which sends them to the wrong
// settings page when something is not working. #14.
void MainWindow::refreshLinkLabel()
{
    if (!m_tciStatus || !m_tciDot) return;
    // Ask the LIVE clients rather than tracking a flag: this is called from
    // startup and from a settings change as well as from the connection
    // signal, and only two of those three carry a connected state.
    const bool rig = usingRigctld();
    const bool connected = rig ? (m_rigctld && m_rigctld->connected())
                               : (m_tci && m_tci->connected());
    const QString what = rig ? QStringLiteral("CAT") : QStringLiteral("TCI");
    m_tciStatus->setText(what + (connected ? QStringLiteral(" live")
                                           : QStringLiteral(" offline")));
    m_tciDot->setStyleSheet(connected ? kTciDotConnected : kTciDotDisconnected);
    // Same naming in the menu, from the same decision, so the two cannot
    // drift apart.
    if (m_actConnectTci)
        m_actConnectTci->setText(tr("&Connect radio (%1)").arg(what));
    if (m_actDisconnectTci)
        m_actDisconnectTci->setText(tr("&Disconnect radio (%1)").arg(what));
}

void MainWindow::onSettings()
{
    if (!m_model) return;
    SettingsDialog dlg(m_model, this);
    if (dlg.exec() == QDialog::Accepted) {
        // If TCI host/port changed and we're connected, reconnect to pick
        // up the new endpoint.
        // Reconnect if either link was live — the operator may have changed
        // host/port, or switched between TCI and rigctld entirely.
        if (m_tci->connected() || m_rigctld->connected()) {
            onDisconnectTci();          // stops both
            applyAutoConnectFromSettings();
        }
        // Cluster config may have changed too — reapply unconditionally
        // (no-op if nothing changed; enables/disables/reconnects otherwise).
        applyClusterConfigFromSettings();
        applyPotaConfigFromSettings();
        applyLookupConfigFromSettings();
        applyMqttConfigFromSettings();
        applyCwKeyerFromSettings();
        refreshStatusBar();
        // The source may have changed from TCI to rigctld or back.
        refreshLinkLabel();
    }
}

bool MainWindow::chooseAndOpenLog(bool startup)
{
    QSettings settings;
    const QString last = settings.value("lastOperator").toString().toUpper();

    QStringList calls = existingLogCalls();
    calls.removeAll(last);
    if (!last.isEmpty()) calls.prepend(last);    // last-used = default choice

    // At startup the main window isn't on screen yet, so a dialog parented
    // to it has no visible parent and Windows opens it BEHIND whatever has
    // focus. That has hidden the launch three times now — most recently
    // leaving the user driving a stale installed copy without noticing.
    // Stay-on-top plus an explicit raise makes it impossible to miss.
    QInputDialog dlg(this);
    dlg.setWindowTitle("ShackBook — Operator");
    dlg.setLabelText("Operator callsign (pick an existing log, or type a\n"
                     "new callsign to start a fresh one):");
    dlg.setComboBoxItems(calls);
    dlg.setComboBoxEditable(true);
    dlg.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    dlg.show();
    dlg.raise();
    dlg.activateWindow();
    const bool ok = dlg.exec() == QDialog::Accepted;
    QString call = ok ? dlg.textValue().trimmed().toUpper() : QString();

    if (!ok || call.isEmpty()) {
        if (!startup) return false;              // live switch cancelled: keep current
        call = last;
        if (call.isEmpty())
            return m_model->open();              // first run, cancelled: legacy behaviour
    }

    const QString path = callToLogFile(call);

    // Adopt the pre-multi-log single logbook for the first operator.
    const QString legacy = logsDirPath() + "/shackbook.sqlite";
    if (!QFile::exists(path) && QFile::exists(legacy)
        && existingLogCalls().isEmpty()) {
        const auto ans = QMessageBox::question(
            this, "Adopt existing logbook?",
            QString("An existing logbook from before multi-log support was "
                    "found.\n\nAdopt it as %1's log?\n\n(No starts %1 with an "
                    "empty log; the old file is kept untouched.)").arg(call));
        if (ans == QMessageBox::Yes) {
            if (m_model->isOpen()) m_model->close();
            QFile::rename(legacy, path);
        }
    }

    if (!m_model->open(path)) {
        QMessageBox::critical(this, "ShackBook",
                              QString("Could not open logbook for %1:\n%2")
                                  .arg(call, m_model->errorString()));
        return false;
    }
    if (m_model->myCall().isEmpty())
        m_model->setSetting("MY_CALL", call);    // seed a fresh log's identity
    m_operatorCall = call;
    settings.setValue("lastOperator", call);
    setWindowTitle(windowTitleFor(call));

    // Radio settings live IN the log, so switching logs can change the radio —
    // including which kind. Reconnect against the newly-opened log rather than
    // leaving the previous log's link (or no link) in place. This was invisible
    // while every log used the same TCI endpoint; it stopped being true once a
    // log could choose rigctld instead.
    m_warnedNoHamlib = false;          // a new log deserves the warning afresh
    onDisconnectTci();                 // stops whichever source was live
    applyAutoConnectFromSettings();    // reconnects per the new log's settings

    // Integrity/restore events must reach the operator, not just the log:
    // a silently-restored logbook is how a week of QSOs goes missing twice.
    if (!m_model->openNotice().isEmpty()) {
        QMessageBox::warning(this, "ShackBook — logbook integrity",
                             m_model->openNotice());
    }
    return true;
}

void MainWindow::onHowFar()
{
    // "Who has heard me?" — open PSK Reporter's map filtered to signals
    // SENT BY our callsign over the last 15 minutes, narrowed to the
    // current band (and mode when TCI gave us an unambiguous one —
    // DIGU-class slots leave it empty, and the callsign filter dominates
    // anyway).
    QString call = m_model ? m_model->myCall() : QString{};
    if (call.isEmpty()) call = m_operatorCall;
    if (call.isEmpty()) {
        QMessageBox::information(this, "How far?",
            "Set your callsign first (Settings → Operator).");
        return;
    }

    // PSK Reporter's band parameter is a frequency range in Hz
    // (verified from a working share link: band=12000000-16000000).
    static const QHash<QString, QString> kPskBand = {
        {"160m", "1800000-2000000"},     {"80m", "3500000-4000000"},
        {"60m",  "5250000-5450000"},     {"40m", "7000000-7300000"},
        {"30m",  "10100000-10150000"},   {"20m", "14000000-14350000"},
        {"17m",  "18068000-18168000"},   {"15m", "21000000-21450000"},
        {"12m",  "24890000-24990000"},   {"10m", "28000000-29700000"},
        {"6m",   "50000000-54000000"},   {"4m",  "70000000-70500000"},
        {"2m",   "144000000-148000000"}, {"70cm", "420000000-450000000"}};

    QString url = QStringLiteral(
        "https://pskreporter.info/pskmap.html?preset&callsign=%1"
        "&txrx=tx&timerange=900")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(call)));
    const QString pskBand = kPskBand.value(m_curBand);
    if (!pskBand.isEmpty())     url += QStringLiteral("&band=") + pskBand;
    if (!m_curMode.isEmpty())   url += QStringLiteral("&mode=") + m_curMode;

    QDesktopServices::openUrl(QUrl(url));
}

void MainWindow::onSwitchLog()
{
    if (!chooseAndOpenLog(/*startup*/ false)) return;
    refreshTable();
    refreshStatusBar();
    applyMqttConfigFromSettings();   // a different log may have a different callsign
    applyCwKeyerFromSettings();      // and its own keyer enable and messages
}

void MainWindow::onShowAwards()
{
    if (!m_model || !m_model->isOpen()) return;
    AwardsDialog dlg(m_model, m_operatorCall.isEmpty()
                                  ? m_model->myCall() : m_operatorCall, this);
    dlg.exec();
}

void MainWindow::onShowQsoParty()
{
    if (!m_model || !m_model->isOpen()) return;
    // Modal like Awards: it is a chase view consulted between QSOs, and a
    // modeless one would need live refresh wiring for no clear gain yet.
    QsoPartyDialog dlg(m_model, this);
    dlg.exec();
}

void MainWindow::onShowLotw()
{
    if (!m_model || !m_model->isOpen()) return;
    LotwDialog dlg(m_model, this);
    dlg.exec();
    // Uploads mark lotw_sent in bulk and confirmations set lotw_rcvd row by
    // row — one repaint at close covers both.
    refreshTable();
}

void MainWindow::onImportAdif()
{
    if (!m_model) return;
    const QString path = QFileDialog::getOpenFileName(
        this, "Import ADIF", QString(),
        "ADIF files (*.adi *.adif);;All files (*)");
    if (path.isEmpty()) return;

    LogbookModel::AdifImportResult r;
    {
        // One table refresh at the end — not one per imported QSO.
        const QSignalBlocker block(m_model);
        r = m_model->importAdif(path);
    }
    refreshTable();
    refreshStatusBar();

    if (!r.ok) {
        QMessageBox::critical(this, "Import failed", m_model->errorString());
        return;
    }
    QMessageBox::information(this, "Import complete",
        QString("Imported %1 QSO%2.\nSkipped %3 duplicate%4 and %5 invalid record%6.")
            .arg(r.imported).arg(r.imported == 1 ? "" : "s")
            .arg(r.duplicates).arg(r.duplicates == 1 ? "" : "s")
            .arg(r.invalid).arg(r.invalid == 1 ? "" : "s"));
}

void MainWindow::onExportAdif()
{
    if (!m_model) return;
    const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmm");
    const QString defaultName = QString("shackbook-%1.adi").arg(stamp);
    const QString path = askSavePath(this, "Export ADIF",
                                     "ADIF files (*.adi);;All files (*)",
                                     defaultName);
    if (path.isEmpty()) return;

    LogbookFilter f;
    f.text      = m_filterText    ? m_filterText->text().trimmed() : QString{};
    f.band      = m_filterBand    ? m_filterBand->currentData().toString()    : QString{};
    f.mode      = m_filterMode    ? m_filterMode->currentData().toString()    : QString{};
    f.contestId = m_filterContest ? m_filterContest->currentData().toString() : QString{};

    const int n = m_model->exportAdif(path, f);
    if (n < 0)
        QMessageBox::critical(this, "Export failed", m_model->errorString());
    else
        QMessageBox::information(this, "Export complete",
                                 QString("Wrote %1 QSO%2 to %3.")
                                     .arg(n).arg(n == 1 ? "" : "s").arg(path));
}

void MainWindow::onExportCabrillo()
{
    if (!m_model) return;
    QString contestId = m_filterContest ? m_filterContest->currentData().toString() : QString{};
    if (contestId.isEmpty() || contestId == "<NONE>") contestId = m_model->contestId();
    if (contestId.isEmpty()) {
        QMessageBox::warning(this, "Cabrillo export",
                             "Pick a contest in the filter dropdown or set CONTEST_ID "
                             "in Settings before exporting Cabrillo.");
        return;
    }
    const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmm");
    const QString defaultName = QString("%1-%2.cbr").arg(contestId.toLower()).arg(stamp);
    const QString path = askSavePath(this, "Export Cabrillo",
                                     "Cabrillo files (*.cbr *.log);;All files (*)",
                                     defaultName);
    if (path.isEmpty()) return;
    const int n = m_model->exportCabrillo(path, contestId);
    if (n < 0)
        QMessageBox::critical(this, "Export failed", m_model->errorString());
    else
        QMessageBox::information(this, "Export complete",
                                 QString("Wrote %1 QSO%2 to %3.")
                                     .arg(n).arg(n == 1 ? "" : "s").arg(path));
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, "About ShackBook",
        QString("<h3>ShackBook %1</h3>").arg(qApp->applicationVersion()) +
        "<p>Standalone ham radio logbook with TCI integration.</p>"
        "<p>Live freq / mode from any TCI server (AetherSDR, ExpertSDR2, "
        "SunSDR). ADIF and Cabrillo export.</p>"
        "<p>73 de G0JKN / W3.</p>");
}

// ── TCI ───────────────────────────────────────────────────────────────────

void MainWindow::applyAutoConnectFromSettings()
{
    if (!m_model) return;
    const bool autoConnect = m_model->settingValue("TCI_AUTOCONNECT", "1") == "1";
    if (!autoConnect) return;
    onConnectTci();
}

void MainWindow::onFindRadios()
{
    if (!m_model) return;

    // Sweep loopback plus wherever the operator is already pointed, so a
    // radio on another machine is found without asking for its address again.
    QStringList hosts{QStringLiteral("127.0.0.1")};
    const QString configured = m_model->settingValue("TCI_HOST", "127.0.0.1").trimmed();
    if (!configured.isEmpty()) hosts << configured;

    QProgressDialog progress(tr("Looking for radios…"), tr("Cancel"), 0, 1, this);
    progress.setWindowTitle(tr("Find radios"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);

    TciDiscovery discovery;
    QList<TciServerInfo> found;

    connect(&discovery, &TciDiscovery::progress, &progress,
            [&progress](int done, int total) {
                progress.setMaximum(total);
                progress.setValue(done);
            });

    QEventLoop loop;
    connect(&discovery, &TciDiscovery::finished, &loop,
            [&](const QList<TciServerInfo>& servers) {
                found = servers;
                loop.quit();
            });
    connect(&progress, &QProgressDialog::canceled, &discovery, &TciDiscovery::cancel);

    discovery.scan(hosts, TciDiscovery::defaultPorts());
    loop.exec();
    progress.close();

    if (found.isEmpty()) {
        QMessageBox::information(
            this, tr("Find radios"),
            tr("No TCI servers answered.\n\n"
               "A radio only appears here if its software is running and TCI is "
               "switched on — in AetherSDR that is Tools → TCI. Ports tried: "
               "40001, 40002, 50001, 50002 on this machine%1.")
                .arg(configured == QStringLiteral("127.0.0.1")
                         ? QString()
                         : tr(" and %1").arg(configured)));
        return;
    }

    QStringList labels;
    labels.reserve(found.size());
    for (const TciServerInfo& s : found) labels << s.label();

    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this, tr("Find radios"),
        tr("These answered a TCI handshake — pick one to use:"),
        labels, 0, /*editable*/ false, &ok);
    if (!ok || chosen.isEmpty()) return;

    const int idx = labels.indexOf(chosen);
    if (idx < 0) return;
    const TciServerInfo& pick = found.at(idx);

    m_model->setSetting("TCI_HOST", pick.host);
    m_model->setSetting("TCI_PORT", QString::number(pick.port));
    onConnectTci();
}

bool MainWindow::usingRigctld() const
{
    return m_model
           && m_model->settingValue(QStringLiteral("RADIO_SOURCE"),
                                    QStringLiteral("tci")) == QLatin1String("rigctld");
}

void MainWindow::connectActiveSource()
{
    if (!m_model) return;
    // Both branches can drop a live TCI link: switching to rigctld closes it,
    // and connectToServer() replaces it. Stop any CW first (#32).
    stopCwBeforeLinkGoes();

    // Only one link at a time. Two clients following one radio would poll the
    // same CAT port against each other — the "several programs fighting over
    // one port" failure the Field Day notes name explicitly.
    if (usingRigctld()) {
        m_tci->disconnectFromServer();
        const QString host = m_model->settingValue("RIGCTLD_HOST", "127.0.0.1");
        const quint16 port = static_cast<quint16>(
            m_model->settingValue("RIGCTLD_PORT", "4532").toUInt());

        // ⛔ Distinguish "Hamlib is not installed" from "it is installed but
        // nothing answered". Both end as no-connection, and only the first is
        // fixed by installing something — collapsing them into one message
        // sends the operator hunting the wrong problem. Only report the
        // missing case ONCE per log, or it nags on every reconnect attempt.
        const QString rigctld = RigctldClient::findRigctld(
            m_model->settingValue("HAMLIB_RIGCTLD_PATH"));
        if (rigctld.isEmpty() && !m_warnedNoHamlib) {
            m_warnedNoHamlib = true;
            statusBar()->showMessage(tr("Hamlib (rigctld) not found — see Settings → TCI"),
                                     8000);
        }

        // ADOPT IF SOMETHING IS ALREADY SERVING, otherwise offer to start one.
        // ShackBook used to refuse outright, on the grounds that grabbing the
        // serial port could take it from WSJT-X -- but the operator then ran
        // rigctld by hand anyway, so the port was taken regardless and all the
        // refusal bought was a terminal they had to keep open. #13.
        //
        // The safety that actually matters is never spawning a SECOND server
        // over someone else's, and never leaving behind one we started. Both
        // live in RigctldSupervisor.
        if (!RigctldSupervisor::serverAnswering(host, port)) {
            const QString model = m_model->settingValue("HAMLIB_RIG_MODEL");
            const QString serialPort = m_model->settingValue("HAMLIB_RIG_PORT");
            const QString baud = m_model->settingValue("HAMLIB_RIG_BAUD");
            if (rigctld.isEmpty() || model.isEmpty() || serialPort.isEmpty()) {
                // Nothing to offer -- say what is missing rather than
                // silently failing to connect.
                statusBar()->showMessage(
                    tr("No rigctld is running. Set the radio model and serial "
                       "port in Settings, then connect again."), 8000);
            } else {
                // ASK. Starting a process that seizes a serial port is not
                // something to do behind the operator's back.
                const auto answer = QMessageBox::question(
                    this, tr("Start rigctld?"),
                    tr("No rigctld is running on %1:%2.\n\n"
                       "Start one now for %3 on %4?\n\n"
                       "ShackBook will stop it again when it exits. An "
                       "already-running rigctld is never touched.")
                        .arg(host).arg(port).arg(model, serialPort),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (answer == QMessageBox::Yes) {
                    const auto st = m_rigctldSupervisor->ensureRunning(
                        rigctld, host, port, model, serialPort, baud);
                    if (st == RigctldSupervisor::State::Failed) {
                        // rigctld's own words: "port busy" and "no such
                        // device" need different fixes, and a generic
                        // failure message would hide which one this is.
                        QMessageBox::warning(this, tr("rigctld did not start"),
                                             m_rigctldSupervisor->lastError());
                    }
                }
            }
        }

        m_rigctld->connectToServer(host, port);
        statusBar()->showMessage(
            tr("Connecting to rigctld at %1:%2…").arg(host).arg(port), 3000);
    } else {
        m_rigctld->disconnectFromServer();
        const QString host = m_model->settingValue("TCI_HOST", "127.0.0.1");
        const quint16 port = static_cast<quint16>(
            m_model->settingValue("TCI_PORT", "40001").toUInt());
        m_tci->connectToServer(host, port);
        statusBar()->showMessage(
            tr("Connecting to %1:%2…").arg(host).arg(port), 3000);
    }
}

void MainWindow::onConnectTci()
{
    connectActiveSource();
}

void MainWindow::onDisconnectTci()
{
    // Stop both regardless of which is selected: if the setting changed while
    // one was live, the other would otherwise be left connected invisibly.
    // CW first: after the disconnect a stop has nowhere to go (#32).
    stopCwBeforeLinkGoes();
    m_tci->disconnectFromServer();
    m_rigctld->disconnectFromServer();
    // Stop a rigctld WE started -- otherwise disconnecting would leave it
    // holding the serial port with nothing using it. An adopted one is
    // left alone: it was serving somebody before us and still is.
    if (m_rigctldSupervisor)
        m_rigctldSupervisor->stopIfOurs();
}

void MainWindow::onTciConnectionChanged(bool connected)
{
    m_tciDot->setStyleSheet(connected ? kTciDotConnected : kTciDotDisconnected);
    // Name the link that is actually in use — "TCI offline" while following a
    // radio over CAT is simply wrong, and sends the operator to the wrong
    // settings when something is not working.
    refreshLinkLabel();
    refreshStatusBar();
    publishRadioStatus();
    if (!connected) {
        // Don't clear cached freq/mode — keep them so a brief disconnect
        // doesn't wipe the auto-fill mid-QSO.
    }
}

void MainWindow::onTciFrequencyChanged(double mhz)
{
    m_curFreqMhz = mhz;
    m_curBand    = LogbookModel::bandFromFreqMhz(mhz);
    refreshHeader();
    refreshDupBadge();
    tryAutofillFromSpot();
    publishRadioStatus();
}

void MainWindow::onTciModeChanged(const QString& mode)
{
    m_rawTciMode = mode;
    QString adifMode, adifSub;
    LogbookModel::adifModeFromTciMode(mode, &adifMode, &adifSub);
    m_curMode    = adifMode;
    m_curSubmode = adifSub;

    // Default RST per-mode: 599 for CW/RTTY, 59 otherwise.  Only flip if
    // the user hasn't customised the value.
    const QString defaultRst = (m_curMode == "CW" || m_curMode == "RTTY")
                                 ? "599" : "59";
    if (m_rstSentEdit->text() == "59" || m_rstSentEdit->text() == "599")
        m_rstSentEdit->setText(defaultRst);
    if (m_rstRcvdEdit->text() == "59" || m_rstRcvdEdit->text() == "599")
        m_rstRcvdEdit->setText(defaultRst);

    refreshHeader();
    refreshDupBadge();
    tryAutofillFromSpot();
    publishRadioStatus();
}

// ── Spot autofill (Phase 2) ──────────────────────────────────────────────

void MainWindow::tryAutofillFromSpot()
{
    if (!m_spotIndex || !m_callEdit) return;

    auto logLookup = [this](const QString& outcome) {
        if (!m_dxcLog) return;
        m_dxcLog->appendPlainText(
            QString("[%1] LOOKUP @ %2 MHz mode=%3 callField='%4' indexSize=%5 → %6")
                .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                .arg(m_curFreqMhz, 0, 'f', 4)
                .arg(m_curMode.isEmpty() ? "?" : m_curMode)
                .arg(m_callEdit->text().trimmed())
                .arg(m_spotIndex ? m_spotIndex->size() : 0)
                .arg(outcome));
    };

    if (m_curFreqMhz <= 0.0 || m_curMode.isEmpty()) {
        logLookup("skip: freq/mode unknown");
        return;
    }

    const QString currentCall = m_callEdit->text().trimmed();
    const bool fieldIsOurs    = currentCall.isEmpty() ||
                                currentCall == m_lastAutofilledCall;
    if (!fieldIsOurs) {
        // Operator typed something — never overwrite their input.
        logLookup("skip: call field user-edited");
        return;
    }

    auto hit = m_spotIndex->findAt(m_curFreqMhz, m_curMode);
    if (!hit) {
        // No matching spot.  Leave any previous auto-fill in place — the
        // operator may have done a small QSY around the same station.
        logLookup(currentCall.isEmpty()
                      ? QString("no spot in index")
                      : QString("no spot — keeping previous auto-fill %1").arg(currentCall));
        return;
    }

    // We have a match for the current bucket — replace whatever was
    // there (ours or empty) with the new spot.
    m_callEdit->setText(hit->call);
    m_lastAutofilledCall = hit->call;
    m_saveBtn->setEnabled(true);
    refreshDupBadge();
    startCallsignLookup(hit->call);

    // Comment field: replace if it's empty OR if it equals the comment
    // we put there from the previous auto-fill (so spot-to-spot hops
    // refresh the park ref / cluster note cleanly).
    if (m_commentEdit) {
        const QString currentComment = m_commentEdit->text().trimmed();
        const bool commentIsOurs = currentComment.isEmpty() ||
                                   currentComment == m_lastAutofilledComment;
        if (commentIsOurs) {
            m_commentEdit->setText(hit->comment);
            m_lastAutofilledComment = hit->comment;
        }
    }
    logLookup(QString("HIT %1 (%2)").arg(hit->call, hit->source));
    statusBar()->showMessage(
        QString("Auto-filled from %1 (%2)").arg(hit->source, hit->call), 3500);
}

void MainWindow::onClusterConnectionChanged(bool connected)
{
    if (m_dxcLog) {
        m_dxcLog->appendPlainText(QString("[%1] %2")
                                      .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                      .arg(connected ? "CONNECTED" : "DISCONNECTED"));
    }
    refreshStatusBar();
}

void MainWindow::onClusterSpotReceived(const SpotData& spot)
{
    if (!m_spotIndex) return;
    m_spotIndex->addOrUpdate(spot);
    if (m_dxcLog) {
        m_dxcLog->appendPlainText(QString("[%1] SPOT %2 @ %3 MHz  %4")
                                      .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                      .arg(spot.call)
                                      .arg(spot.freqMhz, 0, 'f', 4)
                                      .arg(spot.comment));
    }
    refreshStatusBar();
}

void MainWindow::onClusterRawLine(const QString& line)
{
    if (!m_dxcLog) return;
    m_dxcLog->appendPlainText(QString("[%1] %2")
                                  .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                  .arg(line));
}

void MainWindow::onShowSpotIndex()
{
    if (!m_spotIndex) return;

    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("Spot Index");
    dlg->resize(900, 520);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    auto* lay = new QVBoxLayout(dlg);
    auto* hint = new QLabel(QString("%1 spots currently held — sorted by frequency. "
                                    "Use this to verify a spot you expected to autofill "
                                    "is actually in the index.").arg(m_spotIndex->size()));
    hint->setWordWrap(true);
    hint->setStyleSheet("QLabel { color: #6b8099; font-size: 10px; }");
    lay->addWidget(hint);

    auto* table = new QTableWidget;
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setVisible(false);
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels({"Freq (MHz)", "Mode", "Call", "Source", "Comment"});
    table->horizontalHeader()->setStretchLastSection(true);

    const auto rows = m_spotIndex->snapshot();
    table->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const auto& s = rows[i];
        table->setItem(i, 0, new QTableWidgetItem(QString::number(s.freqMhz, 'f', 4)));
        table->setItem(i, 1, new QTableWidgetItem(s.mode));
        table->setItem(i, 2, new QTableWidgetItem(s.call));
        table->setItem(i, 3, new QTableWidgetItem(s.source));
        table->setItem(i, 4, new QTableWidgetItem(s.comment));
    }
    table->resizeColumnsToContents();
    table->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(table, 1);


    // Double-click tunes the radio to the spot (#9). OFF by default: this
    // MOVES somebody's transceiver, and an operator running split or sharing
    // the rig with another program should not discover that by accident.
    const bool tuneOnDouble =
        m_model && m_model->settingValue("SPOT_DOUBLECLICK_TUNES", "0") == "1";
    const bool alsoMode =
        m_model && m_model->settingValue("SPOT_DOUBLECLICK_SETS_MODE", "1") == "1";

    auto* status = new QLabel;
    status->setStyleSheet("QLabel { color: #6b8099; font-size: 10px; }");
    status->setText(tuneOnDouble
        ? QStringLiteral("Double-click a spot to tune the radio to it.")
        : QStringLiteral("Double-click tuning is off - enable it in Settings > TCI."));
    lay->addWidget(status);

    connect(table, &QTableWidget::doubleClicked, dlg,
            [this, rows, tuneOnDouble, alsoMode, status](const QModelIndex& ix) {
        if (!ix.isValid() || ix.row() < 0 || ix.row() >= rows.size()) return;
        const SpotData& s = rows[ix.row()];

        if (!tuneOnDouble) {
            status->setText(QStringLiteral(
                "Double-click tuning is off - enable it in Settings > TCI."));
            return;
        }
        // Say WHY nothing happened. A double-click with no visible effect
        // reads as a broken feature rather than a disconnected radio.
        if (!m_tci || !m_tci->connected()) {
            status->setText(QStringLiteral("No radio connected - cannot tune to %1.")
                                .arg(s.call));
            return;
        }
        if (!m_tci->tuneToMhz(s.freqMhz)) {
            status->setText(QStringLiteral("Could not tune to %1 MHz.")
                                .arg(s.freqMhz, 0, 'f', 4));
            return;
        }
        // Mode is best-effort: many spots carry none, and an unrecognised one
        // leaves the radio's current mode alone rather than guessing.
        QString note = QStringLiteral("Tuned to %1 - %2 MHz")
                           .arg(s.call).arg(s.freqMhz, 0, 'f', 4);
        if (alsoMode && !s.mode.trimmed().isEmpty()) {
            if (m_tci->setModeString(s.mode))
                note += QStringLiteral(" (%1)").arg(s.mode.toUpper());
            else
                note += QStringLiteral(" - mode not recognised, left unchanged");
        }
        status->setText(note);
    });

    auto* row = new QHBoxLayout;
    auto* closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    row->addStretch();
    row->addWidget(closeBtn);
    lay->addLayout(row);

    dlg->show();
}

void MainWindow::onShowClusterLog()
{
    if (!m_dxcLog) return;
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("DX Cluster Log");
    dlg->resize(900, 520);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    auto* lay = new QVBoxLayout(dlg);
    // Re-parent so the log lives in the dialog while it's open;
    // putting it back to nullptr-parent on close keeps the buffer
    // alive across opens (history persists).
    m_dxcLog->setParent(dlg);
    lay->addWidget(m_dxcLog, 1);

    auto* row = new QHBoxLayout;
    auto* clearBtn = new QPushButton("Clear");
    auto* closeBtn = new QPushButton("Close");
    connect(clearBtn, &QPushButton::clicked, m_dxcLog, &QPlainTextEdit::clear);
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    row->addStretch();
    row->addWidget(clearBtn);
    row->addWidget(closeBtn);
    lay->addLayout(row);

    connect(dlg, &QDialog::destroyed, this, [this]() {
        if (m_dxcLog) m_dxcLog->setParent(nullptr);
    });

    m_dxcLog->moveCursor(QTextCursor::End);
    dlg->show();
}

void MainWindow::purgeStaleSpots()
{
    if (m_spotIndex) m_spotIndex->purgeExpired();
    refreshStatusBar();
}

void MainWindow::applyClusterConfigFromSettings()
{
    if (!m_model || !m_dxc) return;

    // Default DXC_ENABLE to "1" if AetherSDR has a usable cluster config,
    // else "0" — first-run UX picks up the operator's existing setup
    // without prompting.
    const auto aether = AetherSettingsReader::readDxClusterConfig();
    const QString defEnable = aether.found ? "1" : "0";
    const bool enable = m_model->settingValue("DXC_ENABLE", defEnable) == "1";

    if (!enable) {
        m_dxc->disconnectFromCluster();
        refreshStatusBar();
        return;
    }

    const bool autoDetect = m_model->settingValue("DXC_AUTODETECT", "1") == "1";
    QString host;
    int port = 0;
    QString call;
    if (autoDetect && aether.found) {
        host = aether.host;
        port = aether.port;
        call = aether.callsign;
    } else {
        host = m_model->settingValue("DXC_HOST");
        port = m_model->settingValue("DXC_PORT").toInt();
        call = m_model->settingValue("DXC_CALLSIGN");
    }
    // Fall back to the operator's main MY_CALL setting if nothing else
    // was found — beats refusing to connect.
    if (call.isEmpty()) call = m_model->myCall();

    if (host.isEmpty() || port <= 0 || call.isEmpty()) {
        if (m_dxcLog)
            m_dxcLog->appendPlainText(
                QString("[apply] missing host/port/call (host=%1 port=%2 call=%3) — not connecting")
                    .arg(host).arg(port).arg(call));
        m_dxc->disconnectFromCluster();
        refreshStatusBar();
        return;
    }
    const QString suffix = m_model->settingValue("DXC_LOGIN_SUFFIX", "-2");
    if (m_dxcLog)
        m_dxcLog->appendPlainText(
            QString("[apply] connect %1:%2 as %3%4 (auto=%5)")
                .arg(host).arg(port).arg(call).arg(suffix)
                .arg(autoDetect ? "yes" : "no"));
    m_dxc->connectToCluster(host, static_cast<quint16>(port), call, suffix);
    refreshStatusBar();
}

void MainWindow::onPotaSpotReceived(const SpotData& spot)
{
    if (!m_spotIndex) return;
    m_spotIndex->addOrUpdate(spot);
    // Don't log every POTA spot individually — there are usually 200+
    // active and that swamps the cluster log.  onPotaPollCompleted gives
    // a per-poll summary instead.
    refreshStatusBar();
}

void MainWindow::onPotaPollCompleted(int spots, const QString& errorOrEmpty)
{
    if (m_dxcLog) {
        if (errorOrEmpty.isEmpty()) {
            m_dxcLog->appendPlainText(QString("[%1] POTA poll: %2 spots ingested")
                                          .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                          .arg(spots));
        } else {
            m_dxcLog->appendPlainText(QString("[%1] POTA poll FAILED: %2")
                                          .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                          .arg(errorOrEmpty));
        }
    }
    refreshStatusBar();
}

void MainWindow::applyPotaConfigFromSettings()
{
    if (!m_model || !m_pota) return;
    const bool enable = m_model->settingValue("POTA_ENABLE", "1") == "1";
    if (!enable) {
        m_pota->stop();
        if (m_dxcLog)
            m_dxcLog->appendPlainText("[apply] POTA polling disabled");
        return;
    }
    int sec = m_model->settingValue("POTA_POLL_SEC", "30").toInt();
    if (sec < 5) sec = 5;
    m_pota->start(sec);
    if (m_dxcLog)
        m_dxcLog->appendPlainText(QString("[apply] POTA polling started (every %1 s)").arg(sec));
}

// ── Callsign lookup chain ────────────────────────────────────────────────

void MainWindow::applyLookupConfigFromSettings()
{
    if (!m_model || !m_lookup) return;
    const QString prov = m_model->settingValue("LOOKUP_PROVIDER", "none");
    auto p = CallsignLookup::Provider::None;
    if (prov == "qrz")         p = CallsignLookup::Provider::Qrz;
    else if (prov == "hamqth") p = CallsignLookup::Provider::HamQth;
    m_lookup->configure(p,
                        m_model->settingValue("LOOKUP_USERNAME"),
                        m_model->settingValue("LOOKUP_PASSWORD"),
                        m_model->settingValue("LOOKUP_CALLOOK", "1") == "1");
}

void MainWindow::startCallsignLookup(const QString& callIn)
{
    const QString call = callIn.trimmed().toUpper();
    if (call == m_lookupFillCall) return;        // already resolved / in flight
    m_lookupFill = Qso{};
    m_lookupFillCall = call;
    if (call.isEmpty() || !m_model || !m_model->isOpen()) return;

    // Tier 1 — worked before: previous QSO details beat any online source.
    bool havePersonal = false;
    if (m_model->settingValue("LOOKUP_WORKEDBEFORE", "1") == "1") {
        const Qso prev = m_model->lastQsoWith(call);
        if (prev.id >= 0) {
            m_lookupFill.name       = prev.name;
            m_lookupFill.qth        = prev.qth;
            m_lookupFill.gridsquare = prev.gridsquare;
            m_lookupFill.state      = prev.state;
            m_lookupFill.cnty       = prev.cnty;
            m_lookupFill.country    = prev.country;
            m_lookupFill.cont       = prev.cont;
            m_lookupFill.dxcc       = prev.dxcc;
            m_lookupFill.cqz        = prev.cqz;
            m_lookupFill.ituz       = prev.ituz;
            havePersonal = !prev.name.isEmpty() || !prev.qth.isEmpty();
            if (havePersonal) {
                QStringList bits;
                if (!prev.name.isEmpty()) bits << prev.name;
                if (!prev.qth.isEmpty())  bits << prev.qth;
                statusBar()->showMessage(
                    QString("Worked before (%1 on %2): %3 — fills on save")
                        .arg(prev.band, prev.qsoDate, bits.join(", ")),
                    6000);
            }
        }
    }

    // Tier 2 — cty.dat: instant, offline country / continent / zones.
    bool isUs = false;
    if (m_cty.isLoaded()) {
        const auto e = m_cty.lookup(call);
        if (e.valid) {
            isUs = e.country == QLatin1String("United States");
            if (m_model->settingValue("LOOKUP_CTY", "1") == "1") {
                if (m_lookupFill.country.isEmpty()) m_lookupFill.country = e.country;
                if (m_lookupFill.cont.isEmpty())    m_lookupFill.cont    = e.cont;
                if (!m_lookupFill.cqz)              m_lookupFill.cqz     = e.cqz;
                if (!m_lookupFill.ituz)             m_lookupFill.ituz    = e.ituz;
            }
        }
    }

    // Tier 3 — online (QRZ / HamQTH / callook), only for the gaps tier 1
    // couldn't fill.  Async; the result lambda merges when it lands.
    if (!havePersonal) m_lookup->lookup(call, isUs);
}

// ── Window title ─────────────────────────────────────────────────────────

QString MainWindow::windowTitleFor(const QString& operatorCall)
{
    // Always carry the version, and mark a working build as such. Two copies
    // of ShackBook look identical on screen — installed vs freshly built — and
    // testing the wrong one wastes a whole session before anyone notices.
    QString t = QStringLiteral("ShackBook");
    if (!operatorCall.isEmpty()) t += QStringLiteral(" — %1").arg(operatorCall);
    t += QStringLiteral(" — %1").arg(versionString());
    return t;
}

// ── Radio identity ───────────────────────────────────────────────────────

QString MainWindow::currentRadioName() const
{
    if (!m_model) return {};

    if (usingRigctld()) {
        if (!m_rigctld || !m_rigctld->connected()) return {};
        // A nickname still wins — an operator with two IC-9700s needs to tell
        // them apart — but it is rarely needed here: `\get_info` names the
        // RADIO ("IC-9700"), not the application, so unlike TCI the announced
        // name is already the right answer.
        const QString host = m_model->settingValue("RIGCTLD_HOST", "127.0.0.1");
        const QString port = m_model->settingValue("RIGCTLD_PORT", "4532");
        const QString nick =
            m_model->settingValue(rigctldNicknameKey(host, port), {}).trimmed();
        if (!nick.isEmpty()) return nick;
        return m_rigctld->modelName().trimmed();
    }

    if (!m_tci || !m_tci->connected()) return {};

    // A nickname is stored per host:port, so two radios reached at different
    // endpoints keep separate names. It wins over `device:` because TCI's
    // device name is the APPLICATION — AetherSDR answers "AetherSDR" for a
    // Hermes-Lite 2 and a FLEX-6700 alike, so it cannot tell them apart.
    const QString host = m_model->settingValue("TCI_HOST", "127.0.0.1");
    const QString port = m_model->settingValue("TCI_PORT", "40001");
    const QString nick = m_model->settingValue(tciNicknameKey(host, port), {}).trimmed();
    if (!nick.isEmpty()) return nick;

    return m_tci->deviceName().trimmed();
}

void MainWindow::applyMqttConfigFromSettings()
{
    if (!m_model || !m_mqtt) return;
    const QString call = m_model->myCall();

    MqttConfig cfg;
    cfg.enabled  = m_model->settingValue("MQTT_ENABLE", "0") == "1";
    cfg.host     = m_model->settingValue("MQTT_HOST").trimmed();
    const int port = m_model->settingValue("MQTT_PORT", "1883").toInt();
    cfg.port     = static_cast<quint16>((port > 0 && port < 65536) ? port : 1883);
    cfg.username = m_model->settingValue("MQTT_USERNAME").trimmed();
    cfg.password = m_model->settingValue("MQTT_PASSWORD");
    QString prefix = m_model->settingValue("MQTT_TOPIC_PREFIX").trimmed();
    while (prefix.endsWith(QLatin1Char('/'))) prefix.chop(1);
    cfg.topicPrefix = prefix.isEmpty() ? ShackStatus::defaultTopicPrefix(call) : prefix;
    // Unique per machine AND log: a broker disconnects the older of two
    // clients sharing an id, so two ShackBooks would knock each other off.
    cfg.clientId = QStringLiteral("shackbook-%1-%2")
                       .arg(ShackStatus::nodeId(call),
                            ShackStatus::nodeId(QSysInfo::machineHostName()));
    m_mqtt->configure(cfg);

    if (!cfg.enabled) {
        m_mqttDayTimer->stop();
        return;
    }
    m_mqttDayTimer->start();

    const bool haDiscovery = m_model->settingValue("MQTT_HA_DISCOVERY", "1") == "1";
    const bool qsoDetails  = m_model->settingValue("MQTT_PUBLISH_QSO", "0") == "1";
    for (const auto& msg : ShackStatus::discoveryMessages(cfg.topicPrefix, call,
                                                          versionString(), qsoDetails)) {
        // Discovery off: retained deletes, so entities announced earlier go away.
        m_mqtt->publishAbsolute(msg.topic, haDiscovery ? msg.payload : QByteArray{});
    }
    // Privacy: switching QSO details off must also clear the retained value,
    // or the broker keeps serving the last callsign to anyone who subscribes.
    if (!qsoDetails)
        m_mqtt->publishState(QString::fromLatin1(ShackStatus::kLastQso), QByteArray{});

    publishRadioStatus();
    publishQsoStatus();
}

void MainWindow::publishRadioStatus()
{
    if (!m_mqtt) return;
    using namespace ShackStatus;
    const bool linked = (m_tci && m_tci->connected()) || (m_rigctld && m_rigctld->connected());
    m_mqtt->publishState(QString::fromLatin1(kConnected), onOff(linked));
    m_mqtt->publishState(QString::fromLatin1(kTransmitting),
                         onOff(m_tci && m_tci->connected() && m_tci->transmitting()));
    const QByteArray hz = frequencyPayload(m_curFreqMhz);
    if (!hz.isEmpty())        m_mqtt->publishState(QString::fromLatin1(kFrequency), hz);
    if (!m_curMode.isEmpty()) m_mqtt->publishState(QString::fromLatin1(kMode),
                                                   modePayload(m_curMode, m_curSubmode));
    if (!m_curBand.isEmpty()) m_mqtt->publishState(QString::fromLatin1(kBand),
                                                   m_curBand.toUtf8());
}

void MainWindow::publishQsoStatus(qint64 addedQsoId)
{
    if (!m_mqtt || !m_model || !m_model->isOpen()) return;
    if (m_model->settingValue("MQTT_ENABLE", "0") != "1") return;
    LogbookFilter today;
    today.dateFrom = today.dateTo = QDateTime::currentDateTimeUtc().toString("yyyyMMdd");
    m_mqtt->publishState(QString::fromLatin1(ShackStatus::kCountToday),
                         QByteArray::number(m_model->countQsos(today)));
    if (addedQsoId >= 0 && m_model->settingValue("MQTT_PUBLISH_QSO", "0") == "1") {
        bool ok = false;
        const Qso q = m_model->getQso(addedQsoId, &ok);
        if (ok) m_mqtt->publishState(QString::fromLatin1(ShackStatus::kLastQso),
                                     ShackStatus::lastQsoPayload(q));
    }
}

double MainWindow::txPowerForNewQso() const
{
    // #23: log what the radio measured rather than a number typed once in
    // Settings. Only TCI reports it; rigctld and a disconnected radio keep the
    // default, as does an operator who has switched the feature off. The
    // measured value is the radio's forward power — with an amplifier in line
    // it is the drive level (see the Settings tooltip).
    if (m_model->settingValue("TX_PWR_FROM_RADIO", "1") == "1"
        && m_tci && m_tci->connected()) {
        if (const auto w = m_tci->measuredTxPowerW())
            return *w;
    }
    return m_model->defaultTxPwr();
}

// ── Status bar ───────────────────────────────────────────────────────────

void MainWindow::refreshStatusBar()
{
    if (!m_model) return;
    // Label the link by what it actually is, so "no radio" is never ambiguous
    // between "TCI is off" and "rigctld is not running".
    const bool rig = usingRigctld();
    const QString label = rig ? QStringLiteral("CAT") : QStringLiteral("TCI");
    const QString host = rig ? m_model->settingValue("RIGCTLD_HOST", "127.0.0.1")
                             : m_model->settingValue("TCI_HOST", "127.0.0.1");
    const QString port = rig ? m_model->settingValue("RIGCTLD_PORT", "4532")
                             : m_model->settingValue("TCI_PORT", "40001");
    const bool up = rig ? (m_rigctld && m_rigctld->connected())
                        : (m_tci && m_tci->connected());
    // Show the radio being logged against, so what lands in the QSO is
    // visible before it is saved rather than discovered afterwards.
    const QString radio = currentRadioName();
    m_sbTci->setText(radio.isEmpty()
                         ? QString("%1: %2 %3:%4")
                               .arg(label).arg(up ? "✓" : "✗")
                               .arg(host).arg(port)
                         : QString("%1: %2 %3 @ %4:%5")
                               .arg(label).arg(up ? "✓" : "✗")
                               .arg(radio).arg(host).arg(port));

    if (m_dxc && m_sbDxc) {
        const bool dxcEnabled = m_model->settingValue("DXC_ENABLE", "0") == "1";
        if (!dxcEnabled) {
            m_sbDxc->setText("DXC: off");
        } else {
            const QString status = m_dxc->connected() ? "✓" : "…";
            m_sbDxc->setText(QString("DXC: %1 %2 spots").arg(status)
                                .arg(m_spotIndex ? m_spotIndex->size() : 0));
        }
    }
    m_sbDb->setText(QString("DB: %1").arg(m_model->databasePath()));
}

} // namespace ShackBook
