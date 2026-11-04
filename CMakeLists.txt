cmake_minimum_required(VERSION 3.22)
project(realtime-gateway VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GATEWAY_SANITIZERS "Enable ASan and UBSan" OFF)
option(GATEWAY_TSAN "Enable ThreadSanitizer separately" OFF)
if(GATEWAY_SANITIZERS AND GATEWAY_TSAN)
    message(FATAL_ERROR "TSan and ASan/UBSan require separate build directories")
endif()
if(GATEWAY_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
    add_link_options(-fsanitize=address,undefined -no-pie)
endif()
if(GATEWAY_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -fno-pie)
    add_link_options(-fsanitize=thread -no-pie)
endif()

find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(Protobuf REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

execute_process(
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}" --version
    OUTPUT_VARIABLE PROTOC_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT PROTOC_VERSION STREQUAL "libprotoc ${Protobuf_VERSION}")
    message(FATAL_ERROR "protoc/runtime mismatch: ${PROTOC_VERSION} vs ${Protobuf_VERSION}")
endif()

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS proto/gateway.proto)
add_library(gateway_proto ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(gateway_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gateway_proto PUBLIC protobuf::libprotobuf)

add_library(gateway_core
    src/connection_policy.cpp
    src/resource_budget.cpp
    src/frame.cpp
    src/buffered_input.cpp
    src/config.cpp
    src/business.cpp
    src/bounded_task_queue.cpp
    src/dispatcher.cpp
    src/server.cpp
)
target_include_directories(gateway_core PUBLIC include)
target_link_libraries(gateway_core PUBLIC
    gateway_proto
    Boost::system
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    Threads::Threads
)
target_compile_options(gateway_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(gateway_server src/main.cpp)
target_link_libraries(gateway_server PRIVATE gateway_core)

add_executable(device_client src/client.cpp)
target_link_libraries(device_client PRIVATE gateway_core)

# 独立压测客户端；不引入生产服务端配置或延迟模拟。
add_executable(gateway_bench benchmarks/gateway_bench.cpp)
target_link_libraries(gateway_bench PRIVATE gateway_core)
target_compile_options(gateway_bench PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import google.protobuf"
        RESULT_VARIABLE PY_PROTOBUF
    )
    if(NOT PY_PROTOBUF EQUAL 0)
        message(FATAL_ERROR
            "Python protobuf missing; use /usr/bin/python3 and apt python3-protobuf"
        )
    endif()

    add_executable(gateway_tests
        tests/connection_policy_test.cpp
        tests/unit.cpp
        tests/buffered_input_test.cpp
        tests/dispatcher_test.cpp
        tests/resource_budget_test.cpp
    )
    target_link_libraries(gateway_tests PRIVATE gateway_core GTest::gtest_main)
    add_test(NAME unit COMMAND gateway_tests)
    set_tests_properties(unit PROPERTIES TIMEOUT 30)

    add_executable(gateway_async_tests tests/async_server_test.cpp)
    target_link_libraries(gateway_async_tests PRIVATE gateway_core GTest::gtest_main)
    add_test(NAME async_server_integration COMMAND gateway_async_tests)
    set_tests_properties(async_server_integration PROPERTIES TIMEOUT 40)

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/python")
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/python/gateway_pb2.py"
        COMMAND protobuf::protoc
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
            --python_out=${CMAKE_CURRENT_BINARY_DIR}/python
            ${CMAKE_CURRENT_SOURCE_DIR}/proto/gateway.proto
        DEPENDS proto/gateway.proto
        VERBATIM
    )
    add_custom_target(python_proto ALL
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/python/gateway_pb2.py"
    )

    add_test(
        NAME socket_integration
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py"
            "$<TARGET_FILE:gateway_server>"
            "${CMAKE_CURRENT_BINARY_DIR}/python"
            "$<TARGET_FILE:device_client>"
    )
    set_tests_properties(socket_integration PROPERTIES TIMEOUT 40)
    add_test(
        NAME benchmark_analysis
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_analysis_test.py"
    )
    set_tests_properties(benchmark_analysis PROPERTIES TIMEOUT 10)
endif()
