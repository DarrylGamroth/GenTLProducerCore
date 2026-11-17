#include "gentl_producer_core/producer.hpp"

namespace gentl_producer_core::runtime {
namespace {
class Backend final : public CopyBackend {
public:
  const BackendInfo &info() const override { return info_; }
  bool initialize(std::string *) override { return true; }
  std::vector<DeviceDescriptor> enumerate_devices(std::string *) override {
    return {};
  }
  std::unique_ptr<PortModel>
  make_remote_port_model(const DeviceDescriptor &) override {
    return {};
  }
  bool get_stream_description(const DeviceDescriptor &, StreamDescription *,
                              std::string *) override {
    return false;
  }
  bool start_stream(const DeviceDescriptor &, std::string *) override {
    return false;
  }
  bool stop_stream(const DeviceDescriptor &, std::string *) override {
    return true;
  }
  bool acquire_frame(const DeviceDescriptor &, std::uint64_t, void *,
                     std::size_t, AcquireFrameResult *, std::string *) override {
    return false;
  }
  bool remote_write_requires_idle(std::uint64_t, std::size_t) const override {
    return false;
  }
  std::vector<std::string>
  remote_invalidated_features(std::uint64_t, std::size_t) const override {
    return {};
  }
  std::vector<std::string> acquisition_invalidated_features() const override {
    return {};
  }

private:
  BackendInfo info_{"system", "Test", "Mock", "Test system", "interface",
                    "Test interface", "Custom"};
};

std::unique_ptr<CopyBackend> make_backend() {
  return std::make_unique<Backend>();
}
} // namespace

const ProducerDefinition<RuntimeBackend> &producer_definition() {
  static constexpr ProducerDefinition<RuntimeBackend> definition{
      .version = "1.0",
      .cti_filename = "gentl_error_test.cti",
      .tl = {"tl.xml", "<System/>"},
      .interface = {},
      .device = {},
      .stream = {},
      .make_backend = make_backend};
  return definition;
}
} // namespace gentl_producer_core::runtime
