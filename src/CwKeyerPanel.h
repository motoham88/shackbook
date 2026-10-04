#pragma once

// CwKeyerPanel — the CW keyer's dock: F1-F8, speed, status, STOP (#32).
//
//   ┌ CW keyer ───────────────────────────────── 25 wpm [−][+] ┐
//   │ [F1 CQ] [F2 Exch] [F3 TU] [F4 My call]                   │
//   │ [F5 His call] [F6 Again] [F7 ?] [F8 Nr?]                 │
//   │ ● SENDING  "TU G0JKN"                     [Esc  STOP]    │
//   └──────────────────────────────────────────────────────────┘
//
// The panel holds no rules of its own: every button goes through CwKeyer,
// which decides whether anything is sent. Its job is to show the operator
// exactly what is going out, or exactly why nothing did.
//
// The buttons never take keyboard focus, so clicking one leaves the cursor in
// the call field, the way a run operator expects.

#include <QDockWidget>
#include <QVector>

class QLabel;
class QPushButton;

namespace ShackBook {

class CwKeyer;

class CwKeyerPanel : public QDockWidget {
    Q_OBJECT

public:
    explicit CwKeyerPanel(CwKeyer* keyer, QWidget* parent = nullptr);

    // Send F<index+1>, as a button click or an F-key does, and show the
    // result. The only path from the panel to sendMacro().
    void trigger(int index);
    // Esc / STOP: stop, and say so.
    void stopNow();

    // Re-read the button labels after the macros change.
    void refreshLabels();

    // What the radio link is doing, for the disabled-state reason.
    // `tciLink` is false when the log uses rigctld, which has no CW path yet.
    void setRadioState(bool connected, const QString& mode, bool tciLink);
    // The radio's reported speed; 0 when unknown.
    void setSpeed(int wpm);

    // For tests.
    QString statusText() const;
    QPushButton* macroButton(int index) const { return m_buttons.value(index); }
    QPushButton* stopButton() const { return m_stop; }

private:
    void refresh();
    void showMessage(const QString& text, bool warning);

    CwKeyer* m_keyer;

    QVector<QPushButton*> m_buttons;
    QPushButton* m_stop{};
    QPushButton* m_slower{};
    QPushButton* m_faster{};
    QLabel*      m_speedLabel{};
    QLabel*      m_status{};

    bool    m_connected{false};
    bool    m_tciLink{true};
    QString m_mode;
    int     m_speed{0};
    QString m_message;        // the last result to show while Idle
    bool    m_messageWarning{false};
};

// Esc stops CW anywhere in the application while the keyer is enabled,
// including in a modal dialog, where the main window's shortcuts cannot
// reach. Installed on the QApplication only while the keyer is on. The key
// is never consumed: a dialog still closes on Esc as well.
class CwStopKeyFilter : public QObject {
    Q_OBJECT
public:
    explicit CwStopKeyFilter(CwKeyerPanel* panel, QObject* parent = nullptr)
        : QObject(parent), m_panel(panel) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    CwKeyerPanel* m_panel;
};

} // namespace ShackBook
