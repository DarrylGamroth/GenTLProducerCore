#include "gentl_producer_core/buffer_info.hpp"
#include "gentl_producer_core/detail/module_ports.hpp"
#include "gentl_producer_core/detail/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <thread>

namespace gentl_producer_core::runtime {
namespace {

#ifdef GENTL_RUNTIME_TEST_HOOKS
void (*buffer_publication_hook)(void *, int) = nullptr;
void *buffer_publication_context = nullptr;

void buffer_publication_test_hook(int stage) {
  if (buffer_publication_hook != nullptr)
    buffer_publication_hook(buffer_publication_context, stage);
}
#endif

GenTL::GC_ERROR invalid_handle() {
  Runtime::instance().set_last_error(GenTL::GC_ERR_INVALID_HANDLE,
                                     "invalid or stale handle");
  return GenTL::GC_ERR_INVALID_HANDLE;
}

GenTL::GC_ERROR invalid_id(const std::string &text) {
  Runtime::instance().set_last_error(GenTL::GC_ERR_INVALID_ID, text);
  return GenTL::GC_ERR_INVALID_ID;
}

GenTL::GC_ERROR not_implemented(const std::string &text) {
  Runtime::instance().set_last_error(GenTL::GC_ERR_NOT_IMPLEMENTED, text);
  return GenTL::GC_ERR_NOT_IMPLEMENTED;
}

GenTL::GC_ERROR not_initialized() {
  Runtime::instance().set_last_error(GenTL::GC_ERR_NOT_INITIALIZED,
                                     "GenTL library is not initialized");
  return GenTL::GC_ERR_NOT_INITIALIZED;
}

GenTL::GC_ERROR resource_in_use(const std::string &text) {
  Runtime::instance().set_last_error(GenTL::GC_ERR_RESOURCE_IN_USE, text);
  return GenTL::GC_ERR_RESOURCE_IN_USE;
}

GenTL::GC_ERROR busy(const std::string &text) {
  Runtime::instance().set_last_error(GenTL::GC_ERR_BUSY, text);
  return GenTL::GC_ERR_BUSY;
}

GenTL::GC_ERROR invalid_buffer(const std::string &text) {
  Runtime::instance().set_last_error(GenTL::GC_ERR_INVALID_BUFFER, text);
  return GenTL::GC_ERR_INVALID_BUFFER;
}

GenTL::GC_ERROR require_initialized() {
  return Runtime::instance().is_initialized() ? GenTL::GC_ERR_SUCCESS
                                              : not_initialized();
}

GenTL::GC_ERROR invalid_address() {
  Runtime::instance().set_last_error(GenTL::GC_ERR_INVALID_ADDRESS,
                                     "invalid port register address");
  return GenTL::GC_ERR_INVALID_ADDRESS;
}

GenTL::GC_ERROR access_denied(const std::string &text) {
  Runtime::instance().set_last_error(GenTL::GC_ERR_ACCESS_DENIED, text);
  return GenTL::GC_ERR_ACCESS_DENIED;
}

GenTL::GC_ERROR io_error(const std::string &text) {
  Runtime::instance().set_last_error(GenTL::GC_ERR_IO, text);
  return GenTL::GC_ERR_IO;
}

GenTL::DEVICE_ACCESS_STATUS
access_status_from_flags(GenTL::DEVICE_ACCESS_FLAGS flags) {
  switch (flags) {
  case GenTL::DEVICE_ACCESS_READONLY:
    return GenTL::DEVICE_ACCESS_STATUS_OPEN_READONLY;
  case GenTL::DEVICE_ACCESS_CONTROL:
  case GenTL::DEVICE_ACCESS_EXCLUSIVE:
    return GenTL::DEVICE_ACCESS_STATUS_OPEN_READWRITE;
  default:
    return GenTL::DEVICE_ACCESS_STATUS_UNKNOWN;
  }
}

GenTL::GC_ERROR device_info_response(
    const gentl_producer_core::DeviceDescriptor &device,
    GenTL::DEVICE_ACCESS_STATUS access_status, GenTL::DEVICE_INFO_CMD info_cmd,
    GenTL::INFO_DATATYPE *type, void *buffer, std::size_t *size) {
  switch (info_cmd) {
  case GenTL::DEVICE_INFO_ID:
    return info_string(device.id, type, buffer, size);
  case GenTL::DEVICE_INFO_VENDOR:
    return info_string(device.vendor, type, buffer, size);
  case GenTL::DEVICE_INFO_MODEL:
    return info_string(device.model, type, buffer, size);
  case GenTL::DEVICE_INFO_TLTYPE:
    return info_string(Runtime::instance().backend_info().tl_type, type, buffer,
                       size);
  case GenTL::DEVICE_INFO_DISPLAYNAME:
    return info_string(device.model + " (" + device.id + ")", type, buffer,
                       size);
  case GenTL::DEVICE_INFO_ACCESS_STATUS:
    return info_scalar<int32_t>(access_status, GenTL::INFO_DATATYPE_INT32, type,
                                buffer, size);
  case GenTL::DEVICE_INFO_USER_DEFINED_NAME:
    return info_string("", type, buffer, size);
  case GenTL::DEVICE_INFO_SERIAL_NUMBER:
    return info_string(device.serial_number, type, buffer, size);
  case GenTL::DEVICE_INFO_VERSION:
    return info_string(producer_definition().version, type, buffer, size);
  case GenTL::DEVICE_INFO_TIMESTAMP_FREQUENCY:
    return info_scalar<uint64_t>(device.timestamp_frequency_hz,
                                 GenTL::INFO_DATATYPE_UINT64, type, buffer,
                                 size);
  default:
    return GenTL::GC_ERR_INVALID_ID;
  }
}

GenTL::GC_ERROR require_camera(std::string_view device_id,
                               gentl_producer_core::DeviceDescriptor *device) {
  if (device == nullptr) {
    return GenTL::GC_ERR_INVALID_PARAMETER;
  }
  GenTL::GC_ERROR error_code = GenTL::GC_ERR_SUCCESS;
  const auto maybe_camera =
      Runtime::instance().find_camera_by_id(device_id, &error_code);
  if (error_code != GenTL::GC_ERR_SUCCESS) {
    return error_code;
  }
  if (!maybe_camera.has_value()) {
    return invalid_id("unknown device id");
  }
  *device = *maybe_camera;
  return GenTL::GC_ERR_SUCCESS;
}

gentl_producer_core::runtime::BufferHandleData *dequeue_input_buffer_locked(
    gentl_producer_core::runtime::DsHandleData *stream) {
  if (stream->input_queue.empty()) {
    return nullptr;
  }
  auto *buffer = stream->input_queue.front();
  stream->input_queue.pop_front();
  buffer->is_queued = false;
  buffer->is_acquiring = true;
  buffer->has_new_data = false;
  buffer->is_incomplete = false;
  buffer->size_filled = 0;
  return buffer;
}

void snapshot_buffer_description_locked(DsHandleData *stream,
                                        BufferHandleData *buffer) {
  buffer->image = {.buffer_alignment = stream->current_buffer_alignment,
                   .width = stream->current_width,
                   .height = stream->current_height,
                   .payload_size_bytes = stream->current_payload_size,
                   .pixel_format = stream->current_pixel_format,
                   .pixel_size_bits = stream->current_pixel_size_bits,
                   .offset_x = stream->current_offset_x,
                   .offset_y = stream->current_offset_y,
                   .pixel_endianness = stream->current_pixel_endianness};
}

template <typename Container>
std::vector<gentl_producer_core::runtime::EventHandleData *>
retain_events_locked(const Container &source) {
  std::vector<gentl_producer_core::runtime::EventHandleData *> events;
  events.reserve(source.size());
  for (auto *event : source) {
    if (gentl_producer_core::runtime::retain_event(event))
      events.push_back(event);
  }
  return events;
}

void release_events(
    const std::vector<gentl_producer_core::runtime::EventHandleData *>
        &events) {
  for (auto *event : events)
    gentl_producer_core::runtime::release_event(event);
}

void publish_new_buffer_event(
    gentl_producer_core::runtime::EventHandleData *event,
    gentl_producer_core::runtime::BufferHandleData *buffer) {
#ifdef GENTL_RUNTIME_TEST_HOOKS
  buffer_publication_test_hook(0);
#endif
  std::scoped_lock lock(event->mutex);
  if (!event->registered) {
    return;
  }
  event->new_buffer_queue.push_back(GenTL::EVENT_NEW_BUFFER_DATA{
      .BufferHandle = buffer,
      .pUserPointer = buffer->private_data,
  });
  ++event->fired_count;
  event->condition.notify_one();
}

void publish_feature_invalidate_event(
    gentl_producer_core::runtime::EventHandleData *event,
    std::string_view feature_name) {
  std::scoped_lock lock(event->mutex);
  if (!event->registered) {
    return;
  }
  event->feature_invalidate_queue.emplace_back(feature_name);
  ++event->fired_count;
  event->condition.notify_one();
}

void publish_feature_invalidations(
    gentl_producer_core::runtime::PortHandleData *port,
    const std::vector<std::string> &features) {
  std::vector<gentl_producer_core::runtime::EventHandleData *> events;
  {
    std::scoped_lock lock(port->mutex);
    events = retain_events_locked(port->feature_invalidate_events);
  }

  for (const auto &feature : features) {
    for (auto *event : events) {
      publish_feature_invalidate_event(event, feature);
    }
  }
  release_events(events);
}

void publish_acquisition_status_invalidation(
    gentl_producer_core::runtime::DevHandleData *device) {
  publish_feature_invalidations(
      device->remote_port.get(),
      gentl_producer_core::runtime::Runtime::instance()
          .acquisition_invalidated_features());
}

void unregister_event_from_source(
    gentl_producer_core::runtime::EventHandleData *event) {
  if (event->source == nullptr) {
    return;
  }

  switch (event->source->kind) {
  case gentl_producer_core::runtime::HandleKind::kDs: {
    auto *stream = static_cast<gentl_producer_core::runtime::DsHandleData *>(
        event->source);
    std::scoped_lock lock(stream->mutex);
    auto &events = stream->new_buffer_events;
    events.erase(std::remove(events.begin(), events.end(), event),
                 events.end());
    break;
  }
  case gentl_producer_core::runtime::HandleKind::kPort: {
    auto *port = static_cast<gentl_producer_core::runtime::PortHandleData *>(
        event->source);
    std::scoped_lock lock(port->mutex);
    auto &events = port->feature_invalidate_events;
    events.erase(std::remove(events.begin(), events.end(), event),
                 events.end());
    break;
  }
  default:
    break;
  }
}

void stream_worker_impl(gentl_producer_core::runtime::DsHandleData *stream) {
  for (;;) {
    gentl_producer_core::runtime::BufferHandleData *buffer = nullptr;
    {
      std::scoped_lock lock(stream->mutex);
      if (stream->stop_requested) {
        break;
      }
      buffer = dequeue_input_buffer_locked(stream);
    }

    if (buffer == nullptr) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    gentl_producer_core::AcquireFrameResult frame_result;
    std::string error_text;
    if (!gentl_producer_core::runtime::Runtime::instance().acquire_frame(
            stream->parent->device, 1000U, buffer->base, buffer->size,
            &frame_result, &error_text)) {
      std::scoped_lock lock(stream->mutex);
      stream->worker_error = std::move(error_text);
      buffer->is_acquiring = false;
      buffer->is_queued = true;
      stream->input_queue.push_front(buffer);
      break;
    }

    if (frame_result.status ==
        gentl_producer_core::AcquireFrameStatus::kTimeout) {
      std::scoped_lock lock(stream->mutex);
      buffer->is_acquiring = false;
      buffer->is_queued = true;
      stream->input_queue.push_front(buffer);
      continue;
    }

    if (frame_result.status ==
        gentl_producer_core::AcquireFrameStatus::kStopped) {
      bool should_stop = false;
      {
        std::scoped_lock lock(stream->mutex);
        buffer->is_acquiring = false;
        buffer->is_queued = true;
        stream->input_queue.push_front(buffer);
        should_stop = stream->stop_requested;
      }
      if (should_stop)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    std::vector<gentl_producer_core::runtime::EventHandleData *> events;
    {
      std::scoped_lock lock(stream->mutex);
      buffer->is_acquiring = false;
      ++stream->started_count;
      buffer->has_new_data = true;
      buffer->awaiting_delivery = true;
      buffer->is_incomplete = frame_result.incomplete;
      buffer->size_filled = frame_result.bytes_filled;
      snapshot_buffer_description_locked(stream, buffer);
      buffer->timestamp = frame_result.timestamp_ns;
      stream->frame_sequence += frame_result.frame_id_increment;
      buffer->frame_id = stream->frame_sequence;
      ++stream->delivered_count;
      stream->underrun_count += frame_result.skipped_frames;
      events = retain_events_locked(stream->new_buffer_events);
      // Ownership and event publication must be atomic with queue discard.
      for (auto *event : events) {
        publish_new_buffer_event(event, buffer);
      }
    }
    release_events(events);

    bool reached_limit = false;
    {
      std::scoped_lock lock(stream->mutex);
      reached_limit = stream->acquisition_limit != GENTL_INFINITE &&
                      stream->delivered_count >= stream->acquisition_limit;
    }
    if (reached_limit) {
      std::string stop_error;
      gentl_producer_core::runtime::Runtime::instance().stop_stream(
          stream->parent->device, &stop_error);
      break;
    }
  }

  {
    std::scoped_lock stream_lock(stream->mutex);
    stream->worker_running = false;
    stream->is_acquiring = false;
  }
  {
    std::scoped_lock device_lock(stream->parent->mutex);
    stream->parent->acquisition_active = false;
  }
  publish_acquisition_status_invalidation(stream->parent);
}

} // namespace

void stream_worker(DsHandleData *stream) { stream_worker_impl(stream); }
void publish_stream_buffer(EventHandleData *event, BufferHandleData *buffer) {
  publish_new_buffer_event(event, buffer);
}
void publish_port_feature_invalidations(
    PortHandleData *port, const std::vector<std::string> &features) {
  publish_feature_invalidations(port, features);
}
void detach_event_from_source(EventHandleData *event) {
  unregister_event_from_source(event);
}

} // namespace gentl_producer_core::runtime

namespace GenTL {

#ifdef GENTL_RUNTIME_TEST_HOOKS
extern "C" GC_IMPORT_EXPORT void GC_CALLTYPE
GenTLTestSetBufferPublicationHook(void (*hook)(void *, int), void *context) {
  gentl_producer_core::runtime::buffer_publication_hook = hook;
  gentl_producer_core::runtime::buffer_publication_context = context;
}
#endif

#define GENTL_REQUIRE_INITIALIZED()                                            \
  do {                                                                         \
    const auto initialization_result =                                         \
        gentl_producer_core::runtime::require_initialized();                   \
    if (initialization_result != GC_ERR_SUCCESS)                               \
      return initialization_result;                                            \
  } while (false)

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCReadPort(PORT_HANDLE, uint64_t, void *,
                                                 size_t *);

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCWritePort(PORT_HANDLE, uint64_t,
                                                  const void *, size_t *);

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCRegisterEvent(EVENTSRC_HANDLE hEventSrc,
                                                      EVENT_TYPE iEventID,
                                                      EVENT_HANDLE *phEvent) {
  GENTL_REQUIRE_INITIALIZED();
  if (phEvent == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }

  if (iEventID == EVENT_NEW_BUFFER) {
    auto *stream = gentl_producer_core::runtime::Runtime::as_ds(
        static_cast<DS_HANDLE>(hEventSrc));
    if (stream == nullptr) {
      return gentl_producer_core::runtime::invalid_handle();
    }

    auto *event =
        new gentl_producer_core::runtime::EventHandleData(stream, iEventID);
    {
      std::scoped_lock lock(stream->mutex);
      if (!stream->new_buffer_events.empty()) {
        delete event;
        return GC_ERR_RESOURCE_IN_USE;
      }
      stream->new_buffer_events.push_back(event);
    }
    *phEvent = event;
    return GC_ERR_SUCCESS;
  }

  if (iEventID == EVENT_FEATURE_INVALIDATE) {
    auto *port = gentl_producer_core::runtime::Runtime::as_port(
        static_cast<PORT_HANDLE>(hEventSrc));
    if (port == nullptr ||
        port->port_kind !=
            gentl_producer_core::runtime::PortKind::kRemoteDevice) {
      return gentl_producer_core::runtime::invalid_handle();
    }

    auto *event =
        new gentl_producer_core::runtime::EventHandleData(port, iEventID);
    {
      std::scoped_lock lock(port->mutex);
      if (!port->feature_invalidate_events.empty()) {
        delete event;
        return GC_ERR_RESOURCE_IN_USE;
      }
      port->feature_invalidate_events.push_back(event);
    }
    *phEvent = event;
    return GC_ERR_SUCCESS;
  }

  return gentl_producer_core::runtime::not_implemented(
      "only EVENT_NEW_BUFFER and EVENT_FEATURE_INVALIDATE are implemented");
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
GCUnregisterEvent(EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID) {
  GENTL_REQUIRE_INITIALIZED();
  std::vector<gentl_producer_core::runtime::EventHandleData *> removed;

  if (iEventID == EVENT_NEW_BUFFER) {
    auto *stream = gentl_producer_core::runtime::Runtime::as_ds(
        static_cast<DS_HANDLE>(hEventSrc));
    if (stream == nullptr) {
      return gentl_producer_core::runtime::invalid_handle();
    }

    {
      std::scoped_lock lock(stream->mutex);
      auto &events = stream->new_buffer_events;
      auto end = std::remove_if(
          events.begin(), events.end(),
          [&](gentl_producer_core::runtime::EventHandleData *event) {
            if (event->event_type != iEventID) {
              return false;
            }
            removed.push_back(event);
            return true;
          });
      events.erase(end, events.end());
    }
  } else if (iEventID == EVENT_FEATURE_INVALIDATE) {
    auto *port = gentl_producer_core::runtime::Runtime::as_port(
        static_cast<PORT_HANDLE>(hEventSrc));
    if (port == nullptr ||
        port->port_kind !=
            gentl_producer_core::runtime::PortKind::kRemoteDevice) {
      return gentl_producer_core::runtime::invalid_handle();
    }

    {
      std::scoped_lock lock(port->mutex);
      auto &events = port->feature_invalidate_events;
      auto end = std::remove_if(
          events.begin(), events.end(),
          [&](gentl_producer_core::runtime::EventHandleData *event) {
            if (event->event_type != iEventID) {
              return false;
            }
            removed.push_back(event);
            return true;
          });
      events.erase(end, events.end());
    }
  } else {
    return GC_ERR_NOT_AVAILABLE;
  }

  for (auto *event : removed) {
    std::scoped_lock lock(event->mutex);
    event->registered = false;
    event->source = nullptr;
    event->new_buffer_queue.clear();
    event->feature_invalidate_queue.clear();
    event->condition.notify_all();
  }
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE EventGetData(EVENT_HANDLE hEvent,
                                                   void *pBuffer,
                                                   size_t *piSize,
                                                   uint64_t iTimeout) {
  GENTL_REQUIRE_INITIALIZED();
  auto *event = gentl_producer_core::runtime::Runtime::as_event(hEvent);
  if (event == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (piSize == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }

  const auto deadline = iTimeout == GENTL_INFINITE
                            ? std::chrono::steady_clock::time_point::max()
                            : std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(iTimeout);
  for (;;) {
    std::unique_lock lock(event->mutex);
    auto has_payload = [&]() {
      switch (event->event_type) {
      case EVENT_NEW_BUFFER:
        return !event->new_buffer_queue.empty();
      case EVENT_FEATURE_INVALIDATE:
        return !event->feature_invalidate_queue.empty();
      default:
        return false;
      }
    };
    auto ready = [&]() { return has_payload() || !event->registered; };

    bool signaled = false;
    if (iTimeout == GENTL_INFINITE) {
      event->condition.wait(lock, ready);
      signaled = has_payload();
    } else {
      signaled =
          event->condition.wait_until(lock, deadline, ready) && has_payload();
    }

    if (!signaled) {
      return GC_ERR_TIMEOUT;
    }

    if (event->event_type == EVENT_NEW_BUFFER) {
      const size_t required_size = sizeof(EVENT_NEW_BUFFER_DATA);
      if (pBuffer == nullptr) {
        *piSize = required_size;
        return GC_ERR_SUCCESS;
      }
      if (*piSize < required_size) {
        *piSize = required_size;
        return GC_ERR_BUFFER_TOO_SMALL;
      }

      // Wait without the stream lock, then transfer queue ownership under the
      // same stream -> event order used by publication and DSFlushQueue.
      auto *stream = static_cast<gentl_producer_core::runtime::DsHandleData *>(
          event->source);
      if (stream == nullptr || !event->registered)
        return GC_ERR_TIMEOUT;
      lock.unlock();
      std::scoped_lock stream_lock(stream->mutex);
      lock.lock();
      if (!event->registered || event->source != stream)
        return GC_ERR_TIMEOUT;
      if (event->new_buffer_queue.empty())
        continue; // A concurrent discard consumed it; retain the original
                  // deadline.
      const auto payload = event->new_buffer_queue.front();
      event->new_buffer_queue.pop_front();
      if (auto *delivered = gentl_producer_core::runtime::Runtime::as_buffer(
              payload.BufferHandle);
          delivered != nullptr) {
        delivered->awaiting_delivery = false;
      }
      std::memcpy(pBuffer, &payload, required_size);
      *piSize = required_size;
      return GC_ERR_SUCCESS;
    }

    if (event->event_type == EVENT_FEATURE_INVALIDATE) {
      const std::string payload = event->feature_invalidate_queue.front();
      event->feature_invalidate_queue.pop_front();
      const size_t required_size = payload.size() + 1U;
      if (pBuffer == nullptr) {
        *piSize = required_size;
        return GC_ERR_SUCCESS;
      }
      if (*piSize < required_size) {
        *piSize = required_size;
        return GC_ERR_BUFFER_TOO_SMALL;
      }
      std::memcpy(pBuffer, payload.c_str(), required_size);
      *piSize = required_size;
      return GC_ERR_SUCCESS;
    }

    return GC_ERR_NOT_AVAILABLE;
  }
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
EventGetDataInfo(EVENT_HANDLE hEvent, const void *pInBuffer, size_t iInSize,
                 EVENT_DATA_INFO_CMD iInfoCmd, INFO_DATATYPE *piType,
                 void *pOutBuffer, size_t *piOutSize) {
  GENTL_REQUIRE_INITIALIZED();
  auto *event = gentl_producer_core::runtime::Runtime::as_event(hEvent);
  if (event == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (pInBuffer == nullptr || iInSize == 0U) {
    return GC_ERR_INVALID_PARAMETER;
  }

  if (event->event_type == EVENT_NEW_BUFFER) {
    if (iInSize < sizeof(EVENT_NEW_BUFFER_DATA)) {
      return GC_ERR_INVALID_PARAMETER;
    }

    const auto *payload = static_cast<const EVENT_NEW_BUFFER_DATA *>(pInBuffer);
    auto *buffer =
        gentl_producer_core::runtime::Runtime::as_buffer(payload->BufferHandle);
    if (buffer == nullptr) {
      return gentl_producer_core::runtime::invalid_handle();
    }

    switch (iInfoCmd) {
    case EVENT_DATA_ID:
      return gentl_producer_core::runtime::info_string("NewBuffer", piType,
                                                       pOutBuffer, piOutSize);
    case EVENT_DATA_VALUE:
      return gentl_producer_core::runtime::info_scalar<void *>(
          payload->pUserPointer, INFO_DATATYPE_PTR, piType, pOutBuffer,
          piOutSize);
    case EVENT_DATA_NUMID:
      return gentl_producer_core::runtime::info_scalar<uint64_t>(
          buffer->frame_id, INFO_DATATYPE_UINT64, piType, pOutBuffer,
          piOutSize);
    default:
      return GC_ERR_INVALID_ID;
    }
  }

  if (event->event_type == EVENT_FEATURE_INVALIDATE) {
    const std::string feature_name(
        static_cast<const char *>(pInBuffer),
        strnlen(static_cast<const char *>(pInBuffer), iInSize));
    switch (iInfoCmd) {
    case EVENT_DATA_ID:
    case EVENT_DATA_VALUE:
      return gentl_producer_core::runtime::info_string(feature_name, piType,
                                                       pOutBuffer, piOutSize);
    case EVENT_DATA_NUMID:
      return gentl_producer_core::runtime::info_scalar<uint64_t>(
          0, INFO_DATATYPE_UINT64, piType, pOutBuffer, piOutSize);
    default:
      return GC_ERR_INVALID_ID;
    }
  }

  return GC_ERR_NOT_AVAILABLE;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE EventGetInfo(EVENT_HANDLE hEvent,
                                                   EVENT_INFO_CMD iInfoCmd,
                                                   INFO_DATATYPE *piType,
                                                   void *pBuffer,
                                                   size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  auto *event = gentl_producer_core::runtime::Runtime::as_event(hEvent);
  if (event == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  std::scoped_lock lock(event->mutex);
  const auto queue_size = event->event_type == EVENT_NEW_BUFFER
                              ? event->new_buffer_queue.size()
                              : event->feature_invalidate_queue.size();
  const auto size_max = event->event_type == EVENT_NEW_BUFFER
                            ? sizeof(EVENT_NEW_BUFFER_DATA)
                            : static_cast<size_t>(128);
  switch (iInfoCmd) {
  case EVENT_EVENT_TYPE:
    return gentl_producer_core::runtime::info_scalar<int32_t>(
        event->event_type, INFO_DATATYPE_INT32, piType, pBuffer, piSize);
  case EVENT_NUM_IN_QUEUE:
    return gentl_producer_core::runtime::info_scalar<size_t>(
        queue_size, INFO_DATATYPE_SIZET, piType, pBuffer, piSize);
  case EVENT_NUM_FIRED:
    return gentl_producer_core::runtime::info_scalar<uint64_t>(
        event->fired_count, INFO_DATATYPE_UINT64, piType, pBuffer, piSize);
  case EVENT_SIZE_MAX:
  case EVENT_INFO_DATA_SIZE_MAX:
    return gentl_producer_core::runtime::info_scalar<size_t>(
        size_max, INFO_DATATYPE_SIZET, piType, pBuffer, piSize);
  default:
    return GC_ERR_INVALID_ID;
  }
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE EventFlush(EVENT_HANDLE hEvent) {
  GENTL_REQUIRE_INITIALIZED();
  auto *event = gentl_producer_core::runtime::Runtime::as_event(hEvent);
  if (event == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  std::unique_lock event_lock(event->mutex);
  if (event->event_type != EVENT_NEW_BUFFER || event->source == nullptr ||
      !event->registered) {
    event->new_buffer_queue.clear();
    event->feature_invalidate_queue.clear();
    return GC_ERR_SUCCESS;
  }
  auto *stream =
      static_cast<gentl_producer_core::runtime::DsHandleData *>(event->source);
  event_lock.unlock();
  std::scoped_lock stream_lock(stream->mutex);
  event_lock.lock();
  if (event->registered && event->source == stream) {
#ifdef GENTL_RUNTIME_TEST_HOOKS
    gentl_producer_core::runtime::buffer_publication_test_hook(2);
#endif
    for (const auto &payload : event->new_buffer_queue) {
      auto *buffer = gentl_producer_core::runtime::Runtime::as_buffer(
          payload.BufferHandle);
      if (buffer == nullptr)
        continue;
      buffer->awaiting_delivery = false;
      buffer->has_new_data = false;
      buffer->is_incomplete = false;
      buffer->size_filled = 0;
    }
  }
  event->new_buffer_queue.clear();
  event->feature_invalidate_queue.clear();
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE EventKill(EVENT_HANDLE hEvent) {
  GENTL_REQUIRE_INITIALIZED();
  auto *event = gentl_producer_core::runtime::Runtime::as_event(hEvent);
  if (event == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  gentl_producer_core::runtime::detach_event_from_source(event);
  {
    std::scoped_lock lock(event->mutex);
    event->registered = false;
    event->source = nullptr;
    event->new_buffer_queue.clear();
    event->feature_invalidate_queue.clear();
    event->condition.notify_all();
  }
  gentl_producer_core::runtime::release_event(event);
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE TLClose(TL_HANDLE hTL) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::TlHandleData *tl =
      gentl_producer_core::runtime::Runtime::as_tl(hTL);
  if (tl == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  {
    std::scoped_lock lock(tl->mutex);
    if (tl->open_interfaces != 0U) {
      return gentl_producer_core::runtime::access_denied(
          "transport layer has open interfaces");
    }
  }
  delete tl;
  gentl_producer_core::runtime::Runtime::instance().release_system_open();
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE TLOpenInterface(TL_HANDLE hTL,
                                                      const char *sIfaceID,
                                                      IF_HANDLE *phIface) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::TlHandleData *tl =
      gentl_producer_core::runtime::Runtime::as_tl(hTL);
  if (tl == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (phIface == nullptr || sIfaceID == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  const auto &backend_info =
      gentl_producer_core::runtime::Runtime::instance().backend_info();
  if (std::string(sIfaceID) != backend_info.interface_id) {
    return gentl_producer_core::runtime::invalid_id("unknown interface id");
  }
  auto *iface = new gentl_producer_core::runtime::IfHandleData(
      tl, backend_info.interface_id);
  {
    std::scoped_lock lock(tl->mutex);
    if (tl->open_interfaces != 0U) {
      delete iface;
      return gentl_producer_core::runtime::resource_in_use(
          "Interface module is already open");
    }
    ++tl->open_interfaces;
  }
  *phIface = iface;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE IFClose(IF_HANDLE hIface) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::IfHandleData *iface =
      gentl_producer_core::runtime::Runtime::as_if(hIface);
  if (iface == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  {
    std::scoped_lock lock(iface->mutex);
    if (iface->open_devices != 0U) {
      return gentl_producer_core::runtime::access_denied(
          "interface has open devices");
    }
  }
  auto *parent = iface->parent;
  {
    std::scoped_lock lock(parent->mutex);
    if (parent->open_interfaces != 0U)
      --parent->open_interfaces;
  }
  delete iface;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE IFGetDeviceInfo(
    IF_HANDLE hIface, const char *sDeviceID, DEVICE_INFO_CMD iInfoCmd,
    INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  if (gentl_producer_core::runtime::Runtime::as_if(hIface) == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (sDeviceID == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }

  gentl_producer_core::DeviceDescriptor device;
  const auto result =
      gentl_producer_core::runtime::require_camera(sDeviceID, &device);
  if (result != GC_ERR_SUCCESS) {
    return result;
  }
  return gentl_producer_core::runtime::device_info_response(
      device, DEVICE_ACCESS_STATUS_READWRITE, iInfoCmd, piType, pBuffer,
      piSize);
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
IFOpenDevice(IF_HANDLE hIface, const char *sDeviceID,
             DEVICE_ACCESS_FLAGS iOpenFlag, DEV_HANDLE *phDevice) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::IfHandleData *iface =
      gentl_producer_core::runtime::Runtime::as_if(hIface);
  if (iface == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (phDevice == nullptr || sDeviceID == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  if (iOpenFlag != DEVICE_ACCESS_READONLY &&
      iOpenFlag != DEVICE_ACCESS_CONTROL &&
      iOpenFlag != DEVICE_ACCESS_EXCLUSIVE) {
    return GC_ERR_INVALID_PARAMETER;
  }

  gentl_producer_core::DeviceDescriptor device;
  const auto result =
      gentl_producer_core::runtime::require_camera(sDeviceID, &device);
  if (result != GC_ERR_SUCCESS) {
    return result;
  }
  if (!gentl_producer_core::runtime::Runtime::instance().claim_device_open(
          device.id, iOpenFlag)) {
    return gentl_producer_core::runtime::resource_in_use(
        "Device module is already open");
  }
  *phDevice = new gentl_producer_core::runtime::DevHandleData(
      iface, std::move(device), iOpenFlag);
  {
    std::scoped_lock lock(iface->mutex);
    ++iface->open_devices;
  }
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DevOpenDataStream(
    DEV_HANDLE hDevice, const char *sDataStreamID, DS_HANDLE *phDataStream) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DevHandleData *device =
      gentl_producer_core::runtime::Runtime::as_dev(hDevice);
  if (device == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (phDataStream == nullptr || sDataStreamID == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  const std::string expected_id = device->device.id + "::stream0";
  if (expected_id != sDataStreamID) {
    return gentl_producer_core::runtime::invalid_id("unknown data stream id");
  }
  std::scoped_lock configuration_lock(device->configuration_mutex);
  {
    std::scoped_lock lock(device->mutex);
    if (device->open_stream != nullptr)
      return gentl_producer_core::runtime::resource_in_use(
          "Data Stream module is already open");
  }
  std::string error_text;
  gentl_producer_core::StreamDescription description;
  if (!gentl_producer_core::runtime::Runtime::instance().get_stream_description(
          device->device, &description, &error_text))
    return gentl_producer_core::runtime::io_error(error_text);
  auto *stream = new gentl_producer_core::runtime::DsHandleData(device);
  gentl_producer_core::runtime::apply_stream_description_locked(stream,
                                                                description);
  {
    std::scoped_lock lock(device->mutex);
    device->open_stream = stream;
  }
  *phDataStream = stream;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DevClose(DEV_HANDLE hDevice) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DevHandleData *device =
      gentl_producer_core::runtime::Runtime::as_dev(hDevice);
  if (device == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  std::unique_lock configuration_lock(device->configuration_mutex);
  {
    std::scoped_lock lock(device->mutex);
    if (device->open_stream != nullptr) {
      return gentl_producer_core::runtime::access_denied(
          "device has open data streams; close streams before DevClose");
    }
  }
  std::vector<gentl_producer_core::runtime::EventHandleData *> events;
  {
    std::scoped_lock lock(device->remote_port->mutex);
    events = device->remote_port->feature_invalidate_events;
    device->remote_port->feature_invalidate_events.clear();
  }
  for (auto *event : events) {
    std::scoped_lock lock(event->mutex);
    event->registered = false;
    event->source = nullptr;
    event->feature_invalidate_queue.clear();
    event->condition.notify_all();
  }
  gentl_producer_core::runtime::Runtime::instance().release_device_open(
      device->device.id, device->access);
  {
    std::scoped_lock lock(device->parent->mutex);
    if (device->parent->open_devices != 0U)
      --device->parent->open_devices;
  }
  configuration_lock.unlock();
  delete device;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
DSAnnounceBuffer(DS_HANDLE hDataStream, void *pBuffer, size_t iSize,
                 void *pPrivate, BUFFER_HANDLE *phBuffer) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DsHandleData *stream =
      gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  if (stream == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (pBuffer == nullptr || iSize == 0U || phBuffer == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  std::scoped_lock lock(stream->mutex);
  if (reinterpret_cast<std::uintptr_t>(pBuffer) %
          stream->current_buffer_alignment !=
      0U) {
    return gentl_producer_core::runtime::invalid_buffer(
        "buffer base does not meet stream alignment");
  }
  if (stream->buffer_allocation_mode ==
      gentl_producer_core::runtime::BufferAllocationMode::kProducer) {
    return gentl_producer_core::runtime::resource_in_use(
        "consumer and producer buffer allocation cannot be mixed");
  }
  const auto duplicate =
      std::ranges::find_if(stream->buffers, [pBuffer](const auto &buffer) {
        return !buffer->producer_owned && buffer->base == pBuffer;
      });
  if (duplicate != stream->buffers.end()) {
    return gentl_producer_core::runtime::resource_in_use(
        "buffer is already announced");
  }
  auto buffer =
      std::make_unique<gentl_producer_core::runtime::BufferHandleData>(
          stream, pBuffer, iSize, pPrivate, false);
  *phBuffer = buffer.get();
  stream->buffer_allocation_mode =
      gentl_producer_core::runtime::BufferAllocationMode::kConsumer;
  stream->buffers.push_back(std::move(buffer));
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
DSAllocAndAnnounceBuffer(DS_HANDLE hDataStream, size_t iSize, void *pPrivate,
                         BUFFER_HANDLE *phBuffer) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DsHandleData *stream =
      gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  if (stream == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (iSize == 0U || phBuffer == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }

  std::scoped_lock lock(stream->mutex);
  if (iSize > std::numeric_limits<std::size_t>::max() -
                  (stream->current_buffer_alignment - 1U)) {
    return gentl_producer_core::runtime::invalid_buffer(
        "aligned buffer allocation size overflows");
  }
  if (stream->buffer_allocation_mode ==
      gentl_producer_core::runtime::BufferAllocationMode::kConsumer) {
    return gentl_producer_core::runtime::resource_in_use(
        "consumer and producer buffer allocation cannot be mixed");
  }
  auto buffer =
      std::make_unique<gentl_producer_core::runtime::BufferHandleData>(
          stream, nullptr, iSize, pPrivate, true);
  *phBuffer = buffer.get();
  stream->buffer_allocation_mode =
      gentl_producer_core::runtime::BufferAllocationMode::kProducer;
  stream->buffers.push_back(std::move(buffer));
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSFlushQueue(DS_HANDLE hDataStream,
                                                   ACQ_QUEUE_TYPE iOperation) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DsHandleData *stream =
      gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  if (stream == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  if (iOperation != ACQ_QUEUE_ALL_TO_INPUT &&
      iOperation != ACQ_QUEUE_UNQUEUED_TO_INPUT &&
      iOperation != ACQ_QUEUE_OUTPUT_DISCARD &&
      iOperation != ACQ_QUEUE_ALL_DISCARD &&
      iOperation != ACQ_QUEUE_INPUT_TO_OUTPUT)
    return GC_ERR_INVALID_PARAMETER;

  std::vector<gentl_producer_core::runtime::BufferHandleData *> moved_to_output;
  std::vector<gentl_producer_core::runtime::EventHandleData *> events;
  {
#ifdef GENTL_RUNTIME_TEST_HOOKS
    std::unique_lock lock(stream->mutex, std::defer_lock);
    if (!lock.try_lock()) {
      gentl_producer_core::runtime::buffer_publication_test_hook(1);
      lock.lock();
    }
#else
    std::scoped_lock lock(stream->mutex);
#endif
    events = gentl_producer_core::runtime::retain_events_locked(
        stream->new_buffer_events);
    if (iOperation == ACQ_QUEUE_INPUT_TO_OUTPUT) {
      while (!stream->input_queue.empty()) {
        auto *buffer = stream->input_queue.front();
        stream->input_queue.pop_front();
        buffer->is_queued = false;
        buffer->has_new_data = false;
        buffer->awaiting_delivery = true;
        buffer->is_incomplete = true;
        buffer->size_filled = 0;
        gentl_producer_core::runtime::snapshot_buffer_description_locked(
            stream, buffer);
        moved_to_output.push_back(buffer);
      }
    } else if (iOperation == ACQ_QUEUE_ALL_TO_INPUT) {
      stream->input_queue.clear();
      for (auto &buffer : stream->buffers) {
        if (buffer->is_acquiring)
          continue;
        buffer->is_queued = true;
        buffer->has_new_data = false;
        buffer->awaiting_delivery = false;
        buffer->is_incomplete = false;
        buffer->size_filled = 0;
        stream->input_queue.push_back(buffer.get());
      }
    } else if (iOperation == ACQ_QUEUE_UNQUEUED_TO_INPUT) {
      for (auto &buffer : stream->buffers) {
        if (buffer->is_queued || buffer->is_acquiring ||
            buffer->awaiting_delivery)
          continue;
        buffer->is_queued = true;
        buffer->has_new_data = false;
        buffer->is_incomplete = false;
        buffer->size_filled = 0;
        stream->input_queue.push_back(buffer.get());
      }
    } else if (iOperation == ACQ_QUEUE_OUTPUT_DISCARD) {
      for (auto &buffer : stream->buffers) {
        if (!buffer->awaiting_delivery)
          continue;
        buffer->has_new_data = false;
        buffer->awaiting_delivery = false;
        buffer->is_incomplete = false;
        buffer->size_filled = 0;
      }
    } else {
      stream->input_queue.clear();
      for (auto &buffer : stream->buffers) {
        if (buffer->is_acquiring)
          continue;
        buffer->is_queued = false;
        buffer->has_new_data = false;
        buffer->awaiting_delivery = false;
        buffer->is_incomplete = false;
        buffer->size_filled = 0;
      }
    }
    // Publication, consumption, and discard share stream -> event ordering.
    if (iOperation == ACQ_QUEUE_OUTPUT_DISCARD ||
        iOperation == ACQ_QUEUE_ALL_DISCARD ||
        iOperation == ACQ_QUEUE_ALL_TO_INPUT) {
      for (auto *event : events) {
        std::scoped_lock event_lock(event->mutex);
        event->new_buffer_queue.clear();
      }
    }
    for (auto *buffer : moved_to_output) {
      for (auto *event : events)
        gentl_producer_core::runtime::publish_stream_buffer(event, buffer);
    }
  }
  gentl_producer_core::runtime::release_events(events);
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
DSStartAcquisition(DS_HANDLE hDataStream, ACQ_START_FLAGS iStartFlags,
                   uint64_t iNumToAcquire) {
  GENTL_REQUIRE_INITIALIZED();
  auto *stream = gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  if (stream == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  if (iStartFlags != ACQ_START_FLAGS_DEFAULT) {
    return gentl_producer_core::runtime::not_implemented(
        "unsupported acquisition start flag");
  }
  if (iNumToAcquire == 0U) {
    return GC_ERR_INVALID_PARAMETER;
  }
  std::scoped_lock configuration_lock(stream->parent->configuration_mutex);
  gentl_producer_core::StreamDescription description;
  std::string error_text;
  {
    std::scoped_lock lock(stream->mutex);
    if (stream->acquisition_session_active) {
      return gentl_producer_core::runtime::resource_in_use(
          "data stream acquisition is active");
    }
    if (!gentl_producer_core::runtime::Runtime::instance()
             .get_stream_description(stream->parent->device, &description,
                                     &error_text)) {
      return gentl_producer_core::runtime::io_error(error_text);
    }
    if (stream->buffers.empty()) {
      return gentl_producer_core::runtime::invalid_buffer(
          "fewer than STREAM_INFO_BUF_ANNOUNCE_MIN buffers are announced");
    }
    if (std::ranges::any_of(stream->buffers, [&](const auto &buffer) {
          return buffer->size < description.payload_size_bytes;
        })) {
      return GC_ERR_BUFFER_TOO_SMALL;
    }
    if (std::ranges::any_of(stream->buffers, [&](const auto &buffer) {
          return reinterpret_cast<std::uintptr_t>(buffer->base) %
                     description.buffer_alignment !=
                 0U;
        })) {
      return gentl_producer_core::runtime::invalid_buffer(
          "buffer base does not meet stream alignment");
    }
    if (!gentl_producer_core::runtime::Runtime::instance().start_stream(
            stream->parent->device, &error_text)) {
      return gentl_producer_core::runtime::io_error(error_text);
    }
    gentl_producer_core::runtime::apply_stream_description_locked(stream,
                                                                  description);
    stream->stop_requested = false;
    stream->worker_error.clear();
    stream->worker_running = true;
    stream->is_acquiring = true;
    stream->acquisition_session_active = true;
    stream->acquisition_limit = iNumToAcquire;
    stream->delivered_count = 0;
    stream->started_count = 0;
    {
      std::scoped_lock device_lock(stream->parent->mutex);
      stream->parent->acquisition_active = true;
    }
    stream->worker =
        std::thread(gentl_producer_core::runtime::stream_worker, stream);
  }
  gentl_producer_core::runtime::publish_port_feature_invalidations(
      stream->parent->remote_port.get(),
      gentl_producer_core::runtime::Runtime::instance()
          .acquisition_invalidated_features());
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
DSStopAcquisition(DS_HANDLE hDataStream, ACQ_STOP_FLAGS iStopFlags) {
  GENTL_REQUIRE_INITIALIZED();
  auto *stream = gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  if (stream == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  if (iStopFlags != ACQ_STOP_FLAGS_DEFAULT &&
      iStopFlags != ACQ_STOP_FLAGS_KILL) {
    return gentl_producer_core::runtime::not_implemented(
        "unsupported acquisition stop flag");
  }

  std::scoped_lock configuration_lock(stream->parent->configuration_mutex);
  {
    std::scoped_lock lock(stream->mutex);
    if (!stream->acquisition_session_active) {
      return gentl_producer_core::runtime::resource_in_use(
          "data stream acquisition was not started or was already stopped");
    }
    stream->stop_requested = true;
  }

  std::string error_text;
  const bool stopped =
      gentl_producer_core::runtime::Runtime::instance().stop_stream(
          stream->parent->device, &error_text);

  if (stream->worker.joinable()) {
    stream->worker.join();
  }

  {
    std::scoped_lock lock(stream->mutex);
    stream->worker_running = false;
    stream->is_acquiring = false;
    stream->acquisition_session_active = false;
  }
  {
    std::scoped_lock lock(stream->parent->mutex);
    stream->parent->acquisition_active = false;
  }
  gentl_producer_core::runtime::publish_port_feature_invalidations(
      stream->parent->remote_port.get(),
      gentl_producer_core::runtime::Runtime::instance()
          .acquisition_invalidated_features());
  return stopped ? GC_ERR_SUCCESS
                 : gentl_producer_core::runtime::io_error(error_text);
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSGetInfo(DS_HANDLE hDataStream,
                                                STREAM_INFO_CMD iInfoCmd,
                                                INFO_DATATYPE *piType,
                                                void *pBuffer, size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DsHandleData *stream =
      gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  if (stream == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  std::scoped_lock lock(stream->mutex);
  const auto queued =
      stream->input_queue.size() +
      static_cast<size_t>(std::count_if(
          stream->buffers.begin(), stream->buffers.end(),
          [](const auto &buffer) { return buffer->is_acquiring; }));
  const auto awaiting_delivery = static_cast<size_t>(std::count_if(
      stream->buffers.begin(), stream->buffers.end(),
      [](const auto &buffer) { return buffer->awaiting_delivery; }));

  switch (iInfoCmd) {
  case STREAM_INFO_ID:
    return gentl_producer_core::runtime::info_string(stream->id, piType,
                                                     pBuffer, piSize);
  case STREAM_INFO_NUM_DELIVERED:
    return gentl_producer_core::runtime::info_scalar<uint64_t>(
        stream->delivered_count, INFO_DATATYPE_UINT64, piType, pBuffer, piSize);
  case STREAM_INFO_NUM_UNDERRUN:
    return gentl_producer_core::runtime::info_scalar<uint64_t>(
        stream->underrun_count, INFO_DATATYPE_UINT64, piType, pBuffer, piSize);
  case STREAM_INFO_NUM_ANNOUNCED:
    return gentl_producer_core::runtime::info_scalar<size_t>(
        stream->buffers.size(), INFO_DATATYPE_SIZET, piType, pBuffer, piSize);
  case STREAM_INFO_NUM_QUEUED:
    return gentl_producer_core::runtime::info_scalar<size_t>(
        queued, INFO_DATATYPE_SIZET, piType, pBuffer, piSize);
  case STREAM_INFO_NUM_AWAIT_DELIVERY:
    return gentl_producer_core::runtime::info_scalar<size_t>(
        awaiting_delivery, INFO_DATATYPE_SIZET, piType, pBuffer, piSize);
  case STREAM_INFO_NUM_STARTED:
    return gentl_producer_core::runtime::info_scalar<uint64_t>(
        stream->started_count, INFO_DATATYPE_UINT64, piType, pBuffer, piSize);
  case STREAM_INFO_PAYLOAD_SIZE:
    return gentl_producer_core::runtime::info_scalar<size_t>(
        stream->current_payload_size, INFO_DATATYPE_SIZET, piType, pBuffer,
        piSize);
  case STREAM_INFO_IS_GRABBING:
    return gentl_producer_core::runtime::info_scalar<bool8_t>(
        stream->is_acquiring ? 1 : 0, INFO_DATATYPE_BOOL8, piType, pBuffer,
        piSize);
  case STREAM_INFO_DEFINES_PAYLOADSIZE:
    return gentl_producer_core::runtime::info_scalar<bool8_t>(
        0, INFO_DATATYPE_BOOL8, piType, pBuffer, piSize);
  case STREAM_INFO_TLTYPE:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::Runtime::instance()
            .backend_info()
            .tl_type,
        piType, pBuffer, piSize);
  case STREAM_INFO_NUM_CHUNKS_MAX:
    return gentl_producer_core::runtime::info_scalar<size_t>(
        0, INFO_DATATYPE_SIZET, piType, pBuffer, piSize);
  case STREAM_INFO_BUF_ANNOUNCE_MIN:
    return gentl_producer_core::runtime::info_scalar<size_t>(
        1, INFO_DATATYPE_SIZET, piType, pBuffer, piSize);
  case STREAM_INFO_BUF_ALIGNMENT:
    return gentl_producer_core::runtime::info_scalar<size_t>(
        stream->current_buffer_alignment, INFO_DATATYPE_SIZET, piType, pBuffer,
        piSize);
  default:
    return GC_ERR_INVALID_ID;
  }
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSClose(DS_HANDLE hDataStream) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DsHandleData *stream =
      gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  if (stream == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  auto *device = stream->parent;
  std::scoped_lock configuration_lock(device->configuration_mutex);
  {
    std::scoped_lock lock(stream->mutex);
    stream->stop_requested = true;
  }
  std::string error_text;
  gentl_producer_core::runtime::Runtime::instance().stop_stream(device->device,
                                                                &error_text);
  if (stream->worker.joinable()) {
    stream->worker.join();
  }
  std::vector<gentl_producer_core::runtime::EventHandleData *> events;
  {
    std::scoped_lock lock(stream->mutex);
    events = stream->new_buffer_events;
    stream->new_buffer_events.clear();
  }
  for (auto *event : events) {
    std::scoped_lock lock(event->mutex);
    event->registered = false;
    event->source = nullptr;
    event->condition.notify_all();
  }
  {
    std::scoped_lock lock(device->mutex);
    device->open_stream = nullptr;
    device->acquisition_active = false;
  }
  delete stream;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSRevokeBuffer(DS_HANDLE hDataStream,
                                                     BUFFER_HANDLE hBuffer,
                                                     void **pBuffer,
                                                     void **pPrivate) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DsHandleData *stream =
      gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  gentl_producer_core::runtime::BufferHandleData *buffer =
      gentl_producer_core::runtime::Runtime::as_buffer(hBuffer);
  if (stream == nullptr || buffer == nullptr || buffer->parent != stream) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  std::scoped_lock lock(stream->mutex);
  auto it = std::find_if(
      stream->buffers.begin(), stream->buffers.end(),
      [buffer](const auto &entry) { return entry.get() == buffer; });
  if (it == stream->buffers.end()) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (buffer->is_queued || buffer->is_acquiring || buffer->awaiting_delivery) {
    return gentl_producer_core::runtime::busy(
        "cannot revoke a buffer owned by a stream queue");
  }
  stream->input_queue.erase(std::remove(stream->input_queue.begin(),
                                        stream->input_queue.end(), buffer),
                            stream->input_queue.end());
  if (pBuffer != nullptr)
    *pBuffer = buffer->producer_owned ? nullptr : buffer->base;
  if (pPrivate != nullptr)
    *pPrivate = buffer->private_data;
  stream->buffers.erase(it);
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSQueueBuffer(DS_HANDLE hDataStream,
                                                    BUFFER_HANDLE hBuffer) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DsHandleData *stream =
      gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  gentl_producer_core::runtime::BufferHandleData *buffer =
      gentl_producer_core::runtime::Runtime::as_buffer(hBuffer);
  if (stream == nullptr || buffer == nullptr || buffer->parent != stream) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  std::scoped_lock lock(stream->mutex);
  if (buffer->is_queued || buffer->is_acquiring || buffer->awaiting_delivery) {
    return gentl_producer_core::runtime::resource_in_use(
        "buffer is already owned by a stream queue");
  }
  if (buffer->size < stream->current_payload_size)
    return GC_ERR_BUFFER_TOO_SMALL;
  buffer->is_queued = true;
  buffer->has_new_data = false;
  buffer->is_incomplete = false;
  buffer->awaiting_delivery = false;
  buffer->size_filled = 0;
  stream->input_queue.push_back(buffer);
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferInfo(
    DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, BUFFER_INFO_CMD iInfoCmd,
    INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DsHandleData *stream =
      gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  gentl_producer_core::runtime::BufferHandleData *buffer =
      gentl_producer_core::runtime::Runtime::as_buffer(hBuffer);
  if (stream == nullptr || buffer == nullptr || buffer->parent != stream) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  std::scoped_lock lock(stream->mutex);
  const bool has_snapshot = buffer->has_new_data || buffer->is_incomplete;
  const gentl_producer_core::BufferInfo info{
      .base = buffer->base,
      .size = buffer->size,
      .user_ptr = buffer->private_data,
      .timestamp = buffer->timestamp,
      .new_data = buffer->has_new_data,
      .is_queued = buffer->is_queued || buffer->is_acquiring ||
                   buffer->awaiting_delivery,
      .is_acquiring = buffer->is_acquiring,
      .is_incomplete = buffer->is_incomplete,
      .tl_type = gentl_producer_core::runtime::Runtime::instance()
                     .backend_info()
                     .tl_type,
      .size_filled = buffer->size_filled,
      .width = has_snapshot ? buffer->image.width : stream->current_width,
      .height = has_snapshot ? buffer->image.height : stream->current_height,
      // Partial payloads do not establish which rows are complete.
      .delivered_image_height =
          buffer->image.payload_size_bytes != 0 &&
                  buffer->size_filled >= buffer->image.payload_size_bytes
              ? buffer->image.height
              : 0,
      .x_offset =
          has_snapshot ? buffer->image.offset_x : stream->current_offset_x,
      .y_offset =
          has_snapshot ? buffer->image.offset_y : stream->current_offset_y,
      .x_padding = 0,
      .y_padding = 0,
      .frame_id = buffer->frame_id,
      .image_present = buffer->size_filled != 0,
      .pixel_format = has_snapshot ? buffer->image.pixel_format
                                   : stream->current_pixel_format,
      .pixel_endianness = has_snapshot ? buffer->image.pixel_endianness
                                       : stream->current_pixel_endianness,
      .data_size = buffer->has_new_data ? buffer->image.payload_size_bytes : 0};
  return gentl_producer_core::info_buffer(info, iInfoCmd, piType, pBuffer,
                                          piSize);
}

#include "exports_common.inc"

#undef GENTL_REQUIRE_INITIALIZED

} // namespace GenTL
