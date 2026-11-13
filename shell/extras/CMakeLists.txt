# Version
add_executable(version version.cpp)
target_compile_definitions(version PRIVATE
    PROJECT_NAME="${PROJECT_NAME}"
    VERSION="${VERSION}"
    GIT_REVISION="${GIT_REVISION}"
    DISTRIBUTOR="${DISTRIBUTOR}"
)
install(TARGETS version DESTINATION ${INSTALL_LIBDIR})

find_package(Qt6 REQUIRED COMPONENTS Core Gui DBus)
find_package(KF6GlobalAccel REQUIRED)
add_executable(caelestia-shortcut-recovery
    ../../src/bin/caelestia-shortcut-recovery.cpp
    ../../src/lib/shortcutrecovery.cpp)
target_link_libraries(caelestia-shortcut-recovery PRIVATE
    Qt6::Core Qt6::Gui Qt6::DBus KF6::GlobalAccel)
install(TARGETS caelestia-shortcut-recovery DESTINATION ${INSTALL_BINDIR})
