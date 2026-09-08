#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>
#include <type_traits>

#include <GenTL.h>

namespace gentl_producer_core {

GenTL::GC_ERROR copy_string(std::string_view value, GenTL::INFO_DATATYPE type,
                            GenTL::INFO_DATATYPE expected_type, void *buffer,
                            std::size_t *size);

template <typename T>
GenTL::GC_ERROR copy_scalar(const T &value, GenTL::INFO_DATATYPE type,
                            GenTL::INFO_DATATYPE expected_type, void *buffer,
                            std::size_t *size) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (type != expected_type || size == nullptr) {
    return GenTL::GC_ERR_INVALID_PARAMETER;
  }

  constexpr std::size_t required_size = sizeof(T);
  if (buffer == nullptr) {
    *size = required_size;
    return GenTL::GC_ERR_SUCCESS;
  }
  if (*size < required_size) {
    *size = required_size;
    return GenTL::GC_ERR_BUFFER_TOO_SMALL;
  }

  std::memcpy(buffer, &value, required_size);
  *size = required_size;
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR info_string(std::string_view value, GenTL::INFO_DATATYPE *type,
                            void *buffer, std::size_t *size);

template <typename T>
GenTL::GC_ERROR info_scalar(const T &value, GenTL::INFO_DATATYPE expected_type,
                            GenTL::INFO_DATATYPE *type, void *buffer,
                            std::size_t *size) {
  if (type == nullptr) {
    return GenTL::GC_ERR_INVALID_PARAMETER;
  }
  *type = expected_type;
  return copy_scalar(value, *type, expected_type, buffer, size);
}

} // namespace gentl_producer_core
