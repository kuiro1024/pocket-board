#include "ResizeHandle.h"
#include "MainWindow.h"
#include <QPainter>
#include <QPen>
#include <QMouseEvent>
#include <QCursor>
#include <QWidget>
#include <cmath>

ResizeHandle::ResizeHandle(QWidget *parent)
    : QFrame(parent)
{
    setFixedSize(40, 40);
    setCursor(Qt::SizeFDiagCursor);
}

void ResizeHandle::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Python版と同じ: 右下に向かう斜め線2本
    // painter.drawLine(w-18, h-8, w-8, h-18)
    // painter.drawLine(w-12, h-8, w-8, h-12)
    QColor penColor = m_hover ? QColor(255, 255, 255, 255) : QColor(255, 255, 255, 220);
    painter.setPen(QPen(penColor, 1.5, Qt::SolidLine, Qt::RoundCap));

    int w = width(), h = height();
    painter.drawLine(w-18, h-8,  w-8, h-18);
    painter.drawLine(w-12, h-8,  w-8, h-12);
}

void ResizeHandle::enterEvent(QEnterEvent *)
{
    m_hover = true;
    update();
}

void ResizeHandle::leaveEvent(QEvent *)
{
    m_hover = false;
    update();
}

void ResizeHandle::mousePressEvent(QMouseEvent *event)
{
    // ウィンドウ全画面（独自管理）状態のときはリサイズさせない。
    // 通常はウィンドウ全画面中このハンドル自体が非表示になっている
    // （MainWindow::setWindowFullscreen()参照）が、念のためここでも保険をかける。
    auto *mw = qobject_cast<MainWindow*>(window());
    const bool fullscreen = mw && mw->isWindowFullscreen();
    if (event->button() == Qt::LeftButton && !fullscreen) {
        m_dragging       = true;
        m_dragStartGlobal = event->globalPosition().toPoint();
        m_startGeo       = window()->geometry();
        event->accept();
    }
}

void ResizeHandle::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) return;
    auto *mw = qobject_cast<MainWindow*>(window());
    if (mw && mw->isWindowFullscreen()) {
        // ドラッグ中に何らかの経路でウィンドウ全画面になった場合の保険。
        m_dragging = false;
        return;
    }
    QPoint delta = event->globalPosition().toPoint() - m_dragStartGlobal;
    // 2026-09-16: 幅の下限は MainWindow::kMinWindowWidth（島とラベルが並ぶ幅）
    // 2026-09-18: 350/250 の直値をやめ、辺リサイズ（MainWindow::nativeEvent の
    // WM_SIZING）と同じ定数を参照して下限をそろえる。
    int newW = qMax(MainWindow::kMinFreeWindowWidth,  m_startGeo.width()  + delta.x());
    int newH = qMax(MainWindow::kMinFreeWindowHeight, m_startGeo.height() + delta.y());

    if (m_aspectLocked && m_aspectW > 0 && m_aspectH > 0) {
        double ar = m_aspectW / m_aspectH;
        // 2026-07-12 バグ修正: 以前はdelta.x()とdelta.y()の大小で採用する
        // 軸（W基準かH基準か）を切り替えた上、H→Wの2段階で整数丸めを
        // 往復させていた。そのため、マウスを角の対角線（アスペクト比の
        // 向き）からわずかに外れて動かすだけで採用する軸がひっくり返り、
        // 丸め誤差も相まってガクついていた（対角線ぴったりに動かした
        // 時だけ滑らかだった）。
        // 「右下寄りに動かせば拡大、左上寄りに動かせば縮小」という直感的な
        // 操作にするため、ドラッグ量をアスペクト比の対角線方向（W:Hの
        // 比率を保ったまま伸び縮みする唯一の方向）へ射影した1次元の量
        // として扱い、そこからW/Hを1回で計算する（往復計算をしないので
        // 途中で軸が切り替わることも無い）。
        const double diagLen = std::hypot(ar, 1.0);
        const double dirX = ar  / diagLen;
        const double dirY = 1.0 / diagLen;
        const double amount = delta.x() * dirX + delta.y() * dirY;
        double rawW = m_startGeo.width() + amount * dirX;
        if (rawW <= 0.0) rawW = 1.0; // 対角線方向へ極端に振り切れた場合の0/負値防止
        double rawH = rawW / ar;

        // 2026-07-12 バグ修正: 最小サイズいっぱいまで縮めた時、動画の比率と
        // ウィンドウの比率がズレる不具合があった。原因はW/Hそれぞれを
        // 独立に200pxで下限クランプしていたこと。例えば16:9の動画で
        // W=200まで縮むとH=W/ar≈112になるが、これをそのままqMax(200,112)で
        // 200へ引き上げてしまうと、Wは200のままHだけ200になり比率が
        // 崩れる。W・H片方だけを個別にクランプするのではなく、両方が
        // 200px以上になるまで「同じ倍率」で両方を一緒に引き上げる
        // （scale倍することでW:H比そのものは変わらない）ことで、
        // 最小サイズでも常に指定アスペクト比を保つ。
        // 2026-09-16: 下限を 200×200 から MainWindow の最小サイズへ。1:1 の動画で
        // 島の展開幅（240）よりウィンドウが狭くなっていたため。考え方（両方を
        // 同じ倍率で引き上げて比率を保つ）は上のとおり変えていない。
        const double kMinW = MainWindow::kMinWindowWidth;
        const double kMinH = MainWindow::kMinWindowHeight;
        const double scale = qMax(1.0, qMax(kMinW / rawW, kMinH / rawH));
        newW = static_cast<int>(std::round(rawW * scale));
        newH = static_cast<int>(std::round(rawH * scale));
    }

    window()->resize(newW, newH);
    event->accept();
}

void ResizeHandle::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    event->accept();
}
