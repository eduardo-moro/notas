QT += widgets network
CONFIG += c++20 release
TARGET = notas
DESTDIR = $$PWD/build
OBJECTS_DIR = $$PWD/build/obj
MOC_DIR = $$PWD/build/moc

SOURCES += \
    src/main.cpp \
    src/settings.cpp \
    src/theme.cpp \
    src/editor.cpp \
    src/markdown.cpp \
    src/widgets.cpp \
    src/settingspanel.cpp \
    src/mainwindow.cpp

HEADERS += \
    src/settings.h \
    src/theme.h \
    src/editor.h \
    src/markdown.h \
    src/widgets.h \
    src/settingspanel.h \
    src/mainwindow.h

# make install PREFIX=~/.local   (default /usr/local)
isEmpty(PREFIX): PREFIX = /usr/local
target.path = $$PREFIX/bin
desktop.files = data/notas.desktop
desktop.path = $$PREFIX/share/applications
man.files = data/notas.1
man.path = $$PREFIX/share/man/man1
fish.files = data/notas.fish
fish.path = $$PREFIX/share/fish/vendor_completions.d
INSTALLS += target desktop man fish
