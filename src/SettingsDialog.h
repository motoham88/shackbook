#pragma once

// SettingsDialog — operator / contest / Cabrillo / TCI server settings.
//
// Lives in tabs:
//   • Operator — MY_CALL, MY_GRIDSQUARE, MY_STATE, default TX power, OPERATOR
//   • TCI      — host, port, autoconnect on launch
//   • Lookup   — callsign → name/QTH lookup chain (worked-before, cty.dat,
//                QRZ XML / HamQTH / callook.info online providers)
//   • Contest  — contest mode toggle, contest id, next STX serial
//   • CW keyer — the master enable (off by default), F1-F8 messages,
//                cut numbers, {NAME} (#32)
//   • Cabrillo — header fields used by Cabrillo export
//
// All values are persisted to the LogbookModel's settings table on Save.

#include <QDialog>
#include <QVector>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;

namespace ShackBook {

class LogbookModel;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(LogbookModel* model, QWidget* parent = nullptr);

private slots:
    void onAccept();
    // Scan for TCI servers and fill in the host/port of whichever is chosen.
    void onScanForRadios();
    // Say whether Hamlib is present, and what to do if it is not. Rerun
    // whenever the source or the configured path changes.
    void refreshHamlibGuidance();
    // Fill the rig-model picker from the installed Hamlib, or the
    // bundled fallback when it is absent. Rerun when the path changes.
    void refreshRigModels();
    // Re-read the serial ports. They come and go with USB cables, so
    // this is read fresh rather than cached.
    void refreshSerialPorts();
    // Show the exact command to paste, built from the three pickers.
    void refreshRigCommand();

private:
    void buildUI();
    void populate();

    LogbookModel* m_model;

    // Operator
    QLineEdit*  m_myCall{};
    QLineEdit*  m_myGrid{};
    QLineEdit*  m_myState{};
    QDoubleSpinBox* m_defaultTxPwr{};
    QCheckBox*      m_txPwrFromRadio{};
    QLineEdit*  m_myOperator{};

    // TCI
    QLineEdit*  m_tciHost{};
    QSpinBox*   m_tciPort{};
    QComboBox*   m_radioSource{};
    QLabel*      m_hamlibStatus{};
    QLineEdit*   m_rigctldPath{};
    QComboBox*   m_rigModel{};
    QComboBox*   m_rigPort{};
    QComboBox*   m_rigBaud{};
    QLabel*      m_rigCommand{};
    QPushButton* m_tciScan{};
    QLineEdit*  m_tciNickname{};
    QCheckBox*  m_tciAutoConnect{};
    QCheckBox*  m_spotTunes{};
    QCheckBox*  m_spotSetsMode{};

    // DX Cluster
    QCheckBox*  m_dxcEnable{};
    QCheckBox*  m_dxcAutoDetect{};
    QLineEdit*  m_dxcHost{};
    QSpinBox*   m_dxcPort{};
    QLineEdit*  m_dxcCallsign{};
    QComboBox*  m_dxcLoginSuffix{};
    QLabel*     m_dxcDetected{};

    // POTA (HTTP poll, independent of cluster)
    QCheckBox*  m_potaEnable{};
    QSpinBox*   m_potaPollSec{};

    // Callsign lookup
    QCheckBox*  m_lkWorkedBefore{};
    QCheckBox*  m_lkCty{};
    QComboBox*  m_lkProvider{};
    QLineEdit*  m_lkUser{};
    QLineEdit*  m_lkPass{};
    QCheckBox*  m_lkCallook{};

    // Contest
    QCheckBox*  m_contestMode{};

    // MQTT (#24)
    QCheckBox*  m_mqttEnable{};
    QLineEdit*  m_mqttHost{};
    QSpinBox*   m_mqttPort{};
    QLineEdit*  m_mqttUser{};
    QLineEdit*  m_mqttPass{};
    QLineEdit*  m_mqttPrefix{};
    QCheckBox*  m_mqttHaDiscovery{};
    QCheckBox*  m_mqttPublishQso{};
    QComboBox*  m_contestId{};
    QSpinBox*   m_stxNext{};

    // CW keyer (#32)
    QCheckBox*          m_cwEnable{};
    QVector<QLineEdit*> m_cwLabels;
    QVector<QLineEdit*> m_cwTexts;
    QCheckBox*          m_cwCutRst{};
    QCheckBox*          m_cwCutNr{};
    QCheckBox*          m_cwCutOne{};
    QLineEdit*          m_cwName{};

    // Cabrillo
    QLineEdit*  m_cbName{};
    QLineEdit*  m_cbAddress{};
    QLineEdit*  m_cbEmail{};
    QLineEdit*  m_cbClub{};
    QLineEdit*  m_cbLocation{};
    QComboBox*  m_cbCatOp{};
    QComboBox*  m_cbCatAssisted{};
    QComboBox*  m_cbCatBand{};
    QComboBox*  m_cbCatMode{};
    QComboBox*  m_cbCatPower{};
    QComboBox*  m_cbCatStation{};
    QComboBox*  m_cbCatTransmitter{};
};

} // namespace ShackBook
