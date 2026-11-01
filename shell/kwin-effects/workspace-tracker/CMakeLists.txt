cmake_minimum_required(VERSION 3.19)
project(caelestia-workspace-tracker VERSION 6.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(ECM 6.0.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(QT_NO_CREATE_VERSIONLESS_FUNCTIONS TRUE)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets)
find_package(KF6 REQUIRED COMPONENTS CoreAddons)
find_package(KWin REQUIRED)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES KWin::kwin)
check_cxx_source_compiles("
    #include <effect/effecthandler.h>
    int main() {
        KWin::EffectsHandler *handler = nullptr;
        KWin::LogicalOutput *output = nullptr;
        handler->setCurrentDesktop(handler->currentDesktop(output), output);
    }
" CAELESTIA_KWIN_OUTPUT_DESKTOPS)
unset(CMAKE_REQUIRED_LIBRARIES)

set(workspace_tracker_SRCS
    workspace_tracker.cpp
)

kcoreaddons_add_plugin(kwin_workspace_tracker
    SOURCES ${workspace_tracker_SRCS}
    INSTALL_NAMESPACE "kwin/effects/plugins"
)
target_compile_definitions(kwin_workspace_tracker PRIVATE
    CAELESTIA_KWIN_OUTPUT_DESKTOPS=$<BOOL:${CAELESTIA_KWIN_OUTPUT_DESKTOPS}>)

target_link_libraries(kwin_workspace_tracker
    Qt::Core
    Qt::Gui
    Qt::Network
    KF6::CoreAddons
    KWin::kwin
)
