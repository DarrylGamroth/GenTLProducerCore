include_guard(GLOBAL)

set(GenTLHeaders_ROOT "" CACHE PATH
    "Optional root containing GenTL/GenTL.h or GenTL.h")
option(GenTLProducerCore_FETCH_GENTL_HEADERS
       "Download the official EMVA GenTL 1.6 header when it is not supplied"
       ON)

function(gentl_producer_core_provide_gentl_headers)
  if(TARGET GenTLHeaders::GenTLHeaders)
    return()
  endif()

  # find_path caches its result. Clear both forms so changing
  # GenTLHeaders_ROOT cannot silently retain a header from an older configure.
  unset(_gentl_include_dir)
  unset(_gentl_include_dir CACHE)
  if(GenTLHeaders_ROOT)
    find_path(_gentl_include_dir
      NAMES GenTL.h
      HINTS "${GenTLHeaders_ROOT}"
      PATH_SUFFIXES GenTL
      NO_DEFAULT_PATH)
  endif()

  if(NOT _gentl_include_dir AND GenTLProducerCore_FETCH_GENTL_HEADERS)
    set(_gentl_download_dir "${CMAKE_BINARY_DIR}/_deps/gentl-1.6")
    set(_gentl_archive "${_gentl_download_dir}/GenTL_v1_6.zip")
    set(_gentl_header "${_gentl_download_dir}/GenTL.h")
    file(MAKE_DIRECTORY "${_gentl_download_dir}")

    if(NOT EXISTS "${_gentl_header}")
      message(STATUS "Downloading the official EMVA GenTL 1.6 header")
      file(DOWNLOAD
        "https://www.emva.org/wp-content/uploads/GenTL_v1_6.zip"
        "${_gentl_archive}"
        EXPECTED_HASH
          "SHA256=48c0867219c4d04a968a27e740d81068fae08b6ab94c9834b221530d941bb4d1"
        TLS_VERIFY ON
        STATUS _gentl_download_status)
      list(GET _gentl_download_status 0 _gentl_download_code)
      list(GET _gentl_download_status 1 _gentl_download_message)
      if(NOT _gentl_download_code EQUAL 0)
        message(FATAL_ERROR
          "Failed to download GenTL_v1_6.zip: ${_gentl_download_message}")
      endif()
      file(ARCHIVE_EXTRACT INPUT "${_gentl_archive}"
           DESTINATION "${_gentl_download_dir}")
    endif()
    file(SHA256 "${_gentl_header}" _gentl_header_hash)
    if(NOT _gentl_header_hash STREQUAL
       "5b656a413f4cdc347e80494b87f94b09ac925e391a9022edb7f028d996e35e4e")
      message(FATAL_ERROR
        "The cached GenTL.h does not match the pinned GenTL 1.6 header")
    endif()
    set(_gentl_include_dir "${_gentl_download_dir}")
  endif()

  if(NOT _gentl_include_dir OR
     NOT EXISTS "${_gentl_include_dir}/GenTL.h")
    message(FATAL_ERROR
      "GenTL.h was not found. Set GenTLHeaders_ROOT or enable "
      "GenTLProducerCore_FETCH_GENTL_HEADERS")
  endif()

  add_library(GenTLHeaders::GenTLHeaders INTERFACE IMPORTED GLOBAL)
  set_target_properties(GenTLHeaders::GenTLHeaders PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_gentl_include_dir}"
    INTERFACE_INCLUDE_DIRECTORIES "${_gentl_include_dir}")
endfunction()
