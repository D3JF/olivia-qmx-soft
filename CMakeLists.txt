cmake_minimum_required(VERSION 3.16)

project(olivia_qmx_cpp LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(olivia_core
    cpp/src/olivia_modem.cpp
)

target_include_directories(olivia_core
    PUBLIC
        cpp/include
)

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
    target_link_libraries(olivia_gui PRIVATE
        olivia_core
        Qt5::Widgets
        Qt5::Network
    )
    if(WIN32)
        target_link_libraries(olivia_gui PRIVATE ws2_32)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(olivia_core_tests
        cpp/tests/olivia_modem_tests.cpp
    )
    target_link_libraries(olivia_core_tests PRIVATE olivia_core)
    add_test(NAME olivia_core_tests COMMAND olivia_core_tests)
endif()
