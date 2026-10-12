cmake_minimum_required(VERSION 3.31)
project(kde-headphones VERSION 0.2.0 LANGUAGES C CXX)

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
include(GNUInstallDirs)

add_executable(headphonesd
    daemon/main.cpp
    daemon/HeadphonesService.cpp
    daemon/SonyBackend.cpp
    daemon/GalaxyBackend.cpp
    daemon/Log.cpp
)
target_link_libraries(headphonesd PRIVATE mdr mdr::bt Qt6::Core Qt6::DBus)
install(TARGETS headphonesd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if (CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    set(_unit_dir "lib/systemd/user")
else()
    set(_unit_dir "${CMAKE_INSTALL_DATADIR}/systemd/user")
endif()
set(HEADPHONES_SYSTEMD_USER_UNIT_DIR "${_unit_dir}" CACHE PATH "Where to install the systemd user unit")

configure_file(systemd/headphonesd.service.in headphonesd.service @ONLY)
configure_file(systemd/io.github.rodiada.Headphones.service.in io.github.rodiada.Headphones.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/headphonesd.service DESTINATION ${HEADPHONES_SYSTEMD_USER_UNIT_DIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.rodiada.Headphones.service DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)

set(PLASMOID_ID io.github.rodiada.headphones)
set(PLASMOID_DIR ${CMAKE_INSTALL_DATADIR}/plasma/plasmoids/${PLASMOID_ID})
install(FILES plasmoid/${PLASMOID_ID}/metadata.json DESTINATION ${PLASMOID_DIR})
install(DIRECTORY plasmoid/${PLASMOID_ID}/contents/ui DESTINATION ${PLASMOID_DIR}/contents)

find_program(MSGFMT_EXECUTABLE msgfmt REQUIRED)
file(GLOB PO_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/po/*.po)
set(MO_FILES)
foreach(po ${PO_FILES})
    get_filename_component(lang ${po} NAME_WE)
    set(mo ${CMAKE_CURRENT_BINARY_DIR}/locale/${lang}/plasma_applet_${PLASMOID_ID}.mo)
    add_custom_command(
        OUTPUT ${mo}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/locale/${lang}
        COMMAND ${MSGFMT_EXECUTABLE} -o ${mo} ${po}
        DEPENDS ${po}
    )
    install(FILES ${mo} DESTINATION ${PLASMOID_DIR}/contents/locale/${lang}/LC_MESSAGES)
    list(APPEND MO_FILES ${mo})
endforeach()
add_custom_target(translations ALL DEPENDS ${MO_FILES})

option(HEADPHONES_TESTS "Build tests" OFF)
if (HEADPHONES_TESTS)
    add_executable(galaxy-test tests/galaxy_test.cpp daemon/GalaxyBackend.cpp daemon/Log.cpp)
    target_include_directories(galaxy-test PRIVATE daemon)
    target_link_libraries(galaxy-test PRIVATE mdr Qt6::Core Qt6::DBus)
endif()

set(CPACK_PACKAGE_NAME kde-headphones)
set(CPACK_PACKAGE_VENDOR "Rodion Adamenok")
set(CPACK_PACKAGE_CONTACT "https://github.com/RodiAda/kde-headphones")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "KDE Plasma tray widget for Sony and Samsung Galaxy headphones")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/RodiAda/kde-headphones")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "plasma-workspace (>= 4:6.0), bluez")
set(CPACK_DEBIAN_PACKAGE_SECTION kde)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE MIT)
set(CPACK_RPM_PACKAGE_REQUIRES "plasma-workspace >= 6.0, bluez")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/lib/systemd /usr/lib/systemd/user /usr/share/dbus-1 /usr/share/dbus-1/services /usr/share/plasma /usr/share/plasma/plasmoids)
include(CPack)
