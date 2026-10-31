#include "gentl_producer_core/port_model.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace gentl_producer_core {
namespace {

constexpr std::size_t integer_register_size = sizeof(std::uint64_t);

// Resolve relative loader names before application code can change the cwd.
const std::filesystem::path runtime_module_path = [] {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void *>(&cti_runtime_path), &info) == 0 ||
      info.dli_fname == nullptr) {
    return std::filesystem::path{};
  }
  std::error_code error;
  auto path = std::filesystem::absolute(info.dli_fname, error);
  return error ? std::filesystem::path{} : path.lexically_normal();
}();

std::string make_file_url(const std::filesystem::path &path,
                          const XmlFileInfo &file) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string url = "file://";
  for (const unsigned char byte : path.lexically_normal().generic_string()) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
        byte == '_' || byte == '~' || byte == '/') {
      url += static_cast<char>(byte);
    } else {
      url += '%';
      url += hex[byte >> 4U];
      url += hex[byte & 0x0fU];
    }
  }
  return url + "?SchemaVersion=" + std::to_string(file.schema_major) + '.' +
         std::to_string(file.schema_minor) + ".0";
}

std::uint64_t region_size(const PortModel::RegisterRegion &region) {
  return region.size == 0U ? static_cast<std::uint64_t>(region.data.size())
                           : region.size;
}

bool contains(const PortModel::RegisterRegion &region, std::uint64_t address,
              std::size_t size) {
  if (address < region.address) {
    return false;
  }
  const std::uint64_t offset = address - region.address;
  const std::uint64_t length = region_size(region);
  if ((!region.access && length > region.data.size()) ||
      (length != 0U && length - 1U >
          std::numeric_limits<std::uint64_t>::max() - region.address)) {
    return false;
  }
  return offset <= length && static_cast<std::uint64_t>(size) <= length - offset;
}

} // namespace

PortModel::RegisterRegion integer_register(std::uint64_t address,
                                           IntegerGetter getter,
                                           IntegerSetter setter) {
  const bool writable = static_cast<bool>(setter);
  return PortModel::RegisterRegion{
      .address = address,
      .data = {},
      .size = integer_register_size,
      .readable = static_cast<bool>(getter),
      .writable = writable,
      .access = [getter = std::move(getter), setter = std::move(setter),
                 mutex = std::make_shared<std::mutex>()](
                    std::uint64_t offset, std::span<std::byte> bytes,
                    bool write) -> GenTL::GC_ERROR {
        std::scoped_lock lock(*mutex);
        if (write) {
          if (!setter) {
            return GenTL::GC_ERR_ACCESS_DENIED;
          }
          std::uint64_t value = 0;
          if (offset == 0U && bytes.size() == integer_register_size) {
            std::memcpy(&value, bytes.data(), sizeof(value));
          } else {
            if (!getter) {
              return GenTL::GC_ERR_INVALID_PARAMETER;
            }
            value = getter();
            if (!bytes.empty()) {
              std::memcpy(reinterpret_cast<std::byte *>(&value) + offset,
                          bytes.data(), bytes.size());
            }
          }
          return setter(value);
        }
        if (!getter) {
          return GenTL::GC_ERR_ACCESS_DENIED;
        }
        const std::uint64_t value = getter();
        std::array<std::byte, integer_register_size> encoded{};
        std::memcpy(encoded.data(), &value, sizeof(value));
        if (!bytes.empty()) {
          std::memcpy(bytes.data(), encoded.data() + offset, bytes.size());
        }
        return GenTL::GC_ERR_SUCCESS;
      }};
}

PortModel::RegisterRegion string_register(std::uint64_t address,
                                          std::size_t length,
                                          StringGetter getter) {
  return PortModel::RegisterRegion{
      .address = address,
      .data = {},
      .size = static_cast<std::uint64_t>(length),
      .readable = static_cast<bool>(getter),
      .writable = false,
      .access = [length, getter = std::move(getter)](
                    std::uint64_t offset, std::span<std::byte> bytes,
                    bool write) -> GenTL::GC_ERROR {
        if (write || !getter) {
          return GenTL::GC_ERR_ACCESS_DENIED;
        }
        std::vector<std::byte> encoded(length);
        const std::string value = getter();
        const std::size_t count = length == 0U ? 0U : std::min(value.size(), length - 1U);
        if (count != 0U) {
          std::memcpy(encoded.data(), value.data(), count);
        }
        if (!bytes.empty()) {
          std::memcpy(bytes.data(), encoded.data() + offset, bytes.size());
        }
        return GenTL::GC_ERR_SUCCESS;
      }};
}

PortModel::PortModel(PortDescriptor descriptor, std::vector<PortUrlInfo> urls,
                     std::vector<RegisterRegion> registers)
    : descriptor_(std::move(descriptor)), urls_(std::move(urls)),
      registers_(std::move(registers)) {}

const PortDescriptor &PortModel::descriptor() const noexcept { return descriptor_; }

const std::vector<PortUrlInfo> &PortModel::urls() const noexcept { return urls_; }

const PortModel::RegisterRegion *PortModel::find_region(std::uint64_t address,
                                                         std::size_t size) const {
  const auto found = std::find_if(registers_.begin(), registers_.end(),
                                  [address, size](const RegisterRegion &region) {
                                    return contains(region, address, size);
                                  });
  return found == registers_.end() ? nullptr : &*found;
}

PortModel::RegisterRegion *PortModel::find_region(std::uint64_t address,
                                                   std::size_t size) {
  const auto found = std::find_if(registers_.begin(), registers_.end(),
                                  [address, size](const RegisterRegion &region) {
                                    return contains(region, address, size);
                                  });
  return found == registers_.end() ? nullptr : &*found;
}

GenTL::GC_ERROR PortModel::read(std::uint64_t address, void *buffer,
                                std::size_t *size) const {
  if (size == nullptr) {
    return GenTL::GC_ERR_INVALID_PARAMETER;
  }
  const std::size_t requested = *size;
  *size = 0U;
  if (buffer == nullptr) {
    return GenTL::GC_ERR_INVALID_PARAMETER;
  }
  const RegisterRegion *region = find_region(address, requested);
  if (region == nullptr) {
    return GenTL::GC_ERR_INVALID_ADDRESS;
  }
  if (!region->readable) {
    return GenTL::GC_ERR_ACCESS_DENIED;
  }

  const std::uint64_t offset = address - region->address;
  if (region->access) {
    const auto result = region->access(
        offset, {static_cast<std::byte *>(buffer), requested}, false);
    if (result != GenTL::GC_ERR_SUCCESS) {
      return result;
    }
  } else if (requested != 0U) {
    std::memcpy(buffer, region->data.data() + offset, requested);
  }
  *size = requested;
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR PortModel::write(std::uint64_t address, const void *buffer,
                                 std::size_t *size) {
  if (size == nullptr) {
    return GenTL::GC_ERR_INVALID_PARAMETER;
  }
  const std::size_t requested = *size;
  *size = 0U;
  if (buffer == nullptr) {
    return GenTL::GC_ERR_INVALID_PARAMETER;
  }
  RegisterRegion *region = find_region(address, requested);
  if (region == nullptr) {
    return GenTL::GC_ERR_INVALID_ADDRESS;
  }
  if (!region->writable) {
    return GenTL::GC_ERR_ACCESS_DENIED;
  }

  const std::uint64_t offset = address - region->address;
  if (region->access) {
    const auto result = region->access(
        offset, {static_cast<std::byte *>(const_cast<void *>(buffer)), requested},
        true);
    if (result != GenTL::GC_ERR_SUCCESS) {
      return result;
    }
  } else if (requested != 0U) {
    std::memcpy(region->data.data() + offset, buffer, requested);
  }
  *size = requested;
  return GenTL::GC_ERR_SUCCESS;
}

std::unique_ptr<PortModel> make_embedded_xml_port(
    PortDescriptor descriptor, std::string_view filename, std::string_view xml,
    std::vector<PortModel::RegisterRegion> registers, std::uint64_t xml_address) {
  std::ostringstream url;
  url << "local:" << filename << ';' << std::hex << xml_address << ';' << xml.size()
      << "?SchemaVersion=1.1.0";

  std::vector<std::byte> bytes(xml.size());
  if (!xml.empty()) {
    std::memcpy(bytes.data(), xml.data(), xml.size());
  }
  registers.push_back(PortModel::RegisterRegion{
      .address = xml_address,
      .data = std::move(bytes),
      .readable = true,
      .writable = false,
      .access = {}});

  return std::make_unique<PortModel>(
      std::move(descriptor),
      std::vector<PortUrlInfo>{PortUrlInfo{
          .url = url.str(),
          .filename = std::string(filename),
          .schema_major = 1,
          .schema_minor = 1,
          .file_major = 0,
          .file_minor = 1,
          .file_subminor = 0,
          .file_register_address = xml_address,
          .file_size = static_cast<std::uint64_t>(xml.size()),
          .scheme = GenTL::URL_SCHEME_LOCAL}},
      std::move(registers));
}

PortDescriptor make_descriptor(std::string id, std::string vendor,
                               std::string model, std::string tl_type,
                               std::string module_name, std::string port_name,
                               std::string version, bool write_access) {
  return PortDescriptor{.id = std::move(id),
                        .vendor = std::move(vendor),
                        .model = std::move(model),
                        .tl_type = std::move(tl_type),
                        .module_name = std::move(module_name),
                        .port_name = std::move(port_name),
                        .version = std::move(version),
                        .little_endian = true,
                        .read_access = true,
                        .write_access = write_access};
}

std::filesystem::path cti_runtime_path(const std::filesystem::path &fallback) {
  if (!runtime_module_path.empty()) {
    return runtime_module_path;
  }
  if (fallback.empty()) {
    return std::filesystem::current_path() / "producer.cti";
  }
  return fallback.is_absolute() ? fallback
                                : std::filesystem::current_path() / fallback;
}

std::filesystem::path cti_runtime_directory() {
  return cti_runtime_path().parent_path();
}

std::vector<PortUrlInfo> make_file_urls(std::initializer_list<XmlFileInfo> files) {
  std::vector<PortUrlInfo> urls;
  urls.reserve(files.size());
  const std::filesystem::path base_dir = cti_runtime_directory();
  for (const XmlFileInfo &file : files) {
    const std::filesystem::path absolute_path = base_dir / file.relative_path;
    std::error_code error;
    const bool exists = std::filesystem::exists(absolute_path, error);
    const std::uint64_t file_size = exists && !error
        ? static_cast<std::uint64_t>(std::filesystem::file_size(absolute_path, error))
        : 0U;
    urls.push_back(PortUrlInfo{.url = make_file_url(absolute_path, file),
                               .filename = absolute_path.filename().string(),
                               .schema_major = file.schema_major,
                               .schema_minor = file.schema_minor,
                               .file_major = file.file_major,
                               .file_minor = file.file_minor,
                               .file_subminor = file.file_subminor,
                               .file_register_address = 0,
                               .file_size = error ? 0U : file_size,
                               .scheme = GenTL::URL_SCHEME_FILE});
  }
  return urls;
}

} // namespace gentl_producer_core
