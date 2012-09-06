#-------------------------------------------------
#
# Project created by QtCreator 2011-12-11T09:50:59
#
#-------------------------------------------------

QT -= core gui

TARGET = libamc-test

TEMPLATE = app

SOURCES += \
    main.cpp

INCLUDEPATH += ../libamc ../../utility/event-listener ../at-manager ../../simulation
DEPENDPATH += ../libamc ../../utility/event-listener ../at-manager ../../simulation

CONFIG(debug, debug|release) {
    DESTDIR = ../../build/debug
} else {
    DESTDIR = ../../build/release
}

LIBS += -L$$DESTDIR -llibamc -lrt -levent-listener -lat-parser -ltty-handler -lat-manager -lproperty -lsimulation
