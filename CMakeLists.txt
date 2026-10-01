# CMake build for cppdecnet.
#
# The Makefile is the primary build on Linux and the BSDs.  This file exists
# for Windows (MSVC), where there is no make; it also works on Linux.
#
#   cmake -S . -B build/cmake -G Ninja
#   cmake --build build/cmake
#   ctest --test-dir build/cmake

cmake_minimum_required (VERSION 3.20)
project (cppdecnet VERSION 0.1.0 LANGUAGES CXX)

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)

option (CPPDECNET_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})

if (MSVC)
    set (DN_WARN /W4 /permissive- /Zc:__cplusplus /utf-8
                 /wd4100   # unreferenced parameter, as -Wno-unused-parameter
                 /wd4458)  # declaration hides class member: style of this code
    set (DN_DEFS _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX
                 _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE
                 _WINSOCK_DEPRECATED_NO_WARNINGS)
else ()
    set (DN_WARN -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)
    set (DN_DEFS)
endif ()

# ------------------------------------------------------------------ library
file (GLOB_RECURSE DN_LIB_SRCS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cc")
list (FILTER DN_LIB_SRCS EXCLUDE REGEX "/src/main/")

add_library (decnet STATIC ${DN_LIB_SRCS})
target_include_directories (decnet PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_options (decnet PRIVATE ${DN_WARN})
target_compile_definitions (decnet PUBLIC ${DN_DEFS})

find_package (Threads REQUIRED)
target_link_libraries (decnet PUBLIC Threads::Threads)
if (WIN32)
    target_link_libraries (decnet PUBLIC ws2_32 bcrypt)
else ()
    target_link_libraries (decnet PUBLIC crypt)
endif ()

# libpcap (Ethernet datalink): optional, and not used on Windows for now.
if (NOT WIN32)
    find_library (PCAP_LIBRARY pcap)
    if (PCAP_LIBRARY)
        target_compile_definitions (decnet PUBLIC DN_HAVE_PCAP=1)
        target_link_libraries (decnet PUBLIC ${PCAP_LIBRARY})
    endif ()
endif ()

# ------------------------------------------------------------ daemon, tools
add_executable (decnetd src/main/main.cc)
target_link_libraries (decnetd PRIVATE decnet)
target_compile_options (decnetd PRIVATE ${DN_WARN})

file (GLOB DN_TOOL_SRCS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tools/*.cc")
foreach (src IN LISTS DN_TOOL_SRCS)
    get_filename_component (name "${src}" NAME_WE)
    add_executable (${name} "${src}")
    target_link_libraries (${name} PRIVATE decnet)
    target_compile_options (${name} PRIVATE ${DN_WARN})
endforeach ()

# -------------------------------------------------------------------- tests
if (CPPDECNET_TESTS)
    enable_testing ()
    file (GLOB DN_TEST_SRCS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tests/test_*.cc")
    file (GLOB DN_TEST_SUPPORT CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tests/*.cc")
    list (REMOVE_ITEM DN_TEST_SUPPORT ${DN_TEST_SRCS})
    add_library (dntestharness STATIC ${DN_TEST_SUPPORT})
    target_link_libraries (dntestharness PUBLIC decnet)
    foreach (src IN LISTS DN_TEST_SRCS)
        get_filename_component (name "${src}" NAME_WE)
        add_executable (${name} "${src}")
        target_link_libraries (${name} PRIVATE dntestharness)
        target_compile_options (${name} PRIVATE ${DN_WARN})
        add_test (NAME ${name} COMMAND ${name}
                  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    endforeach ()
endif ()

install (TARGETS decnetd)
