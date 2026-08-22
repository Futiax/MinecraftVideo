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

# Static build (for a portable binary bundled inside the server plugin):
#   cmake -S . -B build-static -DMCMM_STATIC=ON
# Links libgomp/zlib/libc statically so the binary runs without those installed.
option(MCMM_STATIC "Link statically for a self-contained binary" OFF)

# OpenMP
find_package(OpenMP REQUIRED)

# zlib — in static mode, resolve libz.a instead of libz.so
if(MCMM_STATIC AND NOT MSVC)
    set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)

add_executable(mcmm mcmm.c)

target_link_libraries(mcmm PRIVATE ZLIB::ZLIB)

if(MCMM_STATIC AND NOT MSVC)
    # The OpenMP::OpenMP_C imported target links libgomp.so by absolute path,
    # which clashes with -static. Pass the flags raw instead (mirrors the
    # known-good: gcc -O2 -fopenmp -static ... -lz -lm) so -static picks the
    # static libgomp/libz/libc.
    target_compile_options(mcmm PRIVATE -fopenmp)
    target_link_options(mcmm PRIVATE -static -fopenmp)
else()
    target_link_libraries(mcmm PRIVATE OpenMP::OpenMP_C)
endif()

if(NOT WIN32)
    target_link_libraries(mcmm PRIVATE m)
endif()
