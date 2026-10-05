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

    // Each key falls back to its own default only when absent: a text the
    // operator cleared is stored as empty, and stays that way.
    for (int i = 0; i < cfg.macros.size(); ++i) {
        CwMacro& m = cfg.macros[i];
        m.label = get(key("CW_F%1_LABEL", i + 1), m.label).trimmed();
        m.text  = get(key("CW_F%1_TEXT",  i + 1), m.text).trimmed();
    }

    cfg.cut.cutRst = get(QStringLiteral("CW_CUT_RST"), QStringLiteral("1")) == QLatin1String("1");
    cfg.cut.cutNr  = get(QStringLiteral("CW_CUT_NR"),  QStringLiteral("1")) == QLatin1String("1");
    cfg.cut.cutOne = get(QStringLiteral("CW_CUT_ONE"), QStringLiteral("0")) == QLatin1String("1");
    cfg.name = get(QStringLiteral("CW_NAME"), QString()).trimmed();
    return cfg;
}

void saveCwKeyerConfig(const CwKeyerConfig& cfg, const CwSettingGetter& get,
                       const CwSettingSetter& set)
{
    const CwKeyerConfig now = loadCwKeyerConfig(get);
    auto put = [&](const QString& k, const QString& v, const QString& was) {
        if (v != was) set(k, v);
    };
    put(QStringLiteral("CW_KEYER_ENABLED"), onOff(cfg.enabled), onOff(now.enabled));
    for (int i = 0; i < cfg.macros.size() && i < now.macros.size(); ++i) {
        put(key("CW_F%1_LABEL", i + 1), cfg.macros[i].label.trimmed(), now.macros[i].label);
        put(key("CW_F%1_TEXT",  i + 1), cfg.macros[i].text.trimmed(),  now.macros[i].text);
    }
    put(QStringLiteral("CW_CUT_RST"), onOff(cfg.cut.cutRst), onOff(now.cut.cutRst));
    put(QStringLiteral("CW_CUT_NR"),  onOff(cfg.cut.cutNr),  onOff(now.cut.cutNr));
    put(QStringLiteral("CW_CUT_ONE"), onOff(cfg.cut.cutOne), onOff(now.cut.cutOne));
    put(QStringLiteral("CW_NAME"),    cfg.name.trimmed(),    now.name);
}

} // namespace ShackBook
