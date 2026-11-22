#pragma once
#include <QAbstractButton>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>

// Tab close button: shows a dot while the note has unsaved changes, an × otherwise / on hover.
class CloseButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit CloseButton(QWidget *parent = nullptr);
    QSize sizeHint() const override { return {16, 16}; }
    void setDirty(bool d) { if (d != m_dirty) { m_dirty = d; update(); } }
    void setColors(const QColor &fg, const QColor &hover) { m_fg = fg; m_hover = hover; update(); }

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    bool m_dirty = false;
    QColor m_fg, m_hover;
};

// Empty space next to the tabs: drag moves the window, double-click opens a new tab.
class DragArea : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
signals:
    void doubleClicked();

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;

private:
    QPoint m_press;
    bool m_pressed = false;
};

class FindBar : public QFrame {
    Q_OBJECT
public:
    explicit FindBar(QWidget *parent = nullptr);
    QLineEdit *edit;
    QLabel *info;

signals:
    void next(bool backwards);
    void selectAll();
    void closed();
    void changed();

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
};
