#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <GenTL.h>

namespace gentl_producer_core {

// Caller-supplied values for contiguous PFNC image buffers
// without chunk data. This does not own memory or determine buffer lifecycle.
struct BufferInfo {
  void *base = nullptr;
  std::size_t size = 0;
  void *user_ptr = nullptr;
  std::uint64_t timestamp = 0; // Nanoseconds; both GenTL timestamp queries use this value.
  bool new_data = false;
  bool is_queued = false;
  bool is_acquiring = false;
  bool is_incomplete = false;
  std::string_view tl_type;
  std::size_t size_filled = 0;
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t delivered_image_height = 0;
  std::size_t x_offset = 0;
  std::size_t y_offset = 0;
  std::size_t x_padding = 0;
  std::size_t y_padding = 0;
  std::uint64_t frame_id = 0;
  bool image_present = false;
  std::uint64_t pixel_format = 0;
  std::int32_t pixel_endianness = GenTL::PIXELENDIANNESS_LITTLE;
  std::size_t data_size = 0;
};

// The caller validates handles and synchronizes access before constructing
// BufferInfo. tl_type must remain valid for this call. Unknown commands return
// GC_ERR_NOT_IMPLEMENTED; producer-specific queries stay in the producer.
GenTL::GC_ERROR info_buffer(const BufferInfo &info,
                            GenTL::BUFFER_INFO_CMD command,
                            GenTL::INFO_DATATYPE *type, void *buffer,
                            std::size_t *size);

} // namespace gentl_producer_core
