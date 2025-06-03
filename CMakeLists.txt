cmake_minimum_required(VERSION 3.22.1 FATAL_ERROR)
project(amixdemo VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wall -Wextra")
set(CMAKE_CXX_EXTENSIONS OFF)

#-flto=auto 链接时优化，提高链接速度，减小程序文件大小，提高运行性能
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto=auto")
set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
find_package(PkgConfig REQUIRED)
# pkg_check_modules(pgsql REQUIRED IMPORTED_TARGET "libpqxx>=7.9.0")
pkg_check_modules(ffmpeg REQUIRED IMPORTED_TARGET libavutil=58.29.100 libswresample=4.12.100 libavformat=60.16.100 libavdevice=60.3.100 libavfilter=9.12.100 libswscale=7.5.100 libavcodec=60.31.102)

INCLUDE_DIRECTORIES("/usr/include") 
LINK_DIRECTORIES("/usr/lib")
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/head)
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/src SOURCES)

add_executable(amixdemo ${SOURCES})
target_link_libraries(amixdemo PkgConfig::ffmpeg)