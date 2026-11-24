cmake_minimum_required(VERSION 3.24)

# clang 19 builds everything, here and in the engines. Has to be set before project(), which is
# where cmake goes looking. C as well as C++, since BOSS and ViewEngine both have C in them.
if(NOT CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER clang++-19)
endif()
if(NOT CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER clang-19)
endif()

project(Ablation C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # clangd reads ./compile_commands.json (symlink into build/)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Where the TPC-H .tbl files live. Both BOSS and the SQL systems read from here, so it is set
# once and handed to both.
set(TPCH_DATA_DIR "/mnt/data/projects/lazy-transformations/tpch/sf1" CACHE STRING "directory holding the TPC-H .tbl files")

# The dbgen build the stream experiments shell out to for query parameters. Nothing here builds
# it, so it stays a path.
set(DBGEN_DIR "/mnt/apps/projects/lazy-transformations/BOSSKernelBenchmarks/dbgen"
    CACHE STRING "TPC-H dbgen directory, for the qgen binary")

find_package(Threads REQUIRED)
include(ExternalProject)

set(BOSS_SOURCE_BRANCH a7f0d49 CACHE STRING "BOSS commit to build against")
set(BOSS_SOURCE_REPOSITORY https://github.com/symbol-store/BOSS.git CACHE STRING "URL for the BOSS repository")
set(BOSS_DEPS "${CMAKE_BINARY_DIR}/deps")

ExternalProject_Add(BOSS
  GIT_REPOSITORY ${BOSS_SOURCE_REPOSITORY}
  GIT_TAG ${BOSS_SOURCE_BRANCH}
  DOWNLOAD_DIR $ENV{HOME}/.cmake-downloads/${CMAKE_PROJECT_NAME}
  UPDATE_COMMAND ""
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${BOSS_DEPS}
    # cmake strips the build tree's rpath on install, and libBOSS needs chibi-scheme from
    # deps/lib at runtime. Without this the binaries only run under LD_LIBRARY_PATH.
    -DCMAKE_INSTALL_RPATH=${BOSS_DEPS}/lib
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  BUILD_BYPRODUCTS ${BOSS_DEPS}/lib/libBOSS.so)

set(VIEWENGINE_SOURCE_BRANCH 854fc2e CACHE STRING "ViewEngine commit to build against")
set(VIEWENGINE_SOURCE_REPOSITORY https://github.com/SvilenDilchev/ViewEngine.git CACHE STRING "URL for the ViewEngine repository")

ExternalProject_Add(ViewEngine
  GIT_REPOSITORY ${VIEWENGINE_SOURCE_REPOSITORY}
  GIT_TAG ${VIEWENGINE_SOURCE_BRANCH}
  DOWNLOAD_DIR $ENV{HOME}/.cmake-downloads/${CMAKE_PROJECT_NAME}
  UPDATE_COMMAND ""
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${BOSS_DEPS}
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  BUILD_BYPRODUCTS ${BOSS_DEPS}/lib/libViewEngine.so)

# ArrowComputeEngine, from the fork carrying the changes the evaluation was run against. It
# builds Arrow from source, so the first build here takes a while. Installs into deps/lib too.
set(ACE_SOURCE_BRANCH a023f49 CACHE STRING "ArrowComputeEngine commit to build against")
set(ACE_SOURCE_REPOSITORY https://github.com/SvilenDilchev/ArrowComputeEngine.git CACHE STRING "URL for the ArrowComputeEngine repository")

ExternalProject_Add(ArrowComputeEngine
  GIT_REPOSITORY ${ACE_SOURCE_REPOSITORY}
  GIT_TAG ${ACE_SOURCE_BRANCH}
  DOWNLOAD_DIR $ENV{HOME}/.cmake-downloads/${CMAKE_PROJECT_NAME}
  UPDATE_COMMAND ""
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${BOSS_DEPS}
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  BUILD_BYPRODUCTS ${BOSS_DEPS}/lib/libArrowComputeEngine.so)

add_library(ablation STATIC
  Source/BossQueries.cpp Source/Queries.cpp Source/Streams.cpp Source/Output.cpp
  Source/LiteViews.cpp Source/Views.cpp Source/StreamExperiment.cpp)
add_dependencies(ablation BOSS ViewEngine ArrowComputeEngine)
target_include_directories(ablation PUBLIC "${BOSS_DEPS}/include" Source)
target_link_libraries(ablation PUBLIC "${BOSS_DEPS}/lib/libBOSS.so" Threads::Threads dl)
# Nothing is looked up relative to the working directory: the paths are compiled in, so the
# binaries run correctly from anywhere and the repo can be checked out anywhere.
target_compile_definitions(ablation PRIVATE
  REPO_DIR="${CMAKE_SOURCE_DIR}"
  ENGINE_DIR="${BOSS_DEPS}/lib"
  TPCH_DATA_DIR="${TPCH_DATA_DIR}"
  DBGEN_DIR="${DBGEN_DIR}")

add_library(systems_embedded STATIC Source/SystemsEmbedded.cpp)
set_target_properties(systems_embedded PROPERTIES CXX_STANDARD 17) # duckdb.hpp is not C++23-clean
target_compile_definitions(systems_embedded PRIVATE TPCH_DATA_DIR="${TPCH_DATA_DIR}")
target_include_directories(systems_embedded PRIVATE
  "${CMAKE_SOURCE_DIR}/systems/duckdb-0.8.1"
  "${CMAKE_SOURCE_DIR}/systems/monetdb-jun2023/install/include")
target_link_libraries(systems_embedded PUBLIC
  "${CMAKE_SOURCE_DIR}/systems/duckdb-0.8.1/libduckdb.so"
  "${CMAKE_SOURCE_DIR}/systems/monetdb-jun2023/install/lib/libmonetdbe.so")

foreach(exp Exp0 Exp1 Exp2 Exp3 Exp4 Exp5)
  add_executable(${exp} Source/${exp}.cpp)
  target_link_libraries(${exp} PRIVATE ablation)
  target_compile_definitions(${exp} PRIVATE REPO_DIR="${CMAKE_SOURCE_DIR}")
  set_target_properties(${exp} PROPERTIES BUILD_RPATH "${BOSS_DEPS}/lib")
endforeach()
target_link_libraries(Exp1 PRIVATE systems_embedded)
# The harness of exp1 and exp2: google benchmark v1.8.3, MultiKernelBOSS's pin (systems/setup.sh).
foreach(exp Exp1 Exp2)
  target_include_directories(${exp} PRIVATE "${CMAKE_SOURCE_DIR}/systems/benchmark/install/include")
  target_link_libraries(${exp} PRIVATE "${CMAKE_SOURCE_DIR}/systems/benchmark/install/lib/libbenchmark.a")
endforeach()
set_target_properties(Exp1 PROPERTIES BUILD_RPATH
  "${BOSS_DEPS}/lib;${CMAKE_SOURCE_DIR}/systems/duckdb-0.8.1;${CMAKE_SOURCE_DIR}/systems/monetdb-jun2023/install/lib")
