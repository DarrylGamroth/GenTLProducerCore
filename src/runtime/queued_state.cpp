#include "gentl_producer_core/detail/runtime.hpp"

#include <cstring>
#include <ranges>

#include "gentl_producer_core/error.hpp"

#include "gentl_producer_core/detail/module_ports.hpp"

namespace gentl_producer_core::runtime {
TlHandleData::TlHandleData()
    : BaseHandle(HandleKind::kTl),
      port_model(make_tl_port_model(Runtime::instance().backend_info(), this)) {
}

IfHandleData::IfHandleData(TlHandleData *in_parent, std::string in_id)
    : BaseHandle(HandleKind::kIf), parent(in_parent), id(std::move(in_id)),
      port_model(make_if_port_model(Runtime::instance().backend_info(), this)) {
}

DevHandleData::DevHandleData(IfHandleData *in_parent,
                             DeviceDescriptor in_device,
                             GenTL::DEVICE_ACCESS_FLAGS in_access)
    : BaseHandle(HandleKind::kDev), parent(in_parent),
      device(std::move(in_device)), access(in_access),
      remote_port(
          std::make_unique<PortHandleData>(PortKind::kRemoteDevice, this)),
      port_model(
          make_dev_port_model(Runtime::instance().backend_info(), this)) {}

DsHandleData::DsHandleData(DevHandleData *in_parent)
    : BaseHandle(HandleKind::kDs), parent(in_parent),
      id(in_parent->device.id + "::stream0"),
      port_model(make_ds_port_model(Runtime::instance().backend_info(), this)),
      current_width(in_parent->device.width),
      current_height(in_parent->device.height),
      current_payload_size(in_parent->device.payload_size_bytes) {}

PortHandleData::PortHandleData(PortKind in_port_kind, DevHandleData *in_owner)
    : BaseHandle(HandleKind::kPort), port_kind(in_port_kind), owner(in_owner) {
  id = owner->device.id + "::remote";
  port_model = Runtime::instance().make_remote_port_model(owner->device);
}

EventHandleData::EventHandleData(BaseHandle *in_source,
                                 GenTL::EVENT_TYPE in_event_type)
    : BaseHandle(HandleKind::kEvent), source(in_source),
      event_type(in_event_type) {}

Runtime &Runtime::instance() {
  static Runtime runtime;
  return runtime;
}

Runtime::Runtime() : backend_(producer_definition().make_backend()) {}















const BackendInfo &Runtime::backend_info() const { return backend_->info(); }



bool Runtime::open_device(DeviceDescriptor *device, std::string *error_text) {
  return backend_->open_device(device, error_text);
}

bool Runtime::close_device(const DeviceDescriptor &device,
                           std::string *error_text) {
  return backend_->close_device(device, error_text);
}







bool Runtime::queue_buffer(const DeviceDescriptor &device, void *buffer,
                           std::size_t allocation_size,
                           std::size_t transfer_size, std::string *error_text) {
  return backend_->queue_buffer(device, buffer, allocation_size, transfer_size,
                                error_text);
}

bool Runtime::wait_frame(const DeviceDescriptor &device,
                         std::uint64_t timeout_ms, AcquireFrameResult *result,
                         std::string *error_text) {
  return backend_->wait_frame(device, timeout_ms, result, error_text);
}

GenTL::GC_ERROR Runtime::refresh_cameras_locked(bool8_t *changed) {
  std::string error_text;
  auto devices = backend_->enumerate_devices(&error_text);
  if (!error_text.empty()) {
    set_last_error(GenTL::GC_ERR_IO, error_text);
    return GenTL::GC_ERR_IO;
  }

  const auto same_topology = [](const DeviceList &left,
                                const DeviceList &right) {
    if (left.size() != right.size())
      return false;
    return std::ranges::equal(left, right, {}, &DeviceDescriptor::id,
                              &DeviceDescriptor::id);
  };
  if (changed != nullptr)
    *changed = same_topology(devices, cached_cameras_) ? 0 : 1;
  cached_cameras_ = std::move(devices);
  camera_list_valid_ = true;
  return GenTL::GC_ERR_SUCCESS;
}











GenTL::DEVICE_ACCESS_STATUS
Runtime::device_access_status(std::string_view device_id) const {
  std::scoped_lock lock(mutex_);
  const auto found = device_opens_.find(std::string(device_id));
  if (found == device_opens_.end()) {
    return GenTL::DEVICE_ACCESS_STATUS_READWRITE;
  }
  const auto &state = found->second;
  if (state.readonly_count != 0) {
    return GenTL::DEVICE_ACCESS_STATUS_OPEN_READONLY;
  }
  if (state.control_open || state.exclusive_open) {
    return GenTL::DEVICE_ACCESS_STATUS_OPEN_READWRITE;
  }
  return GenTL::DEVICE_ACCESS_STATUS_UNKNOWN;
}

void Runtime::register_event_handle(EventHandleData *event) {
  std::scoped_lock lock(event_registry_mutex_);
  live_events_.insert(event);
}

EventHandleData *Runtime::acquire_event_handle(GenTL::EVENT_HANDLE handle) {
  auto *event = static_cast<EventHandleData *>(handle);
  std::scoped_lock lock(event_registry_mutex_);
  if (event == nullptr || !live_events_.contains(event)) {
    return nullptr;
  }
  retain_event(event);
  return event;
}

EventHandleData *Runtime::retire_event_handle(GenTL::EVENT_HANDLE handle) {
  auto *event = static_cast<EventHandleData *>(handle);
  std::scoped_lock lock(event_registry_mutex_);
  if (event == nullptr || live_events_.erase(event) == 0U) {
    return nullptr;
  }
  return event;
}

bool retain_event(EventHandleData *event) {
  if (event == nullptr)
    return false;
  event->references.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void release_event(EventHandleData *event) {
  if (event != nullptr &&
      event->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete event;
  }
}

TlHandleData *Runtime::as_tl(GenTL::TL_HANDLE handle) {
  return checked_cast<TlHandleData>(handle, HandleKind::kTl);
}

IfHandleData *Runtime::as_if(GenTL::IF_HANDLE handle) {
  return checked_cast<IfHandleData>(handle, HandleKind::kIf);
}

DevHandleData *Runtime::as_dev(GenTL::DEV_HANDLE handle) {
  return checked_cast<DevHandleData>(handle, HandleKind::kDev);
}

DsHandleData *Runtime::as_ds(GenTL::DS_HANDLE handle) {
  return checked_cast<DsHandleData>(handle, HandleKind::kDs);
}

BufferHandleData *Runtime::as_buffer(GenTL::BUFFER_HANDLE handle) {
  return checked_cast<BufferHandleData>(handle, HandleKind::kBuffer);
}

PortHandleData *Runtime::as_port(GenTL::PORT_HANDLE handle) {
  return checked_cast<PortHandleData>(handle, HandleKind::kPort);
}

EventHandleData *Runtime::as_event(GenTL::EVENT_HANDLE handle) {
  return checked_cast<EventHandleData>(handle, HandleKind::kEvent);
}

PortModel *port_model_from_handle(void *raw_handle) {
  if (raw_handle == nullptr) {
    return nullptr;
  }

  auto *base = static_cast<BaseHandle *>(raw_handle);
  switch (base->kind) {
  case HandleKind::kTl:
    return static_cast<TlHandleData *>(raw_handle)->port_model.get();
  case HandleKind::kIf:
    return static_cast<IfHandleData *>(raw_handle)->port_model.get();
  case HandleKind::kDev:
    return static_cast<DevHandleData *>(raw_handle)->port_model.get();
  case HandleKind::kDs:
    return static_cast<DsHandleData *>(raw_handle)->port_model.get();
  case HandleKind::kPort:
    return static_cast<PortHandleData *>(raw_handle)->port_model.get();
  case HandleKind::kEvent:
    return nullptr;
  case HandleKind::kBuffer:
    return nullptr;
  }
  return nullptr;
}

const PortModel *port_model_from_handle(const void *raw_handle) {
  return port_model_from_handle(const_cast<void *>(raw_handle));
}

BufferHandleData::BufferHandleData(DsHandleData *in_parent, void *in_base,
                                   std::size_t in_size, void *in_private,
                                   bool in_producer_owned)
    : BaseHandle(HandleKind::kBuffer), parent(in_parent), size(in_size),
      private_data(in_private), producer_owned(in_producer_owned) {
  if (producer_owned) {
    const auto alignment = parent->current_buffer_alignment;
    auto storage_size = size + (alignment - 1U);
    owned_storage = std::make_unique<std::byte[]>(storage_size);
    base = owned_storage.get();
    std::align(alignment, size, base, storage_size);
  } else {
    base = in_base;
  }
}

StopResult Runtime::stop_stream(const DeviceDescriptor &device,
                          std::string *error_text) {
  return backend_->stop_stream(device, error_text);
}

#include "state_common.inc"

} // namespace gentl_producer_core::runtime
