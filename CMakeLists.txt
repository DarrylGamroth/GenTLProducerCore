cmake_minimum_required(VERSION 3.20)

project(GenTLProducerCore VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(GenTLHeaders_ROOT "" CACHE PATH
    "Root of the unpacked GenICam package containing GenTL/GenTL.h")

if(NOT GenTLHeaders_ROOT)
  message(FATAL_ERROR
    "GenTLHeaders_ROOT is required; set it to the unpacked GenICam package root")
endif()

find_path(GenTLHeaders_INCLUDE_DIR
  NAMES GenTL.h
  HINTS "${GenTLHeaders_ROOT}"
  PATH_SUFFIXES GenTL
  NO_DEFAULT_PATH)

if(NOT GenTLHeaders_INCLUDE_DIR)
  message(FATAL_ERROR
    "GenTL.h was not found below GenTLHeaders_ROOT='${GenTLHeaders_ROOT}'")
endif()

add_library(GenTLHeaders::GenTLHeaders INTERFACE IMPORTED GLOBAL)
set_target_properties(GenTLHeaders::GenTLHeaders PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${GenTLHeaders_INCLUDE_DIR}"
  INTERFACE_INCLUDE_DIRECTORIES "${GenTLHeaders_INCLUDE_DIR}")

add_library(gentl_producer_core STATIC
  src/info.cpp
  src/port_model.cpp)
add_library(GenTLProducerCore::gentl_producer_core ALIAS gentl_producer_core)

set_target_properties(gentl_producer_core PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  POSITION_INDEPENDENT_CODE YES)

target_compile_features(gentl_producer_core PUBLIC cxx_std_20)
target_include_directories(gentl_producer_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(gentl_producer_core PUBLIC GenTLHeaders::GenTLHeaders PRIVATE dl)
target_compile_options(gentl_producer_core PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

install(TARGETS gentl_producer_core
  EXPORT GenTLProducerCoreTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

set(GENTL_PRODUCER_CORE_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/GenTLProducerCore")
configure_package_config_file(
  cmake/GenTLProducerCoreConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/GenTLProducerCoreConfig.cmake"
  INSTALL_DESTINATION "${GENTL_PRODUCER_CORE_CMAKE_DIR}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/GenTLProducerCoreConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMinorVersion)
install(EXPORT GenTLProducerCoreTargets
  FILE GenTLProducerCoreTargets.cmake
  NAMESPACE GenTLProducerCore::
  DESTINATION "${GENTL_PRODUCER_CORE_CMAKE_DIR}")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/GenTLProducerCoreConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/GenTLProducerCoreConfigVersion.cmake"
  DESTINATION "${GENTL_PRODUCER_CORE_CMAKE_DIR}")

include(CTest)
if(BUILD_TESTING)
  add_executable(gentl_producer_core_tests tests/core_tests.cpp)
  target_link_libraries(gentl_producer_core_tests PRIVATE gentl_producer_core)
  target_compile_options(gentl_producer_core_tests PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -UNDEBUG)
  add_test(NAME gentl_producer_core_tests COMMAND gentl_producer_core_tests)
endif()
