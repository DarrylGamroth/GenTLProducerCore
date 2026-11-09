#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "GenTL.h"

#include "gentl_producer_core/producer.hpp"
#include "gentl_producer_core/detail/module_ports.hpp"
#include "gentl_producer_core/info.hpp"

namespace gentl_producer_core::runtime {

enum class HandleKind {
  kTl,
  kIf,
  kDev,
  kDs,
  kPort,
  kBuffer,
  kEvent,
};

struct BaseHandle {
  explicit BaseHandle(HandleKind in_kind) : kind(in_kind) {}
  virtual ~BaseHandle() = default;
  HandleKind kind;
};

struct TlHandleData final : BaseHandle {
  TlHandleData();
  std::mutex mutex;
  struct IfHandleData *open_interface = nullptr;
  std::uint64_t interface_selector = 0;
  std::unique_ptr<PortModel> port_model;
};

struct IfHandleData final : BaseHandle {
  IfHandleData(TlHandleData *in_parent, std::string in_id);
  TlHandleData *parent = nullptr;
  std::string id;
  std::mutex mutex;
  std::vector<struct DevHandleData *> open_devices;
  std::uint64_t device_selector = 0;
  std::unique_ptr<PortModel> port_model;
};

struct DevHandleData;

enum class PortKind {
  kRemoteDevice,
};

struct PortHandleData final : BaseHandle {
  PortHandleData(PortKind in_port_kind, DevHandleData *in_owner);
  PortKind port_kind;
  DevHandleData *owner = nullptr;
  std::string id;
  std::mutex mutex;
  std::vector<struct EventHandleData *> feature_invalidate_events;
  std::unique_ptr<PortModel> port_model;
};

struct DevHandleData final : BaseHandle {
  DevHandleData(IfHandleData *in_parent, DeviceDescriptor in_device,
                GenTL::DEVICE_ACCESS_FLAGS in_access);
  IfHandleData *parent = nullptr;
  DeviceDescriptor device;
  GenTL::DEVICE_ACCESS_FLAGS access = GenTL::DEVICE_ACCESS_NONE;
  std::mutex mutex;
  std::mutex configuration_mutex;
  struct DsHandleData *open_stream = nullptr;
  bool acquisition_active = false;
  std::uint64_t stream_selector = 0;
  std::unique_ptr<PortHandleData> remote_port;
  std::unique_ptr<PortModel> port_model;
};

struct EventHandleData;

enum class BufferAllocationMode {
  kUnset,
  kConsumer,
  kProducer,
};

// Captured by the backend callback without retaining or dereferencing a stream.
// A snapshot before readiness checks plus atomic wait prevents lost wakes.
struct CompletionWakeup {
  std::atomic<std::uint64_t> generation{0};
  void notify() noexcept {
    generation.fetch_add(1, std::memory_order_release);
    generation.notify_one();
  }
};

struct DsHandleData final : BaseHandle {
  explicit DsHandleData(DevHandleData *in_parent);
  DevHandleData *parent = nullptr;
  std::string id;
  bool is_acquiring = false;
  std::uint64_t delivered_count = 0;
  std::uint64_t started_count = 0;
  std::uint64_t underrun_count = 0;
  std::uint64_t frame_sequence = 0;
  std::uint64_t acquisition_limit = 0;
  bool acquisition_session_active = false;
  BufferAllocationMode buffer_allocation_mode = BufferAllocationMode::kUnset;
  BufferCompletionOrder completion_order = BufferCompletionOrder::kFifo;
  std::vector<std::unique_ptr<struct BufferHandleData>> buffers;
  std::deque<struct BufferHandleData *> input_queue;
  std::deque<struct BufferHandleData *> sdk_queue;
  std::unique_ptr<PortModel> port_model;
  std::mutex mutex;
  std::condition_variable work_available;
  std::shared_ptr<CompletionWakeup> completion_wakeup;
  std::thread worker;
  // Host lifecycle operations serialize joins; retirement serializes backend stops.
  std::mutex lifecycle_mutex;
  std::mutex retirement_mutex;
  bool stop_attempted = false;
  StopResult stop_result{true, true};
  std::string stop_error;
  bool stop_requested = false;
  bool kill_requested = false;
  bool worker_running = false;
  std::size_t current_width = 0;
  std::size_t current_height = 0;
  std::size_t current_x_offset = 0;
  std::size_t current_y_offset = 0;
  std::size_t current_x_padding = 0;
  std::size_t current_y_padding = 0;
  std::size_t current_payload_size = 0;
  std::size_t current_buffer_alignment = 1;
  std::uint32_t current_pixel_format = 0;
  std::uint32_t current_pixel_size_bits = 0;
  std::int32_t current_pixel_endianness = GenTL::PIXELENDIANNESS_LITTLE;
  std::string worker_error;
  std::vector<EventHandleData *> new_buffer_events;
};

struct BufferHandleData final : BaseHandle {
  BufferHandleData(DsHandleData *in_parent, void *in_base, std::size_t in_size,
                   void *in_private, bool in_producer_owned);
  DsHandleData *parent = nullptr;
  std::unique_ptr<std::byte[]> owned_storage;
  void *base = nullptr;
  std::size_t size = 0;
  void *private_data = nullptr;
  bool producer_owned = false;
  bool is_queued = false;
  bool is_sdk_queued = false;
  bool has_new_data = false;
  bool awaiting_delivery = false;
  bool is_acquiring = false;
  bool is_incomplete = false;
  std::size_t size_filled = 0;
  std::size_t payload_size = 0;
  std::uint64_t frame_id = 0;
  std::uint64_t timestamp = 0;
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t x_offset = 0;
  std::size_t y_offset = 0;
  std::size_t x_padding = 0;
  std::size_t y_padding = 0;
  std::uint64_t pixel_format = 0;
  std::uint32_t pixel_size_bits = 0;
  std::int32_t pixel_endianness = GenTL::PIXELENDIANNESS_LITTLE;
};

struct EventHandleData final : BaseHandle {
  EventHandleData(BaseHandle *in_source, GenTL::EVENT_TYPE in_event_type);
  BaseHandle *source = nullptr;
  GenTL::EVENT_TYPE event_type = GenTL::EVENT_ERROR;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<GenTL::EVENT_NEW_BUFFER_DATA> new_buffer_queue;
  std::deque<std::string> feature_invalidate_queue;
  std::uint64_t fired_count = 0;
  bool registered = true;
  std::atomic<std::uint32_t> references{1};
};

class Runtime {
public:
  using DeviceList = std::vector<DeviceDescriptor>;

  static Runtime &instance();

  GenTL::GC_ERROR init();
  GenTL::GC_ERROR close();
  bool is_initialized() const;
  bool claim_system_open();
  void release_system_open();

  void set_last_error(GenTL::GC_ERROR code, std::string text);
  std::pair<GenTL::GC_ERROR, std::string> last_error() const;
  const BackendInfo &backend_info() const;
  std::unique_ptr<PortModel>
  make_remote_port_model(const DeviceDescriptor &device);
  bool open_device(DeviceDescriptor *device, std::string *error_text);
  bool close_device(const DeviceDescriptor &device, std::string *error_text);
  bool get_stream_description(const DeviceDescriptor &device,
                              StreamDescription *description,
                              std::string *error_text);
  bool start_stream(const DeviceDescriptor &device, std::string *error_text);
  StopResult stop_stream(const DeviceDescriptor &device, std::string *error_text);
  bool cancel_wait_on_stop(const DeviceDescriptor &device) const {
    return backend_->cancel_wait_on_stop(device);
  }
  std::size_t native_buffer_count(const DeviceDescriptor &device) const {
    return backend_->native_buffer_count(device);
  }
  void *allocate_native_buffer(const DeviceDescriptor &device, std::size_t size,
                               std::string *error) {
    return backend_->allocate_native_buffer(device, size, error);
  }
  bool release_native_buffers(const DeviceDescriptor &device, std::string *error) {
    return backend_->release_native_buffers(device, error);
  }
  BufferCompletionOrder completion_order(const DeviceDescriptor &device) const {
    return backend_->completion_order(device);
  }
  bool set_completion_callback(const DeviceDescriptor &device,
                               std::function<void()> callback) {
    return backend_->set_completion_callback(device, std::move(callback));
  }
  bool queue_buffer(const DeviceDescriptor &device, void *buffer,
                    std::size_t allocation_size, std::size_t transfer_size,
                    std::string *error_text);
  bool revoke_buffer(const DeviceDescriptor &device, void *buffer,
                     std::string *error_text);
  bool wait_frame(const DeviceDescriptor &device, std::uint64_t timeout_ms,
                  AcquireFrameResult *result, std::string *error_text);

  bool remote_write_requires_idle(std::uint64_t address, std::size_t size) const {
    return backend_->remote_write_requires_idle(address, size);
  }
  bool remote_write_changes_stream_description(std::uint64_t address,
                                               std::size_t size) const {
    return backend_->remote_write_changes_stream_description(address, size);
  }
  std::vector<std::string> remote_invalidated_features(
      std::uint64_t address, std::size_t size) const {
    return backend_->remote_invalidated_features(address, size);
  }
  std::vector<std::string> acquisition_invalidated_features() const {
    return backend_->acquisition_invalidated_features();
  }

  GenTL::GC_ERROR refresh_cameras(bool8_t *changed);
  DeviceList snapshot_cameras(GenTL::GC_ERROR *error_code);
  std::optional<DeviceDescriptor>
  find_camera_by_id(std::string_view device_id, GenTL::GC_ERROR *error_code);
  bool claim_device_open(std::string_view device_id,
                         GenTL::DEVICE_ACCESS_FLAGS access);
  void release_device_open(std::string_view device_id,
                           GenTL::DEVICE_ACCESS_FLAGS access);
  GenTL::DEVICE_ACCESS_STATUS
  device_access_status(std::string_view device_id) const;
  void register_event_handle(EventHandleData *event);
  EventHandleData *acquire_event_handle(GenTL::EVENT_HANDLE handle);
  EventHandleData *retire_event_handle(GenTL::EVENT_HANDLE handle);

  static TlHandleData *as_tl(GenTL::TL_HANDLE handle);
  static IfHandleData *as_if(GenTL::IF_HANDLE handle);
  static DevHandleData *as_dev(GenTL::DEV_HANDLE handle);
  static DsHandleData *as_ds(GenTL::DS_HANDLE handle);
  static BufferHandleData *as_buffer(GenTL::BUFFER_HANDLE handle);
  static PortHandleData *as_port(GenTL::PORT_HANDLE handle);
  static EventHandleData *as_event(GenTL::EVENT_HANDLE handle);

private:
  Runtime();
  GenTL::GC_ERROR refresh_cameras_locked(bool8_t *changed);

  template <typename T>
  static T *checked_cast(void *raw_handle, HandleKind expected_kind) {
    if (raw_handle == nullptr) {
      return nullptr;
    }
    auto *base = static_cast<BaseHandle *>(raw_handle);
    if (base->kind != expected_kind) {
      return nullptr;
    }
    return static_cast<T *>(base);
  }

  mutable std::mutex mutex_;
  bool initialized_ = false;
  bool system_open_ = false;
  std::unique_ptr<RuntimeBackend> backend_;
  DeviceList cached_cameras_;
  bool camera_list_valid_ = false;
  struct DeviceOpenState {
    std::size_t readonly_count = 0;
    bool control_open = false;
    bool exclusive_open = false;
  };
  std::unordered_map<std::string, DeviceOpenState> device_opens_;
  std::mutex event_registry_mutex_;
  std::unordered_set<EventHandleData *> live_events_;
};

bool retain_event(EventHandleData *event);
void release_event(EventHandleData *event);

using gentl_producer_core::copy_scalar;
using gentl_producer_core::copy_string;
using gentl_producer_core::info_scalar;
using gentl_producer_core::info_string;

PortModel *port_model_from_handle(void *raw_handle);
const PortModel *port_model_from_handle(const void *raw_handle);

} // namespace gentl_producer_core::runtime
