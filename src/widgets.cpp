#include "widgets.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWindow>

CloseButton::CloseButton(QWidget *parent) : QAbstractButton(parent)
{
    setFixedSize(16, 16);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
}

void CloseButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool hov = underMouse();
    if (hov) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_hover);
        p.drawRoundedRect(rect(), 4, 4);
    }
    if (m_dirty && !hov) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_fg);
        p.drawEllipse(QPointF(8, 8), 3, 3);
        return;
    }
    QPen pen(m_fg, 1.4);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.drawLine(QPointF(5, 5), QPointF(11, 11));
    p.drawLine(QPointF(11, 5), QPointF(5, 11));
}

void DragArea::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_pressed = true;
        m_press = e->position().toPoint();
    }
}

void DragArea::mouseMoveEvent(QMouseEvent *e)
{
    if (m_pressed && (e->position().toPoint() - m_press).manhattanLength() > QApplication::startDragDistance()) {
        m_pressed = false;
        if (QWindow *w = window()->windowHandle())
            w->startSystemMove();
    }
}

void DragArea::mouseDoubleClickEvent(QMouseEvent *e)
{
    m_pressed = false;
    if (e->button() == Qt::LeftButton)
        emit doubleClicked();
}

FindBar::FindBar(QWidget *parent) : QFrame(parent)
{
    setObjectName("findbar");
    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(10, 4, 10, 4);
    l->setSpacing(6);
    edit = new QLineEdit(this);
    edit->setPlaceholderText("buscar");
    info = new QLabel(this);
    info->setObjectName("muted");
    l->addWidget(edit, 1);
    l->addWidget(info);
    setFixedWidth(320);
    edit->installEventFilter(this);
    connect(edit, &QLineEdit::textChanged, this, &FindBar::changed);
}

bool FindBar::eventFilter(QObject *o, QEvent *e)
{
    if (o == edit && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            if (k->modifiers() & Qt::AltModifier)
                emit selectAll();
            else
                emit next(k->modifiers() & Qt::ShiftModifier);
            return true;
        }
        if (k->key() == Qt::Key_Escape) {
            emit closed();
            return true;
        }
    }
    return QFrame::eventFilter(o, e);
}
