cmake_minimum_required(VERSION 3.5)

# Must come before project() — enables CMAKE_MSVC_RUNTIME_LIBRARY property
# which writes <RuntimeLibrary> directly into .vcxproj, overriding any /MT or /MD
# flags that subprojects (yaml-cpp) inject into CMAKE_CXX_FLAGS_*.
cmake_policy(SET CMP0091 NEW)

project(Bourgeon
  LANGUAGES CXX
  VERSION 0.1.0
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prevent 64-bit build
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR
    "This project cannot be built for the x64 platform, Ragnarok "
    "only provides a 32-bit client."
  )
endif()

set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo CACHE STRING INTERNAL FORCE)

# Dynamic CRT (/MD) for all configs and all subprojects.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)

add_compile_definitions(_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)

add_subdirectory("src")
add_subdirectory("thirdparty")
