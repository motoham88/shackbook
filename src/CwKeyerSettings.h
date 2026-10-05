#pragma once

// CwKeyerSettings — the CW keyer's settings, per log (#32).
//
// Stored with the rest of a log's settings, so each operator's log carries
// its own messages and its own enable. Loading and saving go through plain
// get/set functions rather than LogbookModel so they can be tested without a
// database.
//
// Keys:
//   CW_KEYER_ENABLED      "1" / "0"; ABSENT MEANS OFF
//   CW_F<n>_LABEL         button label, n = 1..8
//   CW_F<n>_TEXT          message text with {TOKENS}; ABSENT MEANS THE
//                         DEFAULT, while a stored empty text is a message
//                         the operator cleared, and stays cleared
//   CW_CUT_RST / CW_CUT_NR / CW_CUT_ONE   cut-number options
//   CW_NAME               what {NAME} sends (a first name, not the
//                         Cabrillo full name)

#include "CwKeyer.h"

#include <QString>
#include <QVector>

#include <functional>

namespace ShackBook {

struct CwKeyerConfig {
    bool             enabled = false;
    QVector<CwMacro> macros = defaultCwMacros();
    CwCutOptions     cut;
    QString          name;
};

using CwSettingGetter = std::function<QString(const QString& key, const QString& def)>;
using CwSettingSetter = std::function<void(const QString& key, const QString& value)>;

// Missing keys fall back to the defaults. A message cleared in Settings
// loads as empty, not as its default: the keyer refuses to send it and says
// so, rather than quietly sending something the operator removed.
CwKeyerConfig loadCwKeyerConfig(const CwSettingGetter& get);
// Writes only the keys whose value differs from what `get` loads now, so an
// OK in Settings with nothing changed on this tab writes nothing.
void          saveCwKeyerConfig(const CwKeyerConfig& cfg, const CwSettingGetter& get,
                                const CwSettingSetter& set);

} // namespace ShackBook
