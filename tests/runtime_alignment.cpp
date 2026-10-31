#include "gentl_producer_core/detail/runtime.hpp"

#include <array>
#include <cassert>
#include <limits>

namespace gentl_producer_core::runtime {
namespace {

std::size_t backend_alignment = 64U;

class AlignmentBackend final : public RuntimeBackend {
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
    device.payload_size_bytes = 16U;
    return {device};
  }
  std::unique_ptr<PortModel>
  make_remote_port_model(const DeviceDescriptor &) override {
    return std::make_unique<PortModel>(PortDescriptor{}, std::vector<PortUrlInfo>{});
  }
  bool get_stream_description(const DeviceDescriptor &, StreamDescription *value,
                              std::string *) override {
    *value = {.buffer_alignment = backend_alignment, .payload_size_bytes = 16U};
    return true;
  }
  bool start_stream(const DeviceDescriptor &, std::string *) override { return true; }
#if defined(GENTL_RUNTIME_COPY)
  bool stop_stream(const DeviceDescriptor &, std::string *) override { return true; }
#else
  StopResult stop_stream(const DeviceDescriptor &, std::string *) override { return {true, true}; }
#endif
  bool remote_write_requires_idle(std::uint64_t, std::size_t) const override { return false; }
  std::vector<std::string>
  remote_invalidated_features(std::uint64_t, std::size_t) const override { return {}; }
  std::vector<std::string> acquisition_invalidated_features() const override { return {}; }
#if defined(GENTL_RUNTIME_COPY)
  bool acquire_frame(const DeviceDescriptor &, std::uint64_t, void *, std::size_t,
                     AcquireFrameResult *, std::string *) override { return false; }
#else
  bool open_device(DeviceDescriptor *, std::string *) override { return true; }
  bool close_device(const DeviceDescriptor &, std::string *) override { return true; }
  bool queue_buffer(const DeviceDescriptor &, void *, std::size_t, std::size_t,
                    std::string *) override { return false; }
  bool wait_frame(const DeviceDescriptor &, std::uint64_t, AcquireFrameResult *,
                  std::string *) override { return false; }
#endif
};

std::unique_ptr<RuntimeBackend> make_backend() {
  return std::make_unique<AlignmentBackend>();
}

} // namespace

const ProducerDefinition<RuntimeBackend> &producer_definition() {
  static const ProducerDefinition<RuntimeBackend> value{
      .version = "test", .cti_filename = "test.cti",
      .tl = {"tl.xml", "test"}, .interface = {"interface.xml", "test"},
      .device = {"device.xml", "test"}, .stream = {"stream.xml", "test"},
      .make_backend = make_backend};
  return value;
}
} // namespace gentl_producer_core::runtime

int main() {
  using namespace GenTL;
  using namespace gentl_producer_core;
  using namespace gentl_producer_core::runtime;
  assert(GCInitLib() == GC_ERR_SUCCESS);
  StreamDescription description;
  std::string error;
  for (const auto invalid : {0U, 3U}) {
    backend_alignment = invalid;
    assert(!Runtime::instance().get_stream_description({}, &description, &error));
    assert(!error.empty());
  }
  backend_alignment = 64U;
  assert(Runtime::instance().get_stream_description({}, &description, &error));
  TL_HANDLE tl = nullptr;
  IF_HANDLE interface = nullptr;
  DEV_HANDLE device = nullptr;
  DS_HANDLE stream = nullptr;
  assert(TLOpen(&tl) == GC_ERR_SUCCESS);
  assert(GCCloseLib() == GC_ERR_RESOURCE_IN_USE);
  assert(TLOpenInterface(tl, "test-interface", &interface) == GC_ERR_SUCCESS);
  assert(IFOpenDevice(interface, "test-device", DEVICE_ACCESS_CONTROL, &device) == GC_ERR_SUCCESS);
  assert(DevOpenDataStream(device, "test-device::stream0", &stream) == GC_ERR_SUCCESS);
  INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
  std::size_t alignment = 0;
  std::size_t size = sizeof(alignment);
  assert(DSGetInfo(stream, STREAM_INFO_BUF_ALIGNMENT, &type, &alignment, &size) == GC_ERR_SUCCESS);
  assert(alignment == 64U);
  std::uint64_t xml_alignment = 0;
  size = sizeof(xml_alignment);
  assert(GCReadPort(stream, 0xe0, &xml_alignment, &size) == GC_ERR_SUCCESS);
  assert(xml_alignment == alignment);
  BUFFER_HANDLE buffer = nullptr;
  assert(DSAllocAndAnnounceBuffer(stream, std::numeric_limits<std::size_t>::max(), nullptr, &buffer) == GC_ERR_INVALID_BUFFER);
  assert(buffer == nullptr);
  assert(DSAllocAndAnnounceBuffer(stream, 17U, nullptr, &buffer) == GC_ERR_SUCCESS);
  void *base = nullptr;
  size = sizeof(base);
  assert(DSGetBufferInfo(stream, buffer, BUFFER_INFO_BASE, &type, &base, &size) == GC_ERR_SUCCESS);
  assert(reinterpret_cast<std::uintptr_t>(base) % alignment == 0U);
  std::size_t reported_size = 0;
  size = sizeof(reported_size);
  assert(DSGetBufferInfo(stream, buffer, BUFFER_INFO_SIZE, &type, &reported_size, &size) == GC_ERR_SUCCESS);
  assert(reported_size == 17U);
  assert(DSRevokeBuffer(stream, buffer, nullptr, nullptr) == GC_ERR_SUCCESS);
  assert(DSClose(stream) == GC_ERR_SUCCESS);
  assert(DevOpenDataStream(device, "test-device::stream0", &stream) == GC_ERR_SUCCESS);
  alignas(128) std::array<std::byte, 256> storage{};
  buffer = nullptr;
  assert(DSAnnounceBuffer(stream, storage.data() + 1U, 16U, nullptr, &buffer) == GC_ERR_INVALID_BUFFER);
  assert(buffer == nullptr);
  assert(DSAnnounceBuffer(stream, storage.data() + 64U, 16U, nullptr, &buffer) == GC_ERR_SUCCESS);
  backend_alignment = 128U;
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, 1U) == GC_ERR_INVALID_BUFFER);
  backend_alignment = 64U;
  assert(DSRevokeBuffer(stream, buffer, nullptr, nullptr) == GC_ERR_SUCCESS);
  assert(DSClose(stream) == GC_ERR_SUCCESS);
  assert(DevClose(device) == GC_ERR_SUCCESS);
  assert(IFClose(interface) == GC_ERR_SUCCESS);
  assert(TLClose(tl) == GC_ERR_SUCCESS);
  assert(GCCloseLib() == GC_ERR_SUCCESS);
}
