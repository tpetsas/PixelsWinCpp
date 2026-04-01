cmake_minimum_required(VERSION 3.20)
project(PixelsWinCpp VERSION 1.0.0 LANGUAGES CXX)

# Require C++17 for coroutines
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 10 SDK minimum version
set(CMAKE_SYSTEM_VERSION 10.0.17134.0)

# Find CppWinRT - use vcpkg or NuGet package
# For simplicity, we'll use the NuGet package approach via a custom target

# Define source files for the library
set(PIXELS_LIB_SOURCES
    BluetoothLE.cpp
    ComHelper.cpp
    Peripheral.cpp
    Pixel.cpp
    PixelBleUuids.cpp
    PixelInfo.cpp
    PixelScanner.cpp
)

set(PIXELS_LIB_HEADERS
    Systemic/BluetoothLE/BleTypes.h
    Systemic/BluetoothLE/BluetoothLE.h
    Systemic/BluetoothLE/Characteristic.h
    Systemic/BluetoothLE/Peripheral.h
    Systemic/BluetoothLE/ScannedPeripheral.h
    Systemic/BluetoothLE/Scanner.h
    Systemic/BluetoothLE/Service.h
    Systemic/ComHelper.h
    Systemic/Internal/GuardedList.h
    Systemic/Internal/Logger.h
    Systemic/Internal/Utils.h
    Systemic/Pixels/Helpers.h
    Systemic/Pixels/Messages.h
    Systemic/Pixels/MessageSerialization.h
    Systemic/Pixels/Pixel.h
    Systemic/Pixels/PixelBleUuids.h
    Systemic/Pixels/PixelInfo.h
    Systemic/Pixels/PixelScanner.h
    Systemic/Pixels/PixelTypes.h
    Systemic/Pixels/ScannedPixel.h
)

# Create the Pixels library as a static library
add_library(PixelsLib STATIC ${PIXELS_LIB_SOURCES} ${PIXELS_LIB_HEADERS} pch.cpp pch.h)

target_include_directories(PixelsLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Precompiled header
target_precompile_headers(PixelsLib PRIVATE pch.h)

# Compiler definitions
target_compile_definitions(PixelsLib PRIVATE
    WIN32_LEAN_AND_MEAN
    WINRT_LEAN_AND_MEAN
    NOMINMAX
    _CONSOLE
)

# Compiler options for MSVC
if(MSVC)
    target_compile_options(PixelsLib PRIVATE
        /permissive-
        /bigobj
        /await
        /W4
    )
endif()

# Link Windows Runtime libraries
target_link_libraries(PixelsLib PUBLIC
    windowsapp
)

# Create the example executable
add_executable(Pixels
    app_main.cpp
    App/CliOptions.cpp
    App/ConfigManager.cpp
    App/DieConnection.cpp
    App/DiceManager.cpp
    App/CliOptions.h
    App/ConfigManager.h
    App/DieConnection.h
    App/DiceManager.h
)

target_link_libraries(Pixels PRIVATE PixelsLib)

# Use precompiled header for example too
target_precompile_headers(Pixels REUSE_FROM PixelsLib)

# Compiler definitions for example
target_compile_definitions(Pixels PRIVATE
    WIN32_LEAN_AND_MEAN
    WINRT_LEAN_AND_MEAN
    NOMINMAX
    _CONSOLE
)

if(MSVC)
    target_compile_options(Pixels PRIVATE
        /permissive-
        /bigobj
        /await
        /W4
    )
endif()

# Set output directories
set_target_properties(PixelsLib Pixels PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Install targets
install(TARGETS PixelsLib Pixels
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY Systemic/ DESTINATION include/Systemic
    FILES_MATCHING PATTERN "*.h"
)

message(STATUS "")
message(STATUS "=== Pixels Dice Library for Windows ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "Targets:")
message(STATUS "  - PixelsLib (static library)")
message(STATUS "  - Pixels (test application)")
message(STATUS "")
