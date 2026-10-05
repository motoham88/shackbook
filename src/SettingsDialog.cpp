#include "SettingsDialog.h"

#include "LogbookModel.h"
#include "AetherSettingsReader.h"
#include "TciClient.h"      // tciNicknameKey()
#include "RigctldClient.h"   // rigctldNicknameKey()
#include "HamlibRigList.h"

#include <QSerialPortInfo>
#include "TciDiscovery.h"
#include "ShackStatus.h"   // default MQTT topic prefix
#include "CwKeyerSettings.h"

#include <QComboBox>
#include <QEventLoop>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QTabWidget>

namespace ShackBook {

namespace {
const QStringList kContestIds = {
    "",
    "CQ-WW-CW", "CQ-WW-SSB", "CQ-WW-RTTY",
    "CQ-WPX-CW", "CQ-WPX-SSB", "CQ-WPX-RTTY",
    "ARRL-DX-CW", "ARRL-DX-SSB",
    "ARRL-FD",
    "ARRL-SS-CW", "ARRL-SS-SSB",
    "ARRL-10", "ARRL-160",
    "IARU-HF",
    "WAEDC-CW", "WAEDC-SSB",
    "RDXC", "IOTA",
    "BARTG-RTTY",
    "NAQP-CW", "NAQP-SSB",
};

const QStringList kCatOp      = { "SINGLE-OP", "MULTI-OP", "CHECKLOG" };
const QStringList kCatAssist  = { "NON-ASSISTED", "ASSISTED" };
const QStringList kCatBand    = { "ALL", "160M", "80M", "40M", "20M", "15M", "10M", "6M", "2M" };
const QStringList kCatMode    = { "MIXED", "CW", "SSB", "RTTY", "DIGI", "FM" };
const QStringList kCatPower   = { "HIGH", "LOW", "QRP" };
const QStringList kCatStation = { "FIXED", "MOBILE", "PORTABLE", "ROVER", "HQ", "SCHOOL" };
const QStringList kCatTx      = { "ONE", "TWO", "LIMITED", "UNLIMITED", "SWL" };
} // namespace

SettingsDialog::SettingsDialog(LogbookModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("ShackBook Settings");
    setMinimumWidth(560);
    buildUI();
    populate();
}

void SettingsDialog::buildUI()
{
    auto* tabs = new QTabWidget;

    // ── Operator ────────────────────────────────────────────────────────
    auto* op = new QWidget;
    auto* opL = new QFormLayout(op);
    m_myCall       = new QLineEdit;
    m_myGrid       = new QLineEdit;
    m_myState      = new QLineEdit;
    m_defaultTxPwr = new QDoubleSpinBox;
    m_defaultTxPwr->setRange(0.0, 99999.0);
    m_defaultTxPwr->setDecimals(1);
    m_defaultTxPwr->setSuffix(" W");
    m_txPwrFromRadio = new QCheckBox("Use the power measured by the radio (TCI) when available");
    m_txPwrFromRadio->setToolTip(
        "Logs the peak forward power of your last transmission (within 2 minutes)\n"
        "instead of the default. Falls back to the default when nothing was measured,\n"
        "or with rigctld.\n\n"
        "This is the radio's own output: with an amplifier in line it is the drive\n"
        "power, not the power at the antenna. A tune carrier on AetherSDR counts as\n"
        "a transmission, so key up after tuning before logging.");
    m_myOperator   = new QLineEdit;
    opL->addRow("My call",       m_myCall);
    opL->addRow("My grid",       m_myGrid);
    opL->addRow("My state",      m_myState);
    opL->addRow("Default power", m_defaultTxPwr);
    opL->addRow("",              m_txPwrFromRadio);
    opL->addRow("Operator",      m_myOperator);
    tabs->addTab(op, "Operator");

    // ── TCI ─────────────────────────────────────────────────────────────
    auto* tci = new QWidget;
    auto* tciL = new QFormLayout(tci);
    m_tciHost = new QLineEdit;
    m_tciHost->setPlaceholderText("127.0.0.1");
    m_tciPort = new QSpinBox;
    m_tciPort->setRange(1, 65535);
    m_tciNickname = new QLineEdit;
    m_tciNickname->setPlaceholderText("e.g. Hermes-Lite 2");
    m_tciNickname->setToolTip(
        "Name recorded on each QSO made through this server.\n\n"
        "TCI reports the application (\"AetherSDR\"), not the radio, so two "
        "rigs driven by the same software look identical without a nickname.\n\n"
        "Stored per host:port. Leave blank to record the announced name.");
    m_tciAutoConnect = new QCheckBox("Connect to TCI server on launch");
    // Off by default: tuning MOVES the radio, and an operator running split
    // should not discover that by double-clicking a spot out of curiosity.
    m_spotTunes = new QCheckBox("Double-clicking a spot tunes the radio to it");
    m_spotSetsMode = new QCheckBox("...and sets the mode when the spot carries one");
    m_radioSource = new QComboBox;
    m_radioSource->addItem("TCI — AetherSDR, ExpertSDR, SunSDR", "tci");
    m_radioSource->addItem("Hamlib rigctld — Icom, Yaesu, Kenwood, others", "rigctld");
    m_radioSource->setToolTip(
        "How ShackBook follows your radio.\n\n"
        "TCI is built into AetherSDR and ExpertSDR — nothing else to install.\n\n"
        "rigctld covers everything else, but needs Hamlib installed and running "
        "(ShackBook does not include it). Its advantage: it reports the RADIO's "
        "model, so contacts are attributed correctly without a nickname.");
    tciL->addRow("Follow radio via", m_radioSource);

    // ShackBook does not ship Hamlib, so say plainly whether it is here. Without
    // this the operator gets "CAT: ✗" and no way to tell "not installed" from
    // "radio switched off" — different problems with different fixes.
    m_hamlibStatus = new QLabel;
    m_hamlibStatus->setWordWrap(true);
    m_hamlibStatus->setOpenExternalLinks(true);
    m_hamlibStatus->setTextFormat(Qt::RichText);
    tciL->addRow(QString(), m_hamlibStatus);

    m_rigctldPath = new QLineEdit;
    m_rigctldPath->setPlaceholderText("auto-detect");
    m_rigctldPath->setToolTip(
        "Where rigctld lives, if ShackBook cannot find it on its own.\n\n"
        "Leave blank to look in the usual places and on PATH.");
    tciL->addRow("rigctld program", m_rigctldPath);

    // THE THREE THINGS rigctld NEEDS, none of which ShackBook used to help
    // with: a model NUMBER, a port, and a baud rate. The operator had to
    // find 3081 among 314 rigs, know which COM port the radio landed on
    // (it drifts between sessions here), and know the baud from a radio
    // menu. Issue #13.
    m_rigModel = new QComboBox;
    m_rigModel->setEditable(true);            // so it can be typed at, and searched
    m_rigModel->setInsertPolicy(QComboBox::NoInsert);
    m_rigModel->setToolTip(
        "Your radio, as Hamlib knows it."
        "This is the -m number rigctld wants. Type to search: \"9700\" finds "
        "the IC-9700 without scrolling 300 rigs.");
    tciL->addRow("Radio model", m_rigModel);

    m_rigPort = new QComboBox;
    m_rigPort->setEditable(true);             // a port can be typed if absent
    m_rigPort->setInsertPolicy(QComboBox::NoInsert);
    m_rigPort->setToolTip(
        "The serial port the radio is on."
        "Read from the system, with the adapter name shown, so you can tell "
        "a radio from a mouse. Re-read each time this page opens, because "
        "USB ports move.");
    tciL->addRow("Serial port", m_rigPort);

    m_rigBaud = new QComboBox;
    m_rigBaud->setEditable(true);
    m_rigBaud->setInsertPolicy(QComboBox::NoInsert);
    for (const char* b : {"4800", "9600", "19200", "38400", "57600", "115200"})
        m_rigBaud->addItem(QString::fromLatin1(b));
    m_rigBaud->setCurrentText(QStringLiteral("19200"));
    m_rigBaud->setToolTip(
        "The radio's CAT baud rate."
        "This is a setting in the RADIO's menu -- the port cannot report it. "
        "It has to match, or rigctld connects and reads nothing.");
    tciL->addRow("Baud", m_rigBaud);

    // The whole point: a command that is READY, not a template with holes.
    m_rigCommand = new QLabel;
    m_rigCommand->setWordWrap(true);
    m_rigCommand->setTextFormat(Qt::RichText);
    m_rigCommand->setTextInteractionFlags(Qt::TextSelectableByMouse
                                          | Qt::TextSelectableByKeyboard);
    tciL->addRow(QString(), m_rigCommand);

    connect(m_rigModel, &QComboBox::currentTextChanged, this,
            &SettingsDialog::refreshRigCommand);
    connect(m_rigPort, &QComboBox::currentTextChanged, this,
            &SettingsDialog::refreshRigCommand);
    connect(m_rigBaud, &QComboBox::currentTextChanged, this,
            &SettingsDialog::refreshRigCommand);

    m_tciScan = new QPushButton("Find radios…");
    m_tciScan->setToolTip(
        "Look for TCI servers on this machine and the host above.\n\n"
        "Only servers that answer a real TCI handshake are offered — a port "
        "that merely accepts a connection is not a radio.");
    connect(m_tciScan, &QPushButton::clicked, this, &SettingsDialog::onScanForRadios);
    tciL->addRow("Host", m_tciHost);
    tciL->addRow("Port", m_tciPort);
    tciL->addRow(QString(), m_tciScan);
    tciL->addRow("Radio nickname", m_tciNickname);
    tciL->addRow(m_tciAutoConnect);
    tciL->addRow(m_spotTunes);
    tciL->addRow(m_spotSetsMode);
    // The mode option is meaningless on its own.
    connect(m_spotTunes, &QCheckBox::toggled, m_spotSetsMode, &QWidget::setEnabled);
    tabs->addTab(tci, "TCI");

    // ── DX Cluster ──────────────────────────────────────────────────────
    auto* dxc = new QWidget;
    auto* dxcL = new QFormLayout(dxc);
    m_dxcEnable     = new QCheckBox("Enable DX cluster spotting (auto-fill CALL on QSY)");
    m_dxcAutoDetect = new QCheckBox("Auto-detect cluster from AetherSDR's settings file");
    m_dxcHost       = new QLineEdit;
    m_dxcHost->setPlaceholderText("dxc.nc7j.com");
    m_dxcPort       = new QSpinBox;
    m_dxcPort->setRange(1, 65535);
    m_dxcPort->setValue(7300);
    m_dxcCallsign   = new QLineEdit;
    m_dxcCallsign->setPlaceholderText("G0JKN");
    m_dxcLoginSuffix = new QComboBox;
    m_dxcLoginSuffix->setEditable(true);
    m_dxcLoginSuffix->addItem("-2",  "-2");      // DXSpider preferred
    m_dxcLoginSuffix->addItem("-1",  "-1");
    m_dxcLoginSuffix->addItem("-3",  "-3");
    m_dxcLoginSuffix->addItem("-L (CC Cluster / AR-Cluster)", "-L");
    m_dxcLoginSuffix->addItem("(none — bare callsign)",       "");
    m_dxcDetected   = new QLabel("(no AetherSDR config detected)");
    m_dxcDetected->setStyleSheet("QLabel { color: #6b8099; font-size: 10px; }");
    m_dxcDetected->setWordWrap(true);

    dxcL->addRow(m_dxcEnable);
    dxcL->addRow(m_dxcAutoDetect);
    dxcL->addRow("Detected", m_dxcDetected);
    dxcL->addRow("Host (override)",     m_dxcHost);
    dxcL->addRow("Port (override)",     m_dxcPort);
    dxcL->addRow("Callsign (override)", m_dxcCallsign);
    dxcL->addRow("Login suffix",        m_dxcLoginSuffix);

    // ── POTA section (lives on the same tab — both feed the SpotIndex) ──
    auto* potaSep = new QLabel("─── POTA (api.pota.app) ───");
    potaSep->setStyleSheet("QLabel { color: #6b8099; font-size: 9px; "
                           "font-weight: bold; letter-spacing: 0.08em; }");
    dxcL->addRow(potaSep);

    m_potaEnable  = new QCheckBox("Enable POTA spotting (Parks On The Air HTTP feed)");
    m_potaPollSec = new QSpinBox;
    m_potaPollSec->setRange(5, 600);
    m_potaPollSec->setSuffix(" s");
    dxcL->addRow(m_potaEnable);
    dxcL->addRow("POTA poll interval", m_potaPollSec);

    auto refreshDxcEditable = [this]() {
        const bool manual = !m_dxcAutoDetect->isChecked();
        m_dxcHost->setEnabled(manual);
        m_dxcPort->setEnabled(manual);
        m_dxcCallsign->setEnabled(manual);
    };
    connect(m_dxcAutoDetect, &QCheckBox::toggled, this, [refreshDxcEditable](bool){
        refreshDxcEditable();
    });

    tabs->addTab(dxc, "DX Cluster");

    // ── Callsign lookup ─────────────────────────────────────────────────
    auto* lk = new QWidget;
    auto* lkL = new QFormLayout(lk);
    m_lkWorkedBefore = new QCheckBox("Fill name/QTH from previous QSOs with this call");
    m_lkCty          = new QCheckBox("Fill country/continent/zones from cty.dat (offline)");
    m_lkProvider     = new QComboBox;
    m_lkProvider->addItem("None",                                "none");
    m_lkProvider->addItem("QRZ.com (XML subscription required)", "qrz");
    m_lkProvider->addItem("HamQTH.com (free account)",           "hamqth");
    m_lkUser = new QLineEdit;
    m_lkPass = new QLineEdit;
    m_lkPass->setEchoMode(QLineEdit::Password);
    m_lkCallook = new QCheckBox(
        "Use callook.info for US calls when no provider is set (no account)");
    auto* lkNote = new QLabel(
        "Looked-up details only ever fill EMPTY fields — anything you type "
        "wins. QRZ XML lookups need a paid QRZ XML-data subscription; a free "
        "QRZ web login will return an error. The password is stored in this "
        "log's database file.");
    lkNote->setStyleSheet("QLabel { color: #6b8099; font-size: 10px; }");
    lkNote->setWordWrap(true);
    lkL->addRow(m_lkWorkedBefore);
    lkL->addRow(m_lkCty);
    lkL->addRow("Online provider", m_lkProvider);
    lkL->addRow("Username",        m_lkUser);
    lkL->addRow("Password",        m_lkPass);
    lkL->addRow(m_lkCallook);
    lkL->addRow(lkNote);

    auto refreshLkEditable = [this]() {
        const bool online =
            m_lkProvider->currentData().toString() != QLatin1String("none");
        m_lkUser->setEnabled(online);
        m_lkPass->setEnabled(online);
        m_lkCallook->setEnabled(!online);
    };
    connect(m_lkProvider, &QComboBox::currentIndexChanged, this,
            [refreshLkEditable](int){ refreshLkEditable(); });

    tabs->addTab(lk, "Lookup");

    // ── Contest ─────────────────────────────────────────────────────────
    auto* ctst = new QWidget;
    auto* ctstL = new QFormLayout(ctst);
    m_contestMode = new QCheckBox("Contest mode (show contest panel in main window)");
    m_contestId   = new QComboBox;
    m_contestId->setEditable(true);
    m_contestId->addItems(kContestIds);
    m_stxNext = new QSpinBox;
    m_stxNext->setRange(1, 999999);
    ctstL->addRow(m_contestMode);
    ctstL->addRow("Contest ID",      m_contestId);
    ctstL->addRow("Next STX serial", m_stxNext);
    tabs->addTab(ctst, "Contest");

    // ── CW keyer (#32) ──────────────────────────────────────────────────
    // The first thing in ShackBook that can transmit, so it is off until the
    // operator turns it on here, and says so in as many words.
    auto* cw  = new QWidget;
    auto* cwL = new QFormLayout(cw);
    m_cwEnable = new QCheckBox("Allow ShackBook to send CW");
    auto* cwNote = new QLabel(
        "Sends CW through the TCI radio link (AetherSDR, ExpertSDR or a TCI bridge). "
        "When on, the CW keyer panel appears and F1-F8 send these messages while "
        "ShackBook has focus. Nothing is ever sent except by a button or F-key: "
        "not on saving a QSO, a spot click or connecting. Esc stops sending anywhere "
        "in ShackBook. Only sends while the radio is in CW.");
    cwNote->setWordWrap(true);
    cwL->addRow(m_cwEnable);
    cwL->addRow(cwNote);
    auto* tokens = new QLabel(
        "Tokens: {CALL} {MYCALL} {RST} {NR} {EXCH} {NAME}. A message whose token is "
        "empty is not sent. {NR} (sent as three digits) and {EXCH} come from the "
        "contest row, so they are filled only in contest mode.");
    tokens->setWordWrap(true);
    cwL->addRow(tokens);
    for (int i = 0; i < 8; ++i) {
        auto* label = new QLineEdit;
        label->setMaximumWidth(90);
        label->setPlaceholderText("label");
        auto* text = new QLineEdit;
        auto* row = new QHBoxLayout;
        row->addWidget(label);
        row->addWidget(text, 1);
        cwL->addRow(QStringLiteral("F%1").arg(i + 1), row);
        m_cwLabels << label;
        m_cwTexts  << text;
    }
    m_cwCutRst = new QCheckBox("Cut numbers in {RST} (599 → 5NN)");
    m_cwCutNr  = new QCheckBox("Cut numbers in {NR} (001 → TT1)");
    m_cwCutOne = new QCheckBox("Also cut 1 → A");
    m_cwName   = new QLineEdit;
    m_cwName->setPlaceholderText("first name, e.g. TONY");
    cwL->addRow(m_cwCutRst);
    cwL->addRow(m_cwCutNr);
    cwL->addRow(m_cwCutOne);
    cwL->addRow("{NAME}", m_cwName);
    auto* cwDefaults = new QPushButton("Restore default messages");
    connect(cwDefaults, &QPushButton::clicked, this, [this] {
        const auto d = defaultCwMacros();
        for (int i = 0; i < m_cwTexts.size() && i < d.size(); ++i) {
            m_cwLabels[i]->setText(d[i].label);
            m_cwTexts[i]->setText(d[i].text);
        }
    });
    cwL->addRow(cwDefaults);
    // Eight message rows make this the tallest tab; scroll rather than
    // stretch the whole dialog past a laptop screen.
    auto* cwScroll = new QScrollArea;
    cwScroll->setWidgetResizable(true);
    cwScroll->setFrameShape(QFrame::NoFrame);
    cwScroll->setWidget(cw);
    tabs->addTab(cwScroll, "CW keyer");

    // ── Cabrillo ────────────────────────────────────────────────────────
    auto* cab = new QWidget;
    auto* cabL = new QFormLayout(cab);
    m_cbName     = new QLineEdit;
    m_cbAddress  = new QLineEdit;
    m_cbEmail    = new QLineEdit;
    m_cbClub     = new QLineEdit;
    m_cbLocation = new QLineEdit;
    auto buildCombo = [](const QStringList& items) {
        auto* c = new QComboBox; c->addItems(items); return c;
    };
    m_cbCatOp          = buildCombo(kCatOp);
    m_cbCatAssisted    = buildCombo(kCatAssist);
    m_cbCatBand        = buildCombo(kCatBand);
    m_cbCatMode        = buildCombo(kCatMode);
    m_cbCatPower       = buildCombo(kCatPower);
    m_cbCatStation     = buildCombo(kCatStation);
    m_cbCatTransmitter = buildCombo(kCatTx);
    cabL->addRow("Name",          m_cbName);
    cabL->addRow("Address",       m_cbAddress);
    cabL->addRow("Email",         m_cbEmail);
    cabL->addRow("Club",          m_cbClub);
    cabL->addRow("Location",      m_cbLocation);
    cabL->addRow("Operator cat",  m_cbCatOp);
    cabL->addRow("Assisted",      m_cbCatAssisted);
    cabL->addRow("Band cat",      m_cbCatBand);
    cabL->addRow("Mode cat",      m_cbCatMode);
    cabL->addRow("Power cat",     m_cbCatPower);
    cabL->addRow("Station cat",   m_cbCatStation);
    cabL->addRow("Transmitter",   m_cbCatTransmitter);
    tabs->addTab(cab, "Cabrillo");

    // ── MQTT (#24) ──────────────────────────────────────────────────────
    auto* mq  = new QWidget;
    auto* mqL = new QFormLayout(mq);
    m_mqttEnable = new QCheckBox("Publish shack status to an MQTT broker (e.g. for Home Assistant)");
    m_mqttHost   = new QLineEdit;
    m_mqttHost->setPlaceholderText("broker address, e.g. 10.0.0.51");
    m_mqttPort   = new QSpinBox;
    m_mqttPort->setRange(1, 65535);
    m_mqttUser   = new QLineEdit;
    m_mqttPass   = new QLineEdit;
    m_mqttPass->setEchoMode(QLineEdit::Password);
    m_mqttPrefix = new QLineEdit;
    m_mqttPrefix->setPlaceholderText(ShackStatus::defaultTopicPrefix(m_model->myCall()));
    m_mqttHaDiscovery = new QCheckBox("Home Assistant discovery (entities appear automatically)");
    m_mqttPublishQso  = new QCheckBox("Publish QSO details (the other station's callsign)");
    m_mqttPublishQso->setToolTip(
        "Off by default: puts the last contact's callsign, band and mode on the broker, "
        "where anything allowed to subscribe can read it. Radio status and the "
        "day's QSO count are published either way.");
    auto* mqNote = new QLabel(
        "LAN broker only (no TLS). The password is stored in this logbook's settings "
        "in plain text, so use a dedicated broker account that can only publish "
        "under the topic prefix, not a personal password. Logging never waits on the broker.");
    mqNote->setWordWrap(true);
    mqL->addRow(m_mqttEnable);
    mqL->addRow("Broker host",   m_mqttHost);
    mqL->addRow("Port",          m_mqttPort);
    mqL->addRow("Username",      m_mqttUser);
    mqL->addRow("Password",      m_mqttPass);
    mqL->addRow("Topic prefix",  m_mqttPrefix);
    mqL->addRow(m_mqttHaDiscovery);
    mqL->addRow(m_mqttPublishQso);
    mqL->addRow(mqNote);
    tabs->addTab(mq, "MQTT");

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(btns, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* main = new QVBoxLayout(this);
    main->addWidget(tabs);
    main->addWidget(btns);
}

void SettingsDialog::populate()
{
    m_myCall->setText(m_model->myCall());
    m_myGrid->setText(m_model->myGridsquare());
    m_myState->setText(m_model->myState());
    m_defaultTxPwr->setValue(m_model->defaultTxPwr());
    m_txPwrFromRadio->setChecked(m_model->settingValue("TX_PWR_FROM_RADIO", "1") == "1");
    m_myOperator->setText(m_model->settingValue("MY_OPERATOR"));

    {
        const QString src = m_model->settingValue("RADIO_SOURCE", "tci");
        const int idx = m_radioSource->findData(src);
        m_radioSource->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    // Host and port belong to whichever source is selected, so switching the
    // combo swaps them rather than leaving a TCI port against a CAT radio.
    const auto loadEndpoint = [this]() {
        const bool rig = m_radioSource->currentData().toString() == QLatin1String("rigctld");
        m_tciHost->setText(m_model->settingValue(rig ? "RIGCTLD_HOST" : "TCI_HOST",
                                                 "127.0.0.1"));
        m_tciPort->setValue(m_model->settingValue(rig ? "RIGCTLD_PORT" : "TCI_PORT",
                                                  rig ? "4532" : "40001").toInt());
        const QString h = m_tciHost->text();
        const QString p = QString::number(m_tciPort->value());
        m_tciNickname->setText(m_model->settingValue(
            rig ? rigctldNicknameKey(h, p) : tciNicknameKey(h, p)));
    };
    m_rigctldPath->setText(m_model->settingValue("HAMLIB_RIGCTLD_PATH"));
    loadEndpoint();
    connect(m_radioSource, &QComboBox::currentIndexChanged, this,
            [this, loadEndpoint](int) {
                loadEndpoint();
                refreshHamlibGuidance();
                refreshRigCommand();
            });
    connect(m_rigctldPath, &QLineEdit::textChanged, this,
            [this](const QString&) {
                refreshHamlibGuidance();
                // A new path can mean a different Hamlib with a
                // different catalogue, so the list is re-read too.
                refreshRigModels();
                refreshRigCommand();
            });
    refreshHamlibGuidance();
    refreshRigModels();
    refreshSerialPorts();
    // Restore the operator's stored choices AFTER the lists exist.
    {
        const QString m = m_model->settingValue("HAMLIB_RIG_MODEL");
        if (!m.isEmpty()) {
            const int at = m_rigModel->findData(m);
            if (at >= 0) m_rigModel->setCurrentIndex(at);
            else         m_rigModel->setCurrentText(m);
        }
        const QString prt = m_model->settingValue("HAMLIB_RIG_PORT");
        if (!prt.isEmpty()) {
            const int at = m_rigPort->findData(prt);
            if (at >= 0) m_rigPort->setCurrentIndex(at);
            else         m_rigPort->setCurrentText(prt);
        }
        const QString b = m_model->settingValue("HAMLIB_RIG_BAUD");
        if (!b.isEmpty()) m_rigBaud->setCurrentText(b);
    }
    refreshRigCommand();
    m_tciAutoConnect->setChecked(m_model->settingValue("TCI_AUTOCONNECT", "1") == "1");
    m_spotTunes->setChecked(m_model->settingValue("SPOT_DOUBLECLICK_TUNES", "0") == "1");
    m_spotSetsMode->setChecked(m_model->settingValue("SPOT_DOUBLECLICK_SETS_MODE", "1") == "1");
    m_spotSetsMode->setEnabled(m_spotTunes->isChecked());

    // DX Cluster — auto-detect from AetherSDR's config, default to "on" if
    // a cluster is configured there; otherwise off until the user opts in.
    const auto aether = AetherSettingsReader::readDxClusterConfig();
    if (aether.found) {
        m_dxcDetected->setText(
            QString("AetherSDR: %1:%2 as %3 (login will be sent as %3-L)")
                .arg(aether.host).arg(aether.port).arg(aether.callsign));
    } else {
        m_dxcDetected->setText(
            "(AetherSDR config not found at " + AetherSettingsReader::defaultSettingsPath()
            + " — uncheck auto-detect to enter cluster details manually)");
    }
    const QString defEnable = aether.found ? "1" : "0";
    m_dxcEnable->setChecked(m_model->settingValue("DXC_ENABLE", defEnable) == "1");
    m_dxcAutoDetect->setChecked(m_model->settingValue("DXC_AUTODETECT", "1") == "1");
    m_dxcHost->setText(m_model->settingValue("DXC_HOST",
                            aether.found ? aether.host : QString("dxc.nc7j.com")));
    m_dxcPort->setValue(m_model->settingValue("DXC_PORT",
                            QString::number(aether.found ? aether.port : 7300)).toInt());
    m_dxcCallsign->setText(m_model->settingValue("DXC_CALLSIGN", aether.callsign));
    const QString storedSuffix = m_model->settingValue("DXC_LOGIN_SUFFIX", "-2");
    int suffixIdx = -1;
    for (int i = 0; i < m_dxcLoginSuffix->count(); ++i) {
        if (m_dxcLoginSuffix->itemData(i).toString() == storedSuffix) {
            suffixIdx = i;
            break;
        }
    }
    if (suffixIdx >= 0) m_dxcLoginSuffix->setCurrentIndex(suffixIdx);
    else                m_dxcLoginSuffix->setEditText(storedSuffix);

    m_potaEnable->setChecked(m_model->settingValue("POTA_ENABLE", "1") == "1");
    m_potaPollSec->setValue(m_model->settingValue("POTA_POLL_SEC", "30").toInt());

    const bool manual = !m_dxcAutoDetect->isChecked();
    m_dxcHost->setEnabled(manual);
    m_dxcPort->setEnabled(manual);
    m_dxcCallsign->setEnabled(manual);

    const QString lkProv = m_model->settingValue("LOOKUP_PROVIDER", "none");
    for (int i = 0; i < m_lkProvider->count(); ++i) {
        if (m_lkProvider->itemData(i).toString() == lkProv) {
            m_lkProvider->setCurrentIndex(i);
            break;
        }
    }
    m_lkWorkedBefore->setChecked(m_model->settingValue("LOOKUP_WORKEDBEFORE", "1") == "1");
    m_lkCty->setChecked(m_model->settingValue("LOOKUP_CTY", "1") == "1");
    m_lkUser->setText(m_model->settingValue("LOOKUP_USERNAME"));
    m_lkPass->setText(m_model->settingValue("LOOKUP_PASSWORD"));
    m_lkCallook->setChecked(m_model->settingValue("LOOKUP_CALLOOK", "1") == "1");
    {
        const bool online = lkProv != QLatin1String("none");
        m_lkUser->setEnabled(online);
        m_lkPass->setEnabled(online);
        m_lkCallook->setEnabled(!online);
    }

    m_contestMode->setChecked(m_model->contestMode());
    const QString cid = m_model->contestId();
    int idx = m_contestId->findText(cid);
    if (idx >= 0) m_contestId->setCurrentIndex(idx);
    else          m_contestId->setEditText(cid);
    m_stxNext->setValue(m_model->settingValue("CONTEST_STX_NEXT", "1").toInt());

    m_mqttEnable->setChecked(m_model->settingValue("MQTT_ENABLE", "0") == "1");
    m_mqttHost->setText(m_model->settingValue("MQTT_HOST"));
    m_mqttPort->setValue(m_model->settingValue("MQTT_PORT", "1883").toInt());
    m_mqttUser->setText(m_model->settingValue("MQTT_USERNAME"));
    m_mqttPass->setText(m_model->settingValue("MQTT_PASSWORD"));
    m_mqttPrefix->setText(m_model->settingValue("MQTT_TOPIC_PREFIX"));
    m_mqttHaDiscovery->setChecked(m_model->settingValue("MQTT_HA_DISCOVERY", "1") == "1");
    m_mqttPublishQso->setChecked(m_model->settingValue("MQTT_PUBLISH_QSO", "0") == "1");

    m_cbName->setText(m_model->settingValue("CABRILLO_NAME"));
    m_cbAddress->setText(m_model->settingValue("CABRILLO_ADDRESS"));
    m_cbEmail->setText(m_model->settingValue("CABRILLO_EMAIL"));
    m_cbClub->setText(m_model->settingValue("CABRILLO_CLUB"));
    m_cbLocation->setText(m_model->settingValue("CABRILLO_LOCATION"));
    auto setCombo = [](QComboBox* c, const QString& v) {
        const int i = c->findText(v);
        if (i >= 0) c->setCurrentIndex(i);
    };
    setCombo(m_cbCatOp,          m_model->settingValue("CABRILLO_CAT_OPERATOR",     "SINGLE-OP"));
    setCombo(m_cbCatAssisted,    m_model->settingValue("CABRILLO_CAT_ASSISTED",     "NON-ASSISTED"));
    setCombo(m_cbCatBand,        m_model->settingValue("CABRILLO_CAT_BAND",         "ALL"));
    setCombo(m_cbCatMode,        m_model->settingValue("CABRILLO_CAT_MODE",         "MIXED"));
    setCombo(m_cbCatPower,       m_model->settingValue("CABRILLO_CAT_POWER",        "HIGH"));
    setCombo(m_cbCatStation,     m_model->settingValue("CABRILLO_CAT_STATION",      "FIXED"));
    setCombo(m_cbCatTransmitter, m_model->settingValue("CABRILLO_CAT_TRANSMITTER",  "ONE"));

    {
        const CwKeyerConfig cw = loadCwKeyerConfig(
            [this](const QString& k, const QString& d) { return m_model->settingValue(k, d); });
        m_cwEnable->setChecked(cw.enabled);
        for (int i = 0; i < m_cwTexts.size() && i < cw.macros.size(); ++i) {
            m_cwLabels[i]->setText(cw.macros[i].label);
            m_cwTexts[i]->setText(cw.macros[i].text);
        }
        m_cwCutRst->setChecked(cw.cut.cutRst);
        m_cwCutNr->setChecked(cw.cut.cutNr);
        m_cwCutOne->setChecked(cw.cut.cutOne);
        m_cwName->setText(cw.name);
    }
}

void SettingsDialog::refreshHamlibGuidance()
{
    if (!m_hamlibStatus) return;

    const bool rig = m_radioSource->currentData().toString() == QLatin1String("rigctld");
    // Only relevant when a CAT radio is selected. Saying nothing for TCI users
    // keeps the requirement from reading as a barrier to the whole app.
    m_hamlibStatus->setVisible(rig);
    m_rigctldPath->setVisible(rig);
    if (!rig) return;

    const QString found = RigctldClient::findRigctld(m_rigctldPath->text());

    if (found.isEmpty()) {
        m_hamlibStatus->setStyleSheet("QLabel { color: #d08a3e; font-size: 11px; }");
        m_hamlibStatus->setText(tr(
            "<b>Hamlib not found.</b> Following a non-TCI radio needs Hamlib's "
            "<code>rigctld</code>, which ShackBook does not include. "
            "<a href=\"https://hamlib.github.io/\">Get Hamlib</a> — or if you "
            "already run WSJT-X or fldigi you very likely have it, and can point "
            "at it above. ShackBook will not start it for you: run it yourself so "
            "nothing else loses the serial port."));
        return;
    }

    m_hamlibStatus->setStyleSheet("QLabel { color: #6b8099; font-size: 11px; }");
    m_hamlibStatus->setText(tr(
        "Hamlib found at <code>%1</code>. It still has to be RUNNING and "
        "connected to the radio — ShackBook only talks to it, and does not "
        "start it. For example:<br><code>rigctld -m &lt;model&gt; -r &lt;port&gt;</code>")
            .arg(found.toHtmlEscaped()));
}

// Kept together: the same events -- source changed, path changed, dialog
// opened -- have to update all four, and splitting them is how one gets
// forgotten.
void SettingsDialog::refreshRigModels()
{
    const QString found = RigctldClient::findRigctld(m_rigctldPath->text());
    bool usedFallback = false;
    const QList<HamlibRigList::Rig> rigs = HamlibRigList::best(found, &usedFallback);

    // Keep what the operator already chose. Repopulating must not silently
    // reselect a different radio underneath them.
    const QString previous = m_rigModel->currentText();
    m_rigModel->clear();
    for (const HamlibRigList::Rig& r : rigs)
        m_rigModel->addItem(r.label(), r.model);
    if (!previous.isEmpty()) {
        const int at = m_rigModel->findText(previous);
        if (at >= 0) m_rigModel->setCurrentIndex(at);
        else         m_rigModel->setCurrentText(previous);
    }

    // SAY WHICH LIST THIS IS. A partial bundled list presented as the whole
    // catalogue would send someone hunting for a radio that is not in it.
    m_rigModel->setToolTip(usedFallback
        ? tr("A SHORT BUILT-IN LIST -- Hamlib is not installed, so its full "
             "catalogue is unavailable. Install Hamlib to pick from all of "
             "them, or type the model number if you know it.")
        : tr("Your radio, as the installed Hamlib knows it (%1 rigs). "
             "This is the -m number rigctld wants. Type to search.")
              .arg(rigs.size()));
}

void SettingsDialog::refreshSerialPorts()
{
    const QString previous = m_rigPort->currentText();
    m_rigPort->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& info : ports) {
        // "COM19 -- Silicon Labs CP210x". The description is what lets an
        // operator tell the radio from a mouse; the port name alone does not.
        const QString desc = info.description().isEmpty()
                                 ? info.manufacturer()
                                 : info.description();
        m_rigPort->addItem(desc.isEmpty()
                               ? info.portName()
                               : QStringLiteral("%1 - %2").arg(info.portName(), desc),
                           info.portName());
    }
    if (ports.isEmpty())
        m_rigPort->addItem(tr("(no serial ports found)"), QString());
    if (!previous.isEmpty()) {
        const int at = m_rigPort->findText(previous);
        if (at >= 0) m_rigPort->setCurrentIndex(at);
        else         m_rigPort->setCurrentText(previous);
    }
}

void SettingsDialog::refreshRigCommand()
{
    if (!m_rigCommand) return;
    const bool rig =
        m_radioSource->currentData().toString() == QLatin1String("rigctld");
    m_rigModel->setVisible(rig);
    m_rigPort->setVisible(rig);
    m_rigBaud->setVisible(rig);
    m_rigCommand->setVisible(rig);
    if (!rig) return;

    // The model NUMBER, not the label. userData holds it for a chosen row;
    // a typed entry may be the bare number itself.
    const int idx = m_rigModel->currentIndex();
    QString model = idx >= 0 ? m_rigModel->itemData(idx).toString() : QString();
    if (model.isEmpty()) {
        bool ok = false;
        const int typed = m_rigModel->currentText().trimmed().toInt(&ok);
        if (ok && typed > 0) model = QString::number(typed);
    }

    const int portIdx = m_rigPort->currentIndex();
    QString port = portIdx >= 0 ? m_rigPort->itemData(portIdx).toString() : QString();
    if (port.isEmpty()) port = m_rigPort->currentText().trimmed();

    const QString baud = m_rigBaud->currentText().trimmed();

    if (model.isEmpty() || port.isEmpty()) {
        m_rigCommand->setStyleSheet("QLabel { color: #6b8099; font-size: 11px; }");
        m_rigCommand->setText(tr(
            "Pick a radio model and a serial port, and the exact command to "
            "run will appear here."));
        return;
    }

    const QString exe = RigctldClient::findRigctld(m_rigctldPath->text());
    const QString program = exe.isEmpty() ? QStringLiteral("rigctld") : exe;
    const QString cmd = QStringLiteral("\"%1\" -m %2 -r %3%4")
                            .arg(program, model, port,
                                 baud.isEmpty() ? QString()
                                                : QStringLiteral(" -s ") + baud);

    m_rigCommand->setStyleSheet("QLabel { color: #6b8099; font-size: 11px; }");
    m_rigCommand->setText(tr(
        "Run this, and leave it running:<br><code>%1</code><br>"
        "ShackBook talks to it on port 4532; it does not start it, so nothing "
        "else loses the serial port.").arg(cmd.toHtmlEscaped()));
}

void SettingsDialog::onScanForRadios()
{
    // Sweep loopback plus whatever host is TYPED in the box right now — the
    // operator may be pointing at a new machine and not have saved it yet.
    QStringList hosts{QStringLiteral("127.0.0.1")};
    const QString typed = m_tciHost->text().trimmed();
    if (!typed.isEmpty()) hosts << typed;

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
            [&](const QList<TciServerInfo>& servers) { found = servers; loop.quit(); });
    connect(&progress, &QProgressDialog::canceled, &discovery, &TciDiscovery::cancel);

    discovery.scan(hosts, TciDiscovery::defaultPorts());
    loop.exec();
    progress.close();

    if (found.isEmpty()) {
        QMessageBox::information(
            this, tr("Find radios"),
            tr("No TCI servers answered.\n\n"
               "A radio only appears here if its software is running and TCI is "
               "switched on — in AetherSDR that is Tools → TCI."));
        return;
    }

    QStringList labels;
    labels.reserve(found.size());
    for (const TciServerInfo& s : found) labels << s.label();

    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this, tr("Find radios"),
        tr("These answered a TCI handshake — pick one to use:"),
        labels, 0, false, &ok);
    if (!ok || chosen.isEmpty()) return;

    const int idx = labels.indexOf(chosen);
    if (idx < 0) return;
    const TciServerInfo& pick = found.at(idx);

    // Fill the fields; nothing is saved until the operator accepts the dialog.
    m_tciHost->setText(pick.host);
    m_tciPort->setValue(pick.port);

    // Offer the announced name as a starting nickname when none is set — it is
    // the application name, so it still needs editing when two radios share
    // one server, but it beats a blank box.
    if (m_tciNickname->text().trimmed().isEmpty() && !pick.device.isEmpty())
        m_tciNickname->setText(pick.device);
}

void SettingsDialog::onAccept()
{
    m_model->setSetting("MY_CALL",          m_myCall->text().trimmed().toUpper());
    m_model->setSetting("MY_GRIDSQUARE",    m_myGrid->text().trimmed());
    m_model->setSetting("MY_STATE",         m_myState->text().trimmed().toUpper());
    m_model->setSetting("DEFAULT_TX_PWR",   QString::number(m_defaultTxPwr->value(), 'f', 1));
    m_model->setSetting("TX_PWR_FROM_RADIO", m_txPwrFromRadio->isChecked() ? "1" : "0");
    m_model->setSetting("MY_OPERATOR",      m_myOperator->text().trimmed().toUpper());

    const QString source = m_radioSource->currentData().toString();
    const bool rig = source == QLatin1String("rigctld");
    m_model->setSetting("RADIO_SOURCE", source);

    // Host and port are saved against the SELECTED source, so switching back
    // to TCI later finds its own endpoint rather than a CAT one.
    const QString host = m_tciHost->text().trimmed().isEmpty()
                             ? QStringLiteral("127.0.0.1")
                             : m_tciHost->text().trimmed();
    const QString port = QString::number(m_tciPort->value());
    m_model->setSetting(rig ? "RIGCTLD_HOST" : "TCI_HOST", host);
    m_model->setSetting(rig ? "RIGCTLD_PORT" : "TCI_PORT", port);
    m_model->setSetting("HAMLIB_RIGCTLD_PATH", m_rigctldPath->text().trimmed());
    {
        // Store the NUMBER, never the label: the label changes with the
        // Hamlib version, the number does not.
        const int mi = m_rigModel->currentIndex();
        QString model = mi >= 0 ? m_rigModel->itemData(mi).toString() : QString();
        if (model.isEmpty()) {
            bool ok = false;
            const int typed = m_rigModel->currentText().trimmed().toInt(&ok);
            if (ok && typed > 0) model = QString::number(typed);
        }
        m_model->setSetting("HAMLIB_RIG_MODEL", model);
        const int pi = m_rigPort->currentIndex();
        QString port = pi >= 0 ? m_rigPort->itemData(pi).toString() : QString();
        if (port.isEmpty()) port = m_rigPort->currentText().trimmed();
        m_model->setSetting("HAMLIB_RIG_PORT", port);
        m_model->setSetting("HAMLIB_RIG_BAUD", m_rigBaud->currentText().trimmed());
    }
    m_model->setSetting("TCI_AUTOCONNECT",  m_tciAutoConnect->isChecked() ? "1" : "0");
    m_model->setSetting("SPOT_DOUBLECLICK_TUNES",    m_spotTunes->isChecked() ? "1" : "0");
    m_model->setSetting("SPOT_DOUBLECLICK_SETS_MODE", m_spotSetsMode->isChecked() ? "1" : "0");

    // Key the nickname off the host:port as SAVED, not as loaded — if the
    // user repointed this dialog at a different radio, the name they just
    // typed belongs to the new endpoint, not the old one.
    m_model->setSetting(rig ? rigctldNicknameKey(host, port) : tciNicknameKey(host, port),
                        m_tciNickname->text().trimmed());

    m_model->setSetting("DXC_ENABLE",       m_dxcEnable->isChecked() ? "1" : "0");
    m_model->setSetting("DXC_AUTODETECT",   m_dxcAutoDetect->isChecked() ? "1" : "0");
    m_model->setSetting("DXC_HOST",         m_dxcHost->text().trimmed());
    m_model->setSetting("DXC_PORT",         QString::number(m_dxcPort->value()));
    m_model->setSetting("DXC_CALLSIGN",     m_dxcCallsign->text().trimmed().toUpper());
    // Stored value is the canonical suffix string (the part actually appended
    // to the callsign).  If the user typed something custom we save the raw
    // text; otherwise we save the data() slot of the selected combo entry.
    {
        const int idx = m_dxcLoginSuffix->currentIndex();
        QString suffix;
        if (idx >= 0 && m_dxcLoginSuffix->currentText() == m_dxcLoginSuffix->itemText(idx)) {
            suffix = m_dxcLoginSuffix->itemData(idx).toString();
        } else {
            suffix = m_dxcLoginSuffix->currentText().trimmed();
        }
        m_model->setSetting("DXC_LOGIN_SUFFIX", suffix);
    }

    m_model->setSetting("POTA_ENABLE",   m_potaEnable->isChecked() ? "1" : "0");
    m_model->setSetting("POTA_POLL_SEC", QString::number(m_potaPollSec->value()));

    m_model->setSetting("LOOKUP_WORKEDBEFORE", m_lkWorkedBefore->isChecked() ? "1" : "0");
    m_model->setSetting("LOOKUP_CTY",          m_lkCty->isChecked() ? "1" : "0");
    m_model->setSetting("LOOKUP_PROVIDER",     m_lkProvider->currentData().toString());
    m_model->setSetting("LOOKUP_USERNAME",     m_lkUser->text().trimmed());
    m_model->setSetting("LOOKUP_PASSWORD",     m_lkPass->text());
    m_model->setSetting("LOOKUP_CALLOOK",      m_lkCallook->isChecked() ? "1" : "0");

    m_model->setSetting("CONTEST_MODE",     m_contestMode->isChecked() ? "1" : "0");
    m_model->setSetting("CONTEST_ID",       m_contestId->currentText().trimmed().toUpper());
    m_model->setSetting("CONTEST_STX_NEXT", QString::number(m_stxNext->value()));

    m_model->setSetting("MQTT_ENABLE",        m_mqttEnable->isChecked() ? "1" : "0");
    m_model->setSetting("MQTT_HOST",          m_mqttHost->text().trimmed());
    m_model->setSetting("MQTT_PORT",          QString::number(m_mqttPort->value()));
    m_model->setSetting("MQTT_USERNAME",      m_mqttUser->text().trimmed());
    m_model->setSetting("MQTT_PASSWORD",      m_mqttPass->text());
    m_model->setSetting("MQTT_TOPIC_PREFIX",  m_mqttPrefix->text().trimmed());
    m_model->setSetting("MQTT_HA_DISCOVERY",  m_mqttHaDiscovery->isChecked() ? "1" : "0");
    m_model->setSetting("MQTT_PUBLISH_QSO",   m_mqttPublishQso->isChecked() ? "1" : "0");

    m_model->setSetting("CABRILLO_NAME",      m_cbName->text().trimmed());
    m_model->setSetting("CABRILLO_ADDRESS",   m_cbAddress->text().trimmed());
    m_model->setSetting("CABRILLO_EMAIL",     m_cbEmail->text().trimmed());
    m_model->setSetting("CABRILLO_CLUB",      m_cbClub->text().trimmed());
    m_model->setSetting("CABRILLO_LOCATION",  m_cbLocation->text().trimmed());
    m_model->setSetting("CABRILLO_CAT_OPERATOR",    m_cbCatOp->currentText());
    m_model->setSetting("CABRILLO_CAT_ASSISTED",    m_cbCatAssisted->currentText());
    m_model->setSetting("CABRILLO_CAT_BAND",        m_cbCatBand->currentText());
    m_model->setSetting("CABRILLO_CAT_MODE",        m_cbCatMode->currentText());
    m_model->setSetting("CABRILLO_CAT_POWER",       m_cbCatPower->currentText());
    m_model->setSetting("CABRILLO_CAT_STATION",     m_cbCatStation->currentText());
    m_model->setSetting("CABRILLO_CAT_TRANSMITTER", m_cbCatTransmitter->currentText());

    {
        CwKeyerConfig cw;
        cw.enabled = m_cwEnable->isChecked();
        for (int i = 0; i < m_cwTexts.size() && i < cw.macros.size(); ++i) {
            cw.macros[i].label = m_cwLabels[i]->text();
            cw.macros[i].text  = m_cwTexts[i]->text();
        }
        cw.cut.cutRst = m_cwCutRst->isChecked();
        cw.cut.cutNr  = m_cwCutNr->isChecked();
        cw.cut.cutOne = m_cwCutOne->isChecked();
        cw.name = m_cwName->text();
        saveCwKeyerConfig(cw, [this](const QString& k, const QString& v) { m_model->setSetting(k, v); });
    }

    accept();
}

} // namespace ShackBook
