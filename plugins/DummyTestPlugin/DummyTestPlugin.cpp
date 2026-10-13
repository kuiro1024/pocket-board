#include "DummyTestPlugin.h"

#include <QDebug>
#include <QTimer>
#include <QUrl>
#include <QFile>
#include <QFileInfo>

DummyTestPlugin::DummyTestPlugin(QObject *parent)
    : QObject(parent)
{
}

QString DummyTestPlugin::pluginId() const
{
    return QStringLiteral("com.pocketboard.dummytest");
}

QString DummyTestPlugin::displayName() const
{
    return QStringLiteral("PocketBoard Dummy Test Plugin");
}

QString DummyTestPlugin::pluginVersion() const
{
    return QStringLiteral("0.1.0");
}

bool DummyTestPlugin::canHandle(const QString &url) const
{
    // Plugin API 動作確認専用のテストスキーム。実サービスは解決しない。
    // 例: pbtest://sample
    return QUrl(url).scheme().compare(QStringLiteral("pbtest"), Qt::CaseInsensitive) == 0;
}

void DummyTestPlugin::resolve(const QString &url, int targetHeight, const PbResolveCallback &callback)
{
    // 実際のプラグイン（動画サイト用など）はここでネットワークアクセス等の
    // 非同期処理を行い、完了後にメインスレッドで callback を呼び出す。
    // ダミー実装は「解決に時間がかかる」ことを模した500msの遅延の後、
    // ローカルにサンプル動画があればそれを、無ければ失敗を返す。
    // targetHeight: このダミー実装は画質選択に対応しないため無視し、
    // 常に同じサンプルファイルを返す（resolvedHeightも設定しない＝0のまま）。
    Q_UNUSED(targetHeight);
    QTimer::singleShot(500, [url, callback]() {
        PbResolvedMedia result;

        // サンプル: 実行ファイルと同階層の sample.mp4 があればそれを使う。
        // 無い場合は「プラグインは動いたがストリームが無い」ことを
        // 明示するため失敗として返す（ネットワークアクセスは行わない）。
        const QString sampleVideo = QStringLiteral("sample.mp4");
        if (QFile::exists(sampleVideo)) {
            result.success = true;
            result.streamUrl = QUrl::fromLocalFile(QFileInfo(sampleVideo).absoluteFilePath()).toString();
            result.title = QStringLiteral("[DummyTestPlugin] ") + url;
            result.kind = MediaKind::LocalVideo;
        } else {
            result.success = false;
            result.errorMessage = QStringLiteral(
                "DummyTestPlugin: sample.mp4 not found next to PocketBoard.exe "
                "(canHandle/resolve pipeline is working; place a sample.mp4 to see actual playback)");
        }
        callback(result);
    });
}

// ============================================================
//  IPbControlProvider（島設定パネル）疎通確認用の実装。
//  全コントロール型を1つずつ宣言し、通知はログに出すだけ。
// ============================================================

QList<PbControlSpec> DummyTestPlugin::controlSpecs() const
{
    QList<PbControlSpec> specs;

    {
        PbControlSpec s;
        s.id = QStringLiteral("testToggle");
        s.label = QStringLiteral("Test Toggle");
        s.kind = PbControlKind::Toggle;
        s.defaultValue = false;
        s.tooltip = QStringLiteral("DummyTestPlugin: Toggle control test");
        specs.append(s);
    }
    {
        PbControlSpec s;
        s.id = QStringLiteral("testSlider");
        s.label = QStringLiteral("Test Slider");
        s.kind = PbControlKind::Slider;
        s.minValue = 0;
        s.maxValue = 100;
        s.step = 5;
        s.defaultValue = 50;
        specs.append(s);
    }
    {
        PbControlSpec s;
        s.id = QStringLiteral("testCount");
        s.label = QStringLiteral("Test Count");
        s.kind = PbControlKind::SpinBox;
        s.minValue = 1;
        s.maxValue = 10;
        s.step = 1;
        s.defaultValue = 3;
        specs.append(s);
    }
    {
        PbControlSpec s;
        s.id = QStringLiteral("testChoice");
        s.label = QStringLiteral("Test Choice");
        s.kind = PbControlKind::Choice;
        s.options = { { QStringLiteral("alpha"), QStringLiteral("Alpha") },
                      { QStringLiteral("beta"),  QStringLiteral("Beta") },
                      { QStringLiteral("gamma"), QStringLiteral("Gamma") } };
        s.defaultValue = QStringLiteral("alpha");
        specs.append(s);
    }
    {
        // TextInput＋同一行Buttonの組み合わせデモ（"IPを入れて接続開始"の
        // ようなUI向け。IID 1.1で追加されたTextInput型の疎通確認）。
        PbControlSpec s;
        s.id = QStringLiteral("testAddress");
        s.label.clear(); // ラベル無し＝入力欄のみ（placeholderが説明を兼ねる）
        s.kind = PbControlKind::TextInput;
        s.placeholder = QStringLiteral("192.168.1.5");
        s.maxLength = 64;
        s.tooltip = QStringLiteral("DummyTestPlugin: TextInput control test");
        specs.append(s);
    }
    {
        PbControlSpec s;
        s.id = QStringLiteral("testAction");
        s.label = QStringLiteral("Test Action");
        s.kind = PbControlKind::Button;
        s.sameRowAsPrevious = true; // 入力欄の右隣に配置
        specs.append(s);
    }
    return specs;
}

void DummyTestPlugin::onControlValueChanged(const QString &controlId, const QVariant &value)
{
    // 【注意】このコールバックはメインスレッドから呼ばれる。実プラグインで
    // 重い処理（ネットワーク等）が必要な場合はここでブロックせず非同期化
    // すること（PbControlApi.h のスレッド注意書き参照）。
    qDebug().noquote() << QStringLiteral("[DummyTestPlugin] control changed: %1 = %2")
                              .arg(controlId, value.toString());
}

void DummyTestPlugin::onControlTriggered(const QString &controlId)
{
    qDebug().noquote() << QStringLiteral("[DummyTestPlugin] control triggered: %1")
                              .arg(controlId);
}

void DummyTestPlugin::onControlValueRestored(const QString &controlId, const QVariant &value)
{
    qDebug().noquote() << QStringLiteral("[DummyTestPlugin] control restored: %1 = %2")
                              .arg(controlId, value.toString());
}
