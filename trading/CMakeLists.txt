set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_CXX_FLAGS "-std=c++2a -Wall -Wextra -Werror -Wpedantic")
set(CMAKE_VERBOSE_MAKEFILE on)

file(GLOB SOURCES "*/*.cpp")

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/trading)

add_library(libtrading STATIC ${SOURCES} strategy/risk_manager.cpp)

enable_testing()
add_executable(strategy_performance_tracker_test strategy/strategy_performance_tracker_test.cpp)
target_link_libraries(strategy_performance_tracker_test PRIVATE libtrading)
add_test(NAME strategy_performance_tracker_test COMMAND strategy_performance_tracker_test)
