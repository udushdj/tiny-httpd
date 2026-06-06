# ARM 交叉编译工具链
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER "/home/wz/usr/local/arm/5.4.0/usr/bin/arm-linux-gcc")
set(CMAKE_CXX_COMPILER "/home/wz/usr/local/arm/5.4.0/usr/bin/arm-linux-g++")

set(CMAKE_FIND_ROOT_PATH "/home/wz/usr/local/arm/5.4.0")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
