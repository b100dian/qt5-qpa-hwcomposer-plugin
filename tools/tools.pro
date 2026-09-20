TEMPLATE = app
TARGET = hwcomposer_screencap_egl_test
CONFIG -= qt
CONFIG += console c++11 link_pkgconfig

SOURCES += hwcomposer_screencap_egl_test.cpp

# Keep this tool at the same public libhybris EGL/GLES boundary as QPA. It
# intentionally does not link droidmedia or Android framework C++ libraries.
PKGCONFIG += egl glesv2 hybris-egl-platform
LIBS += -ldl

# Avoid X11 header collision in EGL headers, as in the QPA plugin.
DEFINES += MESA_EGL_NO_X11_HEADERS

target.path = /usr/libexec/qt5-qpa-hwcomposer-plugin
INSTALLS += target
