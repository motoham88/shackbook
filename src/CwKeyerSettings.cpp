#include "CwKeyerSettings.h"

namespace ShackBook {

namespace {

QString key(const char* fmt, int n) { return QString::fromLatin1(fmt).arg(n); }
QString onOff(bool b) { return b ? QStringLiteral("1") : QStringLiteral("0"); }

} // namespace

CwKeyerConfig loadCwKeyerConfig(const CwSettingGetter& get)
{
    CwKeyerConfig cfg;
    // Only an explicit "1" turns the keyer on: a missing, empty or garbled
    // value is off, because off is the safe reading of anything unclear.
    cfg.enabled = get(QStringLiteral("CW_KEYER_ENABLED"), QStringLiteral("0")) == QLatin1String("1");

    for (int i = 0; i < cfg.macros.size(); ++i) {
        CwMacro& m = cfg.macros[i];
        const QString label = get(key("CW_F%1_LABEL", i + 1), QString()).trimmed();
        const QString text  = get(key("CW_F%1_TEXT",  i + 1), QString()).trimmed();
        if (!text.isEmpty()) {
            m.text  = text;
            m.label = label;   // a custom message with no label shows just "F<n>"
        } else if (!label.isEmpty()) {
            m.label = label;
        }
    }

    cfg.cut.cutRst = get(QStringLiteral("CW_CUT_RST"), QStringLiteral("1")) == QLatin1String("1");
    cfg.cut.cutNr  = get(QStringLiteral("CW_CUT_NR"),  QStringLiteral("1")) == QLatin1String("1");
    cfg.cut.cutOne = get(QStringLiteral("CW_CUT_ONE"), QStringLiteral("0")) == QLatin1String("1");
    cfg.name = get(QStringLiteral("CW_NAME"), QString()).trimmed();
    return cfg;
}

void saveCwKeyerConfig(const CwKeyerConfig& cfg, const CwSettingSetter& set)
{
    set(QStringLiteral("CW_KEYER_ENABLED"), onOff(cfg.enabled));
    for (int i = 0; i < cfg.macros.size(); ++i) {
        set(key("CW_F%1_LABEL", i + 1), cfg.macros[i].label.trimmed());
        set(key("CW_F%1_TEXT",  i + 1), cfg.macros[i].text.trimmed());
    }
    set(QStringLiteral("CW_CUT_RST"), onOff(cfg.cut.cutRst));
    set(QStringLiteral("CW_CUT_NR"),  onOff(cfg.cut.cutNr));
    set(QStringLiteral("CW_CUT_ONE"), onOff(cfg.cut.cutOne));
    set(QStringLiteral("CW_NAME"),    cfg.name.trimmed());
}

} // namespace ShackBook
