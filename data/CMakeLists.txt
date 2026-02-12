# Install PolicyKit policy
install(FILES polkit/org.nxos.nuts.policy
    DESTINATION ${KDE_INSTALL_DATADIR}/polkit-1/actions
)

# Configure and install D-Bus service file with correct libexec path
configure_file(dbus/org.nxos.nuts.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/org.nxos.nuts.service
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nxos.nuts.service
    DESTINATION ${KDE_INSTALL_DATADIR}/dbus-1/system-services
)

# Install D-Bus system configuration
install(FILES dbus/org.nxos.nuts.conf
    DESTINATION ${KDE_INSTALL_SYSCONFDIR}/dbus-1/system.d
)

# Install D-Bus interface XML
install(FILES dbus/org.nxos.nuts.xml
    DESTINATION ${KDE_INSTALL_DATADIR}/dbus-1/interfaces
)
