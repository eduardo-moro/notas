#pragma once
#include "settings.h"
#include <QWidget>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QFrame;
class QLineEdit;
class QSpinBox;

// Overlay drawn on top of the window (no separate dialog window). Every change is applied live.
class SettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPanel(QWidget *parent);
    void open(const AppSettings &s);
    void setRadius(int r);

signals:
    void changed(const AppSettings &s);
    void closed();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void emitChanged();
    void layoutCard();

    AppSettings m_s;
    bool m_loading = false;
    int m_radius = 10;

    QFrame *m_card;
    QLineEdit *m_dir;
    QSpinBox *m_autosave, *m_size, *m_padH, *m_padV, *m_tab, *m_radiusSpin, *m_winRadius;
    QFontComboBox *m_font;
    QCheckBox *m_spaces, *m_indent, *m_wrap;
    QComboBox *m_theme, *m_themeDark, *m_themeLight, *m_close;
};
