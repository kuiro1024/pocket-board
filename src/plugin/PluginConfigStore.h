#pragma once

#include <QString>
#include <QVariant>
#include <QVariantMap>

// ============================================================
//  PluginConfigStore
//
//  プラグイン1つ分の設定値（島コントロールの値）を、そのプラグインの
//  フォルダ直下 plugin_config.json へ永続化する。
//
//  - 読み書きは本体のみが行う（プラグインはIPbControlProvider の
//    コールバック経由で値を受け取るだけ。ファイルを直接触らない）
//  - specに無いキーも保持したまま書き戻す（プラグインのバージョン間で
//    コントロールが増減しても値を失わない）
//  - フォルダが書き込み不能（Store版のアクセス権等）の場合は
//    メモリ内のみで動作し、警告ログを出す
//
//  ファイル形式:
//    {
//      "schemaVersion": 1,
//      "pluginId": "com.pocketboard.xxx",
//      "values": { "someKey": true, "otherKey": 3 }
//    }
// ============================================================
class PluginConfigStore
{
public:
    PluginConfigStore(const QString &pluginDir, const QString &pluginId);

    QVariant value(const QString &key, const QVariant &fallback = QVariant()) const;

    // 値を設定し、即座に plugin_config.json へ保存する
    // （AppSettings::save() と同じ即時保存方針）。
    void setValue(const QString &key, const QVariant &value);

    QString filePath() const { return m_filePath; }

private:
    void load();
    void save();

    QString m_filePath;
    QString m_pluginId;
    QVariantMap m_values;
    bool m_writeFailedLogged = false;
};
