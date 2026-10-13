#include "PluginConfigStore.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {
constexpr int kSchemaVersion = 1;
const QString kFileName = QStringLiteral("plugin_config.json");
} // namespace

PluginConfigStore::PluginConfigStore(const QString &pluginDir, const QString &pluginId)
    : m_filePath(QDir(pluginDir).filePath(kFileName))
    , m_pluginId(pluginId)
{
    load();
}

void PluginConfigStore::load()
{
    QFile file(m_filePath);
    if (!file.exists())
        return; // 初回（保存時に生成される）
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << QStringLiteral("[PluginConfig] failed to open: %1").arg(m_filePath);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning().noquote()
            << QStringLiteral("[PluginConfig] parse error in %1: %2 (values reset to defaults)")
                   .arg(m_filePath, parseError.errorString());
        return;
    }

    const QJsonObject root = doc.object();
    // schemaVersion / pluginId は現状情報のみ（v1しか無い）。将来
    // フォーマットを変える場合はここで移行処理を行う。
    m_values = root.value(QStringLiteral("values")).toObject().toVariantMap();
}

void PluginConfigStore::save()
{
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), kSchemaVersion);
    root.insert(QStringLiteral("pluginId"), m_pluginId);
    root.insert(QStringLiteral("values"), QJsonObject::fromVariantMap(m_values));

    // QSaveFile: 書き込み途中のクラッシュで既存configを壊さない
    // （テンポラリへ書いてからatomic rename）。
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        // 書き込み不能（アクセス権等）でも動作は続ける（値はメモリ内に
        // 保持されるため、そのセッション中は機能する）。ログは初回のみ。
        if (!m_writeFailedLogged) {
            m_writeFailedLogged = true;
            qWarning().noquote()
                << QStringLiteral("[PluginConfig] failed to write %1 (%2). "
                                  "Values will not persist across restarts.")
                       .arg(m_filePath, file.errorString());
        }
    }
}

QVariant PluginConfigStore::value(const QString &key, const QVariant &fallback) const
{
    const QVariant v = m_values.value(key);
    return v.isValid() ? v : fallback;
}

void PluginConfigStore::setValue(const QString &key, const QVariant &value)
{
    if (m_values.value(key) == value)
        return;
    m_values.insert(key, value);
    save();
}
