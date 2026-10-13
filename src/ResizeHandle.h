#pragma once
#include <QFrame>
#include <QPoint>
#include <QRect>

class ResizeHandle : public QFrame
{
    Q_OBJECT

public:
    explicit ResizeHandle(QWidget *parent = nullptr);

    // アスペクト比ロック（全画面時に設定する）
    void setAspectLock(bool locked, double aspectW = 0, double aspectH = 0) {
        m_aspectLocked = locked;
        m_aspectW = aspectW;
        m_aspectH = aspectH;
    }
    // 2026-09-17: 辺リサイズ（MainWindow::nativeEvent の WM_SIZING）でも同じ比率を守るため参照する。
    bool   aspectLocked() const { return m_aspectLocked; }
    double aspectW() const { return m_aspectW; }
    double aspectH() const { return m_aspectH; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    bool   m_dragging    {false};
    QPoint m_dragStartGlobal;
    QRect  m_startGeo;
    bool   m_hover       {false};

    bool   m_aspectLocked {false};
    double m_aspectW      {16.0};
    double m_aspectH      {9.0};
};
