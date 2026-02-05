set(HELPER_SOURCES
    main.cpp
    NutsHelper.cpp
)

qt6_add_dbus_adaptor(HELPER_SOURCES
    ${CMAKE_SOURCE_DIR}/data/dbus/org.nxos.nuts.xml
    NutsHelper.h
    Nuts::NutsHelper
)

add_executable(nuts-helper ${HELPER_SOURCES})

target_link_libraries(nuts-helper
    PRIVATE
        nuts
        Qt6::Core
        Qt6::DBus
        Qt6::Concurrent
        KF6::CoreAddons
        PolkitQt6-1::Core
)

install(TARGETS nuts-helper
    RUNTIME DESTINATION ${KDE_INSTALL_LIBEXECDIR}
)
