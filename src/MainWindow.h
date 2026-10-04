#pragma once

// MainWindow — top-level ShackBook UI.
//
// Layout (top → bottom):
//   • MenuBar    — File / Edit / Tools / Help
//   • Header     — operator callsign | current freq / band / mode | TCI dot
//   • QuickEntry — CALL / RST sent / RST rcvd / comment / [SAVE] + DUPE badge
//                  (with optional contest sub-row when contestMode is on)
//   • Filter     — text / band / mode / contest / Reset
//   • Table      — QSO table, newest first
//   • Actions    — count label | [New] [Edit] [Delete]
//   • StatusBar  — TCI server endpoint | DB path
//
// Owns the LogbookModel and a TciClient singleton.

#include "CtyDat.h"
#include "Qso.h"

#include <QMainWindow>
#include <QVector>

#include <memory>

class QAction;
class QCloseEvent;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QShortcut;
class QTableWidget;
class QTimer;

namespace ShackBook {

class LogbookModel;
class TciClient;
class MqttPublisher;
class RigctldClient;
class RigctldSupervisor;
class SpotIndex;
class DxClusterClient;
class PotaClient;
class CallsignLookup;
class SectionMapDialog;
class GridMapDialog;
class QsoPartyDialog;
class SessionMapDialog;
class AprsActivityDialog;
struct SpotData;
class ICwSender;
class CwKeyer;
class CwKeyerPanel;
class CwStopKeyFilter;
struct CwContext;
namespace Server { class WsjtxAdifReceiver; }

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    // Stops CW before the window closes and the link goes with it (#32).
    void closeEvent(QCloseEvent* event) override;

private slots:
    // Quick-entry
    void onCallEdited(const QString& text);
    void onSaveQso();

    // Table / filter
    void onFilterChanged();
    void onResetFilter();
    void onLogbookChanged();
    void onContextSettingsChanged();

    // Actions
    void onNewQso();
    void onEditQso();
    void onDeleteQso();
    void onSettings();
    void onSwitchLog();
    void onShowAwards();
    void onShowQsoParty();
    void onShowSectionMap();
    void onShowGridMap();
    void onShowSessionMap();
    void onShowAprsActivity();
    void onHowFar();
    void onImportAdif();
    void onExportAdif();
    void onExportCabrillo();
    void onAbout();

    // TCI
    void onConnectTci();
    // Scan for TCI servers and offer what answered a real handshake.
    void onFindRadios();
    // Which kind of radio link is in use: TCI (AetherSDR/ExpertSDR/SunSDR) or
    // Hamlib rigctld (everything else). Stored per log as RADIO_SOURCE.
    bool usingRigctld() const;
    // Connect whichever source RADIO_SOURCE selects.
    void connectActiveSource();
    void onDisconnectTci();
    void onTciConnectionChanged(bool connected);
    void onTciFrequencyChanged(double mhz);
    void onTciModeChanged(const QString& mode);

    // DX cluster (Phase 2)
    void onClusterConnectionChanged(bool connected);
    void onClusterSpotReceived(const ShackBook::SpotData& spot);
    void onClusterRawLine(const QString& line);
    void purgeStaleSpots();
    void onShowClusterLog();
    void onShowSpotIndex();
    void onShowLotw();

    // POTA (Phase 3)
    void onPotaSpotReceived(const ShackBook::SpotData& spot);
    void onPotaPollCompleted(int spots, const QString& errorOrEmpty);

private:
    void buildUI();
    void buildMenus();
    void refreshHeader();
    void refreshQuickEntry();
    void refreshContestUI();
    void refreshTable();
    // Operator chooser: pick / create the per-callsign log and (re)open it.
    // At startup a cancel falls back to the last-used (or legacy) log; on a
    // live switch a cancel keeps the current log open.
    bool chooseAndOpenLog(bool startup);
    void populateBandFilter();
    void populateModeFilter();
    void populateContestFilter();
    void refreshDupBadge();
    // Name the link by the source actually configured (CAT vs TCI) and
    // show whether it is up. Called from startup, a settings change and
    // the connection signal -- see #14.
    void refreshLinkLabel();
    void refreshStatusBar();
    // "ShackBook — G0JKN — 0.5.0-dev". Carries the version always, and the
    // -dev tag on any build that is not an installed release.
    static QString windowTitleFor(const QString& operatorCall);
    // The radio to stamp on a QSO, resolved as: per-connection nickname →
    // the TCI-announced device name → empty. Empty when not connected, so an
    // offline QSO is left unattributed rather than credited to the last radio.
    QString currentRadioName() const;
    // TX power for a new QSO: the radio's measured power when TCI supplies
    // one and the operator has not turned that off, otherwise the default.
    double  txPowerForNewQso() const;
    void applyAutoConnectFromSettings();
    void applyClusterConfigFromSettings();
    void applyPotaConfigFromSettings();
    void applyLookupConfigFromSettings();
    // MQTT shack status for Home Assistant (#24). Publishing is best effort:
    // none of these can block or fail logging.
    void applyMqttConfigFromSettings();
    void publishRadioStatus();
    void publishQsoStatus(qint64 addedQsoId = -1);
    void tryAutofillFromSpot();
    // Kick the callsign-lookup chain (worked-before → cty.dat → online).
    // Results land in m_lookupFill and merge into empty fields at save.
    void startCallsignLookup(const QString& call);
    qint64 selectedQsoId() const;

    // ── CW keyer (#32) ────────────────────────────────────────────────
    // Built once; applyCwKeyerFromSettings() turns it on or off from the
    // open log's CW_KEYER_ENABLED. Called only from startup, Settings and a
    // log switch, never from a QSO save: nothing on the save path touches
    // the keyer.
    void buildCwKeyer();
    void applyCwKeyerFromSettings();
    void refreshCwPanelRadioState();
    // Before the TCI link is closed: a server like AetherSDR can keep sending
    // what is already in the radio's CWX buffer after the link drops, and
    // once it has dropped no stop can reach it.
    void stopCwBeforeLinkGoes();
    // QuickEntry and settings as they are right now, for the keyer's tokens.
    CwContext cwContext() const;

    LogbookModel*    m_model{nullptr};
    TciClient*       m_tci{nullptr};
    // The non-TCI path: Hamlib rigctld, for Icom/Yaesu/Kenwood and anything
    // else Hamlib drives. Exactly one of the two is connected at a time —
    // both following the same radio would fight over the CAT port.
    RigctldClient*   m_rigctld{nullptr};
    // "Hamlib not found" is said once per log, not on every reconnect attempt —
    // the backoff would otherwise turn a true statement into nagging.
    bool             m_warnedNoHamlib{false};
    SpotIndex*       m_spotIndex{nullptr};
    DxClusterClient* m_dxc{nullptr};
    PotaClient*      m_pota{nullptr};
    MqttPublisher*   m_mqtt{nullptr};
    QTimer*          m_mqttDayTimer{nullptr};   // rolls qso/count_today over at 00:00 UTC
    QTimer*          m_spotPurgeTimer{nullptr};
    QPlainTextEdit*  m_dxcLog{nullptr};   // diagnostic — see onShowClusterLog()

    // Cached TCI state
    double  m_curFreqMhz{0.0};
    QString m_curBand;
    QString m_curMode;       // ADIF base mode
    QString m_curSubmode;    // ADIF submode (USB/LSB)
    QString m_rawTciMode;    // for display only

    // Last call we auto-filled — used to decide whether the call field
    // is "ours" (safe to overwrite on the next spot click) or the
    // operator's typed input (must be left alone).
    QString m_lastAutofilledCall;
    QString m_lastAutofilledComment;

    // Callsign lookup chain
    CtyDat           m_cty;              // offline prefix → country/zones
    CallsignLookup*  m_lookup{nullptr};  // online QRZ / HamQTH / callook
    QTimer*          m_lookupDebounce{nullptr};  // typed-call settle timer
    SectionMapDialog* m_sectionMap{nullptr};     // non-modal, lazily created
    GridMapDialog*    m_gridMap{nullptr};        // non-modal, lazily created
    SessionMapDialog* m_sessionMap{nullptr};     // non-modal, lazily created
    AprsActivityDialog* m_aprsActivity{nullptr}; // non-modal, lazily created
    Qso     m_lookupFill;        // pending details for the next save…
    QString m_lookupFillCall;    // …valid only while the call field matches
    QString m_operatorCall;          // whose log is open (multi-log)
    Server::WsjtxAdifReceiver* m_wsjtx{};  // WSJT-X UDP/ADIF → active log

    // CW keyer (#32). The F-key shortcuts and the Esc filter exist only
    // while the keyer is enabled, so for anyone who has not opted in the
    // keys do nothing at all.
    std::unique_ptr<ICwSender> m_cwSender;
    CwKeyer*                   m_cwKeyer{};
    CwKeyerPanel*              m_cwPanel{};
    CwStopKeyFilter*           m_cwStopFilter{};
    QVector<QShortcut*>        m_cwShortcuts;

    // Header
    QLabel*  m_myCallLabel{};
    QLabel*  m_freqLabel{};
    QLabel*  m_bandLabel{};
    QLabel*  m_modeLabel{};
    QLabel*  m_tciDot{};
    QLabel*  m_tciStatus{};

    // Quick entry
    QLineEdit*   m_callEdit{};
    QLineEdit*   m_rstSentEdit{};
    QLineEdit*   m_rstRcvdEdit{};
    QLineEdit*   m_commentEdit{};
    QPushButton* m_saveBtn{};
    QLabel*      m_dupBadge{};

    // Contest sub-row
    QFrame*    m_contestFrame{};
    QLabel*    m_contestIdLabel{};
    QLineEdit* m_stxEdit{};
    QLineEdit* m_stxStringEdit{};
    QLineEdit* m_srxEdit{};
    QLineEdit* m_srxStringEdit{};

    // Filter row
    QLineEdit*   m_filterText{};
    QComboBox*   m_filterBand{};
    QComboBox*   m_filterMode{};
    QComboBox*   m_filterContest{};
    QPushButton* m_resetBtn{};

    // Table + actions
    QTableWidget* m_table{};
    QLabel*       m_countLabel{};
    QPushButton*  m_howFarBtn{};
    QPushButton*  m_newBtn{};
    QPushButton*  m_editBtn{};
    QPushButton*  m_deleteBtn{};

    // Status bar permanent widgets
    QLabel* m_sbTci{};
    QLabel* m_sbDxc{};
    QLabel* m_sbDb{};

    // Menu actions
    QAction* m_actSwitchLog{};
    QAction* m_actAwards{};
    QAction* m_actSectionMap{};
    QAction* m_actAprsActivity{};
    QAction* m_actHowFarMap{};
    QAction* m_actImportAdif{};
    QAction* m_actExportAdif{};
    QAction* m_actExportCab{};
    QAction* m_actSettings{};
    QAction* m_actFindRadios{};
    // Starts rigctld only when nothing is already serving, and stops only
    // what it started. See RigctldSupervisor.h. #13.
    RigctldSupervisor* m_rigctldSupervisor{};
    QAction* m_actConnectTci{};
    QAction* m_actDisconnectTci{};
    QAction* m_actDxcLog{};
    QAction* m_actSpotIndex{};
    QAction* m_actLotw{};
    QAction* m_actNew{};
    QAction* m_actEdit{};
    QAction* m_actDelete{};
    QAction* m_actAbout{};
    QAction* m_actQuit{};
};

} // namespace ShackBook
