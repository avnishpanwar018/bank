cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(bank_txn_system VERSION 0.1.0 LANGUAGES CXX)

set(SANITIZE "" CACHE STRING "Enable a sanitizer: thread or address")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

if(SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "SANITIZE requires GCC or Clang; use a Linux/WSL toolchain")
    elseif(NOT SANITIZE STREQUAL "thread" AND NOT SANITIZE STREQUAL "address")
        message(FATAL_ERROR "SANITIZE must be empty, thread, or address")
    else()
        add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${SANITIZE})
    endif()
endif()

find_package(Threads REQUIRED)

if(DEFINED ENV{MYSQL_INCLUDE_DIR} AND EXISTS "$ENV{MYSQL_INCLUDE_DIR}")
    set(MYSQL_INCLUDE_DIR "$ENV{MYSQL_INCLUDE_DIR}")
else()
    find_path(MYSQL_INCLUDE_DIR
        NAMES mysql.h mysql/mysql.h
        HINTS
            "${MYSQL_DIR}"
            ENV MYSQL_DIR
        PATH_SUFFIXES include include/mysql
    )
endif()

if(MYSQL_INCLUDE_DIR MATCHES "/mysql$" OR MYSQL_INCLUDE_DIR MATCHES "\\\\mysql$")
    get_filename_component(MYSQL_INCLUDE_DIR "${MYSQL_INCLUDE_DIR}" DIRECTORY)
endif()

if(DEFINED ENV{MYSQL_LIBRARY} AND EXISTS "$ENV{MYSQL_LIBRARY}")
    set(MYSQL_LIBRARY "$ENV{MYSQL_LIBRARY}")
else()
    find_library(MYSQL_LIBRARY
        NAMES mysqlclient mysql libmysql
        HINTS
            "${MYSQL_DIR}"
            ENV MYSQL_DIR
        PATH_SUFFIXES lib lib64
    )
endif()

if(MINGW AND MYSQL_LIBRARY MATCHES "\\.lib$")
    get_filename_component(MYSQL_LIBRARY_DIR "${MYSQL_LIBRARY}" DIRECTORY)
    find_file(MYSQL_RUNTIME_LIBRARY
        NAMES libmysql.dll
        PATHS "${MYSQL_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
    if(MYSQL_RUNTIME_LIBRARY)
        set(MYSQL_LIBRARY "${MYSQL_RUNTIME_LIBRARY}" CACHE FILEPATH
            "MySQL client library" FORCE)
    else()
        message(FATAL_ERROR
            "The selected MySQL client is an MSVC .lib and no libmysql.dll was found beside it. "
            "Install a MinGW-compatible .dll.a client or provide MYSQL_LIBRARY=libmysql.dll.")
    endif()
endif()

if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
    message(FATAL_ERROR
        "MySQL client development files were not found. Set MYSQL_INCLUDE_DIR "
        "and MYSQL_LIBRARY or install MySQL Connector/C.")
endif()

add_library(MySQL::Client UNKNOWN IMPORTED)
if(WIN32 AND MSVC)
    set_target_properties(MySQL::Client PROPERTIES IMPORTED_IMPLIB "${MYSQL_LIBRARY}")
else()
    set_target_properties(MySQL::Client PROPERTIES IMPORTED_LOCATION "${MYSQL_LIBRARY}")
endif()
set_target_properties(MySQL::Client PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MYSQL_INCLUDE_DIR}")

include(FetchContent)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
# Compile the large third-party implementation once, rather than in every
# dashboard/test translation unit (important on memory-constrained machines).
set(HTTPLIB_COMPILE ON CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)
FetchContent_Declare(
    httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.zip
)
FetchContent_MakeAvailable(httplib)

add_library(bank_core
    src/config.cpp
    src/money.cpp
    src/db/db_connection.cpp
    src/db/connection_pool.cpp
    src/core/account_manager.cpp
    src/core/lock_manager.cpp
    src/core/transaction_manager.cpp
    src/concurrency/thread_pool.cpp
    src/sim/monitor.cpp
    src/sim/simulator.cpp
    src/logging/txn_log.cpp
    src/tools/recovery.cpp
    src/tools/consistency_checker.cpp
    src/web/dashboard_server.cpp
)
target_include_directories(bank_core PUBLIC include)
target_link_libraries(bank_core PUBLIC MySQL::Client Threads::Threads httplib::httplib)

add_executable(bank src/main.cpp)
target_link_libraries(bank PRIVATE bank_core)

if(WIN32 AND MYSQL_LIBRARY MATCHES "\\.dll$")
    add_custom_command(TARGET bank POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MYSQL_LIBRARY}" "$<TARGET_FILE_DIR:bank>")
endif()

enable_testing()
add_executable(bank_tests
    tests/bounded_queue_test.cpp
    tests/concurrency_integration_test.cpp
    tests/synchronization_integration_test.cpp
    tests/phase5_integration_test.cpp
    tests/stress_integration_test.cpp
    tests/config_test.cpp
    tests/money_test.cpp
    tests/monitor_test.cpp
    tests/dashboard_http_test.cpp
    tests/dashboard_counters_test.cpp
    tests/db_connection_smoke_test.cpp
    tests/account_manager_integration_test.cpp
)
target_link_libraries(bank_tests PRIVATE bank_core GTest::gtest_main)
if(WIN32 AND MYSQL_LIBRARY MATCHES "\\.dll$")
    add_custom_command(TARGET bank_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MYSQL_LIBRARY}" "$<TARGET_FILE_DIR:bank_tests>")
endif()
include(GoogleTest)
gtest_discover_tests(bank_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    PROPERTIES RUN_SERIAL TRUE)
