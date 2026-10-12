cmake_minimum_required(VERSION 3.31)
project(kde-headphones C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(MDR_BUILD_CLIENT OFF CACHE BOOL "" FORCE)
set(MDR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MDR_ENABLE_CODEGEN OFF CACHE BOOL "" FORCE)
set(MDR_BLE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SonyHeadphonesClient EXCLUDE_FROM_ALL)

set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core DBus)

add_executable(headphonesd
    daemon/main.cpp
    daemon/HeadphonesService.cpp
    daemon/SonyBackend.cpp
    daemon/GalaxyBackend.cpp
    daemon/Log.cpp
)
target_link_libraries(headphonesd PRIVATE mdr mdr::bt Qt6::Core Qt6::DBus)

include(GNUInstallDirs)
install(TARGETS headphonesd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

option(HEADPHONES_TESTS "Build tests" OFF)
if (HEADPHONES_TESTS)
    add_executable(galaxy-test tests/galaxy_test.cpp daemon/GalaxyBackend.cpp daemon/Log.cpp)
    target_include_directories(galaxy-test PRIVATE daemon)
    target_link_libraries(galaxy-test PRIVATE mdr Qt6::Core Qt6::DBus)
endif()
