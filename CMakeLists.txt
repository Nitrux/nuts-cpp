cmake_minimum_required(VERSION 3.16)

project(nuts VERSION 3.0.0 LANGUAGES CXX)

set(QT_MAJOR_VERSION 6)
set(REQUIRED_QT_VERSION 6.9)
set(REQUIRED_KF6_VERSION 6.13.0)
set(REQUIRED_MAUIKIT_VERSION 4.0.3)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Detect host architecture
cmake_host_system_information(RESULT HOST_ARCH QUERY OS_PLATFORM)
message(STATUS "Host architecture: ${HOST_ARCH}")

# CPU optimization option (only relevant for x86_64)
option(USE_X86_64_V3 "Enable x86-64-v3 optimizations (requires AVX2, Haswell/Zen or newer)" ON)

# Apply architecture-specific optimization flags
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" OR HOST_ARCH MATCHES "^(x86_64|amd64|AMD64)$")
    if(USE_X86_64_V3)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v3 -mtune=generic -fexceptions")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=x86-64-v3 -mtune=generic")
        message(STATUS "Building with x86-64-v3 optimizations (AVX2 required)")
        message(STATUS "  Minimum CPUs: Intel Haswell (2013+), AMD Zen (2017+)")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64 -mtune=generic -fexceptions")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=x86-64 -mtune=generic")
        message(STATUS "Building with x86-64 baseline (maximum compatibility)")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR HOST_ARCH MATCHES "^(aarch64|arm64|ARM64)$")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a -mtune=generic -fexceptions")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a -mtune=generic")
    message(STATUS "Building for ARM64 with ARMv8-A baseline")
else()
    message(STATUS "Building for ${CMAKE_SYSTEM_PROCESSOR} with default flags")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")
endif()

# Find required packages
find_package(ECM ${REQUIRED_KF6_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})

include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(ECMFindQmlModule)
include(ECMSetupVersion)
include(FeatureSummary)

find_package(Qt6 ${REQUIRED_QT_VERSION} REQUIRED COMPONENTS
    Core
    Quick
    Qml
    DBus
    Widgets
    Network
    Concurrent
)

find_package(KF6 ${REQUIRED_KF6_VERSION} REQUIRED COMPONENTS
    CoreAddons
    I18n
    Notifications
    WindowSystem
)

find_package(MauiKit4 ${REQUIRED_MAUIKIT_VERSION} REQUIRED)

ecm_find_qmlmodule(org.mauikit.controls 1.0)

find_package(PolkitQt6-1 REQUIRED)

# Build options
option(BUILD_TESTING "Build tests" OFF)

# Add subdirectories
add_subdirectory(src)
add_subdirectory(data)

# Feature summary
feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
