#include "CwKeyerPanel.h"

#include "CwKeyer.h"

#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace ShackBook {

namespace {

const char* kStopStyle =
    "QPushButton { background:#c62828; color:white; font-weight:bold;"
    " padding:6px 16px; border-radius:4px; }"
    "QPushButton:pressed { background:#8e0000; }";
const char* kSendingStyle = "color:#c62828; font-weight:bold;";
const char* kWarnStyle    = "color:#b26a00;";
const char* kInfoStyle    = "color:palette(mid);";

} // namespace

CwKeyerPanel::CwKeyerPanel(CwKeyer* keyer, QWidget* parent)
    : QDockWidget(tr("CW keyer"), parent)
    , m_keyer(keyer)
{
    setObjectName(QStringLiteral("CwKeyerPanel"));
    // Not closable: a hidden panel with live F-keys would send CW with no
    // visible sign of what went out. The keyer is turned off in Settings.
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(6, 4, 6, 4);
    v->setSpacing(4);

    auto* speedRow = new QHBoxLayout;
    speedRow->addStretch();
    m_speedLabel = new QLabel;
    m_slower = new QPushButton(QStringLiteral("−"));
    m_faster = new QPushButton(QStringLiteral("+"));
    for (QPushButton* b : {m_slower, m_faster}) {
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedWidth(28);
    }
    m_slower->setToolTip(tr("1 WPM slower"));
    m_faster->setToolTip(tr("1 WPM faster"));
    connect(m_slower, &QPushButton::clicked, this, [this] { if (m_speed > 0) m_keyer->setSpeed(m_speed - 1); });
    connect(m_faster, &QPushButton::clicked, this, [this] { if (m_speed > 0) m_keyer->setSpeed(m_speed + 1); });
    speedRow->addWidget(m_speedLabel);
    speedRow->addWidget(m_slower);
    speedRow->addWidget(m_faster);
    v->addLayout(speedRow);

    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    for (int i = 0; i < 8; ++i) {
        auto* b = new QPushButton;
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoRepeat(false);
        connect(b, &QPushButton::clicked, this, [this, i] { trigger(i); });
        grid->addWidget(b, i / 4, i % 4);
        m_buttons << b;
    }
    v->addLayout(grid);

    auto* statusRow = new QHBoxLayout;
    m_status = new QLabel;
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_stop = new QPushButton(tr("Esc  STOP"));
    m_stop->setFocusPolicy(Qt::NoFocus);
    m_stop->setStyleSheet(QString::fromLatin1(kStopStyle));
    m_stop->setToolTip(tr("Stop sending (Esc works anywhere in ShackBook)"));
    connect(m_stop, &QPushButton::clicked, this, &CwKeyerPanel::stopNow);
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_stop);
    v->addLayout(statusRow);

    setWidget(body);

    connect(m_keyer, &CwKeyer::stateChanged, this, [this] { refresh(); });
    connect(m_keyer, &CwKeyer::sendingTextChanged, this, [this] { refresh(); });
    connect(m_keyer, &CwKeyer::radioDidNotKey, this,
            [this] { showMessage(m_keyer->lastError(), true); });
    connect(m_keyer, &CwKeyer::stopNotConfirmed, this,
            [this] { showMessage(m_keyer->lastError(), true); });

    refreshLabels();
    refresh();
}

void CwKeyerPanel::trigger(int index)
{
    const CwKeyer::Result r = m_keyer->sendMacro(index);
    if (r != CwKeyer::Result::Sent) {
        showMessage(m_keyer->lastError(), true);
        return;
    }
    // Sent, possibly with a warning about characters that were dropped.
    m_message = m_keyer->lastError();
    m_messageWarning = !m_message.isEmpty();
    refresh();
}

void CwKeyerPanel::stopNow()
{
    // Shown once the keyer is Idle again: at once if the radio was not
    // transmitting, or after "Stopping…" once it unkeys.
    if (m_keyer->isEnabled() && m_keyer->state() != CwKeyer::State::Idle) {
        m_message = tr("Stopped");
        m_messageWarning = false;
    }
    m_keyer->stop();
    refresh();
}

void CwKeyerPanel::refreshLabels()
{
    const auto& macros = m_keyer->macros();
    for (int i = 0; i < m_buttons.size(); ++i) {
        const QString label = i < macros.size() ? macros[i].label : QString();
        m_buttons[i]->setText(label.isEmpty() ? QStringLiteral("F%1").arg(i + 1)
                                              : QStringLiteral("F%1 %2").arg(i + 1).arg(label));
        m_buttons[i]->setToolTip(i < macros.size() ? macros[i].text : QString());
    }
}

void CwKeyerPanel::setRadioState(bool connected, const QString& mode, bool tciLink)
{
    m_connected = connected;
    m_mode = mode;
    m_tciLink = tciLink;
    refresh();
}

void CwKeyerPanel::setSpeed(int wpm)
{
    m_speed = wpm;
    refresh();
}

QString CwKeyerPanel::statusText() const
{
    return m_status->text();
}

void CwKeyerPanel::showMessage(const QString& text, bool warning)
{
    m_message = text;
    m_messageWarning = warning;
    refresh();
}

void CwKeyerPanel::refresh()
{
    m_speedLabel->setText(m_speed > 0 ? tr("%1 wpm").arg(m_speed) : tr("— wpm"));
    m_slower->setEnabled(m_speed > 0);
    m_faster->setEnabled(m_speed > 0);

    // Why nothing can be sent, if anything stops it. The keyer checks the
    // same things itself; this is so the operator sees it BEFORE pressing.
    QString blocked;
    if (!m_tciLink)                 blocked = tr("The CW keyer needs a TCI radio link (Settings → TCI)");
    else if (!m_connected)          blocked = tr("Not connected to the radio");
    else if (!isCwMode(m_mode))     blocked = m_mode.isEmpty()
                                        ? tr("Waiting for the radio's mode")
                                        : tr("The radio is in %1, not CW").arg(m_mode);
    for (QPushButton* b : m_buttons) b->setEnabled(blocked.isEmpty());

    switch (m_keyer->state()) {
    case CwKeyer::State::Sending:
        m_status->setStyleSheet(QString::fromLatin1(kSendingStyle));
        m_status->setText(tr("● SENDING  \"%1\"").arg(m_keyer->sendingText())
                          + (m_messageWarning ? QStringLiteral("   ") + m_message : QString()));
        return;
    case CwKeyer::State::Stopping:
        m_status->setStyleSheet(QString::fromLatin1(kSendingStyle));
        m_status->setText(tr("● Stopping…"));
        return;
    case CwKeyer::State::Idle:
        break;
    }

    if (!m_message.isEmpty() && m_messageWarning) {
        m_status->setStyleSheet(QString::fromLatin1(kWarnStyle));
        m_status->setText(m_message);
    } else if (!blocked.isEmpty()) {
        m_status->setStyleSheet(QString::fromLatin1(kInfoStyle));
        m_status->setText(blocked);
    } else {
        m_status->setStyleSheet(QString::fromLatin1(kInfoStyle));
        m_status->setText(m_message.isEmpty() ? tr("Ready") : m_message);
    }
}

bool CwStopKeyFilter::eventFilter(QObject* watched, QEvent* event)
{
    // A key press reaches an application filter once for its window and
    // again for every widget it propagates through. Acting only at the
    // window, which every press from the keyboard passes first, stops once
    // per press, whichever widget, dialog or menu has focus.
    if (event->type() == QEvent::KeyPress && watched->isWindowType()) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && !key->isAutoRepeat())
            m_panel->stopNow();
    }
    return QObject::eventFilter(watched, event);   // never consumed
}

} // namespace ShackBook
