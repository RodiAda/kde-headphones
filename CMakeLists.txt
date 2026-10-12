cmake_minimum_required(VERSION 3.31)
project(sony-headphones-kde C CXX)

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

add_executable(sony-headphonesd
    daemon/main.cpp
    daemon/HeadphonesService.cpp
)
target_link_libraries(sony-headphonesd PRIVATE mdr mdr::bt Qt6::Core Qt6::DBus)

include(GNUInstallDirs)
install(TARGETS sony-headphonesd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
