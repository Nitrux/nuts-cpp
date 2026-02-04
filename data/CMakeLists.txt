# Install PolicyKit policy
install(FILES polkit/org.nxos.nuts.policy
    DESTINATION ${KDE_INSTALL_DATADIR}/polkit-1/actions
)

# Install D-Bus service file
install(FILES dbus/org.nxos.nuts.service
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
