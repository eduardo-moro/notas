QT += widgets network testlib
CONFIG += c++20 testcase
TARGET = tst_notas
DESTDIR = $$PWD/../build/tests
OBJECTS_DIR = $$PWD/../build/tests/obj
MOC_DIR = $$PWD/../build/tests/moc
INCLUDEPATH += ../src

SOURCES += tst_notas.cpp \
    ../src/settings.cpp ../src/theme.cpp ../src/editor.cpp ../src/markdown.cpp ../src/widgets.cpp \
    ../src/settingspanel.cpp ../src/mainwindow.cpp
HEADERS += ../src/settings.h ../src/theme.h ../src/editor.h ../src/markdown.h ../src/widgets.h \
    ../src/settingspanel.h ../src/mainwindow.h
