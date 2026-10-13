#include "IslandControlRegistry.h"

#include <QDebug>
#include <QSet>

IslandControlRegistry &IslandControlRegistry::instance()
{
    static IslandControlRegistry s_instance;
    return s_instance;
}

IslandControlRegistry::IslandControlRegistry(QObject *parent)
    : QObject(parent)
{
}

bool IslandControlRegistry::validateSpec(const PbControlSpec &spec, QString *why)
{
    auto fail = [why](const QString &msg) {
        if (why)
            *why = msg;
        return false;
    };

    if (spec.id.trimmed().isEmpty())
        return fail(QStringLiteral("control id is empty"));
    if (spec.id.length() > 64)
        return fail(QStringLiteral("control id too long (max 64)"));
    if (spec.label.length() > 48)
        return fail(QStringLiteral("label too long (max 48)"));

    // kindはプラグインDLL側から生の整数として渡ってくるため、
    // 本体が知らない将来の値（あるいは壊れた値）が来る可能性がある。
    // 範囲外はここで弾く（未知kindのウィジェットは作れないため）。
    const int kindValue = static_cast<int>(spec.kind);
    if (kindValue < static_cast<int>(PbControlKind::Toggle)
        || kindValue > static_cast<int>(PbControlKind::KeyInput))
        return fail(QStringLiteral("unknown control kind (%1)").arg(kindValue));

    switch (spec.kind) {
    case PbControlKind::Slider:
    case PbControlKind::SpinBox:
        if (spec.minValue > spec.maxValue)
            return fail(QStringLiteral("minValue > maxValue"));
        if (spec.step < 1)
            return fail(QStringLiteral("step must be >= 1"));
        break;
    case PbControlKind::Choice:
        if (spec.options.isEmpty())
            return fail(QStringLiteral("Choice requires at least 1 option"));
        for (const PbControlOption &opt : spec.options) {
            if (opt.id.trimmed().isEmpty())
                return fail(QStringLiteral("Choice option with empty id"));
        }
        break;
    case PbControlKind::Toggle:
    case PbControlKind::Button:
        break;
    case PbControlKind::Label:
        if (spec.label.trimmed().isEmpty())
            return fail(QStringLiteral("Label requires non-empty label"));
        break;
    case PbControlKind::TextInput:
        if (spec.maxLength < 0)
            return fail(QStringLiteral("maxLength must be >= 0"));
        if (spec.placeholder.length() > 64)
            return fail(QStringLiteral("placeholder too long (max 64)"));
        break;
    case PbControlKind::KeyInput:
        break;
    }
    return true;
}

namespace {

// defaultValue 未設定・型不一致のspecへ既定値を補完する。
// UI構築側（IslandSettingsPanel）が型チェックせずに使えるようにするため、
// 登録時にここで一度だけ正規化しておく。
void normalizeDefaultValue(PbControlSpec &spec)
{
    switch (spec.kind) {
    case PbControlKind::Toggle:
        if (spec.defaultValue.typeId() != QMetaType::Bool)
            spec.defaultValue = false;
        break;
    case PbControlKind::Slider:
    case PbControlKind::SpinBox: {
        bool ok = false;
        int v = spec.defaultValue.toInt(&ok);
        if (!ok)
            v = spec.minValue;
        spec.defaultValue = qBound(spec.minValue, v, spec.maxValue);
        break;
    }
    case PbControlKind::Choice: {
        const QString id = spec.defaultValue.toString();
        bool found = false;
        for (const PbControlOption &opt : spec.options) {
            if (opt.id == id) { found = true; break; }
        }
        if (!found)
            spec.defaultValue = spec.options.first().id;
        break;
    }
    case PbControlKind::Button:
    case PbControlKind::Label:
        spec.defaultValue = QVariant();
        break;
    case PbControlKind::TextInput:
        // 未設定(invalid)は空文字列に。maxLength指定があれば切り詰める。
        spec.defaultValue = spec.defaultValue.toString();
        if (spec.maxLength > 0)
            spec.defaultValue = spec.defaultValue.toString().left(spec.maxLength);
        break;
    case PbControlKind::KeyInput:
        // 未設定・数値でない値は「空欄」(0)
        spec.defaultValue = qMax(0, spec.defaultValue.toInt());
        break;
    }
}

} // namespace

bool IslandControlRegistry::registerSection(IslandControlSection section)
{
    if (section.id.trimmed().isEmpty()) {
        qWarning() << "[IslandControls] section rejected: empty section id";
        return false;
    }

    QList<IslandControlEntry> accepted;
    QSet<QString> seenIds;
    for (IslandControlEntry &entry : section.entries) {
        QString why;
        if (!validateSpec(entry.spec, &why)) {
            qWarning().noquote()
                << QStringLiteral("[IslandControls] spec rejected: section=%1 id=%2 (%3)")
                       .arg(section.id, entry.spec.id, why);
            continue;
        }
        if (seenIds.contains(entry.spec.id)) {
            qWarning().noquote()
                << QStringLiteral("[IslandControls] spec rejected: section=%1 id=%2 (duplicate id)")
                       .arg(section.id, entry.spec.id);
            continue;
        }
        if (accepted.size() >= maxControlsPerSection()) {
            qWarning().noquote()
                << QStringLiteral("[IslandControls] section=%1: too many controls, "
                                  "dropping the rest (max %2)")
                       .arg(section.id)
                       .arg(maxControlsPerSection());
            break;
        }
        normalizeDefaultValue(entry.spec);
        seenIds.insert(entry.spec.id);
        accepted.append(entry);
    }
    section.entries = accepted;

    if (section.entries.isEmpty()) {
        qWarning().noquote()
            << QStringLiteral("[IslandControls] section=%1: no valid controls, not registered")
                   .arg(section.id);
        return false;
    }

    // 同一IDの既存セクションは置き換え（プラグイン再インストール等）。
    // 置き換え時は旧セクションの実行時状態上書きも破棄する（新しい宣言に
    // 古い有効/無効・文言が残らないように）。
    for (int i = m_sections.size() - 1; i >= 0; --i) {
        if (m_sections[i].id == section.id)
            m_sections.removeAt(i);
    }
    m_controlStates.remove(section.id);

    // 本体("host.*")セクションは常に先頭に表示する。本体同士は登録順を
    // 保つため、先頭ではなく「既存の本体セクションの直後」へ差し込む。
    // 2026-09-15: カテゴリも本体予約扱い。プラグインが本体カテゴリを
    // 名乗ることはできない（PluginManager は category を設定しないが、
    // 念のためここで矯正する）。
    if (isHostSectionId(section.id)) {
        int insertAt = 0;
        while (insertAt < m_sections.size() && isHostSectionId(m_sections[insertAt].id))
            ++insertAt;
        m_sections.insert(insertAt, section);
    } else {
        section.category = IslandControlCategory::Plugins;
        m_sections.append(section);
    }

    qDebug().noquote()
        << QStringLiteral("[IslandControls] section registered: %1 (%2 controls)")
               .arg(section.id)
               .arg(section.entries.size());

    emit sectionsChanged();
    return true;
}

void IslandControlRegistry::unregisterSection(const QString &sectionId)
{
    bool removed = false;
    for (int i = m_sections.size() - 1; i >= 0; --i) {
        if (m_sections[i].id == sectionId) {
            m_sections.removeAt(i);
            removed = true;
        }
    }
    m_controlStates.remove(sectionId);
    if (removed)
        emit sectionsChanged();
}

// ================================================================
//  実行時の状態上書き（PbControlHost経由。2026-07-13追加）
// ================================================================
bool IslandControlRegistry::hasControl(const QString &sectionId,
                                       const QString &controlId) const
{
    for (const IslandControlSection &section : m_sections) {
        if (section.id != sectionId)
            continue;
        for (const IslandControlEntry &entry : section.entries) {
            if (entry.spec.id == controlId)
                return true;
        }
        return false;
    }
    return false;
}

void IslandControlRegistry::setControlEnabled(const QString &sectionId,
                                              const QString &controlId,
                                              bool enabled)
{
    if (!hasControl(sectionId, controlId)) {
        qWarning().noquote()
            << QStringLiteral("[IslandControls] setControlEnabled ignored: "
                              "unknown control section=%1 id=%2")
                   .arg(sectionId, controlId);
        return;
    }
    IslandControlState &state = m_controlStates[sectionId][controlId];
    if (state.hasEnabled && state.enabled == enabled)
        return; // 変化なし（無駄なシグナル発火を避ける）
    state.hasEnabled = true;
    state.enabled = enabled;
    emit controlStateChanged(sectionId, controlId);
}

void IslandControlRegistry::setControlLabel(const QString &sectionId,
                                            const QString &controlId,
                                            const QString &label)
{
    if (!hasControl(sectionId, controlId)) {
        qWarning().noquote()
            << QStringLiteral("[IslandControls] setControlLabel ignored: "
                              "unknown control section=%1 id=%2")
                   .arg(sectionId, controlId);
        return;
    }
    // PbControlSpec::label と同じ上限（validateSpec()の48文字）へ切り詰める。
    const QString clamped = label.left(48);
    IslandControlState &state = m_controlStates[sectionId][controlId];
    if (state.hasLabel && state.label == clamped)
        return;
    state.hasLabel = true;
    state.label = clamped;
    emit controlStateChanged(sectionId, controlId);
}

void IslandControlRegistry::setControlVisible(const QString &sectionId,
                                              const QString &controlId,
                                              bool visible)
{
    if (!hasControl(sectionId, controlId)) {
        qWarning().noquote()
            << QStringLiteral("[IslandControls] setControlVisible ignored: "
                              "unknown control section=%1 id=%2")
                   .arg(sectionId, controlId);
        return;
    }
    IslandControlState &state = m_controlStates[sectionId][controlId];
    if (state.hasVisible && state.visible == visible)
        return;
    state.hasVisible = true;
    state.visible = visible;
    emit controlStateChanged(sectionId, controlId);
}

void IslandControlRegistry::pushControlValue(const QString &sectionId,
                                             const QString &controlId,
                                             const QVariant &value)
{
    if (!hasControl(sectionId, controlId))
        return;
    emit controlValuePushed(sectionId, controlId, value);
}

IslandControlState IslandControlRegistry::controlState(const QString &sectionId,
                                                       const QString &controlId) const
{
    return m_controlStates.value(sectionId).value(controlId);
}
