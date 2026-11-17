#include "gentl_producer_core/detail/runtime.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

#if defined(GENTL_RUNTIME_COPY)
namespace GenTL {
extern "C" void GC_CALLTYPE GenTLTestFailThreadStart();
}
#endif

namespace gentl_producer_core::runtime {
namespace {

struct Configuration {
  std::mutex mutex;
  std::condition_variable condition;
  std::size_t width = 4;
  std::size_t height = 4;
  std::size_t descriptions = 0;
  std::size_t bytes_to_deliver = 0;
  std::uint64_t frame_id_increment = 1;
  bool fail_description = false;
  bool block_description = false;
  bool description_entered = false;
  bool release_description = false;
  bool acquiring = false;
  bool deliver = false;
  bool fail_stop = false;
  bool fail_acquire = false;
  bool acquire_failed = false;
  std::size_t native_buffers = 0;
  std::deque<void *> borrowed;
} configuration;

class ConfigurationBackend final : public RuntimeBackend {
public:
  const BackendInfo &info() const override {
    static const BackendInfo value = [] {
      BackendInfo result;
      result.system_id = "test-system";
      result.interface_id = "test-interface";
      return result;
    }();
    return value;
  }
  bool initialize(std::string *) override { return true; }
  std::vector<DeviceDescriptor> enumerate_devices(std::string *) override {
    DeviceDescriptor device;
    device.id = "test-device";
    device.width = 4;
    device.height = 4;
    device.payload_size_bytes = 16;
    return {device};
  }
  std::unique_ptr<PortModel>
  make_remote_port_model(const DeviceDescriptor &) override {
    auto write = [](std::uint64_t address) {
      return [address](std::uint64_t value) {
        std::scoped_lock lock(configuration.mutex);
        if (address == 0 || address == 8)
          configuration.width = static_cast<std::size_t>(value);
        if (address == 32)
          configuration.height = static_cast<std::size_t>(value);
        return GenTL::GC_ERR_SUCCESS;
      };
    };
    return std::make_unique<PortModel>(
        PortDescriptor{}, std::vector<PortUrlInfo>{},
        std::vector<PortModel::RegisterRegion>{
            integer_register(0, {}, write(0)),
            integer_register(8, {}, write(8)),
            integer_register(16, {}, write(16)),
            integer_register(24, {}, write(24)),
            integer_register(32, {}, write(32))});
  }
  bool get_stream_description(const DeviceDescriptor &,
                              StreamDescription *value,
                              std::string *error) override {
    std::unique_lock lock(configuration.mutex);
    ++configuration.descriptions;
    if (configuration.fail_description) {
      *error = "injected description failure";
      return false;
    }
    *value = {.width = configuration.width,
              .height = configuration.height,
              .payload_size_bytes = configuration.width * configuration.height,
              .pixel_format = 0x01080001,
              .pixel_size_bits = 8,
              .x_padding_bytes = configuration.width == 8 ? 2U : 0U,
              .pixel_endianness = GenTL::PIXELENDIANNESS_UNKNOWN};
    if (configuration.block_description) {
      configuration.description_entered = true;
      configuration.condition.notify_all();
      configuration.condition.wait(
          lock, [] { return configuration.release_description; });
    }
    return true;
  }
  bool start_stream(const DeviceDescriptor &, std::string *) override {
    std::scoped_lock lock(configuration.mutex);
    configuration.acquiring = true;
    return true;
  }
#if defined(GENTL_RUNTIME_COPY)
  bool stop_stream(const DeviceDescriptor &, std::string *error) override
#else
  StopResult stop_stream(const DeviceDescriptor &, std::string *) override
#endif
  {
    std::scoped_lock lock(configuration.mutex);
#if defined(GENTL_RUNTIME_COPY)
    if (configuration.fail_stop) {
      *error = "injected stop failure";
      return false;
    }
#endif
    configuration.acquiring = false;
    configuration.borrowed.clear();
    configuration.condition.notify_all();
#if defined(GENTL_RUNTIME_COPY)
    return true;
#else
    return {true, true};
#endif
  }
#if defined(GENTL_RUNTIME_QUEUED)
  std::size_t native_buffer_count(const DeviceDescriptor &) const override {
    return configuration.native_buffers;
  }
#endif
  bool remote_write_requires_idle(std::uint64_t address,
                                  std::size_t) const override {
    return address == 0 || address == 8 || address == 32;
  }
  bool remote_write_changes_stream_description(std::uint64_t address,
                                               std::size_t) const override {
    return address != 16;
  }
  std::vector<std::string>
  remote_invalidated_features(std::uint64_t, std::size_t) const override {
    return {};
  }
  std::vector<std::string> acquisition_invalidated_features() const override {
    return {};
  }
#if defined(GENTL_RUNTIME_QUEUED)
  bool open_device(DeviceDescriptor *, std::string *) override { return true; }
  bool close_device(const DeviceDescriptor &, std::string *) override {
    return true;
  }
  bool queue_buffer(const DeviceDescriptor &, void *buffer, std::size_t,
                    std::size_t, std::string *) override {
    std::scoped_lock lock(configuration.mutex);
    configuration.borrowed.push_back(buffer);
    configuration.condition.notify_all();
    return true;
  }
#endif
#if defined(GENTL_RUNTIME_COPY)
  bool acquire_frame(const DeviceDescriptor &, std::uint64_t, void *buffer,
                     std::size_t size, AcquireFrameResult *result,
                     std::string *error) override
#else
  bool wait_frame(const DeviceDescriptor &, std::uint64_t,
                  AcquireFrameResult *result, std::string *) override
#endif
  {
    std::unique_lock lock(configuration.mutex);
#if defined(GENTL_RUNTIME_COPY)
    if (configuration.fail_acquire) {
      *error = "injected acquire failure";
      configuration.acquire_failed = true;
      configuration.condition.notify_all();
      return false;
    }
#endif
    configuration.condition.wait(
        lock, [] { return !configuration.acquiring || configuration.deliver; });
    if (!configuration.acquiring) {
      result->status = AcquireFrameStatus::kStopped;
      return true;
    }
    configuration.deliver = false;
#if defined(GENTL_RUNTIME_QUEUED)
    assert(!configuration.borrowed.empty());
    void *buffer = configuration.borrowed.front();
    configuration.borrowed.pop_front();
#else
    assert(configuration.bytes_to_deliver <= size);
#endif
    std::memset(buffer, 0x5a, configuration.bytes_to_deliver);
    *result = {.status = AcquireFrameStatus::kSuccess,
               .buffer = buffer,
               .bytes_filled = configuration.bytes_to_deliver,
               .incomplete = configuration.bytes_to_deliver <
                             configuration.width * configuration.height,
               .frame_id_increment = configuration.frame_id_increment};
    return true;
  }
};

std::unique_ptr<RuntimeBackend> make_backend() {
  return std::make_unique<ConfigurationBackend>();
}

} // namespace
const ProducerDefinition<RuntimeBackend> &producer_definition() {
  static const ProducerDefinition<RuntimeBackend> value{
      .version = "test",
      .cti_filename = "test.cti",
      .tl = {"tl.xml", "test"},
      .interface = {"interface.xml", "test"},
      .device = {"device.xml", "test"},
      .stream = {"stream.xml", "test"},
      .make_backend = make_backend};
  return value;
}
} // namespace gentl_producer_core::runtime

namespace {
using namespace GenTL;
using namespace gentl_producer_core::runtime;

template <typename T>
T buffer_value(DS_HANDLE stream, BUFFER_HANDLE buffer,
               BUFFER_INFO_CMD command) {
  T value{};
  std::size_t size = sizeof(value);
  INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
  assert(DSGetBufferInfo(stream, buffer, command, &type, &value, &size) ==
         GC_ERR_SUCCESS);
  return value;
}

GC_ERROR write(PORT_HANDLE remote, std::uint64_t address, std::uint64_t value) {
  std::size_t size = sizeof(value);
  return GCWritePort(remote, address, &value, &size);
}

void expect_payload(DS_HANDLE stream, std::size_t expected) {
  INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
  std::size_t value = 0;
  std::size_t size = sizeof(value);
  assert(DSGetInfo(stream, STREAM_INFO_PAYLOAD_SIZE, &type, &value, &size) ==
         GC_ERR_SUCCESS);
  assert(value == expected);
  std::uint64_t xml_value = 0;
  size = sizeof(xml_value);
  assert(GCReadPort(stream, 0xc8, &xml_value, &size) == GC_ERR_SUCCESS);
  assert(xml_value == expected);
}

void deliver(EVENT_HANDLE event, BUFFER_HANDLE expected, std::size_t bytes) {
  {
    std::scoped_lock lock(configuration.mutex);
    configuration.bytes_to_deliver = bytes;
    configuration.deliver = true;
    configuration.condition.notify_all();
  }
  EVENT_NEW_BUFFER_DATA data{};
  std::size_t size = sizeof(data);
  assert(EventGetData(event, &data, &size, 2000) == GC_ERR_SUCCESS);
  assert(data.BufferHandle == expected);
}
} // namespace

int main() {
  using namespace GenTL;
  using namespace gentl_producer_core::runtime;
  assert(GCInitLib() == GC_ERR_SUCCESS);
  TL_HANDLE tl = nullptr;
  IF_HANDLE interface = nullptr;
  DEV_HANDLE device = nullptr;
  PORT_HANDLE remote = nullptr;
  DS_HANDLE stream = nullptr;
  assert(TLOpen(&tl) == GC_ERR_SUCCESS);
  assert(TLOpenInterface(tl, "test-interface", &interface) == GC_ERR_SUCCESS);
  assert(IFOpenDevice(interface, "test-device", DEVICE_ACCESS_CONTROL,
                      &device) == GC_ERR_SUCCESS);
  assert(DevGetPort(device, &remote) == GC_ERR_SUCCESS);

  // Failed opening must leave no stream reservation and no published handle.
  configuration.fail_description = true;
  assert(DevOpenDataStream(device, "test-device::stream0", &stream) ==
         GC_ERR_IO);
  assert(stream == nullptr);
  configuration.fail_description = false;
  assert(DevOpenDataStream(device, "test-device::stream0", &stream) ==
         GC_ERR_SUCCESS);
  expect_payload(stream, 16);
  configuration.fail_description = true;
  assert(write(remote, 0, 4) == GC_ERR_IO);
  expect_payload(stream, 16);
  configuration.fail_description = false;

  std::array<std::byte, 64> memory{};
  BUFFER_HANDLE buffer = nullptr;
  assert(DSAnnounceBuffer(stream, memory.data(), memory.size(), nullptr,
                          &buffer) == GC_ERR_SUCCESS);
  std::array<std::byte, 16> old_memory{};
  BUFFER_HANDLE old_buffer = nullptr;
  assert(DSAnnounceBuffer(stream, old_memory.data(), old_memory.size(), nullptr,
                          &old_buffer) == GC_ERR_SUCCESS);
  assert(write(remote, 0, 8) == GC_ERR_SUCCESS);
  assert(DSQueueBuffer(stream, old_buffer) == GC_ERR_BUFFER_TOO_SMALL);
  assert(DSRevokeBuffer(stream, old_buffer, nullptr, nullptr) ==
         GC_ERR_SUCCESS);
  expect_payload(stream, 32);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_WIDTH) == 8);
  assert(buffer_value<std::size_t>(stream, buffer,
                                   BUFFER_INFO_DELIVERED_IMAGEHEIGHT) == 0);
  assert(buffer_value<std::int32_t>(stream, buffer,
                                    BUFFER_INFO_PIXEL_ENDIANNESS) ==
         PIXELENDIANNESS_UNKNOWN);
  const auto descriptions = configuration.descriptions;
  assert(write(remote, 16, 1) == GC_ERR_SUCCESS);
  assert(configuration.descriptions ==
         descriptions); // policy false avoids a query

  EVENT_HANDLE event = nullptr;
  assert(GCRegisterEvent(stream, EVENT_NEW_BUFFER, &event) == GC_ERR_SUCCESS);
  assert(DSQueueBuffer(stream, buffer) == GC_ERR_SUCCESS);
#if defined(GENTL_RUNTIME_COPY)
  GenTLTestFailThreadStart();
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE) ==
         GC_ERR_IO);
  assert(!configuration.acquiring);
  configuration.fail_stop = true;
  GenTLTestFailThreadStart();
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE) ==
         GC_ERR_IO);
  assert(configuration.acquiring);
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE) ==
         GC_ERR_RESOURCE_IN_USE);
  configuration.fail_stop = false;
  assert(DSStopAcquisition(stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
  assert(!configuration.acquiring);
  configuration.fail_acquire = true;
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE) ==
         GC_ERR_SUCCESS);
  {
    std::unique_lock lock(configuration.mutex);
    assert(configuration.condition.wait_for(lock, std::chrono::seconds(2), [] {
      return configuration.acquire_failed;
    }));
  }
  assert(DSStopAcquisition(stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_IO);
  assert(Runtime::instance().last_error().second == "injected acquire failure");
  configuration.fail_acquire = false;
#endif
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE) ==
         GC_ERR_SUCCESS);
  const auto active_descriptions = configuration.descriptions;
  assert(write(remote, 24, 1) == GC_ERR_SUCCESS);
  assert(configuration.descriptions == active_descriptions);
  std::array<std::byte, 4> small{};
  BUFFER_HANDLE small_buffer = nullptr;
#if defined(GENTL_RUNTIME_COPY)
  assert(DSAnnounceBuffer(stream, small.data(), small.size(), nullptr,
                          &small_buffer) == GC_ERR_SUCCESS);
  assert(DSQueueBuffer(stream, small_buffer) == GC_ERR_BUFFER_TOO_SMALL);
  assert(DSRevokeBuffer(stream, small_buffer, nullptr, nullptr) ==
         GC_ERR_SUCCESS);
#else
  // Queued ownership already rejects undersized announcements.
  assert(DSAnnounceBuffer(stream, small.data(), small.size(), nullptr,
                          &small_buffer) == GC_ERR_INVALID_BUFFER);
#endif
  configuration.frame_id_increment = 4;
  deliver(event, buffer, 32);
#if defined(GENTL_RUNTIME_COPY)
  assert(buffer_value<std::uint64_t>(stream, buffer, BUFFER_INFO_FRAMEID) == 4);
#endif
  configuration.frame_id_increment = 1;
  std::uint64_t underruns = 1;
  std::size_t underrun_size = sizeof(underruns);
  INFO_DATATYPE underrun_type = INFO_DATATYPE_UNKNOWN;
  assert(DSGetInfo(stream, STREAM_INFO_NUM_UNDERRUN, &underrun_type, &underruns,
                   &underrun_size) == GC_ERR_SUCCESS);
  assert(underruns == 0);
  assert(buffer_value<std::size_t>(stream, buffer,
                                   BUFFER_INFO_DELIVERED_IMAGEHEIGHT) == 4);
  assert(DSStopAcquisition(stream, ACQ_STOP_FLAGS_KILL) == GC_ERR_SUCCESS);

  assert(write(remote, 0, 4) == GC_ERR_SUCCESS);
  expect_payload(stream, 16);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_WIDTH) ==
         8); // snapshot
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_XPADDING) == 2);
  assert(DSQueueBuffer(stream, buffer) == GC_ERR_SUCCESS);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_WIDTH) ==
         4); // unfilled
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_XPADDING) == 0);
  assert(buffer_value<std::size_t>(stream, buffer,
                                   BUFFER_INFO_DELIVERED_IMAGEHEIGHT) == 0);

  // Hold description lookup while a competing write attempts to change width.
  // The mutex assertion establishes exclusion without a scheduling delay.
  {
    std::scoped_lock lock(configuration.mutex);
    configuration.block_description = true;
    configuration.description_entered = false;
    configuration.release_description = false;
  }
  auto start = std::async(std::launch::async, [&] {
    return DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE);
  });
  {
    std::unique_lock lock(configuration.mutex);
    assert(configuration.condition.wait_for(lock, std::chrono::seconds(2), [] {
      return configuration.description_entered;
    }));
  }
  auto *device_state = Runtime::as_dev(device);
  const bool unlocked = device_state->configuration_mutex.try_lock();
  if (unlocked)
    device_state->configuration_mutex.unlock();
  assert(!unlocked);
  std::promise<void> writer_ready;
  auto writer = std::async(std::launch::async, [&] {
    writer_ready.set_value();
    return write(remote, 8, 8);
  });
  writer_ready.get_future().wait();
  {
    std::scoped_lock lock(configuration.mutex);
    configuration.release_description = true;
    configuration.condition.notify_all();
  }
  assert(start.get() == GC_ERR_SUCCESS);
  assert(writer.get() == GC_ERR_ACCESS_DENIED);
  assert(configuration.width == 4);
  deliver(event, buffer, 8);
#if defined(GENTL_RUNTIME_COPY)
  assert(buffer_value<std::size_t>(stream, buffer,
                                   BUFFER_INFO_DELIVERED_IMAGEHEIGHT) == 0);
#endif
  assert(DSStopAcquisition(stream, ACQ_STOP_FLAGS_KILL) == GC_ERR_SUCCESS);
  configuration.block_description = false;

  // INPUT_TO_OUTPUT publishes an incomplete, zero-byte buffer. Preserve its
  // publication-time geometry before and after its event is consumed.
  assert(DSQueueBuffer(stream, buffer) == GC_ERR_SUCCESS);
  assert(write(remote, 0, 8) == GC_ERR_SUCCESS);
  assert(DSFlushQueue(stream, ACQ_QUEUE_INPUT_TO_OUTPUT) == GC_ERR_SUCCESS);
  assert(write(remote, 0, 4) == GC_ERR_SUCCESS);
  assert(write(remote, 32, 2) == GC_ERR_SUCCESS);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_HEIGHT) == 4);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_WIDTH) == 8);
  EVENT_NEW_BUFFER_DATA output{};
  std::size_t output_size = sizeof(output);
  assert(EventGetData(event, &output, &output_size, 1000) == GC_ERR_SUCCESS);
  assert(output.BufferHandle == buffer);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_WIDTH) == 8);
  assert(DSQueueBuffer(stream, buffer) == GC_ERR_SUCCESS);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_WIDTH) == 4);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_HEIGHT) == 2);
  assert(write(remote, 32, 4) == GC_ERR_SUCCESS);
#if defined(GENTL_RUNTIME_QUEUED)
  // KILL also publishes zero-byte metadata for its first borrowed buffer.
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE) ==
         GC_ERR_SUCCESS);
  {
    std::unique_lock lock(configuration.mutex);
    assert(configuration.condition.wait_for(lock, std::chrono::seconds(2), [] {
      return !configuration.borrowed.empty();
    }));
  }
  assert(DSStopAcquisition(stream, ACQ_STOP_FLAGS_KILL) == GC_ERR_SUCCESS);
  assert(write(remote, 0, 8) == GC_ERR_SUCCESS);
  assert(write(remote, 32, 2) == GC_ERR_SUCCESS);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_WIDTH) == 4);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_HEIGHT) == 4);
  output_size = sizeof(output);
  assert(EventGetData(event, &output, &output_size, 1000) == GC_ERR_SUCCESS);
  assert(output.BufferHandle == buffer);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_WIDTH) == 4);
  assert(buffer_value<std::size_t>(stream, buffer,
                                   BUFFER_INFO_DELIVERED_IMAGEHEIGHT) == 0);
  assert(DSQueueBuffer(stream, buffer) == GC_ERR_SUCCESS);
  assert(buffer_value<std::size_t>(stream, buffer, BUFFER_INFO_WIDTH) == 8);
#endif
  assert(DSFlushQueue(stream, ACQ_QUEUE_ALL_DISCARD) == GC_ERR_SUCCESS);
  assert(DSRevokeBuffer(stream, buffer, nullptr, nullptr) == GC_ERR_SUCCESS);
#if defined(GENTL_RUNTIME_QUEUED)
  configuration.native_buffers = 3;
  std::size_t minimum = 0;
  std::size_t minimum_size = sizeof(minimum);
  INFO_DATATYPE minimum_type = INFO_DATATYPE_UNKNOWN;
  assert(DSGetInfo(stream, STREAM_INFO_BUF_ANNOUNCE_MIN, &minimum_type, &minimum,
                   &minimum_size) == GC_ERR_SUCCESS);
  assert(minimum == 3);
  std::uint64_t xml_minimum = 0;
  minimum_size = sizeof(xml_minimum);
  assert(GCReadPort(stream, 0x0098, &xml_minimum, &minimum_size) == GC_ERR_SUCCESS);
  assert(xml_minimum == minimum);
  configuration.native_buffers = 0;
#endif
  assert(EventKill(event) == GC_ERR_SUCCESS);
  assert(DSClose(stream) == GC_ERR_SUCCESS);
  assert(DevClose(device) == GC_ERR_SUCCESS);
  assert(IFClose(interface) == GC_ERR_SUCCESS);
  assert(TLClose(tl) == GC_ERR_SUCCESS);
  assert(GCCloseLib() == GC_ERR_SUCCESS);
}
