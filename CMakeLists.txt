cmake_minimum_required(VERSION 3.10)
project(parallel_copy C)

# 设置 C 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 设置源文件编码为 UTF-8
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 添加可执行文件
if(WIN32)
    add_executable(parallel_copy parallel_copy_windows.c)
else()
    add_executable(parallel_copy parallel_copy_linux.c)
endif()

# Windows 特定设置
if(WIN32)
    target_compile_definitions(parallel_copy PRIVATE _WIN32)
    # 添加静态链接设置
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Release>:>")
    target_compile_options(parallel_copy PRIVATE /MT)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(parallel_copy PRIVATE Threads::Threads)
endif()

# 优化选项
if(MSVC)
    target_compile_options(parallel_copy PRIVATE /O2)
else()
    target_compile_options(parallel_copy PRIVATE -O3)
endif() 
