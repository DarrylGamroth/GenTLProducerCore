#include "gentl_producer_core/producer.hpp"

#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace gentl_producer_core::runtime {
namespace {
using namespace GenTL;

struct Controls {
  bool fail_init = false;
  bool fail_discovery = false;
  bool fail_open = false;
  bool fail_close = false;
  bool fail_start = false;
  bool empty = false;
  bool no_xml = false;
  std::uint64_t exposure = 10;
} controls;

class Backend final : public RuntimeBackend {
public:
  const BackendInfo &info() const override {
    static const BackendInfo value{"system", "Vendor",    "Transport",
                                   "System", "interface", "Interface",
                                   "Custom", 1,           6};
    return value;
  }
  bool initialize(std::string *error) override {
    if (!controls.fail_init)
      return true;
    *error = "mock initialization failure";
    return false;
  }
  std::vector<DeviceDescriptor> enumerate_devices(std::string *error) override {
    if (controls.fail_discovery) {
      *error = "mock discovery failure";
      return {};
    }
    if (controls.empty)
      return {};
    DeviceDescriptor first;
    first.id = "camera";
    first.vendor = "Vendor";
    first.model = "Sensor";
    first.serial_number = "1234";
    DeviceDescriptor second = first;
    second.id = "other";
    second.serial_number = "5678";
    return {first, second};
  }
  std::unique_ptr<PortModel>
  make_remote_port_model(const DeviceDescriptor &) override {
    PortDescriptor descriptor{.id = "camera",
                              .vendor = "Vendor",
                              .model = "Sensor",
                              .tl_type = "Custom",
                              .module_name = "Device",
                              .port_name = "RemotePort",
                              .version = "2.1",
                              .little_endian = true,
                              .read_access = true,
                              .write_access = true};
    std::vector<PortModel::RegisterRegion> registers{
        integer_register(
            0x100, [] { return controls.exposure; },
            [](std::uint64_t value) {
              controls.exposure = value;
              return GC_ERR_SUCCESS;
            }),
        integer_register(0x108, [] { return 64; }),
        integer_register(0x110, {},
                         [](std::uint64_t) { return GC_ERR_SUCCESS; })};
    if (controls.no_xml)
      return std::make_unique<PortModel>(descriptor, std::vector<PortUrlInfo>{},
                                         std::move(registers));
    return make_embedded_xml_port(descriptor, "remote.xml", "<Remote/>",
                                  std::move(registers));
  }
  bool get_stream_description(const DeviceDescriptor &,
                              StreamDescription *description,
                              std::string *) override {
    *description = {.width = 4,
                    .height = 4,
                    .payload_size_bytes = 16,
                    .pixel_format = 0x01080001,
                    .pixel_size_bits = 8};
    return true;
  }
  bool start_stream(const DeviceDescriptor &, std::string *error) override {
    if (!controls.fail_start)
      return true;
    *error = "mock start failure";
    return false;
  }
  bool remote_write_requires_idle(std::uint64_t, std::size_t) const override {
    return false;
  }
  std::vector<std::string>
  remote_invalidated_features(std::uint64_t address,
                              std::size_t) const override {
    if (address == 0x100)
      return {"ExposureTime"};
    return {};
  }
  std::vector<std::string> acquisition_invalidated_features() const override {
    return {"AcquisitionStatus"};
  }

#if defined(GENTL_RUNTIME_COPY)
  bool stop_stream(const DeviceDescriptor &, std::string *) override {
    return true;
  }
  bool acquire_frame(const DeviceDescriptor &, std::uint64_t, void *buffer,
                     std::size_t size, AcquireFrameResult *result,
                     std::string *) override {
    assert(size >= 16);
    std::memset(buffer, 0x4d, 16);
    *result = {.status = AcquireFrameStatus::kSuccess,
               .buffer = buffer,
               .bytes_filled = 16,
               .timestamp_ns = 250};
    return true;
  }
#else
  bool open_device(DeviceDescriptor *, std::string *error) override {
    if (!controls.fail_open)
      return true;
    *error = "mock open failure";
    return false;
  }
  bool close_device(const DeviceDescriptor &, std::string *error) override {
    if (!controls.fail_close)
      return true;
    *error = "mock close failure";
    return false;
  }
  StopResult stop_stream(const DeviceDescriptor &, std::string *) override {
    std::scoped_lock lock(queue_mutex_);
    queued_ = nullptr;
    return {true, true};
  }
  bool queue_buffer(const DeviceDescriptor &, void *buffer, std::size_t size,
                    std::size_t payload, std::string *) override {
    assert(size >= payload && payload == 16);
    std::scoped_lock lock(queue_mutex_);
    queued_ = buffer;
    return true;
  }
  bool wait_frame(const DeviceDescriptor &, std::uint64_t,
                  AcquireFrameResult *result, std::string *) override {
    std::scoped_lock lock(queue_mutex_);
    assert(queued_);
    auto *buffer = std::exchange(queued_, nullptr);
    std::memset(buffer, 0x4d, 16);
    *result = {.status = AcquireFrameStatus::kSuccess,
               .buffer = buffer,
               .bytes_filled = 16,
               .timestamp_ns = 250};
    return true;
  }

private:
  std::mutex queue_mutex_;
  void *queued_ = nullptr;
#endif
};

std::unique_ptr<RuntimeBackend> make_backend() {
  return std::make_unique<Backend>();
}
} // namespace

const ProducerDefinition<RuntimeBackend> &producer_definition() {
  static const ProducerDefinition<RuntimeBackend> value{
      .version = "2.1",
      .cti_filename = "mock.cti",
      .tl = {"system.xml", "<System/>"},
      .interface = {"interface.xml", "<Interface/>"},
      .device = {"device.xml", "<Device/>"},
      .stream = {"stream.xml", "<Stream/>"},
      .make_backend = make_backend};
  return value;
}
} // namespace gentl_producer_core::runtime

namespace {
using namespace GenTL;
using gentl_producer_core::runtime::controls;

constexpr auto invalid_command = 123456;

template <class Query, class... Args>
std::string query_string(Query query, Args... args) {
  std::array<char, 512> text{};
  std::size_t size = text.size();
  INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
  assert(query(args..., &type, text.data(), &size) == GC_ERR_SUCCESS);
  assert(type == INFO_DATATYPE_STRING);
  assert(size == std::strlen(text.data()) + 1);
  return text.data();
}

template <class T, class Query, class... Args>
T query_value(INFO_DATATYPE expected_type, Query query, Args... args) {
  T value{};
  std::size_t size = sizeof(value);
  INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
  assert(query(args..., &type, &value, &size) == GC_ERR_SUCCESS);
  assert(type == expected_type);
  assert(size == sizeof(value));
  return value;
}

template <class Query, class Handle>
std::string identifier(Query query, Handle handle, std::uint32_t index) {
  std::size_t size = 0;
  assert(query(handle, index, nullptr, &size) == GC_ERR_SUCCESS);
  assert(size > 1);
  std::string id(size, '\0');
  assert(query(handle, index, id.data(), &size) == GC_ERR_SUCCESS);
  id.pop_back();
  return id;
}

std::uint64_t read_int(PORT_HANDLE port, std::uint64_t addr) {
  std::uint64_t result = 0;
  std::size_t size = sizeof(result);
  assert(GCReadPort(port, addr, &result, &size) == GC_ERR_SUCCESS);
  assert(size == sizeof(result));
  return result;
}

GC_ERROR write_int(PORT_HANDLE port, std::uint64_t addr, std::uint64_t value) {
  std::size_t size = sizeof(value);
  return GCWritePort(port, addr, &value, &size);
}

std::string read_string(PORT_HANDLE port, std::uint64_t addr,
                        std::size_t size) {
  std::string value(size, '\0');
  assert(GCReadPort(port, addr, value.data(), &size) == GC_ERR_SUCCESS);
  value.resize(std::strlen(value.c_str()));
  return value;
}

void expect_error(GC_ERROR expected, std::string_view text) {
  std::size_t size = 0;
  GC_ERROR error = GC_ERR_SUCCESS;
  assert(GCGetLastError(&error, nullptr, &size) == GC_ERR_SUCCESS);
  assert(error == expected && size > 1);
  std::string message(size, '\0');
  assert(GCGetLastError(&error, message.data(), &size) == GC_ERR_SUCCESS);
  assert(std::string_view(message.c_str()).find(text) != std::string::npos);
}

void check_system(TL_HANDLE tl) {
  assert(TLGetInfo(nullptr, TL_INFO_ID, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_HANDLE);
  assert(query_string(TLGetInfo, tl, TL_INFO_ID) == "system");
  for (const auto &[command, text] : std::array{
           std::pair{TL_INFO_ID, "system"}, std::pair{TL_INFO_VENDOR, "Vendor"},
           std::pair{TL_INFO_MODEL, "Transport"},
           std::pair{TL_INFO_VERSION, "2.1"},
           std::pair{TL_INFO_TLTYPE, "Custom"},
           std::pair{TL_INFO_DISPLAYNAME, "System"}})
    assert(query_string(GCGetInfo, command) == text);
  const auto path = query_string(GCGetInfo, TL_INFO_PATHNAME);
  assert(std::filesystem::path(path).filename() ==
         query_string(GCGetInfo, TL_INFO_NAME));
  assert(query_value<std::int32_t>(INFO_DATATYPE_INT32, GCGetInfo,
                                   TL_INFO_CHAR_ENCODING) ==
         TL_CHAR_ENCODING_ASCII);
  assert(query_value<std::uint32_t>(INFO_DATATYPE_UINT32, GCGetInfo,
                                    TL_INFO_GENTL_VER_MAJOR) ==
         GenTLMajorVersion);
  assert(query_value<std::uint32_t>(INFO_DATATYPE_UINT32, GCGetInfo,
                                    TL_INFO_GENTL_VER_MINOR) ==
         GenTLMinorVersion);
  assert(GCGetInfo(invalid_command, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_ID);

  std::uint32_t count = 0;
  assert(TLGetNumInterfaces(nullptr, &count) == GC_ERR_INVALID_HANDLE);
  assert(TLGetNumInterfaces(tl, nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(TLGetNumInterfaces(tl, &count) == GC_ERR_SUCCESS && count == 1);
  assert(TLGetInterfaceID(nullptr, 0, nullptr, nullptr) ==
         GC_ERR_INVALID_HANDLE);
  assert(TLGetInterfaceID(tl, 1, nullptr, nullptr) == GC_ERR_INVALID_INDEX);
  assert(identifier(TLGetInterfaceID, tl, 0) == "interface");
  assert(TLGetInterfaceInfo(nullptr, "interface", INTERFACE_INFO_ID, nullptr,
                            nullptr, nullptr) == GC_ERR_INVALID_HANDLE);
  assert(TLGetInterfaceInfo(tl, "absent", INTERFACE_INFO_ID, nullptr, nullptr,
                            nullptr) == GC_ERR_INVALID_ID);
  assert(TLGetInterfaceInfo(tl, "interface", invalid_command, nullptr, nullptr,
                            nullptr) == GC_ERR_INVALID_ID);
  assert(query_string(TLGetInterfaceInfo, tl, "interface", INTERFACE_INFO_ID) ==
         "interface");
  assert(query_string(TLGetInterfaceInfo, tl, "interface",
                      INTERFACE_INFO_DISPLAYNAME) == "Interface");
  assert(query_string(TLGetInterfaceInfo, tl, "interface",
                      INTERFACE_INFO_TLTYPE) == "Custom");
  bool8_t changed = 1;
  assert(TLUpdateInterfaceList(nullptr, &changed, 0) == GC_ERR_INVALID_HANDLE);
  assert(TLUpdateInterfaceList(tl, &changed, 0) == GC_ERR_SUCCESS &&
         changed == 0);

  assert(read_string(tl, 0, 128) == "system");
  assert(read_string(tl, 0x80, 64) == "Vendor");
  assert(read_string(tl, 0xc0, 64) == "Transport");
  assert(read_string(tl, 0x100, 32) == "2.1");
  assert(read_string(tl, 0x120, 128) == query_string(GCGetInfo, TL_INFO_NAME));
  assert(read_string(tl, 0x1a0, 128) == "System");
  assert(read_string(tl, 0x220, 512) == path);
  assert(read_int(tl, 0x420) == 1);
  assert(read_int(tl, 0x428) == GenTLMajorVersion);
  assert(read_int(tl, 0x430) == GenTLMinorVersion);
  assert(read_int(tl, 0x438) == 1);
  assert(read_int(tl, 0x440) == 2);
  assert(write_int(tl, 0x448, 1) == GC_ERR_SUCCESS);
  assert(write_int(tl, 0x450, 1) == GC_ERR_INVALID_INDEX);
  assert(write_int(tl, 0x450, 0) == GC_ERR_SUCCESS);
  assert(read_int(tl, 0x450) == 0);
  assert(read_string(tl, 0x458, 128) == "interface");
  assert(read_string(tl, 0x4d8, 128) == "Interface");
}

void check_interface(IF_HANDLE iface, TL_HANDLE tl) {
  TL_HANDLE parent = nullptr;
  assert(IFGetParentTL(nullptr, &parent) == GC_ERR_INVALID_HANDLE);
  assert(IFGetParentTL(iface, nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(IFGetParentTL(iface, &parent) == GC_ERR_SUCCESS && parent == tl);
  assert(IFGetInfo(nullptr, INTERFACE_INFO_ID, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_HANDLE);
  assert(IFGetInfo(iface, invalid_command, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_ID);
  assert(query_string(IFGetInfo, iface, INTERFACE_INFO_ID) == "interface");
  assert(query_string(IFGetInfo, iface, INTERFACE_INFO_DISPLAYNAME) ==
         "Interface");
  assert(query_string(IFGetInfo, iface, INTERFACE_INFO_TLTYPE) == "Custom");

  std::uint32_t count = 0;
  bool8_t changed = 0;
  assert(IFUpdateDeviceList(nullptr, &changed, 0) == GC_ERR_INVALID_HANDLE);
  assert(IFGetNumDevices(nullptr, &count) == GC_ERR_INVALID_HANDLE);
  assert(IFGetNumDevices(iface, nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(IFGetDeviceID(nullptr, 0, nullptr, nullptr) == GC_ERR_INVALID_HANDLE);

  // A failed initial enumeration remains retryable through each discovery API.
  controls.fail_discovery = true;
  assert(IFGetNumDevices(iface, &count) == GC_ERR_IO);
  assert(IFGetDeviceID(iface, 0, nullptr, nullptr) == GC_ERR_IO);
  assert(IFGetDeviceInfo(iface, "camera", DEVICE_INFO_ID, nullptr, nullptr,
                         nullptr) == GC_ERR_IO);
  assert(write_int(iface, 0x0120, 0) == GC_ERR_IO);
  controls.fail_discovery = false;
  assert(IFUpdateDeviceList(iface, &changed, 0) == GC_ERR_SUCCESS);
  assert(IFGetNumDevices(iface, &count) == GC_ERR_SUCCESS && count == 2);
  assert(IFGetDeviceID(iface, 2, nullptr, nullptr) == GC_ERR_INVALID_INDEX);
  assert(identifier(IFGetDeviceID, iface, 0) == "camera");
  assert(identifier(IFGetDeviceID, iface, 1) == "other");
  assert(IFGetDeviceInfo(nullptr, "camera", DEVICE_INFO_ID, nullptr, nullptr,
                         nullptr) == GC_ERR_INVALID_HANDLE);
  assert(IFGetDeviceInfo(iface, nullptr, DEVICE_INFO_ID, nullptr, nullptr,
                         nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(IFGetDeviceInfo(iface, "absent", DEVICE_INFO_ID, nullptr, nullptr,
                         nullptr) == GC_ERR_INVALID_ID);
  assert(query_string(IFGetDeviceInfo, iface, "camera", DEVICE_INFO_ID) ==
         "camera");

  assert(read_string(iface, 0x0000, 128) == "interface");
  assert(read_string(iface, 0x0080, 128) == "Interface");
  assert(read_int(iface, 0x0100) == 1);
  assert(read_int(iface, 0x0108) == 1);
  assert(read_int(iface, 0x0110) == 6);
  assert(write_int(iface, 0x0118, 1) == GC_ERR_SUCCESS);
  assert(write_int(iface, 0x0120, 2) == GC_ERR_INVALID_INDEX);
  assert(write_int(iface, 0x0120, 1) == GC_ERR_SUCCESS);
  assert(read_int(iface, 0x0120) == 1);
  assert(read_int(iface, 0x0128) == 1);
  assert(read_string(iface, 0x0130, 128) == "other");
  assert(read_string(iface, 0x01b0, 128) == "Vendor");
  assert(read_string(iface, 0x0230, 128) == "Sensor");
  assert(read_int(iface, 0x02b0) == 1);
  assert(read_string(iface, 0x02b8, 128) == "5678");
  assert(read_int(iface, 0x0338) == 1);
  assert(read_int(iface, 0x0340) == 6);
  controls.empty = true;
  assert(IFUpdateDeviceList(iface, &changed, 0) == GC_ERR_SUCCESS &&
         changed == 1);
  assert(read_string(iface, 0x0130, 128).empty());
  assert(read_int(iface, 0x0128) == 0);
  controls.fail_discovery = true;
  assert(IFUpdateDeviceList(iface, &changed, 0) == GC_ERR_IO);
  expect_error(GC_ERR_IO, "mock discovery failure");
  controls.fail_discovery = false;
  controls.empty = false;
  assert(IFUpdateDeviceList(iface, &changed, 0) == GC_ERR_SUCCESS &&
         changed == 1);
}

void check_device(DEV_HANDLE device, IF_HANDLE iface) {
  IF_HANDLE parent = nullptr;
  assert(DevGetParentIF(nullptr, &parent) == GC_ERR_INVALID_HANDLE);
  assert(DevGetParentIF(device, nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(DevGetParentIF(device, &parent) == GC_ERR_SUCCESS && parent == iface);
  assert(DevGetInfo(nullptr, DEVICE_INFO_ID, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_HANDLE);
  assert(DevGetInfo(device, invalid_command, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_ID);
  for (const auto &[command, text] :
       std::array{std::pair{DEVICE_INFO_ID, "camera"},
                  std::pair{DEVICE_INFO_VENDOR, "Vendor"},
                  std::pair{DEVICE_INFO_MODEL, "Sensor"},
                  std::pair{DEVICE_INFO_TLTYPE, "Custom"},
                  std::pair{DEVICE_INFO_DISPLAYNAME, "Sensor (camera)"},
                  std::pair{DEVICE_INFO_USER_DEFINED_NAME, ""},
                  std::pair{DEVICE_INFO_SERIAL_NUMBER, "1234"},
                  std::pair{DEVICE_INFO_VERSION, "2.1"}})
    assert(query_string(DevGetInfo, device, command) == text);
  assert(query_value<std::uint64_t>(INFO_DATATYPE_UINT64, DevGetInfo, device,
                                    DEVICE_INFO_TIMESTAMP_FREQUENCY) ==
         1000000000ULL);
  assert(query_value<std::int32_t>(INFO_DATATYPE_INT32, DevGetInfo, device,
                                   DEVICE_INFO_ACCESS_STATUS) ==
         DEVICE_ACCESS_STATUS_OPEN_READWRITE);
  std::uint32_t count = 0;
  assert(DevGetNumDataStreams(nullptr, &count) == GC_ERR_INVALID_HANDLE);
  assert(DevGetNumDataStreams(device, nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(DevGetNumDataStreams(device, &count) == GC_ERR_SUCCESS && count == 1);
  assert(DevGetDataStreamID(nullptr, 0, nullptr, nullptr) ==
         GC_ERR_INVALID_HANDLE);
  assert(DevGetDataStreamID(device, 1, nullptr, nullptr) ==
         GC_ERR_INVALID_INDEX);
  assert(identifier(DevGetDataStreamID, device, 0) == "camera::stream0");

  assert(read_string(device, 0x0000, 128) == "camera");
  assert(read_string(device, 0x0080, 128) == "1234");
  assert(read_string(device, 0x0100, 128) == "Vendor");
  assert(read_string(device, 0x0180, 128) == "Sensor");
  assert(read_int(device, 0x0200) == 1);
  assert(read_string(device, 0x0208, 192) == "Sensor (camera)");
  assert(read_int(device, 0x02c8) == 1000000000ULL);
  assert(read_int(device, 0x02d0) == 5);
  assert(read_int(device, 0x02d8) == 0 && read_int(device, 0x02e0) == 0);
  assert(write_int(device, 0x02e8, 1) == GC_ERR_INVALID_INDEX);
  assert(write_int(device, 0x02e8, 0) == GC_ERR_SUCCESS);
  assert(read_int(device, 0x02e8) == 0);
  assert(read_string(device, 0x02f0, 128) == "camera::stream0");
}

void check_ports(PORT_HANDLE remote) {
  assert(GCGetPortInfo(nullptr, PORT_INFO_ID, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_HANDLE);
  assert(GCGetPortInfo(remote, invalid_command, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_ID);
  for (const auto &[command, text] :
       std::array{std::pair{PORT_INFO_ID, "camera"},
                  std::pair{PORT_INFO_VENDOR, "Vendor"},
                  std::pair{PORT_INFO_MODEL, "Sensor"},
                  std::pair{PORT_INFO_TLTYPE, "Custom"},
                  std::pair{PORT_INFO_MODULE, "Device"},
                  std::pair{PORT_INFO_VERSION, "2.1"},
                  std::pair{PORT_INFO_PORTNAME, "RemotePort"}})
    assert(query_string(GCGetPortInfo, remote, command) == text);
  for (const auto command :
       {PORT_INFO_LITTLE_ENDIAN, PORT_INFO_ACCESS_READ, PORT_INFO_ACCESS_WRITE})
    assert(query_value<bool8_t>(INFO_DATATYPE_BOOL8, GCGetPortInfo, remote,
                                command) == 1);
  for (const auto command :
       {PORT_INFO_BIG_ENDIAN, PORT_INFO_ACCESS_NA, PORT_INFO_ACCESS_NI})
    assert(query_value<bool8_t>(INFO_DATATYPE_BOOL8, GCGetPortInfo, remote,
                                command) == 0);

  std::uint32_t count = 0;
  assert(GCGetNumPortURLs(nullptr, &count) == GC_ERR_INVALID_HANDLE);
  assert(GCGetNumPortURLs(remote, nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(GCGetNumPortURLs(remote, &count) == GC_ERR_SUCCESS && count == 1);
  std::array<char, 128> url{};
  std::size_t size = url.size();
  assert(GCGetPortURL(nullptr, url.data(), &size) == GC_ERR_INVALID_HANDLE);
  assert(GCGetPortURL(remote, url.data(), &size) == GC_ERR_SUCCESS);
  assert(query_string(GCGetPortURLInfo, remote, 0U, URL_INFO_URL) ==
         url.data());
  assert(query_string(GCGetPortURLInfo, remote, 0U, URL_INFO_FILENAME) ==
         "remote.xml");
  assert(GCGetPortURLInfo(nullptr, 0, URL_INFO_URL, nullptr, nullptr,
                          nullptr) == GC_ERR_INVALID_HANDLE);
  assert(GCGetPortURLInfo(remote, 1, URL_INFO_URL, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_INDEX);
  assert(GCGetPortURLInfo(remote, 0, invalid_command, nullptr, nullptr,
                          nullptr) == GC_ERR_INVALID_ID);
  assert(GCGetPortURLInfo(remote, 0, URL_INFO_FILE_SHA1_HASH, nullptr, nullptr,
                          nullptr) == GC_ERR_NOT_AVAILABLE);
  for (const auto &[command, expected] :
       std::array{std::pair{URL_INFO_SCHEMA_VER_MAJOR, 1},
                  std::pair{URL_INFO_SCHEMA_VER_MINOR, 1},
                  std::pair{URL_INFO_FILE_VER_MAJOR, 0},
                  std::pair{URL_INFO_FILE_VER_MINOR, 1},
                  std::pair{URL_INFO_FILE_VER_SUBMINOR, 0}})
    assert(query_value<std::int32_t>(INFO_DATATYPE_INT32, GCGetPortURLInfo,
                                     remote, 0U, command) == expected);
  assert(query_value<std::int32_t>(INFO_DATATYPE_INT32, GCGetPortURLInfo,
                                   remote, 0U,
                                   URL_INFO_SCHEME) == URL_SCHEME_LOCAL);
  const auto address =
      query_value<std::uint64_t>(INFO_DATATYPE_UINT64, GCGetPortURLInfo, remote,
                                 0U, URL_INFO_FILE_REGISTER_ADDRESS);
  const auto length = query_value<std::uint64_t>(
      INFO_DATATYPE_UINT64, GCGetPortURLInfo, remote, 0U, URL_INFO_FILE_SIZE);
  assert(length == 9);
  assert(read_string(remote, address, length) == "<Remote/>");

  std::uint64_t value = 0, second = 0;
  size = sizeof(value);
  assert(GCReadPort(nullptr, 0, &value, &size) == GC_ERR_INVALID_HANDLE);
  assert(GCReadPort(remote, 0x110, &value, &size) == GC_ERR_ACCESS_DENIED);
  assert(GCReadPort(remote, 0x1234, &value, &size) == GC_ERR_INVALID_ADDRESS);
  assert(GCReadPort(remote, 0x100, &value, nullptr) ==
         GC_ERR_INVALID_PARAMETER);
  assert(GCWritePort(nullptr, 0, &value, &size) == GC_ERR_INVALID_HANDLE);
  assert(GCWritePort(remote, 0x100, &value, nullptr) ==
         GC_ERR_INVALID_PARAMETER);
  assert(write_int(remote, 0x108, 33) == GC_ERR_ACCESS_DENIED);
  assert(write_int(remote, 0x1234, 33) == GC_ERR_INVALID_ADDRESS);
  std::array<PORT_REGISTER_STACK_ENTRY, 2> stack{
      {{0x100, &value, sizeof(value)}, {0x108, &second, sizeof(second)}}};
  size = stack.size();
  assert(GCReadPortStacked(remote, nullptr, &size) == GC_ERR_INVALID_PARAMETER);
  assert(GCReadPortStacked(remote, stack.data(), &size) == GC_ERR_SUCCESS);
  assert(size == 2 && value == 10 && second == 64);
  stack[1].Address = 0x1234;
  size = stack.size();
  assert(GCReadPortStacked(remote, stack.data(), &size) ==
         GC_ERR_INVALID_ADDRESS);
  assert(size == 1 && stack[1].Size == 0);
  value = 25;
  stack[1] = {0x110, &second, sizeof(second)};
  size = stack.size();
  assert(GCWritePortStacked(remote, nullptr, &size) ==
         GC_ERR_INVALID_PARAMETER);
  assert(GCWritePortStacked(remote, stack.data(), &size) == GC_ERR_SUCCESS);
  assert(size == 2 && read_int(remote, 0x100) == value);
  value = 30;
  stack[1].Address = 0x108;
  size = stack.size();
  assert(GCWritePortStacked(remote, stack.data(), &size) ==
         GC_ERR_ACCESS_DENIED);
  assert(size == 1 && stack[1].Size == 0 && read_int(remote, 0x100) == value);
}

void check_feature_events(PORT_HANDLE remote) {
  EVENT_HANDLE event = nullptr;
  EVENT_HANDLE duplicate = nullptr;
  assert(GCRegisterEvent(nullptr, EVENT_FEATURE_INVALIDATE, &event) ==
         GC_ERR_INVALID_HANDLE);
  assert(GCRegisterEvent(remote, EVENT_FEATURE_INVALIDATE, nullptr) ==
         GC_ERR_INVALID_PARAMETER);
  assert(GCRegisterEvent(remote, EVENT_ERROR, &event) ==
         GC_ERR_NOT_IMPLEMENTED);
  assert(GCRegisterEvent(remote, EVENT_FEATURE_INVALIDATE, &event) ==
         GC_ERR_SUCCESS);
  assert(GCRegisterEvent(remote, EVENT_FEATURE_INVALIDATE, &duplicate) ==
         GC_ERR_RESOURCE_IN_USE);
  assert(duplicate == nullptr);
  assert(query_value<std::int32_t>(INFO_DATATYPE_INT32, EventGetInfo, event,
                                   EVENT_EVENT_TYPE) ==
         EVENT_FEATURE_INVALIDATE);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, EventGetInfo, event,
                                  EVENT_INFO_DATA_SIZE_MAX) >= 13);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, EventGetInfo, event,
                                  EVENT_NUM_IN_QUEUE) == 0);
  assert(write_int(remote, 0x100, 75) == GC_ERR_SUCCESS);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, EventGetInfo, event,
                                  EVENT_NUM_IN_QUEUE) == 1);
  assert(query_value<std::uint64_t>(INFO_DATATYPE_UINT64, EventGetInfo, event,
                                    EVENT_NUM_FIRED) == 1);
  std::array<char, 32> payload{};
  std::size_t size = 0;
  size = 1;
  // An undersized replacement buffer gets the same queued event on retry.
  assert(EventGetData(event, payload.data(), &size, 0) ==
             GC_ERR_BUFFER_TOO_SMALL &&
         size == 13);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, EventGetInfo, event,
                                  EVENT_NUM_IN_QUEUE) == 1);
  size = payload.size();
  assert(EventGetData(event, payload.data(), &size, GENTL_INFINITE) ==
         GC_ERR_SUCCESS);
  assert(std::string_view(payload.data()) == "ExposureTime");
  assert(query_string(EventGetDataInfo, event, payload.data(), size,
                      EVENT_DATA_ID) == "ExposureTime");
  assert(query_string(EventGetDataInfo, event, payload.data(), size,
                      EVENT_DATA_VALUE) == "ExposureTime");
  assert(query_value<std::uint64_t>(INFO_DATATYPE_UINT64, EventGetDataInfo,
                                    event, payload.data(), size,
                                    EVENT_DATA_NUMID) == 0);
  assert(EventGetDataInfo(event, payload.data(), size, invalid_command, nullptr,
                          nullptr, nullptr) == GC_ERR_INVALID_ID);
  assert(write_int(remote, 0x100, 77) == GC_ERR_SUCCESS);
  assert(EventGetData(event, nullptr, &size, 0) == GC_ERR_SUCCESS && size == 0);
  assert(EventGetData(event, payload.data(), &size, 0) == GC_ERR_TIMEOUT);
  assert(write_int(remote, 0x100, 78) == GC_ERR_SUCCESS);
  assert(EventFlush(event) == GC_ERR_SUCCESS);
  assert(EventGetData(event, payload.data(), &size, 0) == GC_ERR_TIMEOUT);
  assert(GCUnregisterEvent(nullptr, EVENT_FEATURE_INVALIDATE) ==
         GC_ERR_INVALID_HANDLE);
  assert(GCUnregisterEvent(remote, EVENT_ERROR) == GC_ERR_NOT_AVAILABLE);
  assert(GCUnregisterEvent(remote, EVENT_FEATURE_INVALIDATE) == GC_ERR_SUCCESS);
  assert(EventGetData(event, payload.data(), &size, GENTL_INFINITE) ==
         GC_ERR_TIMEOUT);
  assert(EventKill(event) == GC_ERR_SUCCESS);
  assert(GCRegisterEvent(remote, EVENT_FEATURE_INVALIDATE, &event) ==
         GC_ERR_SUCCESS);
  assert(EventKill(event) == GC_ERR_SUCCESS);
}

void check_capabilities(DS_HANDLE stream, BUFFER_HANDLE buffer) {
  std::uint32_t count = 0;
  assert(DSGetBufferChunkData(stream, buffer, nullptr, nullptr) ==
         GC_ERR_NOT_AVAILABLE);
  assert(DSGetNumBufferParts(stream, buffer, nullptr) ==
         GC_ERR_INVALID_PARAMETER);
  assert(DSGetNumBufferParts(stream, buffer, &count) == GC_ERR_SUCCESS &&
         count == 1);
  assert(DSGetBufferPartInfo(stream, buffer, 1, 0, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_INDEX);
  assert(DSGetBufferPartInfo(stream, buffer, 0, 0, nullptr, nullptr, nullptr) ==
         GC_ERR_NOT_IMPLEMENTED);
  assert(DSAnnounceCompositeBuffer(stream, 0, nullptr, nullptr, nullptr,
                                   nullptr) == GC_ERR_NOT_IMPLEMENTED);
  assert(DSGetBufferInfoStacked(stream, buffer, nullptr, 0) ==
         GC_ERR_NOT_IMPLEMENTED);
  assert(DSGetBufferPartInfoStacked(stream, buffer, nullptr, 0) ==
         GC_ERR_NOT_IMPLEMENTED);
  assert(DSGetNumFlows(stream, nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(DSGetNumFlows(stream, &count) == GC_ERR_SUCCESS && count == 0);
  assert(DSGetFlowInfo(stream, 0, 0, nullptr, nullptr, nullptr) ==
         GC_ERR_NOT_AVAILABLE);
  assert(DSGetNumBufferSegments(stream, buffer, nullptr) ==
         GC_ERR_INVALID_PARAMETER);
  assert(DSGetNumBufferSegments(stream, buffer, &count) == GC_ERR_SUCCESS &&
         count == 1);
  assert(DSGetBufferSegmentInfo(stream, buffer, 1, 0, nullptr, nullptr,
                                nullptr) == GC_ERR_INVALID_INDEX);
  assert(DSGetBufferSegmentInfo(stream, buffer, 0, 0, nullptr, nullptr,
                                nullptr) == GC_ERR_NOT_IMPLEMENTED);
}

void check_stream_info(DS_HANDLE stream, DEV_HANDLE device) {
  DEV_HANDLE parent = nullptr;
  assert(DSGetParentDev(nullptr, &parent) == GC_ERR_INVALID_HANDLE);
  assert(DSGetParentDev(stream, nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(DSGetParentDev(stream, &parent) == GC_ERR_SUCCESS && parent == device);
  assert(DSGetInfo(nullptr, STREAM_INFO_ID, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_HANDLE);
  assert(DSGetInfo(stream, invalid_command, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_ID);
  assert(query_string(DSGetInfo, stream, STREAM_INFO_ID) == "camera::stream0");
  assert(query_string(DSGetInfo, stream, STREAM_INFO_TLTYPE) == "Custom");
  for (const auto command : {STREAM_INFO_NUM_DELIVERED,
                             STREAM_INFO_NUM_UNDERRUN, STREAM_INFO_NUM_STARTED})
    assert(query_value<std::uint64_t>(INFO_DATATYPE_UINT64, DSGetInfo, stream,
                                      command) == 0);
  for (const auto command :
       {STREAM_INFO_NUM_AWAIT_DELIVERY, STREAM_INFO_NUM_ANNOUNCED,
        STREAM_INFO_NUM_QUEUED, STREAM_INFO_NUM_CHUNKS_MAX})
    assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, DSGetInfo, stream,
                                    command) == 0);
  for (const auto command :
       {STREAM_INFO_IS_GRABBING, STREAM_INFO_DEFINES_PAYLOADSIZE})
    assert(query_value<bool8_t>(INFO_DATATYPE_BOOL8, DSGetInfo, stream,
                                command) == 0);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, DSGetInfo, stream,
                                  STREAM_INFO_PAYLOAD_SIZE) == 16);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, DSGetInfo, stream,
                                  STREAM_INFO_BUF_ALIGNMENT) == 1);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, DSGetInfo, stream,
                                  STREAM_INFO_BUF_ANNOUNCE_MIN) == 1);
  assert(read_string(stream, 0, 128) == "camera::stream0");
  for (const auto address : {0x80U, 0x98U, 0xe0U})
    assert(read_int(stream, address) == 1);
  for (const auto address :
       {0x88U, 0x90U, 0xa0U, 0xa8U, 0xb0U, 0xb8U, 0xc0U, 0xd0U, 0xd8U})
    assert(read_int(stream, address) == 0);
  assert(read_int(stream, 0xc8) == 16);
  assert(write_int(stream, 0x90, 0) == GC_ERR_SUCCESS);
  assert(write_int(stream, 0x90, 1) == GC_ERR_INVALID_VALUE);
}

void check_buffers_and_events(DS_HANDLE stream) {
  EVENT_HANDLE event = nullptr;
  assert(GCRegisterEvent(nullptr, EVENT_NEW_BUFFER, &event) ==
         GC_ERR_INVALID_HANDLE);
  assert(GCRegisterEvent(stream, EVENT_NEW_BUFFER, &event) == GC_ERR_SUCCESS);
  EVENT_HANDLE duplicate = nullptr;
  assert(GCRegisterEvent(stream, EVENT_NEW_BUFFER, &duplicate) ==
         GC_ERR_RESOURCE_IN_USE);
  assert(duplicate == nullptr);
  std::size_t size = 0;
  assert(EventGetData(nullptr, nullptr, &size, 0) == GC_ERR_INVALID_HANDLE);
  assert(EventGetData(event, nullptr, nullptr, 0) == GC_ERR_INVALID_PARAMETER);
  assert(EventGetData(event, nullptr, &size, 0) == GC_ERR_TIMEOUT);
  assert(EventGetInfo(nullptr, EVENT_SIZE_MAX, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_HANDLE);
  assert(EventGetInfo(event, invalid_command, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_ID);
  assert(EventFlush(nullptr) == GC_ERR_INVALID_HANDLE);
  assert(EventKill(nullptr) == GC_ERR_INVALID_HANDLE);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, EventGetInfo, event,
                                  EVENT_SIZE_MAX) ==
         sizeof(EVENT_NEW_BUFFER_DATA));

  BUFFER_HANDLE buffer = nullptr;
  std::array<std::byte, 16> memory{};
  int private_data = 1;
  assert(DSGetBufferID(nullptr, 0, &buffer) == GC_ERR_INVALID_HANDLE);
  assert(DSGetBufferID(stream, 0, nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(DSGetBufferID(stream, 0, &buffer) == GC_ERR_INVALID_INDEX);
  assert(DSAnnounceBuffer(nullptr, memory.data(), memory.size(), nullptr,
                          &buffer) == GC_ERR_INVALID_HANDLE);
  assert(DSAnnounceBuffer(stream, nullptr, memory.size(), nullptr, &buffer) ==
         GC_ERR_INVALID_PARAMETER);
  assert(DSAllocAndAnnounceBuffer(nullptr, memory.size(), nullptr, &buffer) ==
         GC_ERR_INVALID_HANDLE);
  assert(DSAllocAndAnnounceBuffer(stream, 0, nullptr, &buffer) ==
         GC_ERR_INVALID_PARAMETER);
  assert(DSStartAcquisition(nullptr, ACQ_START_FLAGS_DEFAULT, 1) ==
         GC_ERR_INVALID_HANDLE);
  assert(DSStartAcquisition(stream, invalid_command, 1) ==
         GC_ERR_NOT_IMPLEMENTED);
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, 0) ==
         GC_ERR_INVALID_PARAMETER);
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, 1) ==
         GC_ERR_INVALID_BUFFER);
  assert(DSStopAcquisition(nullptr, ACQ_STOP_FLAGS_DEFAULT) ==
         GC_ERR_INVALID_HANDLE);
  assert(DSStopAcquisition(stream, invalid_command) == GC_ERR_NOT_IMPLEMENTED);
  assert(DSStopAcquisition(stream, ACQ_STOP_FLAGS_DEFAULT) ==
         GC_ERR_RESOURCE_IN_USE);
  assert(DSAnnounceBuffer(stream, memory.data(), memory.size(), &private_data,
                          &buffer) == GC_ERR_SUCCESS);
  assert(DSAnnounceBuffer(stream, memory.data(), memory.size(), &private_data,
                          &duplicate) == GC_ERR_RESOURCE_IN_USE);
  assert(DSAllocAndAnnounceBuffer(stream, memory.size(), nullptr, &duplicate) ==
         GC_ERR_RESOURCE_IN_USE);
  BUFFER_HANDLE indexed = nullptr;
  assert(DSGetBufferID(stream, 0, &indexed) == GC_ERR_SUCCESS &&
         indexed == buffer);
  check_capabilities(stream, buffer);

  assert(DSQueueBuffer(stream, nullptr) == GC_ERR_INVALID_HANDLE);
  assert(DSRevokeBuffer(stream, nullptr, nullptr, nullptr) ==
         GC_ERR_INVALID_HANDLE);
  assert(DSGetBufferInfo(stream, nullptr, BUFFER_INFO_BASE, nullptr, nullptr,
                         nullptr) == GC_ERR_INVALID_HANDLE);
  assert(DSFlushQueue(nullptr, ACQ_QUEUE_ALL_DISCARD) == GC_ERR_INVALID_HANDLE);
  assert(DSFlushQueue(stream, invalid_command) == GC_ERR_INVALID_PARAMETER);
  assert(DSQueueBuffer(stream, buffer) == GC_ERR_SUCCESS);
  assert(DSQueueBuffer(stream, buffer) == GC_ERR_RESOURCE_IN_USE);
  assert(DSRevokeBuffer(stream, buffer, nullptr, nullptr) == GC_ERR_BUSY);
  assert(read_int(stream, 0xb0) == 1);
  controls.fail_start = true;
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, 1) == GC_ERR_IO);
  expect_error(GC_ERR_IO, "mock start failure");
  controls.fail_start = false;

  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, 1) ==
         GC_ERR_SUCCESS);
  assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, 1) ==
         GC_ERR_RESOURCE_IN_USE);
  size = 1;
  EVENT_NEW_BUFFER_DATA payload{};
  assert(EventGetData(event, &payload, &size, 1000) == GC_ERR_BUFFER_TOO_SMALL);
  assert(size == sizeof(payload));
  assert(EventGetData(event, &payload, &size, GENTL_INFINITE) ==
         GC_ERR_SUCCESS);
  assert(payload.BufferHandle == buffer &&
         payload.pUserPointer == &private_data);
  assert(memory.front() == std::byte{0x4d});
  assert(query_value<std::uint64_t>(INFO_DATATYPE_UINT64, DSGetBufferInfo,
                                    stream, buffer,
                                    BUFFER_INFO_TIMESTAMP_NS) == 250);
  assert(EventGetDataInfo(nullptr, &payload, size, EVENT_DATA_ID, nullptr,
                          nullptr, nullptr) == GC_ERR_INVALID_HANDLE);
  assert(EventGetDataInfo(event, nullptr, 0, EVENT_DATA_ID, nullptr, nullptr,
                          nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(EventGetDataInfo(event, &payload, 1, EVENT_DATA_ID, nullptr, nullptr,
                          nullptr) == GC_ERR_INVALID_PARAMETER);
  EVENT_NEW_BUFFER_DATA no_buffer{};
  assert(EventGetDataInfo(event, &no_buffer, size, EVENT_DATA_ID, nullptr,
                          nullptr, nullptr) == GC_ERR_INVALID_HANDLE);
  assert(EventGetDataInfo(event, &payload, size, invalid_command, nullptr,
                          nullptr, nullptr) == GC_ERR_INVALID_ID);
  assert(query_string(EventGetDataInfo, event, &payload, size, EVENT_DATA_ID) ==
         "NewBuffer");
  assert(query_value<void *>(INFO_DATATYPE_PTR, EventGetDataInfo, event,
                             &payload, size,
                             EVENT_DATA_VALUE) == &private_data);
  assert(query_value<std::uint64_t>(INFO_DATATYPE_UINT64, EventGetDataInfo,
                                    event, &payload, size,
                                    EVENT_DATA_NUMID) == 1);
  assert(DSStopAcquisition(stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);

  assert(DSFlushQueue(stream, ACQ_QUEUE_UNQUEUED_TO_INPUT) == GC_ERR_SUCCESS);
  assert(DSFlushQueue(stream, ACQ_QUEUE_INPUT_TO_OUTPUT) == GC_ERR_SUCCESS);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, EventGetInfo, event,
                                  EVENT_NUM_IN_QUEUE) == 1);
  assert(EventFlush(event) == GC_ERR_SUCCESS);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, EventGetInfo, event,
                                  EVENT_NUM_IN_QUEUE) == 0);
  assert(DSFlushQueue(stream, ACQ_QUEUE_ALL_TO_INPUT) == GC_ERR_SUCCESS);
  assert(DSFlushQueue(stream, ACQ_QUEUE_INPUT_TO_OUTPUT) == GC_ERR_SUCCESS);
  const auto fired = query_value<std::uint64_t>(
      INFO_DATATYPE_UINT64, EventGetInfo, event, EVENT_NUM_FIRED);
  assert(EventGetData(event, nullptr, &size, 0) == GC_ERR_SUCCESS);
  assert(query_value<std::size_t>(INFO_DATATYPE_SIZET, EventGetInfo, event,
                                  EVENT_NUM_IN_QUEUE) == 0);
  assert(size == 0);
  assert(query_value<std::uint64_t>(INFO_DATATYPE_UINT64, EventGetInfo, event,
                                    EVENT_NUM_FIRED) == fired);
  assert(query_value<bool8_t>(INFO_DATATYPE_BOOL8, DSGetBufferInfo, stream,
                              buffer, BUFFER_INFO_IS_QUEUED) == 0);
  assert(DSQueueBuffer(stream, buffer) == GC_ERR_SUCCESS);
  assert(DSFlushQueue(stream, ACQ_QUEUE_INPUT_TO_OUTPUT) == GC_ERR_SUCCESS);
  assert(DSFlushQueue(stream, ACQ_QUEUE_OUTPUT_DISCARD) == GC_ERR_SUCCESS);
  assert(DSFlushQueue(stream, ACQ_QUEUE_ALL_DISCARD) == GC_ERR_SUCCESS);
  void *base = nullptr;
  void *user = nullptr;
  assert(DSRevokeBuffer(stream, buffer, &base, &user) == GC_ERR_SUCCESS);
  assert(base == memory.data() && user == &private_data);

  assert(GCUnregisterEvent(nullptr, EVENT_NEW_BUFFER) == GC_ERR_INVALID_HANDLE);
  assert(GCUnregisterEvent(stream, EVENT_NEW_BUFFER) == GC_ERR_SUCCESS);
  assert(EventGetData(event, &payload, &size, 0) == GC_ERR_TIMEOUT);
  assert(EventKill(event) == GC_ERR_SUCCESS);
}

void check_open_close() {
  assert(TLOpen(nullptr) == GC_ERR_NOT_INITIALIZED);
  assert(GCCloseLib() == GC_ERR_NOT_INITIALIZED);
  controls.fail_init = true;
  assert(GCInitLib() == GC_ERR_NOT_AVAILABLE);
  controls.fail_init = false;
  assert(GCInitLib() == GC_ERR_SUCCESS);
  assert(GCInitLib() == GC_ERR_RESOURCE_IN_USE);
  expect_error(GC_ERR_RESOURCE_IN_USE, "already initialized");
  std::size_t size = 0;
  assert(GCGetLastError(nullptr, nullptr, &size) == GC_ERR_INVALID_PARAMETER);
  assert(TLOpen(nullptr) == GC_ERR_INVALID_PARAMETER);

  TL_HANDLE tl = nullptr;
  assert(TLOpen(&tl) == GC_ERR_SUCCESS);
  TL_HANDLE duplicate_tl = nullptr;
  assert(TLOpen(&duplicate_tl) == GC_ERR_RESOURCE_IN_USE);
  check_system(tl);
  IF_HANDLE iface = nullptr;
  assert(TLOpenInterface(nullptr, "interface", &iface) ==
         GC_ERR_INVALID_HANDLE);
  assert(TLOpenInterface(tl, nullptr, &iface) == GC_ERR_INVALID_PARAMETER);
  assert(TLOpenInterface(tl, "absent", &iface) == GC_ERR_INVALID_ID);
  assert(TLOpenInterface(tl, "interface", &iface) == GC_ERR_SUCCESS);
  IF_HANDLE duplicate_iface = nullptr;
  assert(TLOpenInterface(tl, "interface", &duplicate_iface) ==
         GC_ERR_RESOURCE_IN_USE);
  check_interface(iface, tl);

  DEV_HANDLE device = nullptr;
  assert(IFOpenDevice(nullptr, "camera", DEVICE_ACCESS_CONTROL, &device) ==
         GC_ERR_INVALID_HANDLE);
  assert(IFOpenDevice(iface, nullptr, DEVICE_ACCESS_CONTROL, &device) ==
         GC_ERR_INVALID_PARAMETER);
  assert(IFOpenDevice(iface, "camera", invalid_command, &device) ==
         GC_ERR_INVALID_PARAMETER);
  assert(IFOpenDevice(iface, "absent", DEVICE_ACCESS_CONTROL, &device) ==
         GC_ERR_INVALID_ID);
#if defined(GENTL_RUNTIME_QUEUED)
  controls.fail_open = true;
  assert(IFOpenDevice(iface, "camera", DEVICE_ACCESS_CONTROL, &device) ==
         GC_ERR_IO);
  expect_error(GC_ERR_IO, "mock open failure");
  controls.fail_open = false;
#endif
  controls.no_xml = true;
  assert(IFOpenDevice(iface, "camera", DEVICE_ACCESS_READONLY, &device) ==
         GC_ERR_SUCCESS);
  assert(query_value<std::int32_t>(INFO_DATATYPE_INT32, DevGetInfo, device,
                                   DEVICE_INFO_ACCESS_STATUS) ==
         DEVICE_ACCESS_STATUS_OPEN_READONLY);
  assert(read_int(device, 0x02d0) == 6);
  PORT_HANDLE remote = nullptr;
  assert(DevGetPort(device, &remote) == GC_ERR_SUCCESS);
  assert(GCGetPortURL(remote, nullptr, &size) == GC_ERR_NOT_AVAILABLE);
  assert(write_int(remote, 0x100, 20) == GC_ERR_ACCESS_DENIED);
  DEV_HANDLE duplicate_device = nullptr;
  assert(IFOpenDevice(iface, "camera", DEVICE_ACCESS_EXCLUSIVE,
                      &duplicate_device) == GC_ERR_RESOURCE_IN_USE);
  assert(DevClose(device) == GC_ERR_SUCCESS);
  controls.no_xml = false;

  assert(IFOpenDevice(iface, "camera", DEVICE_ACCESS_CONTROL, &device) ==
         GC_ERR_SUCCESS);
  assert(IFOpenDevice(iface, "camera", DEVICE_ACCESS_CONTROL,
                      &duplicate_device) == GC_ERR_RESOURCE_IN_USE);
  check_device(device, iface);
  assert(DevGetPort(nullptr, &remote) == GC_ERR_INVALID_HANDLE);
  assert(DevGetPort(device, nullptr) == GC_ERR_INVALID_PARAMETER);
  assert(DevGetPort(device, &remote) == GC_ERR_SUCCESS);
  check_ports(remote);
  check_feature_events(remote);

  DS_HANDLE stream = nullptr;
  assert(DevOpenDataStream(nullptr, "camera::stream0", &stream) ==
         GC_ERR_INVALID_HANDLE);
  assert(DevOpenDataStream(device, nullptr, &stream) ==
         GC_ERR_INVALID_PARAMETER);
  assert(DevOpenDataStream(device, "absent", &stream) == GC_ERR_INVALID_ID);
  assert(DevOpenDataStream(device, "camera::stream0", &stream) ==
         GC_ERR_SUCCESS);
  DS_HANDLE duplicate_stream = nullptr;
  assert(DevOpenDataStream(device, "camera::stream0", &duplicate_stream) ==
         GC_ERR_RESOURCE_IN_USE);
  check_stream_info(stream, device);
  check_buffers_and_events(stream);

  assert(DSClose(nullptr) == GC_ERR_INVALID_HANDLE);
  assert(DevClose(nullptr) == GC_ERR_INVALID_HANDLE);
  assert(IFClose(nullptr) == GC_ERR_INVALID_HANDLE);
  assert(TLClose(nullptr) == GC_ERR_INVALID_HANDLE);
#if defined(GENTL_RUNTIME_COPY)
  assert(DevClose(device) == GC_ERR_ACCESS_DENIED);
  assert(IFClose(iface) == GC_ERR_ACCESS_DENIED);
  assert(TLClose(tl) == GC_ERR_ACCESS_DENIED);
  assert(DSClose(stream) == GC_ERR_SUCCESS);
  assert(DevClose(device) == GC_ERR_SUCCESS);
  assert(IFClose(iface) == GC_ERR_SUCCESS);
  assert(TLClose(tl) == GC_ERR_SUCCESS);
#else
  controls.fail_close = true;
  assert(TLClose(tl) == GC_ERR_IO);
  expect_error(GC_ERR_IO, "mock close failure");
  controls.fail_close = false;
  assert(TLClose(tl) == GC_ERR_SUCCESS);
#endif
  assert(GCCloseLib() == GC_ERR_SUCCESS);
}

} // namespace

int main() { check_open_close(); }
