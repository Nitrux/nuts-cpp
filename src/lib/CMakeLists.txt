set(NUTS_LIB_SOURCES
    Config.cpp
    Logger.cpp
    SystemInterface.cpp
    BackupManager.cpp
    UpdateManager.cpp
)

add_library(nuts SHARED ${NUTS_LIB_SOURCES})

target_include_directories(nuts
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nuts
    PUBLIC
        Qt6::Core
        Qt6::DBus
    PRIVATE
        KF6::CoreAddons
)

set_target_properties(nuts PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "nuts"
)

install(TARGETS nuts
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/nuts
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}
)
