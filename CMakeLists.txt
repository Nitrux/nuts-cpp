cmake_minimum_required(VERSION 3.16)

project(nuts VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# x86-64-v3 optimizations
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v3 -mtune=generic")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=x86-64-v3 -mtune=generic")

# Find required packages
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})

include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(ECMSetupVersion)
include(FeatureSummary)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Quick
    Qml
    DBus
    Widgets
)

find_package(KF6 REQUIRED COMPONENTS
    CoreAddons
    I18n
    Notifications
)

find_package(MauiKit4 REQUIRED)
find_package(PolkitQt6-1 REQUIRED)

# Build options
option(BUILD_TESTING "Build tests" OFF)

# Add subdirectories
add_subdirectory(src)
add_subdirectory(data)

# Feature summary
feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
