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

if(BUILD_TESTING)
    add_executable(olivia_core_tests
        cpp/tests/olivia_modem_tests.cpp
    )
    target_link_libraries(olivia_core_tests PRIVATE olivia_core)
    add_test(NAME olivia_core_tests COMMAND olivia_core_tests)
endif()
