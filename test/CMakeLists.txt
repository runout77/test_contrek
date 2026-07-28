cmake_minimum_required(VERSION 3.10)
project(BenchmarkContour C CXX)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread -march=native -DNDEBUG -Ofast -flto -O3")

include_directories(/usr/local/include)
link_directories(/usr/local/lib)

add_subdirectory(contrek_core ContrekCore_build)

find_package(OpenCV REQUIRED)
if(OpenCV_FOUND)
    message(STATUS "OpenCV found: ${OpenCV_VERSION}")
else()
    message(FATAL_ERROR "OpenCV not found")
endif()

# common dependencies
set(COMMON_LIBS ContrekLib ${OpenCV_LIBS} pthread)

find_library(TCMALLOC_LIB tcmalloc)
if(TCMALLOC_LIB)
    message(STATUS "tcmalloc found")
    list(APPEND COMMON_LIBS ${TCMALLOC_LIB})
endif()

find_library(ZLIB_NG_LIB z-ng)
if(ZLIB_NG_LIB)
    message(STATUS "zlib-ng found: ${ZLIB_NG_LIB}")
    list(APPEND COMMON_LIBS ${ZLIB_NG_LIB})
else()
    find_package(ZLIB REQUIRED)
    list(APPEND COMMON_LIBS ZLIB::ZLIB)
endif()

# =================================================================
# TARGETS
# =================================================================

# 1. Target Original
add_executable(contrek_opencv_benchmark test.cpp)
target_include_directories(contrek_opencv_benchmark PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(contrek_opencv_benchmark PRIVATE ${COMMON_LIBS})

# 2. Target Streaming
add_executable(streaming_benchmark streaming.cpp)
target_include_directories(streaming_benchmark PRIVATE 
    ${OpenCV_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/contrek_core
)
target_link_libraries(streaming_benchmark PRIVATE ${COMMON_LIBS})

# 3. Target Progressive Streaming
add_executable(progressive_streaming_benchmark progressive_streaming.cpp)
target_include_directories(progressive_streaming_benchmark PRIVATE 
    ${OpenCV_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/contrek_core
)
target_link_libraries(progressive_streaming_benchmark PRIVATE ${COMMON_LIBS})

# =================================================================
# 4. GDAL Test
# =================================================================
find_package(GDAL REQUIRED)
if(GDAL_FOUND)
    message(STATUS "GDAL found: ${GDAL_VERSION}")
else()
    message(FATAL_ERROR "GDAL not found")
endif()
add_executable(gdal_test gdal_test.cpp)
target_include_directories(gdal_test PRIVATE 
    ${OpenCV_INCLUDE_DIRS}
    ${GDAL_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/contrek_core
)
target_link_libraries(gdal_test PRIVATE 
    ${COMMON_LIBS}
    ${GDAL_LIBRARIES}
)
find_package(nlohmann_json REQUIRED)
target_link_libraries(gdal_test PRIVATE nlohmann_json::nlohmann_json)
