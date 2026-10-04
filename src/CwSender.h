#pragma once

// ICwSender — the radio link the CW keyer (#32) sends through.
//
// CwKeyer owns every rule about WHEN CW may be sent; a sender only knows how.
// Keeping that split behind an interface is what lets a second backend
// (Hamlib's send_morse / stop_morse, via RigctldClient) slot in later without
// the safety rules being written twice.
//
// Implementations must not queue: a send while the link is down returns false
// and is gone, never replayed on reconnect.

#include "TciClient.h"

#include <QString>

namespace ShackBook {

class ICwSender {
public:
    virtual ~ICwSender() = default;

    virtual bool    cwConnected() const = 0;
    // The radio's current mode, uppercased as reported (e.g. "CW", "CWR",
    // "USB"); empty when unknown.
    virtual QString cwMode() const = 0;
    // Keys the radio. False when nothing was sent.
    virtual bool    sendCwText(const QString& text) = 0;
    // Best effort, even when no send is thought to be in progress. False
    // when nothing could be written.
    virtual bool    stopCwText() = 0;
    virtual bool    setCwTextSpeed(int wpm) = 0;
    // The radio's reported speed (WPM), 0 when unknown.
    virtual int     cwTextSpeed() const = 0;
    // Ask the radio for its speed; the answer updates cwTextSpeed(). A read,
    // not a transmit. Needed because AetherSDR does not report the speed
    // until asked. False when nothing was written.
    virtual bool    requestCwTextSpeed() = 0;
};

// The TCI backend: a thin adapter over TciClient's CW methods. The keyer's
// state also needs TciClient's transmittingChanged / connectionChanged
// signals, which the owner wires to CwKeyer's slots.
class TciCwSender : public ICwSender {
public:
    explicit TciCwSender(TciClient* tci) : m_tci(tci) {}

    bool    cwConnected() const override              { return m_tci && m_tci->connected(); }
    QString cwMode() const override                   { return m_tci ? m_tci->currentMode() : QString{}; }
    bool    sendCwText(const QString& text) override  { return m_tci && m_tci->sendCw(text); }
    bool    stopCwText() override                     { return m_tci && m_tci->stopCw(); }
    bool    setCwTextSpeed(int wpm) override          { return m_tci && m_tci->setCwSpeed(wpm); }
    int     cwTextSpeed() const override              { return m_tci ? m_tci->cwSpeedWpm() : 0; }
    bool    requestCwTextSpeed() override             { return m_tci && m_tci->requestCwSpeed(); }

private:
    TciClient* m_tci;
};

} // namespace ShackBook
