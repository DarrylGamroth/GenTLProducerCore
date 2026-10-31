#pragma once
#include "gentl_producer_core/backend.hpp"
#include <string_view>
namespace gentl_producer_core {
struct ModuleXml {
  std::string_view filename;
  std::string_view xml;
};
template <typename Backend> struct ProducerDefinition {
  std::string_view version;
  std::string_view cti_filename;
  ModuleXml tl;
  ModuleXml interface;
  ModuleXml device;
  ModuleXml stream;
  std::unique_ptr<Backend> (*make_backend)();
};
namespace runtime {
#if defined(GENTL_RUNTIME_COPY)
using RuntimeBackend = CopyBackend;
#elif defined(GENTL_RUNTIME_QUEUED)
using RuntimeBackend = QueuedBackend;
#else
#error                                                                         \
    "Link one GenTLProducerCore runtime target to select its buffer ownership model"
#endif
// Defined by producer assembly; all referenced text must outlive the runtime.
__attribute__((visibility("hidden"))) const ProducerDefinition<RuntimeBackend> &
producer_definition();
} // namespace runtime
} // namespace gentl_producer_core
