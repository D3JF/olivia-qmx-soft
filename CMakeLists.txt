cmake_minimum_required(VERSION 3.16)

project(olivia_qmx_cpp LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OLIVIA_AUDIO "AUTO" CACHE STRING
    "Audio backend selection: AUTO, ON, or OFF")
set_property(CACHE OLIVIA_AUDIO PROPERTY STRINGS AUTO ON OFF)
set(PORTAUDIO_ROOT "" CACHE PATH "PortAudio installation prefix")

add_library(olivia_core
    cpp/src/olivia_modem.cpp
)

add_library(olivia_serial
    cpp/src/serial_port.cpp
    cpp/src/qmx_radio.cpp
)
target_include_directories(olivia_serial PUBLIC cpp/include)

add_executable(olivia_serial_info cpp/src/olivia_serial_info.cpp)
target_link_libraries(olivia_serial_info PRIVATE olivia_serial)

target_include_directories(olivia_core
    PUBLIC
        cpp/include
)

if(NOT OLIVIA_AUDIO STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND AND NOT CMAKE_CROSSCOMPILING)
        pkg_check_modules(PORTAUDIO QUIET IMPORTED_TARGET portaudio-2.0)
    endif()
    if(NOT TARGET PkgConfig::PORTAUDIO AND PORTAUDIO_ROOT)
        find_path(PORTAUDIO_INCLUDE_DIR portaudio.h
            PATHS "${PORTAUDIO_ROOT}/include" NO_DEFAULT_PATH)
        find_library(PORTAUDIO_LIBRARY NAMES portaudio
            PATHS "${PORTAUDIO_ROOT}/lib" "${PORTAUDIO_ROOT}/lib64"
            NO_DEFAULT_PATH)
        if(PORTAUDIO_INCLUDE_DIR AND PORTAUDIO_LIBRARY)
            add_library(PortAudio::PortAudio UNKNOWN IMPORTED)
            set_target_properties(PortAudio::PortAudio PROPERTIES
                IMPORTED_LOCATION "${PORTAUDIO_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${PORTAUDIO_INCLUDE_DIR}")
        endif()
    endif()
endif()
if(TARGET PkgConfig::PORTAUDIO OR TARGET PortAudio::PortAudio)
    if(TARGET PkgConfig::PORTAUDIO)
        set(OLIVIA_PORTAUDIO_TARGET PkgConfig::PORTAUDIO)
    else()
        set(OLIVIA_PORTAUDIO_TARGET PortAudio::PortAudio)
    endif()
    add_library(olivia_audio
        cpp/src/audio_backend.cpp
    )
    target_include_directories(olivia_audio
        PUBLIC
            cpp/include
    )
    target_link_libraries(olivia_audio
        PUBLIC
            ${OLIVIA_PORTAUDIO_TARGET}
    )
    add_executable(olivia_audio_info
        cpp/src/olivia_audio_info.cpp
    )
    target_link_libraries(olivia_audio_info PRIVATE olivia_audio)
endif()

add_executable(olivia_app
    cpp/src/olivia_app.cpp
)
target_link_libraries(olivia_app PRIVATE olivia_core)
if(WIN32)
    target_link_libraries(olivia_app PRIVATE ws2_32)
endif()

find_package(Qt5 COMPONENTS Widgets Network QUIET)
if(Qt5_FOUND)
    set(CMAKE_AUTOMOC ON)
    add_executable(olivia_gui
        cpp/src/olivia_gui.cpp
    )
        if(WIN32)
            set_target_properties(olivia_gui PROPERTIES WIN32_EXECUTABLE TRUE)
        endif()
        target_link_libraries(olivia_gui PRIVATE
        olivia_core
            olivia_serial
            Qt5::Widgets
            Qt5::Network
    )
    if(TARGET olivia_audio)
        target_link_libraries(olivia_gui PRIVATE olivia_audio)
        target_compile_definitions(olivia_gui PRIVATE OLIVIA_HAS_PORTAUDIO=1)
    endif()
    if(WIN32)
        target_link_libraries(olivia_gui PRIVATE ws2_32)
    endif()
endif()
if(OLIVIA_AUDIO STREQUAL "ON" AND NOT TARGET olivia_audio)
    message(FATAL_ERROR
        "OLIVIA_AUDIO=ON but no PortAudio installation was found. "
        "Install PortAudio or set PORTAUDIO_ROOT.")
endif()

if(BUILD_TESTING)
    add_executable(olivia_core_tests
        cpp/tests/olivia_modem_tests.cpp
    )
    target_link_libraries(olivia_core_tests PRIVATE olivia_core)
    add_test(NAME olivia_core_tests COMMAND olivia_core_tests)

    add_executable(olivia_serial_tests
        cpp/tests/qmx_radio_tests.cpp
    )
    target_link_libraries(olivia_serial_tests PRIVATE olivia_serial)
    add_test(NAME olivia_serial_tests COMMAND olivia_serial_tests)
endif()
