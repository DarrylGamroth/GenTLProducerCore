#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <GenTL.h>

namespace gentl_producer_core {

struct XmlFileInfo {
  std::string relative_path;
  std::int32_t schema_major = 1;
  std::int32_t schema_minor = 1;
  std::int32_t file_major = 0;
  std::int32_t file_minor = 1;
  std::int32_t file_subminor = 0;
};

struct PortUrlInfo {
  std::string url;
  std::string filename;
  std::int32_t schema_major = 1;
  std::int32_t schema_minor = 1;
  std::int32_t file_major = 0;
  std::int32_t file_minor = 1;
  std::int32_t file_subminor = 0;
  std::uint64_t file_register_address = 0;
  std::uint64_t file_size = 0;
  GenTL::URL_SCHEME_ID scheme = GenTL::URL_SCHEME_FILE;
};

struct PortDescriptor {
  std::string id;
  std::string vendor;
  std::string model;
  std::string tl_type;
  std::string module_name;
  std::string port_name;
  std::string version;
  bool little_endian = true;
  bool read_access = true;
  bool write_access = false;
};

class PortModel {
public:
  // Callers serialize data-backed accesses and custom callbacks, and keep
  // callback captures alive. Callbacks transfer the complete span on success
  // and no bytes on failure. Write callbacks consume the span without modifying
  // the caller's input buffer.
  using AccessCallback = std::function<GenTL::GC_ERROR(
      std::uint64_t offset, std::span<std::byte> bytes, bool write)>;

  struct RegisterRegion {
    std::uint64_t address = 0;
    std::vector<std::byte> data;
    // Zero selects data.size(); without a callback, size must fit the data.
    std::uint64_t size = 0;
    bool readable = true;
    bool writable = false;
    AccessCallback access;
  };

  PortModel(PortDescriptor descriptor, std::vector<PortUrlInfo> urls,
            std::vector<RegisterRegion> registers = {});

  [[nodiscard]] const PortDescriptor &descriptor() const noexcept;
  [[nodiscard]] const std::vector<PortUrlInfo> &urls() const noexcept;

  GenTL::GC_ERROR read(std::uint64_t address, void *buffer,
                       std::size_t *size) const;
  GenTL::GC_ERROR write(std::uint64_t address, const void *buffer,
                        std::size_t *size);

private:
  [[nodiscard]] const RegisterRegion *find_region(std::uint64_t address,
                                                   std::size_t size) const;
  [[nodiscard]] RegisterRegion *find_region(std::uint64_t address,
                                             std::size_t size);

  PortDescriptor descriptor_;
  std::vector<PortUrlInfo> urls_;
  std::vector<RegisterRegion> registers_;
};

using IntegerGetter = std::function<std::uint64_t()>;
using IntegerSetter = std::function<GenTL::GC_ERROR(std::uint64_t)>;
using StringGetter = std::function<std::string()>;

// The helper serializes getter/setter calls, including partial read-modify-write.
// They must not reenter this register or acquire locks held while accessing it.
// A setter without a getter supports complete eight-byte writes only.
PortModel::RegisterRegion integer_register(std::uint64_t address,
                                           IntegerGetter getter,
                                           IntegerSetter setter = {});
PortModel::RegisterRegion string_register(std::uint64_t address,
                                          std::size_t length,
                                          StringGetter getter);

inline constexpr std::uint64_t embedded_xml_address = 0x20000000ULL;

std::unique_ptr<PortModel> make_embedded_xml_port(
    PortDescriptor descriptor, std::string_view filename, std::string_view xml,
    std::vector<PortModel::RegisterRegion> registers = {},
    std::uint64_t xml_address = embedded_xml_address);

PortDescriptor make_descriptor(std::string id, std::string vendor,
                               std::string model, std::string tl_type,
                               std::string module_name, std::string port_name,
                               std::string version, bool write_access = false);

[[nodiscard]] std::filesystem::path cti_runtime_directory();
[[nodiscard]] std::filesystem::path cti_runtime_path(
    const std::filesystem::path &fallback = {});
std::vector<PortUrlInfo> make_file_urls(std::initializer_list<XmlFileInfo> files);

} // namespace gentl_producer_core
