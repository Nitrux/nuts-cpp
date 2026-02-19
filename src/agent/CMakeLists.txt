set(AGENT_SOURCES
    main.cpp
    NutsAgent.cpp
)

add_executable(nuts-agent ${AGENT_SOURCES})

target_include_directories(nuts-agent
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(nuts-agent
    PRIVATE
        Qt6::Core
)

install(TARGETS nuts-agent
    RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR}
)
