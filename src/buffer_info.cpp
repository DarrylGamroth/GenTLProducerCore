#include "gentl_producer_core/buffer_info.hpp"

#include "gentl_producer_core/info.hpp"

namespace gentl_producer_core {

GenTL::GC_ERROR info_buffer(const BufferInfo &info,
                            GenTL::BUFFER_INFO_CMD command,
                            GenTL::INFO_DATATYPE *type, void *buffer,
                            std::size_t *size) {
  using namespace GenTL;
  switch (command) {
  case BUFFER_INFO_BASE:
    return info_scalar<void *>(info.base, INFO_DATATYPE_PTR, type, buffer,
                               size);
  case BUFFER_INFO_SIZE:
    return info_scalar<size_t>(info.size, INFO_DATATYPE_SIZET, type, buffer,
                               size);
  case BUFFER_INFO_USER_PTR:
    return info_scalar<void *>(info.user_ptr, INFO_DATATYPE_PTR, type, buffer,
                               size);
  case BUFFER_INFO_TIMESTAMP:
  case BUFFER_INFO_TIMESTAMP_NS:
    return info_scalar<uint64_t>(info.timestamp, INFO_DATATYPE_UINT64, type,
                                 buffer, size);
  case BUFFER_INFO_NEW_DATA:
    return info_scalar<bool8_t>(info.new_data ? 1 : 0, INFO_DATATYPE_BOOL8,
                                type, buffer, size);
  case BUFFER_INFO_IS_QUEUED:
    return info_scalar<bool8_t>(info.is_queued ? 1 : 0, INFO_DATATYPE_BOOL8,
                                type, buffer, size);
  case BUFFER_INFO_IS_ACQUIRING:
    return info_scalar<bool8_t>(info.is_acquiring ? 1 : 0, INFO_DATATYPE_BOOL8,
                                type, buffer, size);
  case BUFFER_INFO_IS_INCOMPLETE:
    return info_scalar<bool8_t>(info.is_incomplete ? 1 : 0, INFO_DATATYPE_BOOL8,
                                type, buffer, size);
  case BUFFER_INFO_IMAGEPRESENT:
    return info_scalar<bool8_t>(info.image_present ? 1 : 0, INFO_DATATYPE_BOOL8,
                                type, buffer, size);
  case BUFFER_INFO_CONTAINS_CHUNKDATA:
    // These producers supply contiguous images without appended chunk data.
    return info_scalar<bool8_t>(0, INFO_DATATYPE_BOOL8, type, buffer, size);
  case BUFFER_INFO_IMAGEOFFSET:
  case BUFFER_INFO_DELIVERED_CHUNKPAYLOADSIZE:
    return info_scalar<size_t>(0, INFO_DATATYPE_SIZET, type, buffer, size);
  case BUFFER_INFO_TLTYPE:
    return info_string(info.tl_type, type, buffer, size);
  case BUFFER_INFO_SIZE_FILLED:
    return info_scalar<size_t>(info.size_filled, INFO_DATATYPE_SIZET, type,
                               buffer, size);
  case BUFFER_INFO_WIDTH:
    return info_scalar<size_t>(info.width, INFO_DATATYPE_SIZET, type, buffer,
                               size);
  case BUFFER_INFO_HEIGHT:
    return info_scalar<size_t>(info.height, INFO_DATATYPE_SIZET, type, buffer,
                               size);
  case BUFFER_INFO_DELIVERED_IMAGEHEIGHT:
    return info_scalar<size_t>(info.delivered_image_height, INFO_DATATYPE_SIZET,
                               type, buffer, size);
  case BUFFER_INFO_XOFFSET:
    return info_scalar<size_t>(info.x_offset, INFO_DATATYPE_SIZET, type, buffer,
                               size);
  case BUFFER_INFO_YOFFSET:
    return info_scalar<size_t>(info.y_offset, INFO_DATATYPE_SIZET, type, buffer,
                               size);
  case BUFFER_INFO_XPADDING:
    return info_scalar<size_t>(info.x_padding, INFO_DATATYPE_SIZET, type,
                               buffer, size);
  case BUFFER_INFO_YPADDING:
    return info_scalar<size_t>(info.y_padding, INFO_DATATYPE_SIZET, type,
                               buffer, size);
  case BUFFER_INFO_FRAMEID:
    return info_scalar<uint64_t>(info.frame_id, INFO_DATATYPE_UINT64, type,
                                 buffer, size);
  case BUFFER_INFO_PIXELFORMAT:
    return info_scalar<uint64_t>(info.pixel_format, INFO_DATATYPE_UINT64, type,
                                 buffer, size);
  case BUFFER_INFO_PIXELFORMAT_NAMESPACE:
    return info_scalar<uint64_t>(PIXELFORMAT_NAMESPACE_PFNC_32BIT,
                                 INFO_DATATYPE_UINT64, type, buffer, size);
  case BUFFER_INFO_PAYLOADTYPE:
    return info_scalar<size_t>(PAYLOAD_TYPE_IMAGE, INFO_DATATYPE_SIZET, type,
                               buffer, size);
  case BUFFER_INFO_PIXEL_ENDIANNESS:
    return info_scalar<int32_t>(info.pixel_endianness, INFO_DATATYPE_INT32,
                                type, buffer, size);
  case BUFFER_INFO_DATA_SIZE:
    return info_scalar<size_t>(info.data_size, INFO_DATATYPE_SIZET, type,
                               buffer, size);
  default:
    return GC_ERR_NOT_IMPLEMENTED;
  }
}

} // namespace gentl_producer_core
