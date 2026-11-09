QT += core gui qml quick concurrent opengl

CONFIG += c++17 release link_pkgconfig
PKGCONFIG += wayland-client gbm egl
TARGET = omission
TEMPLATE = app

# Window capture speaks the standard ext-image-copy-capture protocols on its
# own Wayland connection, so the C glue is generated from the system XML.
WL_PROTOCOLS = $$system(pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_XML = \
    $$WL_PROTOCOLS/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml \
    $$WL_PROTOCOLS/staging/ext-image-capture-source/ext-image-capture-source-v1.xml \
    $$WL_PROTOCOLS/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml \
    $$WL_PROTOCOLS/stable/linux-dmabuf/linux-dmabuf-v1.xml

wl_header.input = WAYLAND_XML
wl_header.output = ${QMAKE_FILE_BASE}-client.h
wl_header.commands = wayland-scanner client-header ${QMAKE_FILE_NAME} ${QMAKE_FILE_OUT}
wl_header.CONFIG = no_link target_predeps
wl_header.variable_out = GENERATED_FILES
QMAKE_EXTRA_COMPILERS += wl_header

wl_code.input = WAYLAND_XML
wl_code.output = ${QMAKE_FILE_BASE}-protocol.c
wl_code.commands = wayland-scanner private-code ${QMAKE_FILE_NAME} ${QMAKE_FILE_OUT}
wl_code.variable_out = SOURCES
QMAKE_EXTRA_COMPILERS += wl_code

HEADERS += \
    src/backend.h \
    src/capturer.h \
    src/downscale.h \
    src/hyprland.h \
    src/model.h \
    src/theme.h \
    src/thumbnails.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/capturer.cpp \
    src/downscale.cpp \
    src/hyprland.cpp \
    src/model.cpp \
    src/theme.cpp \
    src/thumbnails.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
