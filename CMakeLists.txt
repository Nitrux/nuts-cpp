cmake_minimum_required(VERSION 3.16)

project(nuts VERSION 3.0.0 LANGUAGES CXX)

set(QT_MAJOR_VERSION 6)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# CPU optimization option
option(USE_X86_64_V3 "Enable x86-64-v3 optimizations (requires AVX2, Haswell/Zen or newer)" ON)

# Apply CPU optimization flags
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

# Find required packages
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})

include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(ECMFindQmlModule)
include(ECMSetupVersion)
include(FeatureSummary)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Quick
    Qml
    DBus
    Widgets
    Network
    Concurrent
)

find_package(KF6 REQUIRED COMPONENTS
    CoreAddons
    I18n
    Notifications
    WindowSystem
)

find_package(MauiKit4 REQUIRED)

ecm_find_qmlmodule(org.mauikit.controls 1.0)

find_package(PolkitQt6-1 REQUIRED)

# Build options
option(BUILD_TESTING "Build tests" OFF)

# Add subdirectories
add_subdirectory(src)
add_subdirectory(data)

# Feature summary
feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
