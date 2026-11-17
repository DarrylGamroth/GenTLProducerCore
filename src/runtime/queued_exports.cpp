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
#include <system_error>
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

gentl_producer_core::runtime::BufferHandleData *
take_input_for_sdk_locked(gentl_producer_core::runtime::DsHandleData *stream) {
  if (stream->input_queue.empty()) {
    return nullptr;
  }
  auto *buffer = stream->input_queue.front();
  stream->input_queue.pop_front();
  buffer->is_queued = false;
  buffer->is_sdk_queued = true;
  buffer->has_new_data = false;
  buffer->is_incomplete = false;
  buffer->size_filled = 0;
  stream->sdk_queue.push_back(buffer);
  for (std::size_t index = 0; index < stream->sdk_queue.size(); ++index) {
    auto *queued = stream->sdk_queue[index];
    queued->is_acquiring =
        stream->completion_order == BufferCompletionOrder::kAny || index == 0U;
    queued->is_queued = !queued->is_acquiring;
  }
  return buffer;
}

void refresh_sdk_queue_states_locked(
    gentl_producer_core::runtime::DsHandleData *stream) {
  for (std::size_t index = 0; index < stream->sdk_queue.size(); ++index) {
    auto *buffer = stream->sdk_queue[index];
    buffer->is_sdk_queued = true;
    // In any-order mode the backend can be filling any accepted address.
    buffer->is_acquiring =
        stream->completion_order == BufferCompletionOrder::kAny || index == 0U;
    buffer->is_queued = !buffer->is_acquiring;
  }
}

void return_sdk_queue_to_input_locked(
    gentl_producer_core::runtime::DsHandleData *stream) {
  while (!stream->sdk_queue.empty()) {
    auto *buffer = stream->sdk_queue.back();
    stream->sdk_queue.pop_back();
    buffer->is_sdk_queued = false;
    buffer->is_acquiring = false;
    buffer->is_queued = true;
    buffer->has_new_data = false;
    buffer->awaiting_delivery = false;
    buffer->is_incomplete = false;
    buffer->size_filled = 0;
    stream->input_queue.push_front(buffer);
  }
}

void snapshot_buffer_description_locked(
    gentl_producer_core::runtime::DsHandleData *stream,
    gentl_producer_core::runtime::BufferHandleData *buffer) {
  buffer->width = stream->current_width;
  buffer->height = stream->current_height;
  buffer->x_offset = stream->current_x_offset;
  buffer->y_offset = stream->current_y_offset;
  buffer->x_padding = stream->current_x_padding;
  buffer->y_padding = stream->current_y_padding;
  buffer->pixel_format = stream->current_pixel_format;
  buffer->pixel_size_bits = stream->current_pixel_size_bits;
  buffer->pixel_endianness = stream->current_pixel_endianness;
  buffer->payload_size = stream->current_payload_size;
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

using RetainedEvent =
    std::unique_ptr<gentl_producer_core::runtime::EventHandleData,
                    void (*)(gentl_producer_core::runtime::EventHandleData *)>;

RetainedEvent acquire_event(GenTL::EVENT_HANDLE handle) {
  return {
      gentl_producer_core::runtime::Runtime::instance().acquire_event_handle(
          handle),
      gentl_producer_core::runtime::release_event};
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

// Caller holds retirement_mutex. A failed attempt remains visible until an
// explicit host retry; the worker never retries a concurrent host stop.
gentl_producer_core::StopResult retire_backend_locked(
    gentl_producer_core::runtime::DsHandleData *stream, bool host_retry) {
  {
    std::scoped_lock lock(stream->mutex);
    if ((stream->stop_result.buffers_released && stream->stop_result.success) ||
        (!host_retry && stream->stop_attempted))
      return stream->stop_result;
  }
  std::string error;
  const auto result = gentl_producer_core::runtime::Runtime::instance().stop_stream(
      stream->parent->device, &error);
  std::scoped_lock lock(stream->mutex);
  stream->stop_attempted = true;
  stream->stop_result = result;
  stream->stop_error = std::move(error);
  return result;
}

void notify_stream_work_locked(DsHandleData *stream) {
  stream->work_available.notify_one();
  if (stream->completion_wakeup)
    stream->completion_wakeup->notify();
}

// Caller has joined the worker and holds lifecycle/configuration locks.
void clear_completion_callback(DsHandleData *stream) {
  std::scoped_lock lock(stream->mutex);
  if (stream->completion_wakeup) {
    Runtime::instance().set_completion_callback(stream->parent->device, {});
    stream->completion_wakeup.reset();
  }
}

// The caller holds the stream mutex, which serializes submission with stop.
bool submit_input_locked(DsHandleData *stream) {
  auto *buffer = take_input_for_sdk_locked(stream);
  if (buffer == nullptr)
    return true;
  std::string error;
  if (Runtime::instance().queue_buffer(stream->parent->device, buffer->base,
                                       buffer->size, stream->current_payload_size,
                                       &error))
    return true;
  stream->worker_error = std::move(error);
  stream->sdk_queue.pop_back();
  buffer->is_sdk_queued = false;
  buffer->is_acquiring = false;
  buffer->is_queued = true;
  stream->input_queue.push_front(buffer);
  refresh_sdk_queue_states_locked(stream);
  return false;
}

void stream_worker_impl(gentl_producer_core::runtime::DsHandleData *stream) {
  const auto wakeup = stream->completion_wakeup;
  const bool single_destination = Runtime::instance().cancel_wait_on_stop(
      stream->parent->device);
  for (;;) {
    // Snapshot before both input admission and the completion poll. An event
    // between either readiness check and parking changes this value.
    const auto observed = wakeup
        ? wakeup->generation.load(std::memory_order_acquire) : 0;
    bool stopping = false;
    for (;;) {
      {
        std::scoped_lock lock(stream->mutex);
        if (stream->stop_requested || !stream->worker_error.empty()) {
          stopping = true;
          break;
        }
        const auto remaining = stream->acquisition_limit == GENTL_INFINITE
            ? std::numeric_limits<std::uint64_t>::max()
            : stream->acquisition_limit - stream->delivered_count;
        // A native ring may need a return later in the GenTL input queue
        // before the next physical slot can be rearmed.
        if (stream->input_queue.empty() ||
            (single_destination && !stream->sdk_queue.empty()) ||
            (stream->completion_order == BufferCompletionOrder::kFifo &&
             stream->sdk_queue.size() >= remaining))
          break;
        stopping = !submit_input_locked(stream);
      }
      if (stopping) break;
    }
    if (stopping) break;
    {
      std::unique_lock lock(stream->mutex);
      if (stream->sdk_queue.empty() && !wakeup) {
        if (stream->completion_order == BufferCompletionOrder::kAny) {
          stream->work_available.wait(lock, [&] {
            return stream->stop_requested || !stream->input_queue.empty();
          });
        }
        stopping = true;
      }
    }
    if (stopping) {
      if (stream->completion_order == BufferCompletionOrder::kFifo)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    gentl_producer_core::AcquireFrameResult frame;
    std::string error;
    const bool waited = gentl_producer_core::runtime::Runtime::instance().wait_frame(
        stream->parent->device,
        stream->completion_order == BufferCompletionOrder::kAny ? 0U : 1000U,
        &frame, &error);
    std::vector<gentl_producer_core::runtime::EventHandleData *> events;
    bool done = false;
    {
      std::unique_lock lock(stream->mutex);
      if (stream->kill_requested) break;
      if (!waited) {
        stream->worker_error = std::move(error);
        done = true;
      } else if (frame.status == gentl_producer_core::AcquireFrameStatus::kStopped) {
        done = true;
      } else if (frame.status == gentl_producer_core::AcquireFrameStatus::kSuccess) {
        auto completed = stream->sdk_queue.begin();
        if (stream->completion_order == BufferCompletionOrder::kAny) {
          completed = std::find_if(
              stream->sdk_queue.begin(), stream->sdk_queue.end(),
              [&](auto *buffer) { return buffer->base == frame.buffer; });
        }
        if (completed == stream->sdk_queue.end() ||
            (*completed)->base != frame.buffer) {
          stream->worker_error = stream->completion_order == BufferCompletionOrder::kFifo
              ? "backend returned a buffer outside the GenTL FIFO"
              : "backend returned a buffer that is not currently submitted";
          done = true;
        } else {
          auto *buffer = *completed;
          if (completed == stream->sdk_queue.begin())
            stream->sdk_queue.pop_front();
          else
            stream->sdk_queue.erase(completed);
          buffer->is_sdk_queued = false;
          buffer->is_acquiring = false;
          buffer->is_queued = false;
          refresh_sdk_queue_states_locked(stream);
          ++stream->started_count;
          buffer->has_new_data = true;
          buffer->awaiting_delivery = true;
          buffer->is_incomplete = frame.incomplete;
          buffer->size_filled = frame.bytes_filled;
          buffer->timestamp = frame.timestamp_ns;
          stream->frame_sequence += frame.frame_id_increment;
          buffer->frame_id = stream->frame_sequence;
          snapshot_buffer_description_locked(stream, buffer);
          ++stream->delivered_count;
          stream->underrun_count += frame.skipped_frames;
          events = retain_events_locked(stream->new_buffer_events);
          for (auto *event : events) publish_new_buffer_event(event, buffer);
          done = stream->acquisition_limit != GENTL_INFINITE &&
                 stream->delivered_count >= stream->acquisition_limit;
        }
      } else if (stream->completion_order == BufferCompletionOrder::kAny) {
        if (wakeup) {
          const bool idle = !stream->stop_requested && stream->input_queue.empty();
          lock.unlock();
          if (idle) wakeup->generation.wait(observed, std::memory_order_acquire);
        } else {
          stream->work_available.wait_for(lock, std::chrono::microseconds(100), [&] {
            return stream->stop_requested || !stream->input_queue.empty();
          });
        }
      }
    }
    release_events(events);
    if (done) break;
  }
  {
    std::scoped_lock retirement_lock(stream->retirement_mutex);
    const auto result = retire_backend_locked(stream, false);
    std::scoped_lock lock(stream->mutex);
    if (result.buffers_released && !stream->stop_requested)
      return_sdk_queue_to_input_locked(stream);
    stream->worker_running = false;
    stream->is_acquiring = false;
  }
  {
    std::scoped_lock lock(stream->parent->mutex, stream->mutex);
    stream->parent->acquisition_active = !stream->stop_result.buffers_released ||
                                         !stream->stop_result.success;
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

GenTL::GC_ERROR close_stream_handle_locked(DsHandleData *stream) {
  std::unique_lock lifecycle_lock(stream->lifecycle_mutex);
  auto *device = stream->parent;
  StopResult stopped;
  {
    std::scoped_lock retirement_lock(stream->retirement_mutex);
    {
      std::scoped_lock lock(stream->mutex);
      stream->stop_requested = true;
      stream->kill_requested = true;
      notify_stream_work_locked(stream);
    }
    stopped = retire_backend_locked(stream, true);
  }
  if (stream->worker.joinable()) stream->worker.join();
  if (stopped.buffers_released) {
    std::scoped_lock lock(stream->mutex);
    return_sdk_queue_to_input_locked(stream);
  }
  if (!stopped.success || !stopped.buffers_released) {
    return io_error(stream->stop_error.empty()
        ? "backend did not release stream buffers" : stream->stop_error);
  }
  clear_completion_callback(stream);
  std::string release_error;
  if (!Runtime::instance().release_native_buffers(device->device, &release_error))
    return io_error(release_error);

  std::vector<EventHandleData *> events;
  {
    std::scoped_lock lock(stream->mutex);
    events = retain_events_locked(stream->new_buffer_events);
    stream->new_buffer_events.clear();
  }
  for (auto *event : events) {
    std::scoped_lock lock(event->mutex);
    event->registered = false;
    event->source = nullptr;
    event->new_buffer_queue.clear();
    event->condition.notify_all();
  }
  release_events(events);

  {
    std::scoped_lock lock(device->mutex);
    if (device->open_stream == stream) {
      device->open_stream = nullptr;
    }
    device->acquisition_active = false;
  }
  lifecycle_lock.unlock();
  delete stream;
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR close_device_handle(DevHandleData *device) {
  std::unique_lock configuration_lock(device->configuration_mutex);
  DsHandleData *stream = nullptr;
  {
    std::scoped_lock lock(device->mutex);
    stream = device->open_stream;
  }
  if (stream != nullptr) {
    const auto result = close_stream_handle_locked(stream);
    if (result != GenTL::GC_ERR_SUCCESS) return result;
  }

  std::string error_text;
  if (!Runtime::instance().close_device(device->device, &error_text))
    return io_error(error_text);

  std::vector<EventHandleData *> events;
  {
    std::scoped_lock lock(device->remote_port->mutex);
    events =
        retain_events_locked(device->remote_port->feature_invalidate_events);
    device->remote_port->feature_invalidate_events.clear();
  }
  for (auto *event : events) {
    std::scoped_lock lock(event->mutex);
    event->registered = false;
    event->source = nullptr;
    event->feature_invalidate_queue.clear();
    event->condition.notify_all();
  }
  release_events(events);

  Runtime::instance().release_device_open(device->device.id, device->access);
  auto *parent = device->parent;
  {
    std::scoped_lock lock(parent->mutex);
    parent->open_devices.erase(std::remove(parent->open_devices.begin(),
                                           parent->open_devices.end(), device),
                               parent->open_devices.end());
  }
  configuration_lock.unlock();
  delete device;
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR close_interface_handle(IfHandleData *iface) {
  std::vector<DevHandleData *> devices;
  {
    std::scoped_lock lock(iface->mutex);
    devices = iface->open_devices;
  }
  for (auto *device : devices) {
    const auto result = close_device_handle(device);
    if (result != GenTL::GC_ERR_SUCCESS) return result;
  }

  auto *parent = iface->parent;
  {
    std::scoped_lock lock(parent->mutex);
    if (parent->open_interface == iface) {
      parent->open_interface = nullptr;
    }
  }
  delete iface;
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR close_tl_handle(TlHandleData *tl) {
  IfHandleData *iface = nullptr;
  {
    std::scoped_lock lock(tl->mutex);
    iface = tl->open_interface;
  }
  if (iface != nullptr) {
    const auto result = close_interface_handle(iface);
    if (result != GenTL::GC_ERR_SUCCESS) return result;
  }
  delete tl;
  Runtime::instance().release_system_open();
  return GenTL::GC_ERR_SUCCESS;
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
    gentl_producer_core::runtime::Runtime::instance().register_event_handle(
        event);
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
    gentl_producer_core::runtime::Runtime::instance().register_event_handle(
        event);
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
            gentl_producer_core::runtime::retain_event(event);
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
            gentl_producer_core::runtime::retain_event(event);
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
  gentl_producer_core::runtime::release_events(removed);
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE EventGetData(EVENT_HANDLE hEvent,
                                                   void *pBuffer,
                                                   size_t *piSize,
                                                   uint64_t iTimeout) {
  GENTL_REQUIRE_INITIALIZED();
  auto event_reference = gentl_producer_core::runtime::acquire_event(hEvent);
  auto *event = event_reference.get();
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
      if (pBuffer != nullptr && *piSize < required_size) {
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
      // A null destination discards the event and releases the output buffer.
      if (pBuffer != nullptr)
        std::memcpy(pBuffer, &payload, required_size);
      *piSize = pBuffer != nullptr ? required_size : 0;
      return GC_ERR_SUCCESS;
    }

    if (event->event_type == EVENT_FEATURE_INVALIDATE) {
      const auto &payload = event->feature_invalidate_queue.front();
      const size_t required_size = payload.size() + 1U;
      if (pBuffer != nullptr && *piSize < required_size) {
        *piSize = required_size;
        return GC_ERR_BUFFER_TOO_SMALL;
      }
      if (pBuffer != nullptr)
        std::memcpy(pBuffer, payload.c_str(), required_size);
      event->feature_invalidate_queue.pop_front();
      *piSize = pBuffer != nullptr ? required_size : 0;
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
  auto event_reference = gentl_producer_core::runtime::acquire_event(hEvent);
  auto *event = event_reference.get();
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
  auto event_reference = gentl_producer_core::runtime::acquire_event(hEvent);
  auto *event = event_reference.get();
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
  auto event_reference = gentl_producer_core::runtime::acquire_event(hEvent);
  auto *event = event_reference.get();
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
  auto *event =
      gentl_producer_core::runtime::Runtime::instance().retire_event_handle(
          hEvent);
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
  return gentl_producer_core::runtime::close_tl_handle(tl);
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
    if (tl->open_interface != nullptr) {
      delete iface;
      return gentl_producer_core::runtime::resource_in_use(
          "Interface module is already open");
    }
    tl->open_interface = iface;
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
  return gentl_producer_core::runtime::close_interface_handle(iface);
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
      device,
      gentl_producer_core::runtime::Runtime::instance().device_access_status(
          device.id),
      iInfoCmd, piType, pBuffer, piSize);
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
  std::string error_text;
  if (!gentl_producer_core::runtime::Runtime::instance().open_device(
          &device, &error_text)) {
    gentl_producer_core::runtime::Runtime::instance().release_device_open(
        device.id, iOpenFlag);
    return gentl_producer_core::runtime::io_error(error_text);
  }
  *phDevice = new gentl_producer_core::runtime::DevHandleData(
      iface, std::move(device), iOpenFlag);
  {
    std::scoped_lock lock(iface->mutex);
    iface->open_devices.push_back(
        static_cast<gentl_producer_core::runtime::DevHandleData *>(*phDevice));
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
  return gentl_producer_core::runtime::close_device_handle(device);
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
  const auto native_count = gentl_producer_core::runtime::Runtime::instance()
                                .native_buffer_count(stream->parent->device);
  if (native_count != 0)
    return gentl_producer_core::runtime::not_implemented(
        "native DMA requires DSAllocAndAnnounceBuffer");
  if (reinterpret_cast<std::uintptr_t>(pBuffer) %
          stream->current_buffer_alignment !=
      0U) {
    return gentl_producer_core::runtime::invalid_buffer(
        "buffer base does not meet stream alignment");
  }
  if (iSize < stream->current_payload_size) {
    return gentl_producer_core::runtime::invalid_buffer(
        "buffer is smaller than the current stream payload");
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
  const auto native_count = gentl_producer_core::runtime::Runtime::instance()
                                .native_buffer_count(stream->parent->device);
  if (native_count != 0 && (stream->acquisition_session_active ||
                            stream->buffers.size() >= native_count))
    return gentl_producer_core::runtime::resource_in_use(
        "native DMA pool is active or fully announced");
  if (iSize > std::numeric_limits<std::size_t>::max() -
                  (stream->current_buffer_alignment - 1U)) {
    return gentl_producer_core::runtime::invalid_buffer(
        "aligned buffer allocation size overflows");
  }
  if (iSize < stream->current_payload_size) {
    return gentl_producer_core::runtime::invalid_buffer(
        "buffer is smaller than the current stream payload");
  }
  if (stream->buffer_allocation_mode ==
      gentl_producer_core::runtime::BufferAllocationMode::kConsumer) {
    return gentl_producer_core::runtime::resource_in_use(
        "consumer and producer buffer allocation cannot be mixed");
  }
  // Reserve handle storage before claiming a native address.
  if (native_count != 0)
    stream->buffers.reserve(native_count);
  auto buffer =
      std::make_unique<gentl_producer_core::runtime::BufferHandleData>(
          stream, nullptr, iSize, pPrivate, native_count == 0);
  if (native_count != 0) {
    std::string error;
    buffer->base = gentl_producer_core::runtime::Runtime::instance()
                       .allocate_native_buffer(stream->parent->device, iSize, &error);
    if (buffer->base == nullptr)
      return gentl_producer_core::runtime::invalid_buffer(error);
    buffer->producer_owned = true;
  }
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
    if (stream->stop_attempted && !stream->stop_result.buffers_released)
      return gentl_producer_core::runtime::busy(
          "stream buffers await backend retirement");
    if (!stream->sdk_queue.empty() && (iOperation == ACQ_QUEUE_ALL_TO_INPUT ||
                                       iOperation == ACQ_QUEUE_ALL_DISCARD)) {
      return gentl_producer_core::runtime::busy(
          "flush cannot reclaim backend-owned buffers during acquisition");
    }
    if ((iOperation == ACQ_QUEUE_ALL_TO_INPUT ||
         iOperation == ACQ_QUEUE_UNQUEUED_TO_INPUT) &&
        gentl_producer_core::runtime::Runtime::instance()
            .native_buffer_count(stream->parent->device) != 0)
      return gentl_producer_core::runtime::not_implemented(
          "native DMA slots must be returned individually with DSQueueBuffer");
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
        buffer->is_queued = true;
        buffer->is_sdk_queued = false;
        buffer->has_new_data = false;
        buffer->awaiting_delivery = false;
        buffer->is_incomplete = false;
        buffer->size_filled = 0;
        stream->input_queue.push_back(buffer.get());
      }
    } else if (iOperation == ACQ_QUEUE_UNQUEUED_TO_INPUT) {
      for (auto &buffer : stream->buffers) {
        if (buffer->is_queued || buffer->is_sdk_queued ||
            buffer->is_acquiring || buffer->awaiting_delivery)
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
        buffer->is_queued = false;
        buffer->is_sdk_queued = false;
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
    gentl_producer_core::runtime::notify_stream_work_locked(stream);
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
  std::scoped_lock lifecycle_lock(stream->lifecycle_mutex);
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
    const auto native_count = gentl_producer_core::runtime::Runtime::instance()
                                  .native_buffer_count(stream->parent->device);
    if (stream->buffers.size() < std::max<std::size_t>(1, native_count)) {
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
    const auto order = gentl_producer_core::runtime::Runtime::instance()
                           .completion_order(stream->parent->device);
    std::shared_ptr<gentl_producer_core::runtime::CompletionWakeup> wakeup;
    if (order == gentl_producer_core::BufferCompletionOrder::kAny) {
      wakeup = std::make_shared<gentl_producer_core::runtime::CompletionWakeup>();
      if (!gentl_producer_core::runtime::Runtime::instance().set_completion_callback(
              stream->parent->device, [wakeup]() noexcept { wakeup->notify(); }))
        wakeup.reset();
    }
    if (!gentl_producer_core::runtime::Runtime::instance().start_stream(
            stream->parent->device, &error_text)) {
      if (wakeup)
        gentl_producer_core::runtime::Runtime::instance().set_completion_callback(
            stream->parent->device, {});
      return gentl_producer_core::runtime::io_error(error_text);
    }
    gentl_producer_core::runtime::apply_stream_description_locked(stream,
                                                                  description);
    stream->stop_attempted = false;
    stream->stop_result = {false, false};
    stream->stop_error.clear();
    stream->stop_requested = false;
    stream->kill_requested = false;
    stream->worker_error.clear();
    stream->worker_running = true;
    stream->is_acquiring = true;
    stream->acquisition_session_active = true;
    stream->acquisition_limit = iNumToAcquire;
    stream->completion_order = order;
    stream->completion_wakeup = std::move(wakeup);
    stream->delivered_count = 0;
    stream->started_count = 0;
    {
      std::scoped_lock device_lock(stream->parent->mutex);
      stream->parent->acquisition_active = true;
    }
    const auto rollback_start = [&](const std::string &error) {
      auto &runtime = gentl_producer_core::runtime::Runtime::instance();
      stream->stop_result = runtime.stop_stream(stream->parent->device,
                                                &stream->stop_error);
      stream->stop_attempted = true;
      if (stream->stop_result.buffers_released) {
        gentl_producer_core::runtime::return_sdk_queue_to_input_locked(stream);
        if (stream->completion_wakeup)
          runtime.set_completion_callback(stream->parent->device, {});
        stream->completion_wakeup.reset();
      }
      const bool retired = stream->stop_result.success &&
                           stream->stop_result.buffers_released;
      stream->worker_running = false;
      stream->is_acquiring = false;
      stream->acquisition_session_active = !retired;
      stream->stop_requested = !retired;
      {
        std::scoped_lock device_lock(stream->parent->mutex);
        stream->parent->acquisition_active = !retired;
      }
      return gentl_producer_core::runtime::io_error(
          stream->stop_error.empty() ? error : stream->stop_error);
    };
    if (native_count != 0) {
      // Arm the initial contiguous prefix before the application can send
      // AcquisitionStart or a software trigger.
      while (!stream->input_queue.empty()) {
        if (!gentl_producer_core::runtime::submit_input_locked(stream))
          return rollback_start(stream->worker_error);
      }
    }
    try {
      stream->worker =
          std::thread(gentl_producer_core::runtime::stream_worker, stream);
    } catch (const std::system_error &error) {
      return rollback_start(error.what());
    }
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
  std::scoped_lock lifecycle_lock(stream->lifecycle_mutex);
  const bool cancel_wait = gentl_producer_core::runtime::Runtime::instance()
                               .cancel_wait_on_stop(stream->parent->device);
  gentl_producer_core::StopResult result;
  bool retry_completed_stop = false;
  {
    std::scoped_lock retirement_lock(stream->retirement_mutex);
    {
      std::scoped_lock lock(stream->mutex);
      if (!stream->acquisition_session_active)
        return gentl_producer_core::runtime::resource_in_use(
            "data stream acquisition was not started or was already stopped");
      retry_completed_stop = stream->stop_attempted && !stream->worker_running;
      stream->stop_requested = true;
      stream->kill_requested = iStopFlags == ACQ_STOP_FLAGS_KILL;
      gentl_producer_core::runtime::notify_stream_work_locked(stream);
    }
    if (iStopFlags == ACQ_STOP_FLAGS_KILL || cancel_wait)
      result = gentl_producer_core::runtime::retire_backend_locked(stream, true);
  }
  if (stream->worker.joinable()) stream->worker.join();
  if (iStopFlags == ACQ_STOP_FLAGS_DEFAULT && !cancel_wait) {
    // DEFAULT finishes the current buffer before retiring the SDK queue. A
    // failure first observed during this stop is returned, never auto-retried.
    std::scoped_lock retirement_lock(stream->retirement_mutex);
    result = gentl_producer_core::runtime::retire_backend_locked(
        stream, retry_completed_stop);
  }
  std::string error_text = stream->stop_error;
  if (!result.buffers_released)
    return gentl_producer_core::runtime::io_error(error_text.empty()
        ? "backend did not release stream buffers" : error_text);
  gentl_producer_core::runtime::clear_completion_callback(stream);
  bool stopped = result.success;

  gentl_producer_core::runtime::BufferHandleData *incomplete_buffer = nullptr;
  std::vector<gentl_producer_core::runtime::EventHandleData *> events;
  {
    std::scoped_lock lock(stream->mutex);
    if (!cancel_wait && stream->kill_requested && !stream->sdk_queue.empty() &&
        stream->completion_order == gentl_producer_core::BufferCompletionOrder::kFifo) {
      incomplete_buffer = stream->sdk_queue.front();
      stream->sdk_queue.pop_front();
      incomplete_buffer->is_sdk_queued = false;
      incomplete_buffer->is_acquiring = false;
      incomplete_buffer->is_queued = false;
      incomplete_buffer->has_new_data = true;
      incomplete_buffer->awaiting_delivery = true;
      incomplete_buffer->is_incomplete = true;
      incomplete_buffer->size_filled = 0;
      incomplete_buffer->timestamp = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      incomplete_buffer->frame_id = ++stream->frame_sequence;
      gentl_producer_core::runtime::snapshot_buffer_description_locked(
          stream, incomplete_buffer);
      ++stream->started_count;
      ++stream->delivered_count;
      events = gentl_producer_core::runtime::retain_events_locked(
          stream->new_buffer_events);
      for (auto *event : events)
        gentl_producer_core::runtime::publish_stream_buffer(event,
                                                            incomplete_buffer);
    }
    gentl_producer_core::runtime::return_sdk_queue_to_input_locked(stream);
    stream->worker_running = false;
    stream->is_acquiring = false;
    stream->acquisition_session_active = !result.success;
    stream->stop_requested = !result.success;
    stream->kill_requested = false;
    if (iStopFlags == ACQ_STOP_FLAGS_DEFAULT && !stream->worker_error.empty()) {
      stopped = false;
      error_text = stream->worker_error;
      // Report the fault once while leaving native consumers a successful
      // stop retry before they discard or revoke the DMA buffers.
      if (gentl_producer_core::runtime::Runtime::instance()
              .native_buffer_count(stream->parent->device) != 0) {
        stream->worker_error.clear();
        stream->acquisition_session_active = true;
      }
    }
  }
  gentl_producer_core::runtime::release_events(events);
  {
    std::scoped_lock lock(stream->parent->mutex);
    stream->parent->acquisition_active = !result.success;
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
      gentl_producer_core::runtime::queued_buffer_count_locked(stream);
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
        gentl_producer_core::runtime::Runtime::instance()
            .native_buffer_count(stream->parent->device) != 0,
        INFO_DATATYPE_BOOL8, piType, pBuffer, piSize);
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
        gentl_producer_core::runtime::buffer_announce_minimum_locked(stream),
        INFO_DATATYPE_SIZET, piType, pBuffer, piSize);
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
  std::scoped_lock configuration_lock(stream->parent->configuration_mutex);
  return gentl_producer_core::runtime::close_stream_handle_locked(stream);
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
  std::scoped_lock configuration_lock(stream->parent->configuration_mutex);
  std::scoped_lock lifecycle_lock(stream->lifecycle_mutex);
  std::scoped_lock lock(stream->mutex);
  auto it = std::find_if(
      stream->buffers.begin(), stream->buffers.end(),
      [buffer](const auto &entry) { return entry.get() == buffer; });
  if (it == stream->buffers.end()) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (buffer->is_queued || buffer->is_sdk_queued || buffer->is_acquiring ||
      buffer->awaiting_delivery) {
    return gentl_producer_core::runtime::busy(
        "cannot revoke a buffer owned by a stream queue");
  }
  std::string error;
  if (!gentl_producer_core::runtime::Runtime::instance().revoke_buffer(
          stream->parent->device, buffer->base, &error)) {
    return gentl_producer_core::runtime::busy(error.empty()
        ? "backend still retains the buffer" : error);
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
  if (stream->stop_requested ||
      (stream->stop_attempted && !stream->stop_result.buffers_released))
    return gentl_producer_core::runtime::resource_in_use(
        "stream buffers await backend retirement");
  if (buffer->is_queued || buffer->is_sdk_queued || buffer->is_acquiring ||
      buffer->awaiting_delivery) {
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
  gentl_producer_core::runtime::notify_stream_work_locked(stream);
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
  switch (iInfoCmd) {
  case BUFFER_INFO_DELIVERED_IMAGEHEIGHT:
    return gentl_producer_core::runtime::info_scalar<size_t>(
        buffer->size_filled != 0 ? buffer->height : 0, INFO_DATATYPE_SIZET,
        piType, pBuffer, piSize);
  case BUFFER_INFO_CHUNKLAYOUTID:
    return gentl_producer_core::runtime::info_scalar<uint64_t>(
        0, INFO_DATATYPE_UINT64, piType, pBuffer, piSize);
  case BUFFER_INFO_FILENAME:
    return gentl_producer_core::runtime::info_string("", piType, pBuffer,
                                                     piSize);
  case BUFFER_INFO_DATA_LARGER_THAN_BUFFER:
  case BUFFER_INFO_IS_COMPOSITE:
    return gentl_producer_core::runtime::info_scalar<bool8_t>(
        0, INFO_DATATYPE_BOOL8, piType, pBuffer, piSize);
  default:
    break;
  }
  const bool has_snapshot = buffer->has_new_data || buffer->is_incomplete;
  const gentl_producer_core::BufferInfo info{
      .base = buffer->base,
      .size = buffer->size,
      .user_ptr = buffer->private_data,
      .timestamp = buffer->timestamp,
      .new_data = buffer->has_new_data,
      .is_queued = buffer->is_queued || buffer->is_acquiring ||
                   buffer->awaiting_delivery || buffer->is_sdk_queued,
      .is_acquiring = buffer->is_acquiring,
      .is_incomplete = buffer->is_incomplete,
      .tl_type = gentl_producer_core::runtime::Runtime::instance()
                     .backend_info()
                     .tl_type,
      .size_filled = buffer->size_filled,
      .width = has_snapshot ? buffer->width : stream->current_width,
      .height = has_snapshot ? buffer->height : stream->current_height,
      .x_offset = has_snapshot ? buffer->x_offset : stream->current_x_offset,
      .y_offset = has_snapshot ? buffer->y_offset : stream->current_y_offset,
      .x_padding = has_snapshot ? buffer->x_padding : stream->current_x_padding,
      .y_padding = has_snapshot ? buffer->y_padding : stream->current_y_padding,
      .frame_id = buffer->frame_id,
      .image_present = buffer->size_filled != 0,
      .pixel_format =
          has_snapshot ? buffer->pixel_format : stream->current_pixel_format,
      .pixel_endianness = has_snapshot ? buffer->pixel_endianness
                                       : stream->current_pixel_endianness,
      .data_size = buffer->has_new_data ? buffer->payload_size : 0};
  return gentl_producer_core::info_buffer(info, iInfoCmd, piType, pBuffer,
                                          piSize);
}

#include "exports_common.inc"

#undef GENTL_REQUIRE_INITIALIZED

} // namespace GenTL
