set(GUI_SOURCES
    main.cpp
    NutsClient.cpp
)

set(GUI_RESOURCES
    qml.qrc
)

qt6_add_resources(GUI_SOURCES ${GUI_RESOURCES})

add_executable(nuts-gui ${GUI_SOURCES})

target_link_libraries(nuts-gui
    PRIVATE
        nuts
        Qt6::Core
        Qt6::Quick
        Qt6::Qml
        Qt6::DBus
        Qt6::Network
        KF6::CoreAddons
        KF6::I18n
        KF6::Notifications
        KF6::WindowSystem
        MauiKit4
)

install(TARGETS nuts-gui
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
)

install(FILES org.nxos.nuts.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)
