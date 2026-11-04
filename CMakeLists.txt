cmake_minimum_required(VERSION 3.16)
project(libdatax VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(DATAX_LIBIIO_SOURCE "" CACHE PATH "libiio v1 source tree (headers in <src>/include)")
set(DATAX_LIBIIO_BUILD "" CACHE PATH "libiio v1 build tree (libiio.so, iio-config.h)")

set(DATAX_WARNINGS -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual
    -Wstrict-prototypes -Wmissing-prototypes -Werror)

add_library(iio_dep INTERFACE)
if(DATAX_LIBIIO_SOURCE AND DATAX_LIBIIO_BUILD)
  target_include_directories(iio_dep SYSTEM INTERFACE
    ${DATAX_LIBIIO_SOURCE}/include ${DATAX_LIBIIO_BUILD})
  target_link_libraries(iio_dep INTERFACE ${DATAX_LIBIIO_BUILD}/libiio.so)
  set(DATAX_IIO_RPATH ${DATAX_LIBIIO_BUILD})
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(IIO REQUIRED IMPORTED_TARGET libiio>=1.0)
  target_link_libraries(iio_dep INTERFACE PkgConfig::IIO)
  set(DATAX_IIO_RPATH "")
endif()

find_package(Threads REQUIRED)

add_library(datax SHARED
  src/error.c
  src/version.c
  src/stubs.c
  src/caps_access.c
  src/context.c
  src/platform/table.c)
target_include_directories(datax
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE src)
target_compile_options(datax PRIVATE ${DATAX_WARNINGS})
target_compile_definitions(datax PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(datax PRIVATE iio_dep Threads::Threads)
target_link_options(datax PRIVATE
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/libdatax.map)
set_target_properties(datax PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/libdatax.map
  BUILD_RPATH "${DATAX_IIO_RPATH}")

include(GNUInstallDirs)
configure_file(cmake/libdatax.pc.in libdatax.pc @ONLY)
install(TARGETS datax LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/datax DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libdatax.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests/c)
endif()
