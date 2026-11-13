cmake_minimum_required(VERSION 3.22)
project(caelestia-notification-presenter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui DBus Qml Quick)
find_package(Plasma 6.6 REQUIRED)
find_package(LibNotificationManager 6.6 REQUIRED)

plasma_add_applet(org.caelestia.notification-presenter QML_ARGS DEPENDENCIES QtQuick org.kde.plasma.core)
target_sources(org.caelestia.notification-presenter PRIVATE presenter.cpp presenter.hpp)
ecm_target_qml_sources(org.caelestia.notification-presenter SOURCES main.qml)
target_link_libraries(org.caelestia.notification-presenter PRIVATE
    Qt::Core Qt::Gui Qt::DBus Qt::Qml Qt::Quick Plasma::Plasma PW::LibNotificationManager)
