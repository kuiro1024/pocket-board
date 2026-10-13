#pragma once

#include <QString>

// 本体UIの表示言語。
//
// 2026-09-18: 以前は Windows の表示言語（GetUserDefaultUILanguage）から
// 自動で決めていたが、設定（全般 → 言語）で明示的に選ぶ方式へ変更した。
// 実際の値は AppSettings が持ち、起動時の load() と設定変更時に
// setLanguage() でここへ流し込む。この窓口の形（text()）は変えていないので
// 呼び出し側は従来どおり。
namespace AppLanguage {

enum class Language {
    Japanese,
    English,
};

// 既定は日本語。AppSettings::load() が保存値で上書きする。
inline Language &currentRef()
{
    static Language lang = Language::Japanese;
    return lang;
}

inline Language language()          { return currentRef(); }
inline void setLanguage(Language l) { currentRef() = l; }
inline bool usesJapanese()          { return currentRef() == Language::Japanese; }

inline QString text(const char *english, const char *japanese)
{
    return QString::fromUtf8(usesJapanese() ? japanese : english);
}

} // namespace AppLanguage
