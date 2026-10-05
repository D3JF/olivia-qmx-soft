cmake_minimum_required(VERSION 3.16)

project(olivia_qmx_cpp VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

option(OLIVIA_BUILD_INSTALLER
    "Prepare a Windows installer with Qt and compiler runtimes"
    ${WIN32})

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
    if(PORTAUDIO_ROOT)
        if(EXISTS "${PORTAUDIO_ROOT}/include/portaudio.h")
            set(PORTAUDIO_INCLUDE_DIR "${PORTAUDIO_ROOT}/include")
        endif()
        foreach(portaudio_library_name
                libportaudio.dll.a libportaudio.a portaudio.lib)
            if(EXISTS "${PORTAUDIO_ROOT}/lib/${portaudio_library_name}")
                set(PORTAUDIO_LIBRARY
                    "${PORTAUDIO_ROOT}/lib/${portaudio_library_name}")
                break()
            endif()
            if(EXISTS "${PORTAUDIO_ROOT}/lib64/${portaudio_library_name}")
                set(PORTAUDIO_LIBRARY
                    "${PORTAUDIO_ROOT}/lib64/${portaudio_library_name}")
                break()
            endif()
        endforeach()
        if(PORTAUDIO_INCLUDE_DIR AND PORTAUDIO_LIBRARY)
            add_library(Olivia::PortAudio UNKNOWN IMPORTED)
            set_target_properties(Olivia::PortAudio PROPERTIES
                IMPORTED_LOCATION "${PORTAUDIO_LIBRARY}"
                IMPORTED_IMPLIB "${PORTAUDIO_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${PORTAUDIO_INCLUDE_DIR}")
        endif()
    endif()
endif()
if(TARGET Olivia::PortAudio OR TARGET PkgConfig::PORTAUDIO)
    if(TARGET Olivia::PortAudio)
        set(OLIVIA_PORTAUDIO_TARGET Olivia::PortAudio)
    else()
        set(OLIVIA_PORTAUDIO_TARGET PkgConfig::PORTAUDIO)
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
    set(CMAKE_AUTORCC ON)
    add_executable(olivia_gui
        cpp/src/olivia_gui.cpp
        resources/olivia.qrc
    )
        if(WIN32)
            target_sources(olivia_gui PRIVATE
                resources/olivia.rc
            )
            set_target_properties(olivia_gui PROPERTIES
                WIN32_EXECUTABLE TRUE
            )
        endif()
        target_link_libraries(olivia_gui PRIVATE
        olivia_core
            olivia_serial
            Qt5::Widgets
            Qt5::Network
    )
    if(TARGET olivia_audio)
        target_sources(olivia_gui PRIVATE
            cpp/include/real_worker.hpp
            cpp/src/real_worker.cpp
        )
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

install(TARGETS olivia_app olivia_serial_info
    RUNTIME DESTINATION .
)
if(TARGET olivia_gui)
    install(TARGETS olivia_gui
        RUNTIME DESTINATION .
    )
endif()
if(TARGET olivia_audio_info)
    install(TARGETS olivia_audio_info
        RUNTIME DESTINATION .
    )
endif()
install(FILES
    README.md
    LICENSE
    DESTINATION .
)

if(WIN32 AND OLIVIA_BUILD_INSTALLER)
    find_program(WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt windeployqt.exe
        HINTS
            "${Qt5Core_DIR}/../../../bin"
    )
    if(NOT WINDEPLOYQT_EXECUTABLE)
        message(FATAL_ERROR
            "OLIVIA_BUILD_INSTALLER=ON requires Qt's windeployqt in PATH "
            "or next to the Qt installation.")
    endif()
    if(NOT TARGET olivia_gui)
        message(FATAL_ERROR
            "OLIVIA_BUILD_INSTALLER=ON requires Qt5 and the olivia_gui target.")
    endif()
    if(TARGET olivia_audio)
        if(NOT PORTAUDIO_ROOT)
            message(FATAL_ERROR
                "The audio installer requires PORTAUDIO_ROOT so its DLL can "
                "be copied next to the application.")
        endif()
        find_file(OLIVIA_PORTAUDIO_DLL
            NAMES libportaudio.dll portaudio.dll
            HINTS
                "${PORTAUDIO_ROOT}/bin"
                "${PORTAUDIO_ROOT}"
            NO_DEFAULT_PATH
        )
        if(NOT OLIVIA_PORTAUDIO_DLL)
            message(FATAL_ERROR
                "Could not find the PortAudio DLL below PORTAUDIO_ROOT.")
        endif()
        install(FILES "${OLIVIA_PORTAUDIO_DLL}" DESTINATION .)
    endif()

    install(CODE "
        execute_process(
            COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                --no-translations
                --compiler-runtime
                \"\\\${CMAKE_INSTALL_PREFIX}/olivia_gui.exe\"
            RESULT_VARIABLE deploy_result
        )
        if(deploy_result)
            message(FATAL_ERROR
                \"windeployqt failed with exit code \\\${deploy_result}\")
        endif()
    ")
endif()

set(CPACK_PACKAGE_NAME "Olivia QMX+")
set(CPACK_PACKAGE_VENDOR "D3JF")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Olivia MFSK terminal for the QRP Labs QMX+")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Olivia QMX+")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "Olivia QMX+")
    set(CPACK_NSIS_PACKAGE_NAME "Olivia QMX+")
    set(CPACK_NSIS_HELP_LINK
        "https://github.com/D3JF/olivia-qmx-soft")
    set(CPACK_NSIS_URL_INFO_ABOUT
        "https://github.com/D3JF/olivia-qmx-soft")
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_MENU_LINKS "olivia_gui.exe" "Olivia QMX+")
endif()
include(CPack)
