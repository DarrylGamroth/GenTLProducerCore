#include "gentl_producer_core/info.hpp"

#include <cstring>

namespace gentl_producer_core {

GenTL::GC_ERROR copy_string(std::string_view value, GenTL::INFO_DATATYPE type,
                            GenTL::INFO_DATATYPE expected_type, void *buffer,
                            std::size_t *size) {
  if (type != expected_type || size == nullptr) {
    return GenTL::GC_ERR_INVALID_PARAMETER;
  }

  const std::size_t required_size = value.size() + 1U;
  if (buffer == nullptr) {
    *size = required_size;
    return GenTL::GC_ERR_SUCCESS;
  }
  if (*size < required_size) {
    *size = required_size;
    return GenTL::GC_ERR_BUFFER_TOO_SMALL;
  }

  if (!value.empty()) {
    std::memcpy(buffer, value.data(), value.size());
  }
  static_cast<char *>(buffer)[value.size()] = '\0';
  *size = required_size;
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR info_string(std::string_view value, GenTL::INFO_DATATYPE *type,
                            void *buffer, std::size_t *size) {
  if (type == nullptr) {
    return GenTL::GC_ERR_INVALID_PARAMETER;
  }
  *type = GenTL::INFO_DATATYPE_STRING;
  return copy_string(value, *type, GenTL::INFO_DATATYPE_STRING, buffer, size);
}

} // namespace gentl_producer_core
