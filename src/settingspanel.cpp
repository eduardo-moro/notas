#include "settingspanel.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QCoreApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSpinBox>
#include <QVBoxLayout>

static QSpinBox *spin(int min, int max, int step, const QString &suffix)
{
    auto *s = new QSpinBox;
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setSuffix(suffix);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    return s;
}

static QLabel *section(const QString &text)
{
    auto *l = new QLabel(text);
    l->setObjectName("section");
    return l;
}

SettingsPanel::SettingsPanel(QWidget *parent) : QWidget(parent)
{
    hide();
    m_card = new QFrame(this);
    m_card->setObjectName("card");
    m_card->setFocusPolicy(Qt::StrongFocus);

    auto *outer = new QVBoxLayout(m_card);
    outer->setContentsMargins(20, 16, 8, 14);
    outer->setSpacing(8);

    auto *title = new QLabel("Configurações");
    title->setObjectName("title");
    outer->addWidget(title);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    auto *v = new QVBoxLayout(content);
    v->setContentsMargins(0, 0, 12, 0);
    v->setSpacing(6);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    form->addRow(section("Arquivo"));

    // notes dir
    m_dir = new QLineEdit;
    m_dir->setReadOnly(true); // changing it moves the notes: only via the picker, never by stray typing
    m_dir->setFocusPolicy(Qt::NoFocus);
    m_dir->setToolTip("use o botão … para escolher outra pasta (as notas são movidas)");
    auto *browse = new QPushButton("…");
    browse->setFixedWidth(34);
    browse->setFocusPolicy(Qt::NoFocus);
    auto *dirRow = new QHBoxLayout;
    dirRow->setSpacing(6);
    dirRow->addWidget(m_dir, 1);
    dirRow->addWidget(browse);
    form->addRow("Pasta das notas", dirRow);

    m_autosave = spin(100, 600000, 250, " ms");
    form->addRow("Salvar após", m_autosave);

    m_close = new QComboBox;
    m_close->addItem("arquivar em .fechadas", "archive");
    m_close->addItem("apagar o arquivo", "delete");
    form->addRow("Ao fechar aba", m_close);

    m_font = new QFontComboBox;
    m_size = spin(4, 96, 1, " pt");
    m_padH = spin(0, 1000, 4, " px");
    m_padV = spin(0, 1000, 4, " px");
    m_tab = spin(1, 16, 1, "");
    m_spaces = new QCheckBox("indentar com espaços");
    m_indent = new QCheckBox("manter indentação ao quebrar linha");
    m_wrap = new QCheckBox("quebra de linha automática");
    m_markdown = new QCheckBox("markdown ao vivo (WYSIWYG)");
    form->addRow(section("Editor"));
    form->addRow("Fonte", m_font);
    form->addRow("Tamanho", m_size);
    form->addRow("Padding horizontal", m_padH);
    form->addRow("Padding vertical", m_padV);
    form->addRow("Largura do tab", m_tab);
    form->addRow("", m_spaces);
    form->addRow("", m_indent);
    form->addRow("", m_wrap);
    form->addRow("", m_markdown);

    m_theme = new QComboBox;
    m_themeDark = new QComboBox;
    m_themeLight = new QComboBox;
    m_radiusSpin = spin(0, 40, 1, " px");
    m_winRadius = spin(0, 40, 1, " px");
    form->addRow(section("Aparência"));
    form->addRow("Tema", m_theme);
    form->addRow("Tema escuro", m_themeDark);
    form->addRow("Tema claro", m_themeLight);
    form->addRow("Cantos da janela", m_winRadius);
    form->addRow("Cantos das abas", m_radiusSpin);
    v->addLayout(form);

    // keyboard shortcuts reference (the app has no menus)
    v->addWidget(section("Atalhos"));
    const QList<QPair<QString, QString>> keys = {
        {"Ctrl+T  Ctrl+N", "nova aba"},
        {"Ctrl+W", "fechar aba"},
        {"Ctrl+Shift+T", "reabrir aba fechada"},
        {"Ctrl+Z  Ctrl+Shift+Z", "desfazer / refazer (persistente)"},
        {"Ctrl+Tab  Ctrl+PgDn", "próxima aba"},
        {"Ctrl+Shift+Tab  Ctrl+PgUp", "aba anterior"},
        {"Ctrl+Shift+PgUp/PgDn", "mover aba"},
        {"Alt+1 … Alt+9", "ir para aba"},
        {"Alt+Shift+↑/↓", "adicionar cursor acima/abaixo"},
        {"Alt+Clique", "adicionar/remover cursor"},
        {"Esc", "remover cursores extras"},
        {"Ctrl+D", "selecionar próxima ocorrência"},
        {"Ctrl+Shift+L", "selecionar todas as ocorrências"},
        {"Alt+↑/↓", "mover linha"},
        {"Ctrl+Shift+Alt+↑/↓", "duplicar linha"},
        {"Ctrl+Shift+K", "apagar linha"},
        {"Ctrl+L", "selecionar linha"},
        {"Ctrl+Enter", "nova linha abaixo"},
        {"Ctrl+Shift+Enter", "nova linha acima"},
        {"Tab  Shift+Tab", "indentar / desindentar"},
        {"Ctrl+]  Ctrl+[", "indentar / desindentar linha"},
        {"Ctrl+C  Ctrl+X", "sem seleção: copia/corta a linha"},
        {"Ctrl+F", "buscar (Enter, Shift+Enter, Alt+Enter)"},
        {"F3  Shift+F3", "próxima / anterior ocorrência"},
        {"Ctrl+=  Ctrl+-  Ctrl+0", "zoom"},
        {"Ctrl+S", "salvar agora"},
        {"Ctrl+,", "configurações"},
        {"Ctrl+Shift+,", "abrir config.json numa aba"},
        {"Ctrl+Shift+M", "liga/desliga markdown ao vivo"},
        {"Ctrl+B  Ctrl+I", "negrito / itálico (markdown)"},
        {"Clique em [ ]", "marca a tarefa (markdown)"},
        {"Ctrl+Clique", "abre o link (markdown)"},
        {"Tab  Ctrl+setas", "anda entre células (tabela)"},
        {"Ctrl+Enter", "sai da tabela"},
        {"Ctrl+Backspace", "apaga a linha da tabela"},
        {"Ctrl+Q", "sair"},
    };
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(3);
    for (int i = 0; i < keys.size(); ++i) {
        auto *k = new QLabel(keys[i].first);
        k->setObjectName("key");
        auto *d = new QLabel(keys[i].second);
        grid->addWidget(k, i, 0, Qt::AlignRight);
        grid->addWidget(d, i, 1);
    }
    grid->setColumnStretch(1, 1);
    v->addLayout(grid);

    v->addWidget(section("Arquivos"));
    auto *paths = new QLabel(QString("config: %1\ntemas: %2/*.json")
                                 .arg(Paths::contract(Paths::configFile()), Paths::contract(Paths::themesDir())));
    paths->setObjectName("muted");
    paths->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(paths);
    v->addStretch();

    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    auto *hint = new QLabel("Esc para fechar · alterações aplicadas na hora");
    hint->setObjectName("muted");
    outer->addWidget(hint);

    // wiring
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, "Pasta das notas", Paths::expand(m_dir->text()));
        if (!d.isEmpty()) {
            m_dir->setText(Paths::contract(d));
            emitChanged();
        }
    });
    for (QSpinBox *s : {m_autosave, m_size, m_padH, m_padV, m_tab, m_radiusSpin, m_winRadius})
        connect(s, &QSpinBox::valueChanged, this, &SettingsPanel::emitChanged);
    for (QCheckBox *c : {m_spaces, m_indent, m_wrap, m_markdown})
        connect(c, &QCheckBox::toggled, this, &SettingsPanel::emitChanged);
    for (QComboBox *c : {m_theme, m_themeDark, m_themeLight, m_close})
        connect(c, &QComboBox::currentIndexChanged, this, &SettingsPanel::emitChanged);
    connect(m_font, &QFontComboBox::currentFontChanged, this, &SettingsPanel::emitChanged);

    // keep fields from demanding more width than the card has (QFontComboBox asks for a lot)
    for (QComboBox *c : findChildren<QComboBox *>()) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(12);
    }
    m_dir->setMinimumWidth(80);

    // scrolling the panel must not change values: wheel only acts on a focused field
    for (QWidget *w : findChildren<QAbstractSpinBox *>()) { w->setFocusPolicy(Qt::StrongFocus); w->installEventFilter(this); }
    for (QWidget *w : findChildren<QComboBox *>()) { w->setFocusPolicy(Qt::StrongFocus); w->installEventFilter(this); }

    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, this, [this] {
        hide();
        emit closed();
    });
}

void SettingsPanel::open(const AppSettings &s)
{
    m_loading = true;
    m_s = s;
    m_dir->setText(Paths::contract(s.notesDir));
    m_autosave->setValue(s.autosaveMs);
    m_font->setCurrentFont(QFont(s.fontFamily));
    m_size->setValue(s.fontSize);
    m_padH->setValue(s.padH);
    m_padV->setValue(s.padV);
    m_tab->setValue(s.tabWidth);
    m_spaces->setChecked(s.useSpaces);
    m_indent->setChecked(s.autoIndent);
    m_wrap->setChecked(s.wordWrap);
    m_markdown->setChecked(s.markdown);
    m_radiusSpin->setValue(s.elementRadius);
    m_winRadius->setValue(s.windowRadius);
    m_close->setCurrentIndex(s.deleteOnClose ? 1 : 0);

    const QStringList themes = Theme::available();
    auto fill = [&](QComboBox *c, QStringList items, const QString &cur) {
        if (!items.contains(cur))
            items << cur;
        c->clear();
        c->addItems(items);
        c->setCurrentText(cur);
    };
    fill(m_theme, QStringList{"auto", "dark", "light"} + themes, s.theme);
    fill(m_themeDark, themes, s.themeDark);
    fill(m_themeLight, themes, s.themeLight);
    m_loading = false;

    layoutCard();
    show();
    raise();
    m_card->setFocus(); // nothing editable has focus, so stray keystrokes change nothing
}

void SettingsPanel::setRadius(int r)
{
    m_radius = r;
    update();
}

void SettingsPanel::emitChanged()
{
    if (m_loading)
        return;
    m_s.notesDir = Paths::expand(m_dir->text());
    m_s.autosaveMs = m_autosave->value();
    m_s.fontFamily = m_font->currentFont().family();
    m_s.fontSize = m_size->value();
    m_s.padH = m_padH->value();
    m_s.padV = m_padV->value();
    m_s.tabWidth = m_tab->value();
    m_s.useSpaces = m_spaces->isChecked();
    m_s.autoIndent = m_indent->isChecked();
    m_s.wordWrap = m_wrap->isChecked();
    m_s.markdown = m_markdown->isChecked();
    m_s.elementRadius = m_radiusSpin->value();
    m_s.windowRadius = m_winRadius->value();
    m_s.deleteOnClose = m_close->currentData().toString() == "delete";
    m_s.theme = m_theme->currentText();
    m_s.themeDark = m_themeDark->currentText();
    m_s.themeLight = m_themeLight->currentText();
    emit changed(m_s);
}

void SettingsPanel::layoutCard()
{
    const int w = qMin(540, width() - 32);
    const int h = qMin(qMax(m_card->sizeHint().height(), 640), height() - 32);
    m_card->setGeometry((width() - w) / 2, (height() - h) / 2, w, h);
}

void SettingsPanel::resizeEvent(QResizeEvent *)
{
    layoutCard();
}

void SettingsPanel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 110));
    p.drawRoundedRect(rect(), m_radius, m_radius);
}

void SettingsPanel::mousePressEvent(QMouseEvent *e)
{
    if (!m_card->geometry().contains(e->position().toPoint())) {
        hide();
        emit closed();
    }
}

bool SettingsPanel::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() == QEvent::Wheel) {
        auto *w = qobject_cast<QWidget *>(o);
        if (w && !w->hasFocus()) {
            if (auto *sa = findChild<QScrollArea *>())
                QCoreApplication::sendEvent(sa->verticalScrollBar(), e); // scroll the panel instead
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}
