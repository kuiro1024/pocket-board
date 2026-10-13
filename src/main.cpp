#include "MainWindow.h"
#include "plugin/PluginManager.h"
#include "StartupRegistration.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSurfaceFormat>
#include <QIcon>
#include <QFile>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QWaitCondition>
#include <QThread>
#include <QStringList>
#include <atomic>
#include <cstdio>

// ----------------------------------------------------------------
// 2026-07-03 診断用ファイルロガー
// ----------------------------------------------------------------
// このアプリは(Debug/Release問わず)MinGWビルドで -mwindows が付いており
// コンソールを持たないため、配布(Release)版ではqDebug/qWarning等の出力が
// 一切目に見えない。Release版特有の不具合（画質切り替えが効かない、
// ファイルドロップが禁止される等）を調査するための最低限の手段として、
// 全ログを %APPDATA%/PocketBoard/PocketBoard/log.txt に書き出す。
// 動作そのものには一切影響しない（挙動を変えるコードではない）。
// QStandardPathsはQCoreApplicationのorganizationName/applicationNameに
// 依存するため、起動直後（app生成前）のログも取りこぼさないよう、
// 環境変数APPDATAから直接パスを組み立てる。
namespace {

QString diagnosticLogFilePath()
{
    QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty())
        appData = QDir::homePath();
    const QDir dir(appData + QStringLiteral("/PocketBoard/PocketBoard"));
    QDir().mkpath(dir.absolutePath());
    return dir.filePath(QStringLiteral("log.txt"));
}

// 2026-09-16: ログ出力をGUIスレッドから逃がす。
// 実測: 30秒ごとの [Memory] ダンプ（約100行）で GUI が 35〜40ms 止まっていた
// （[MemoryDumpDiag] total=35/36ms ≒ [GuiStall] 39/40ms）。原因は従来の
// ハンドラが「1行ごとにファイルを開く→書く→閉じる＋stderrへ書く」を
// 呼び出し元スレッド（ほぼGUI）で同期実行していたこと。
// 新方式: ハンドラは時刻付きの1行をキューに積むだけ。専用スレッドが
// ファイルを開きっぱなしでまとめて書き、stderr にも同じ順序で出す。
// 行の順序・時刻（積んだ時点）・出力先は従来と同じ。
// 【制約】MinGW libstdc++ 食い違い回避のため std::thread 等は使わず QThread 派生。
class LogWriterThread : public QThread
{
public:
    void enqueue(QString line, QByteArray stderrLine)
    {
        QMutexLocker lk(&m_mutex);
        m_fileLines.append(std::move(line));
        m_errLines.append(std::move(stderrLine));
        m_cv.wakeOne();
    }
    void requestStop()
    {
        QMutexLocker lk(&m_mutex);
        m_stop = true;
        m_cv.wakeOne();
    }

protected:
    void run() override
    {
        // 2026-10-12: 常駐中に際限なく大きくならないよう上限を付ける。
        // 超えたら log.old.txt へ回して新しく書き始める（直前の分は1世代残る）。
        constexpr qint64 kMaxLogBytes = 8LL * 1024 * 1024;
        const QString logPath = diagnosticLogFilePath();
        const QString oldLogPath = QFileInfo(logPath).absoluteDir()
                                       .filePath(QStringLiteral("log.old.txt"));
        QFile file(logPath);
        // 起動ごとに前回分を消して新規に書き始める（従来どおり）
        bool opened = file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
        QTextStream ts(&file);
        for (;;) {
            QStringList fileLines;
            QList<QByteArray> errLines;
            bool stop = false;
            {
                QMutexLocker lk(&m_mutex);
                while (m_fileLines.isEmpty() && !m_stop)
                    m_cv.wait(&m_mutex);
                fileLines.swap(m_fileLines);
                errLines.swap(m_errLines);
                stop = m_stop;
            }
            if (opened) {
                for (const QString &l : fileLines)
                    ts << l;
                ts.flush();
                if (file.size() > kMaxLogBytes) {
                    file.close();
                    QFile::remove(oldLogPath);
                    QFile::rename(logPath, oldLogPath);
                    file.setFileName(logPath);
                    opened = file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
                    ts.setDevice(&file);
                }
            }
            for (const QByteArray &e : errLines)
                std::fwrite(e.constData(), 1, static_cast<size_t>(e.size()), stderr);
            std::fflush(stderr);
            if (stop) {
                QMutexLocker lk(&m_mutex);
                if (m_fileLines.isEmpty())
                    break;
            }
        }
    }

private:
    QMutex m_mutex;
    QWaitCondition m_cv;
    QStringList m_fileLines;
    QList<QByteArray> m_errLines;
    bool m_stop = false;
};

std::atomic<LogWriterThread *> g_logWriter {nullptr};

const char *messageTypeString(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "DEBUG";
    case QtInfoMsg:     return "INFO";
    case QtWarningMsg:  return "WARN";
    case QtCriticalMsg: return "CRIT";
    case QtFatalMsg:    return "FATAL";
    }
    return "DEBUG";
}

// ライタースレッドが無い時間帯（起動直後の開始前・終了処理の後）と FATAL 用の同期書き込み。
void writeLogLineSync(const QString &fileLine, const QString &msg)
{
    static QMutex s_mutex;
    const QMutexLocker locker(&s_mutex);
    QFile file(diagnosticLogFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream ts(&file);
        ts << fileLine;
    }
    std::fprintf(stderr, "%s\n", qPrintable(msg));
    std::fflush(stderr);
}

void diagnosticFileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)
#ifdef NDEBUG
    // 2026-10-12: Release では本体の qDebug/qInfo はコンパイル時に消えているが
    // （CMakeLists.txt の QT_NO_DEBUG_OUTPUT / QT_NO_INFO_OUTPUT）、別ビルドの
    // プラグイン等から来た分もここで落とす。WARN 以上だけ残す。
    if (type == QtDebugMsg || type == QtInfoMsg)
        return;
#endif
    QString fileLine = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
        + QStringLiteral(" [") + QLatin1String(messageTypeString(type)) + QStringLiteral("] ")
        + msg + QLatin1Char('\n');

    LogWriterThread *writer = g_logWriter.load();
    if (type == QtFatalMsg || !writer) {
        writeLogLineSync(fileLine, msg);
        if (type == QtFatalMsg)
            std::abort();
        return;
    }
    QByteArray errLine = msg.toLocal8Bit();
    errLine.append('\n');
    writer->enqueue(std::move(fileLine), std::move(errLine));
}

} // namespace

int main(int argc, char *argv[])
{
    // 2026-09-16: ライタースレッドを先に起動してからハンドラを入れる
    // （ファイルの切り詰めはライタースレッドが起動時に1回だけ行う）。
    static LogWriterThread logWriter;
    logWriter.setObjectName(QStringLiteral("LogWriter"));
    logWriter.start(QThread::LowPriority);
    g_logWriter.store(&logWriter);
    qInstallMessageHandler(diagnosticFileMessageHandler);

    // ビルド確認用マーカー: 実行中のexeが最新のソースでビルドされているかを
    // 起動直後のログで一目で確認できるようにする。コンパイル日時は
    // ビルドのたびに自動更新されるため、個別の一時マーカー文字列を
    // 手動で書き換える必要はない（Phase 12でDND-TRASH-FIX-v1等の
    // 使い捨てマーカー文字列を廃止し、この日時ログに一本化した）。
    qInfo().noquote() << QStringLiteral("========================================");
    qInfo().noquote() << QStringLiteral("[BUILD] PocketBoard compiled at %1 %2").arg(QString::fromLatin1(__DATE__), QString::fromLatin1(__TIME__));
    qInfo().noquote() << QStringLiteral("========================================");

    // QOpenGLWidgetでアルファチャンネルを有効にしてノイズを防ぐ
    //
    // 2026-07-03 メモリ最適化: このアプリの描画(Renderer.cpp)は角丸矩形・線・
    // テクスチャのみの完全な2D合成で、GL_DEPTH_TESTは一切有効化されておらず
    // （MpvPlayer::renderToFbo()内でむしろ明示的にglDisableしている）、
    // stencilバッファも一切参照されていない。そのため
    // depth/stencilバッファはウィンドウサイズ分（+MSAA倍）確保されるだけの
    // 完全な無駄だったため削除した（起動直後・動画未読み込みの時点から
    // 常時確保されていたぶん）。
    // MSAAは角丸の縁のアンチエイリアシングをシェーダー側ではなくハード
    // ウェアMSAAに頼っている（Renderer.cppのフラグメントシェーダー参照）ため
    // 完全に0にはできないが、4→2に下げてメモリと見た目のバランスを取った。
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setAlphaBufferSize(8);
    // 2026-09-16: MSAAを既定で無効にした。キャンバスの図形は全部シェーダー側で
    // 縁を滑らかにするようになった（Renderer.cpp の SHAPE_FRAG）ので不要。
    // MSAAは画面全体を2倍の量で持ち毎フレームまとめ直すため、4Kで効いていた。
    // 見比べ用に PB_CANVAS_MSAA=2 のように指定すると以前の設定に戻せる。
    {
        bool ok = false;
        const int samples = qEnvironmentVariableIntValue("PB_CANVAS_MSAA", &ok);
        fmt.setSamples(ok ? qMax(0, samples) : 0);
    }
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setOrganizationName("PocketBoard");
    app.setApplicationName("PocketBoard");
    #ifdef PB_APP_VERSION
    app.setApplicationVersion(QStringLiteral(PB_APP_VERSION));
#else
    app.setApplicationVersion(QStringLiteral("0.0.0-dev"));
#endif
    app.setQuitOnLastWindowClosed(false);

    // アプリアイコンを設定（トレイ・タスクバー・ホットキー呼び出し時に使用）
    // exe と同階層の ico_light.ico を優先し、なければリソースから取得
    QIcon appIcon;
    const QString icoPath = QStringLiteral("ico_light.ico");
    if (QFile::exists(icoPath)) {
        appIcon = QIcon(icoPath);
    } else {
        appIcon = QIcon(QStringLiteral(":/ico/ico_light.ico"));
    }
    if (!appIcon.isNull())
        app.setWindowIcon(appIcon);

    // Phase 8: Plugin API — exe と同階層の Plugins/ フォルダから
    // IPbMediaPlugin 実装DLLを読み込む。フォルダが無ければ何もしない。
    PluginManager::instance().loadPlugins();

    // 2026-10-13: スタートアップ登録先のexeが消えていたら今のexeへ付け替える
    // （ポータブル版を移動した等で自動起動が切れないように）。
    StartupRegistration::refreshPathIfRegistered();

    MainWindow window;
    // Windows のスタートアップ項目には `PocketBoard.exe --background` を
    // 登録する。通常起動時は従来どおりウィンドウを表示し、バックグラウンド
    // 起動時だけはトレイアイコンとグローバルホットキーだけを有効にして待機する。
    // --minimized は既存のスタートアップショートカットとの互換用の別名。
    const QStringList arguments = QCoreApplication::arguments();
    const bool startInBackground = arguments.contains(QStringLiteral("--background"))
        || arguments.contains(QStringLiteral("--minimized"));
    if (startInBackground) {
        qInfo() << "Starting in background; use the tray icon or global hotkey to show PocketBoard.";
    } else {
        window.show();
    }

    const int exitCode = app.exec();

    // 2026-09-16: 残りのログを書き切ってからライターを止める。以降のログ
    // （静的破棄中など）は同期書き込みに戻る。
    g_logWriter.store(nullptr);
    logWriter.requestStop();
    logWriter.wait();
    return exitCode;
}
