cmake_minimum_required(VERSION 3.20)

project(GenTLProducerCore VERSION 0.1.2 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)
include(cmake/GenTLHeaders.cmake)
gentl_producer_core_provide_gentl_headers()

add_library(gentl_producer_core STATIC
  src/buffer_info.cpp
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
target_link_libraries(gentl_producer_core PUBLIC GenTLHeaders::GenTLHeaders PRIVATE dl Threads::Threads)
target_compile_options(gentl_producer_core PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  install(TARGETS gentl_producer_core
    EXPORT GenTLProducerCoreTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

  set(GENTL_PRODUCER_CORE_CMAKE_DIR
      "${CMAKE_INSTALL_LIBDIR}/cmake/GenTLProducerCore")
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
    cmake/GenTLHeaders.cmake
    DESTINATION "${GENTL_PRODUCER_CORE_CMAKE_DIR}")
  install(FILES README.md docs/EMVA_DEPENDENCIES.md
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libgentl-producer-core-dev")
endif()

include(CTest)
if(BUILD_TESTING)
  add_library(gentl_runtime_path_test_module MODULE tests/runtime_path_module.cpp)
  target_link_libraries(gentl_runtime_path_test_module PRIVATE gentl_producer_core)
  add_executable(gentl_producer_core_tests tests/core_tests.cpp)
  target_compile_definitions(gentl_producer_core_tests PRIVATE
    GENTL_RUNTIME_PATH_TEST_MODULE="$<TARGET_FILE:gentl_runtime_path_test_module>")
  add_dependencies(gentl_producer_core_tests gentl_runtime_path_test_module)
  target_link_libraries(gentl_producer_core_tests PRIVATE gentl_producer_core Threads::Threads)
  target_compile_options(gentl_producer_core_tests PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -UNDEBUG)
  add_test(NAME gentl_producer_core_tests COMMAND gentl_producer_core_tests)
  add_executable(gentl_buffer_info_tests tests/buffer_info_tests.cpp)
  target_link_libraries(gentl_buffer_info_tests PRIVATE gentl_producer_core)
  target_compile_options(gentl_buffer_info_tests PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -UNDEBUG)
  add_test(NAME gentl_buffer_info_tests COMMAND gentl_buffer_info_tests)
endif()

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  include(packaging/debian/CPackConfig.cmake)
endif()
