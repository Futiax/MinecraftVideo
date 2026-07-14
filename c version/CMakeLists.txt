cmake_minimum_required(VERSION 3.16)
project(MinecraftVideo C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Optimisation
if(MSVC)
    set(CMAKE_C_FLAGS_RELEASE "/O2 /DNDEBUG")
else()
    set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# OpenMP
find_package(OpenMP REQUIRED)

# zlib
find_package(ZLIB REQUIRED)

add_executable(mcmm mcmm.c)

target_link_libraries(mcmm PRIVATE
    ZLIB::ZLIB
    OpenMP::OpenMP_C
)

if(NOT WIN32)
    target_link_libraries(mcmm PRIVATE m)
endif()
