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
//   CW_F<n>_TEXT          message text with {TOKENS}
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

// Missing keys fall back to the defaults; a macro with no stored text keeps
// its default rather than becoming an empty button.
CwKeyerConfig loadCwKeyerConfig(const CwSettingGetter& get);
void          saveCwKeyerConfig(const CwKeyerConfig& cfg, const CwSettingSetter& set);

} // namespace ShackBook
