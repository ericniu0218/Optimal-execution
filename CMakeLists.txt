# Optimal Execution Engine (Almgren-Chriss) — build configuration.
#
# Layout: one static library (oee) holding all engine code, a test binary
# linking GoogleTest, and (later) thin CLI apps. GoogleTest is pulled via
# FetchContent so a fresh clone builds with no manual dependency setup.

cmake_minimum_required(VERSION 3.20)
project(optimal_execution LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# -DOEE_SANITIZE=address,undefined builds everything under sanitizers.
set(OEE_SANITIZE "" CACHE STRING "Sanitizers to enable (e.g. address,undefined)")
if(OEE_SANITIZE)
  add_compile_options(-fsanitize=${OEE_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${OEE_SANITIZE})
endif()

add_library(oee
  cpp/src/data/lobster_reader.cpp
  cpp/src/data/trade_events.cpp
  cpp/src/data/tape_cache.cpp
  cpp/src/ac/solver.cpp
  cpp/src/market/execution_simulator.cpp
  cpp/src/strategy/scheduled.cpp
  cpp/src/strategy/pov.cpp
  cpp/src/strategy/adaptive_ac.cpp
  cpp/src/backtest/backtester.cpp
  cpp/src/book/rebuild.cpp
)
target_include_directories(oee PUBLIC cpp/include)
target_compile_options(oee PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Apps
# ---------------------------------------------------------------------------
add_executable(oee_smoke cpp/apps/oee_smoke.cpp)
target_link_libraries(oee_smoke PRIVATE oee)

add_executable(oee_backtest cpp/apps/oee_backtest.cpp)
target_link_libraries(oee_backtest PRIVATE oee)

add_executable(oee_dump cpp/apps/oee_dump.cpp)
target_link_libraries(oee_dump PRIVATE oee)

add_executable(oee_frontier cpp/apps/oee_frontier.cpp)
target_link_libraries(oee_frontier PRIVATE oee)

add_executable(oee_windows cpp/apps/oee_windows.cpp)
target_link_libraries(oee_windows PRIVATE oee)

add_executable(oee_rebuild cpp/apps/oee_rebuild.cpp)
target_link_libraries(oee_rebuild PRIVATE oee)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(oee_tests
  cpp/tests/test_lobster_reader.cpp
  cpp/tests/test_trade_events.cpp
  cpp/tests/test_ac_solver.cpp
  cpp/tests/test_execution_simulator.cpp
  cpp/tests/test_strategies.cpp
  cpp/tests/test_backtester.cpp
  cpp/tests/test_rebuild.cpp
  cpp/tests/test_tape_cache.cpp
  cpp/tests/test_impact_model.cpp
  cpp/tests/test_adaptive_ac.cpp
)
target_link_libraries(oee_tests PRIVATE oee GTest::gtest_main)
target_compile_options(oee_tests PRIVATE -Wall -Wextra -Wpedantic)

include(GoogleTest)
gtest_discover_tests(oee_tests)
