#include "gentl_producer_core/detail/module_ports.hpp"

#include <algorithm>

#include "gentl_producer_core/detail/runtime.hpp"

namespace gentl_producer_core::runtime {
namespace {

std::optional<DeviceDescriptor> selected_device(IfHandleData *interface) {
  std::uint64_t selector = 0;
  {
    std::scoped_lock lock(interface->mutex);
    selector = interface->device_selector;
  }
  GenTL::GC_ERROR error = GenTL::GC_ERR_SUCCESS;
  const auto devices = Runtime::instance().snapshot_cameras(&error);
  if (error != GenTL::GC_ERR_SUCCESS || selector >= devices.size()) {
    return std::nullopt;
  }
  return devices[static_cast<std::size_t>(selector)];
}

std::uint64_t device_access_value(GenTL::DEVICE_ACCESS_FLAGS access) {
  switch (access) {
  case GenTL::DEVICE_ACCESS_READONLY:
    return 6;
  case GenTL::DEVICE_ACCESS_CONTROL:
  case GenTL::DEVICE_ACCESS_EXCLUSIVE:
    return 5;
  default:
    return 0;
  }
}

} // namespace

void apply_stream_description_locked(DsHandleData *stream,
                                     const StreamDescription &description) {
  stream->current_width = description.width;
  stream->current_height = description.height;
#if defined(GENTL_RUNTIME_COPY)
  stream->current_offset_x = description.offset_x;
  stream->current_offset_y = description.offset_y;
#else
  stream->current_x_offset = description.offset_x;
  stream->current_y_offset = description.offset_y;
  stream->current_x_padding = description.x_padding_bytes;
  stream->current_y_padding = description.y_padding_bytes;
#endif
  stream->current_payload_size = description.payload_size_bytes;
  stream->current_buffer_alignment = description.buffer_alignment;
  stream->current_pixel_format = description.pixel_format;
  stream->current_pixel_size_bits = description.pixel_size_bits;
  stream->current_pixel_endianness = description.pixel_endianness;
}

void publish_stream_description(DevHandleData *device,
                                const StreamDescription &description) {
  // Stream lifetime is protected by the caller's configuration lock.
  if (auto *stream = device->open_stream) {
    std::scoped_lock lock(stream->mutex);
    apply_stream_description_locked(stream, description);
  }
}

// Shared TL/interface/device module assembly.

std::unique_ptr<PortModel> make_if_port_model(const BackendInfo &backend_info,
                                              IfHandleData *interface) {
  std::vector<PortModel::RegisterRegion> registers;
  registers.push_back(
      string_register(0x0000, 128, [interface] { return interface->id; }));
  registers.push_back(string_register(
      0x0080, 128, [&] { return backend_info.interface_display_name; }));
  registers.push_back(integer_register(0x0100, [] { return 1U; }));
  registers.push_back(integer_register(
      0x0108, [&] { return backend_info.tl_specification_major; }));
  registers.push_back(integer_register(
      0x0110, [&] { return backend_info.tl_specification_minor; }));
  registers.push_back(integer_register(0x0118, {}, [](std::uint64_t) {
    return Runtime::instance().refresh_cameras(nullptr);
  }));
  registers.push_back(integer_register(
      0x0120,
      [interface] {
        std::scoped_lock lock(interface->mutex);
        return interface->device_selector;
      },
      [interface](std::uint64_t value) -> GenTL::GC_ERROR {
        GenTL::GC_ERROR error = GenTL::GC_ERR_SUCCESS;
        const auto devices = Runtime::instance().snapshot_cameras(&error);
        if (error != GenTL::GC_ERR_SUCCESS)
          return error;
        if (value >= devices.size())
          return GenTL::GC_ERR_INVALID_INDEX;
        std::scoped_lock lock(interface->mutex);
        interface->device_selector = value;
        return GenTL::GC_ERR_SUCCESS;
      }));
  registers.push_back(integer_register(0x0128, [] {
    GenTL::GC_ERROR error = GenTL::GC_ERR_SUCCESS;
    const auto devices = Runtime::instance().snapshot_cameras(&error);
    return error == GenTL::GC_ERR_SUCCESS && !devices.empty()
               ? static_cast<std::uint64_t>(devices.size() - 1U)
               : std::uint64_t{0};
  }));
  registers.push_back(string_register(0x0130, 128, [interface] {
    const auto device = selected_device(interface);
    return device ? device->id : std::string{};
  }));
  registers.push_back(string_register(0x01b0, 128, [interface] {
    const auto device = selected_device(interface);
    return device ? device->vendor : std::string{};
  }));
  registers.push_back(string_register(0x0230, 128, [interface] {
    const auto device = selected_device(interface);
    return device ? device->model : std::string{};
  }));
  registers.push_back(integer_register(0x02b0, [] { return 1U; }));
  registers.push_back(string_register(0x02b8, 128, [interface] {
    const auto device = selected_device(interface);
    return device ? device->serial_number : std::string{};
  }));
  registers.push_back(integer_register(
      0x0338, [&] { return backend_info.tl_specification_major; }));
  registers.push_back(integer_register(
      0x0340, [&] { return backend_info.tl_specification_minor; }));
  return make_embedded_xml_port(
      make_descriptor(interface->id, backend_info.system_vendor,
                      backend_info.interface_display_name, backend_info.tl_type,
                      TLInterfaceModuleName, "InterfacePort",
                      std::string(producer_definition().version), true),
      producer_definition().interface.filename,
      producer_definition().interface.xml, std::move(registers));
}

std::unique_ptr<PortModel> make_dev_port_model(const BackendInfo &backend_info,
                                               DevHandleData *device) {
  std::vector<PortModel::RegisterRegion> registers;
  registers.push_back(
      string_register(0x0000, 128, [device] { return device->device.id; }));
  registers.push_back(string_register(
      0x0080, 128, [device] { return device->device.serial_number; }));
  registers.push_back(
      string_register(0x0100, 128, [device] { return device->device.vendor; }));
  registers.push_back(
      string_register(0x0180, 128, [device] { return device->device.model; }));
  registers.push_back(integer_register(0x0200, [] { return 1U; }));
  registers.push_back(string_register(0x0208, 192, [device] {
    return device->device.model + " (" + device->device.id + ")";
  }));
  registers.push_back(integer_register(
      0x02c8, [device] { return device->device.timestamp_frequency_hz; }));
  registers.push_back(integer_register(
      0x02d0, [device] { return device_access_value(device->access); }));
  registers.push_back(integer_register(0x02d8, [] { return 0U; }));
  registers.push_back(integer_register(0x02e0, [] { return 0U; }));
  registers.push_back(integer_register(
      0x02e8,
      [device] {
        std::scoped_lock lock(device->mutex);
        return device->stream_selector;
      },
      [device](std::uint64_t value) -> GenTL::GC_ERROR {
        if (value != 0)
          return GenTL::GC_ERR_INVALID_INDEX;
        std::scoped_lock lock(device->mutex);
        device->stream_selector = value;
        return GenTL::GC_ERR_SUCCESS;
      }));
  registers.push_back(string_register(
      0x02f0, 128, [device] { return device->device.id + "::stream0"; }));
  return make_embedded_xml_port(
      make_descriptor(device->device.id, device->device.vendor,
                      device->device.model, backend_info.tl_type,
                      TLDeviceModuleName, "DevicePort",
                      std::string(producer_definition().version), true),
      producer_definition().device.filename, producer_definition().device.xml,
      std::move(registers));
}


std::unique_ptr<PortModel> make_tl_port_model(const BackendInfo &backend_info,
                                              TlHandleData *system) {
  std::vector<PortModel::RegisterRegion> registers;
  registers.push_back(
      string_register(0x0000, 128, [&] { return backend_info.system_id; }));
  registers.push_back(
      string_register(0x0080, 64, [&] { return backend_info.system_vendor; }));
  registers.push_back(
      string_register(0x00c0, 64, [&] { return backend_info.system_model; }));
  registers.push_back(string_register(
      0x0100, 32, [] { return std::string(producer_definition().version); }));
  registers.push_back(string_register(0x0120, 128, [] {
    return cti_runtime_path(producer_definition().cti_filename)
        .filename()
        .string();
  }));
  registers.push_back(string_register(
      0x01a0, 128, [&] { return backend_info.system_display_name; }));
  registers.push_back(string_register(0x0220, 512, [] {
    return cti_runtime_path(producer_definition().cti_filename).string();
  }));
  registers.push_back(integer_register(0x0420, [] { return 1U; }));
  registers.push_back(integer_register(
      0x0428, [] { return static_cast<std::uint64_t>(GenTLMajorVersion); }));
  registers.push_back(integer_register(
      0x0430, [] { return static_cast<std::uint64_t>(GenTLMinorVersion); }));
  registers.push_back(integer_register(0x0438, [] { return 1U; }));
  registers.push_back(integer_register(0x0440, [] { return 2U; }));
  registers.push_back(integer_register(
      0x0448, {}, [](std::uint64_t) { return GenTL::GC_ERR_SUCCESS; }));
  registers.push_back(integer_register(
      0x0450,
      [system] {
        std::scoped_lock lock(system->mutex);
        return system->interface_selector;
      },
      [system](std::uint64_t value) -> GenTL::GC_ERROR {
        if (value != 0)
          return GenTL::GC_ERR_INVALID_INDEX;
        std::scoped_lock lock(system->mutex);
        system->interface_selector = value;
        return GenTL::GC_ERR_SUCCESS;
      }));
  registers.push_back(
      string_register(0x0458, 128, [&] { return backend_info.interface_id; }));
  registers.push_back(string_register(
      0x04d8, 128, [&] { return backend_info.interface_display_name; }));
  return make_embedded_xml_port(
      make_descriptor(backend_info.system_id, backend_info.system_vendor,
                      backend_info.system_model, backend_info.tl_type,
                      TLSystemModuleName, "TLPort",
                      std::string(producer_definition().version), true),
      producer_definition().tl.filename, producer_definition().tl.xml,
      std::move(registers));
}

std::unique_ptr<PortModel> make_ds_port_model(const BackendInfo &backend_info,
                                              DsHandleData *stream) {
  auto stream_integer = [stream](auto getter) {
    return [stream, getter] {
      std::scoped_lock lock(stream->mutex);
      return static_cast<std::uint64_t>(getter());
    };
  };
  std::vector<PortModel::RegisterRegion> registers;
  registers.push_back(
      string_register(0x0000, 128, [stream] { return stream->id; }));
  registers.push_back(integer_register(0x0080, [] { return 1U; }));
  registers.push_back(integer_register(
      0x0088, stream_integer([stream] { return stream->buffers.size(); })));
  registers.push_back(integer_register(
      0x0090, [] { return 0U; },
      [](std::uint64_t value) -> GenTL::GC_ERROR {
        return value == 0U ? GenTL::GC_ERR_SUCCESS
                           : GenTL::GC_ERR_INVALID_VALUE;
      }));
  registers.push_back(integer_register(0x0098, [] { return 1U; }));
  registers.push_back(integer_register(
      0x00a0, stream_integer([stream] { return stream->delivered_count; })));
  registers.push_back(integer_register(
      0x00a8, stream_integer([stream] { return stream->underrun_count; })));
  registers.push_back(integer_register(
      0x00b0, stream_integer([stream] {
        return stream->input_queue.size() +
               static_cast<std::size_t>(std::count_if(
                   stream->buffers.begin(), stream->buffers.end(),
                   [](const auto &buffer) { return buffer->is_acquiring; }));
      })));
  registers.push_back(integer_register(
      0x00b8, stream_integer([stream] {
        return static_cast<std::size_t>(std::count_if(
            stream->buffers.begin(), stream->buffers.end(),
            [](const auto &buffer) { return buffer->awaiting_delivery; }));
      })));
  registers.push_back(integer_register(
      0x00c0, stream_integer([stream] { return stream->started_count; })));
  registers.push_back(integer_register(0x00c8, stream_integer([stream] {
                                         return stream->current_payload_size;
                                       })));
  registers.push_back(integer_register(0x00d0, stream_integer([stream] {
                                         return stream->is_acquiring ? 1U : 0U;
                                       })));
  registers.push_back(integer_register(0x00d8, [] { return 0U; }));
  registers.push_back(integer_register(0x00e0, [stream] {
    std::scoped_lock lock(stream->mutex);
    return stream->current_buffer_alignment;
  }));
  return make_embedded_xml_port(
      make_descriptor(stream->id, stream->parent->device.vendor,
                      stream->parent->device.model, backend_info.tl_type,
                      TLDataStreamModuleName, "StreamPort",
                      std::string(producer_definition().version), true),
      producer_definition().stream.filename, producer_definition().stream.xml,
      std::move(registers));
}


} // namespace gentl_producer_core::runtime
