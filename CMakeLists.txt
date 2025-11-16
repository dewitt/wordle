# CMakeLists.txt for the C++ Wordle Solver

# Set the minimum required version of CMake
cmake_minimum_required(VERSION 3.10)

# Set the project name
project(WordleSolverCpp)

# Set the C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add the executable and specify its source files
add_executable(solver
  solver_main.cpp
  words_data.cpp
  feedback_cache.cpp
  solver_core.cpp
  solver_runtime.cpp
  lookup_generator.cpp
)

# Optional: Enable optimizations for release builds
set(CMAKE_BUILD_TYPE Release)
