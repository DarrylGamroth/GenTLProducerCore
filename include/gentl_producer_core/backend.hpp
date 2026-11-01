#pragma once
#include "gentl_producer_core/port_model.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace gentl_producer_core {
struct DeviceDescriptor {
  int index = 0;
  std::uint64_t native_device_handle = 0;
  std::string id;
  std::string vendor;
  std::string model;
  std::string serial_number;
  std::size_t width = 0;
  std::size_t height = 0;
  float sensor_pixel_width_um = 0.0F;
  float sensor_pixel_height_um = 0.0F;
  int bit_depth = 16;
  std::size_t payload_size_bytes = 0;
  std::uint64_t timestamp_frequency_hz = 1000000000ULL;

  bool operator==(const DeviceDescriptor &) const = default;
};

struct BackendInfo {
  std::string system_id;
  std::string system_vendor;
  std::string system_model;
  std::string system_display_name;
  std::string interface_id;
  std::string interface_display_name;
  std::string tl_type;
  std::uint32_t tl_specification_major = 1;
  std::uint32_t tl_specification_minor = 2;
};

struct StreamDescription {
  std::size_t buffer_alignment = 1;
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t payload_size_bytes = 0;
  std::uint32_t pixel_format = 0;
  std::uint32_t pixel_size_bits = 0;
  std::size_t offset_x = 0;
  std::size_t offset_y = 0;
  std::size_t x_padding_bytes = 0;
  std::size_t y_padding_bytes = 0;
  std::int32_t pixel_endianness = GenTL::PIXELENDIANNESS_LITTLE;
};

enum class AcquireFrameStatus {
  kSuccess,
  kTimeout,
  kStopped,
};

struct AcquireFrameResult {
  AcquireFrameStatus status = AcquireFrameStatus::kTimeout;
  void *buffer = nullptr;
  std::size_t bytes_filled = 0;
  std::uint64_t timestamp_ns = 0;
  bool incomplete = false;
  std::uint64_t skipped_frames = 0;
  // Copy acquisition sequence advance, independent from underrun accounting.
  std::uint64_t frame_id_increment = 1;
};

// Operational success and memory retirement are independent: a camera stop
// command can fail while a successful flush still returns every buffer.
struct StopResult {
  bool success = false;
  bool buffers_released = false;
};

class ProducerBackend {
public:
  virtual ~ProducerBackend() = default;
  virtual const BackendInfo &info() const = 0;
  virtual bool initialize(std::string *error_text) = 0;
  virtual std::vector<DeviceDescriptor>
  enumerate_devices(std::string *error_text) = 0;
  virtual std::unique_ptr<PortModel>
  make_remote_port_model(const DeviceDescriptor &device) = 0;
  virtual bool get_stream_description(const DeviceDescriptor &device,
                                      StreamDescription *description,
                                      std::string *error_text) = 0;
  virtual bool start_stream(const DeviceDescriptor &device,
                            std::string *error_text) = 0;

  virtual bool remote_write_requires_idle(std::uint64_t address,
                                          std::size_t size) const = 0;
  // Return true when an idle remote write can change the stream description.
  // The runtime refreshes it after the write while serializing host lifecycle
  // operations. The default avoids extra backend queries for fixed geometry.
  virtual bool remote_write_changes_stream_description(std::uint64_t,
                                                       std::size_t) const {
    return false;
  }
  virtual std::vector<std::string>
  remote_invalidated_features(std::uint64_t address,
                              std::size_t size) const = 0;
  virtual std::vector<std::string> acquisition_invalidated_features() const = 0;
};
// Single pull worker, caller-selected storage. acquire_frame must not retain
// that storage after returning. Success writes at most buffer_size bytes;
// timeout/stopped returns no frame. stop_stream may run concurrently and must
// synchronize with the in-flight acquisition before ownership is reclaimed.
class CopyBackend : public ProducerBackend {
public:
  virtual bool stop_stream(const DeviceDescriptor &device,
                           std::string *error_text) = 0;
  virtual bool acquire_frame(const DeviceDescriptor &device,
                             std::uint64_t timeout_ms, void *buffer,
                             std::size_t buffer_size,
                             AcquireFrameResult *result,
                             std::string *error_text) = 0;
};
// Accepted buffers are borrowed until FIFO completion or successful stop.
// queue_buffer receives allocation and transfer lengths separately. wait_frame
// must return the oldest accepted address; out-of-order completion is not
// supported. stop_stream may run concurrently with wait_frame. Only an explicit
// buffers_released result permits reclamation; false must preserve ownership
// and allow a retry. Operational errors do not imply retirement. A kStopped
// wait result alone is not proof of retirement; the runtime must confirm stop.
class QueuedBackend : public ProducerBackend {
public:
  virtual StopResult stop_stream(const DeviceDescriptor &device,
                                 std::string *error_text) = 0;
  virtual bool open_device(DeviceDescriptor *device,
                           std::string *error_text) = 0;
  virtual bool close_device(const DeviceDescriptor &device,
                            std::string *error_text) = 0;
  virtual bool queue_buffer(const DeviceDescriptor &device, void *buffer,
                            std::size_t allocation_size,
                            std::size_t transfer_size,
                            std::string *error_text) = 0;
  virtual bool wait_frame(const DeviceDescriptor &device,
                          std::uint64_t timeout_ms, AcquireFrameResult *result,
                          std::string *error_text) = 0;
};
} // namespace gentl_producer_core
