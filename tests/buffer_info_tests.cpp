#include "gentl_producer_core/buffer_info.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace {

using namespace GenTL;
using gentl_producer_core::BufferInfo;
using gentl_producer_core::info_buffer;

template <typename T>
void check(const BufferInfo &info, BUFFER_INFO_CMD command,
           INFO_DATATYPE expected_type, T expected) {
  INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
  std::size_t size = 0;
  assert(info_buffer(info, command, &type, nullptr, &size) == GC_ERR_SUCCESS);
  assert(type == expected_type && size == sizeof(T));

  T value{};
  std::memset(&value, 0xa5, sizeof(value));
  std::array<std::byte, sizeof(T)> before{};
  std::memcpy(before.data(), &value, sizeof(value));
  size = sizeof(T) - 1;
  assert(info_buffer(info, command, &type, &value, &size) ==
         GC_ERR_BUFFER_TOO_SMALL);
  assert(size == sizeof(T));
  assert(std::memcmp(before.data(), &value, sizeof(value)) == 0);
  assert(info_buffer(info, command, nullptr, &value, &size) ==
         GC_ERR_INVALID_PARAMETER);
  assert(info_buffer(info, command, &type, &value, nullptr) ==
         GC_ERR_INVALID_PARAMETER);
  assert(info_buffer(info, command, &type, &value, &size) == GC_ERR_SUCCESS);
  assert(type == expected_type && size == sizeof(T) && value == expected);
}

} // namespace

int main() {
  std::array<std::byte, 64> memory{};
  int user = 42;
  BufferInfo info{.base = memory.data(),
                  .size = memory.size(),
                  .user_ptr = &user,
                  .timestamp = 0xfedcba9876543210ULL,
                  .new_data = true,
                  .is_queued = false,
                  .is_acquiring = true,
                  .is_incomplete = false,
                  .tl_type = "Custom",
                  .size_filled = 48,
                  .width = 8,
                  .height = 3,
                  .x_offset = 2,
                  .y_offset = 5,
                  .x_padding = 4,
                  .y_padding = 6,
                  .frame_id = 0x1122334455667788ULL,
                  .image_present = false,
                  .pixel_format = 0x01100007,
                  .data_size = 56};

  check<void *>(info, BUFFER_INFO_BASE, INFO_DATATYPE_PTR, memory.data());
  check<void *>(info, BUFFER_INFO_USER_PTR, INFO_DATATYPE_PTR, &user);
  const std::pair<BUFFER_INFO_CMD, std::size_t> sizes[]{
      {BUFFER_INFO_SIZE, 64},
      {BUFFER_INFO_SIZE_FILLED, 48},
      {BUFFER_INFO_WIDTH, 8},
      {BUFFER_INFO_HEIGHT, 3},
      {BUFFER_INFO_XOFFSET, 2},
      {BUFFER_INFO_YOFFSET, 5},
      {BUFFER_INFO_XPADDING, 4},
      {BUFFER_INFO_YPADDING, 6},
      {BUFFER_INFO_IMAGEOFFSET, 0},
      {BUFFER_INFO_DELIVERED_CHUNKPAYLOADSIZE, 0},
      {BUFFER_INFO_PAYLOADTYPE, PAYLOAD_TYPE_IMAGE},
      {BUFFER_INFO_DATA_SIZE, 56}};
  for (const auto &[command, value] : sizes)
    check<std::size_t>(info, command, INFO_DATATYPE_SIZET, value);

  const std::pair<BUFFER_INFO_CMD, std::uint64_t> integers[]{
      {BUFFER_INFO_TIMESTAMP, 0xfedcba9876543210ULL},
      {BUFFER_INFO_TIMESTAMP_NS, 0xfedcba9876543210ULL},
      {BUFFER_INFO_FRAMEID, 0x1122334455667788ULL},
      {BUFFER_INFO_PIXELFORMAT, 0x01100007},
      {BUFFER_INFO_PIXELFORMAT_NAMESPACE, PIXELFORMAT_NAMESPACE_PFNC_32BIT}};
  for (const auto &[command, value] : integers)
    check<std::uint64_t>(info, command, INFO_DATATYPE_UINT64, value);
  check<std::int32_t>(info, BUFFER_INFO_PIXEL_ENDIANNESS, INFO_DATATYPE_INT32,
                      PIXELENDIANNESS_LITTLE);

  // The producer supplies these decisions independently. No lifecycle is
  // inferred by the formatter from size_filled or any other flag.
  check<bool8_t>(info, BUFFER_INFO_NEW_DATA, INFO_DATATYPE_BOOL8, 1);
  check<bool8_t>(info, BUFFER_INFO_IS_QUEUED, INFO_DATATYPE_BOOL8, 0);
  check<bool8_t>(info, BUFFER_INFO_IS_ACQUIRING, INFO_DATATYPE_BOOL8, 1);
  check<bool8_t>(info, BUFFER_INFO_IS_INCOMPLETE, INFO_DATATYPE_BOOL8, 0);
  check<bool8_t>(info, BUFFER_INFO_IMAGEPRESENT, INFO_DATATYPE_BOOL8, 0);
  check<bool8_t>(info, BUFFER_INFO_CONTAINS_CHUNKDATA, INFO_DATATYPE_BOOL8, 0);
  info.new_data = false;
  info.is_queued = true;
  info.is_acquiring = false;
  info.is_incomplete = true;
  info.image_present = true;
  check<bool8_t>(info, BUFFER_INFO_NEW_DATA, INFO_DATATYPE_BOOL8, 0);
  check<bool8_t>(info, BUFFER_INFO_IS_QUEUED, INFO_DATATYPE_BOOL8, 1);
  check<bool8_t>(info, BUFFER_INFO_IS_ACQUIRING, INFO_DATATYPE_BOOL8, 0);
  check<bool8_t>(info, BUFFER_INFO_IS_INCOMPLETE, INFO_DATATYPE_BOOL8, 1);
  check<bool8_t>(info, BUFFER_INFO_IMAGEPRESENT, INFO_DATATYPE_BOOL8, 1);

  INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
  std::size_t size = 0;
  assert(info_buffer(info, BUFFER_INFO_TLTYPE, &type, nullptr, &size) ==
         GC_ERR_SUCCESS);
  assert(type == INFO_DATATYPE_STRING && size == 7);
  std::array<char, 16> text{};
  text.fill('!');
  size = 6;
  assert(info_buffer(info, BUFFER_INFO_TLTYPE, &type, text.data(), &size) ==
         GC_ERR_BUFFER_TOO_SMALL);
  assert(size == 7 && text.front() == '!');
  assert(info_buffer(info, BUFFER_INFO_TLTYPE, nullptr, text.data(), &size) ==
         GC_ERR_INVALID_PARAMETER);
  assert(info_buffer(info, BUFFER_INFO_TLTYPE, &type, text.data(), nullptr) ==
         GC_ERR_INVALID_PARAMETER);
  size = text.size();
  assert(info_buffer(info, BUFFER_INFO_TLTYPE, &type, text.data(), &size) ==
         GC_ERR_SUCCESS);
  assert(size == 7 && std::string(text.data()) == "Custom" && text[7] == '!');

  // Queries that only SDK3 implements must remain outside the common set.
  for (const auto command :
       {BUFFER_INFO_DELIVERED_IMAGEHEIGHT, BUFFER_INFO_CHUNKLAYOUTID,
        BUFFER_INFO_FILENAME, BUFFER_INFO_DATA_LARGER_THAN_BUFFER,
        BUFFER_INFO_IS_COMPOSITE, BUFFER_INFO_CUSTOM_ID}) {
    type = INFO_DATATYPE_UNKNOWN;
    size = text.size();
    assert(info_buffer(info, command, &type, text.data(), &size) ==
           GC_ERR_NOT_IMPLEMENTED);
    assert(type == INFO_DATATYPE_UNKNOWN && size == text.size());
  }
  return 0;
}
