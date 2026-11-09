QT += core gui quick testlib
CONFIG += c++17 testcase
TARGET = omission_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/downscale.h ../src/model.h ../src/theme.h ../src/translator.h
SOURCES += omission_tests.cpp ../src/model.cpp ../src/theme.cpp ../src/translator.cpp \
    ../src/downscale.cpp
