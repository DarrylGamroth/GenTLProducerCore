#pragma once

#include <memory>

#include "gentl_producer_core/port_model.hpp"
#include "gentl_producer_core/backend.hpp"

namespace gentl_producer_core::runtime {

struct TlHandleData;
struct IfHandleData;
struct DevHandleData;
struct DsHandleData;

using gentl_producer_core::PortDescriptor;
using gentl_producer_core::PortUrlInfo;
using gentl_producer_core::XmlFileInfo;
using gentl_producer_core::cti_runtime_directory;
using gentl_producer_core::cti_runtime_path;
using gentl_producer_core::integer_register;
using gentl_producer_core::make_descriptor;
using gentl_producer_core::make_embedded_xml_port;
using gentl_producer_core::make_file_urls;
using gentl_producer_core::string_register;

// Caller holds the device configuration mutex and, when changing a stream,
// its mutex. Workers never acquire the configuration mutex.
void apply_stream_description_locked(DsHandleData *stream,
                                     const StreamDescription &description);
void publish_stream_description(DevHandleData *device,
                                const StreamDescription &description);
std::size_t queued_buffer_count_locked(const DsHandleData *stream);
std::size_t buffer_announce_minimum_locked(const DsHandleData *stream);

std::unique_ptr<PortModel> make_tl_port_model(const BackendInfo &backend_info,
                                              TlHandleData *system);
std::unique_ptr<PortModel> make_if_port_model(const BackendInfo &backend_info,
                                              IfHandleData *interface);
std::unique_ptr<PortModel> make_dev_port_model(const BackendInfo &backend_info,
                                               DevHandleData *device);
std::unique_ptr<PortModel> make_ds_port_model(const BackendInfo &backend_info,
                                              DsHandleData *stream);

} // namespace gentl_producer_core::runtime
