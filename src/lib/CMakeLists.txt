set(NUTS_LIB_SOURCES
    Config.cpp
    Logger.cpp
    SystemInterface.cpp
    BackupManager.cpp
    UpdateManager.cpp
)

set(NUTS_LIB_HEADERS
    ${CMAKE_SOURCE_DIR}/include/nuts/NutsExport.h
    ${CMAKE_SOURCE_DIR}/include/nuts/Types.h
    ${CMAKE_SOURCE_DIR}/include/nuts/Config.h
    ${CMAKE_SOURCE_DIR}/include/nuts/Logger.h
    ${CMAKE_SOURCE_DIR}/include/nuts/SystemInterface.h
    ${CMAKE_SOURCE_DIR}/include/nuts/BackupManager.h
    ${CMAKE_SOURCE_DIR}/include/nuts/UpdateManager.h
)

add_library(nuts SHARED ${NUTS_LIB_SOURCES} ${NUTS_LIB_HEADERS})

set_target_properties(nuts PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_compile_definitions(nuts PRIVATE NUTS_LIBRARY)

target_include_directories(nuts
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nuts
    PUBLIC
        Qt6::Core
        Qt6::DBus
        Qt6::Network
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
