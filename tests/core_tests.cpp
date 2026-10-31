#include "gentl_producer_core/info.hpp"
#include "gentl_producer_core/port_model.hpp"

#include <dlfcn.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char *expression, const char *file, int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": expectation failed: " << expression << '\n';
    ++failures;
  }
}

#define EXPECT(expression) expect((expression), #expression, __FILE__, __LINE__)

void test_port_reads_and_writes() {
  using namespace gentl_producer_core;
  std::uint64_t writable_value = 0;
  std::vector<PortModel::RegisterRegion> registers;
  registers.push_back(integer_register(
      0x10U, [] { return 0x1122334455667788ULL; },
      [&writable_value](std::uint64_t value) {
        writable_value = value;
        return GenTL::GC_ERR_SUCCESS;
      }));
  registers.push_back(string_register(0x20U, 8U, [] { return std::string("camera"); }));
  registers.push_back(PortModel::RegisterRegion{
      .address = 0x30U,
      .data = {std::byte{0x01}, std::byte{0x02}},
      .readable = true,
      .writable = true,
      .access = {}});
  PortModel port({}, {}, std::move(registers));

  std::uint64_t integer = 0;
  std::size_t size = sizeof(integer);
  EXPECT(port.read(0x10U, &integer, &size) == GenTL::GC_ERR_SUCCESS);
  EXPECT(size == sizeof(integer));
  EXPECT(integer == 0x1122334455667788ULL);

  std::array<char, 8> text{};
  size = text.size();
  EXPECT(port.read(0x20U, text.data(), &size) == GenTL::GC_ERR_SUCCESS);
  EXPECT(std::string(text.data()) == "camera");

  const std::uint64_t next = 0x8877665544332211ULL;
  size = sizeof(next);
  EXPECT(port.write(0x10U, &next, &size) == GenTL::GC_ERR_SUCCESS);
  EXPECT(writable_value == next);

  const std::array<std::byte, 2> replacement{std::byte{0x7f}, std::byte{0x55}};
  size = replacement.size();
  EXPECT(port.write(0x30U, replacement.data(), &size) == GenTL::GC_ERR_SUCCESS);
  std::array<std::byte, 2> result{};
  size = result.size();
  EXPECT(port.read(0x30U, result.data(), &size) == GenTL::GC_ERR_SUCCESS);
  EXPECT(result == replacement);

  size = 1;
  std::byte one{};
  EXPECT(port.read(0x99U, &one, &size) == GenTL::GC_ERR_INVALID_ADDRESS);
  EXPECT(port.read(0x10U, nullptr, &size) == GenTL::GC_ERR_INVALID_PARAMETER);
}

void test_embedded_xml() {
  using namespace gentl_producer_core;
  const std::string xml = "<RegisterDescription/>";
  auto port = make_embedded_xml_port(
      make_descriptor("id", "vendor", "model", "Custom", "module", "port",
                      "1.2.3"),
      "port.xml", xml);
  EXPECT(port->urls().size() == 1U);
  const PortUrlInfo &url = port->urls().front();
  EXPECT(url.scheme == GenTL::URL_SCHEME_LOCAL);
  EXPECT(url.filename == "port.xml");
  EXPECT(url.file_register_address == embedded_xml_address);
  EXPECT(url.file_size == xml.size());
  EXPECT(url.url.find("local:port.xml;20000000;") == 0U);
  EXPECT(port->descriptor().version == "1.2.3");

  std::string copied(xml.size(), '\0');
  std::size_t size = copied.size();
  EXPECT(port->read(embedded_xml_address, copied.data(), &size) ==
         GenTL::GC_ERR_SUCCESS);
  EXPECT(copied == xml);
  EXPECT(port->write(embedded_xml_address, copied.data(), &size) ==
         GenTL::GC_ERR_ACCESS_DENIED);
}

void test_info_sizing_and_copying() {
  using namespace gentl_producer_core;
  std::size_t size = 0;
  EXPECT(copy_string("abc", GenTL::INFO_DATATYPE_STRING,
                     GenTL::INFO_DATATYPE_STRING, nullptr, &size) ==
         GenTL::GC_ERR_SUCCESS);
  EXPECT(size == 4U);

  std::array<char, 3> too_small{};
  size = too_small.size();
  EXPECT(copy_string("abc", GenTL::INFO_DATATYPE_STRING,
                     GenTL::INFO_DATATYPE_STRING, too_small.data(), &size) ==
         GenTL::GC_ERR_BUFFER_TOO_SMALL);
  EXPECT(size == 4U);

  std::array<char, 4> string_buffer{};
  size = string_buffer.size();
  GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
  EXPECT(info_string("abc", &type, string_buffer.data(), &size) ==
         GenTL::GC_ERR_SUCCESS);
  EXPECT(type == GenTL::INFO_DATATYPE_STRING);
  EXPECT(std::strcmp(string_buffer.data(), "abc") == 0);

  const std::uint32_t expected = 42U;
  size = 0;
  EXPECT(info_scalar(expected, GenTL::INFO_DATATYPE_UINT32, &type, nullptr, &size) ==
         GenTL::GC_ERR_SUCCESS);
  EXPECT(type == GenTL::INFO_DATATYPE_UINT32);
  EXPECT(size == sizeof(expected));

  std::array<std::byte, sizeof(expected)> scalar_buffer{};
  size = scalar_buffer.size();
  EXPECT(copy_scalar(expected, GenTL::INFO_DATATYPE_UINT32,
                     GenTL::INFO_DATATYPE_UINT32, scalar_buffer.data(), &size) ==
         GenTL::GC_ERR_SUCCESS);
  std::uint32_t actual = 0;
  std::memcpy(&actual, scalar_buffer.data(), sizeof(actual));
  EXPECT(actual == expected);
  EXPECT(copy_scalar(expected, GenTL::INFO_DATATYPE_INT32,
                     GenTL::INFO_DATATYPE_UINT32, scalar_buffer.data(), &size) ==
         GenTL::GC_ERR_INVALID_PARAMETER);
}

void test_integer_partial_writes() {
  using namespace gentl_producer_core;
  std::uint64_t value = 0x1122334455667788ULL;
  int setter_calls = 0;
  PortModel port({}, {}, {integer_register(0x100, [&] { return value; },
      [&](std::uint64_t next) {
        ++setter_calls;
        value = next;
        return GenTL::GC_ERR_SUCCESS;
      })});
  const std::array<std::byte, 2> replacement{std::byte{0xaa}, std::byte{0xbb}};
  std::array<std::byte, sizeof(value)> expected{};
  std::memcpy(expected.data(), &value, sizeof(value));
  std::memcpy(expected.data() + 2, replacement.data(), replacement.size());
  std::size_t size = replacement.size();
  EXPECT(port.write(0x102, replacement.data(), &size) == GenTL::GC_ERR_SUCCESS);
  EXPECT(size == replacement.size());
  EXPECT(std::memcmp(&value, expected.data(), sizeof(value)) == 0);
  EXPECT(setter_calls == 1);

  PortModel write_only({}, {}, {integer_register(0x200, {},
      [&](std::uint64_t next) { value = next; return GenTL::GC_ERR_SUCCESS; })});
  size = sizeof(value);
  const std::uint64_t next = 42;
  EXPECT(write_only.write(0x200, &next, &size) == GenTL::GC_ERR_SUCCESS);
  EXPECT(value == next);
  size = 1;
  EXPECT(write_only.write(0x201, replacement.data(), &size) ==
         GenTL::GC_ERR_INVALID_PARAMETER);
  EXPECT(size == 0U);
  EXPECT(value == next);
}

void test_concurrent_integer_partial_writes() {
  using namespace gentl_producer_core;
  std::atomic<std::uint64_t> value{0};
  std::atomic<bool> first_read{true};
  std::promise<void> reading;
  std::promise<void> release;
  auto released = release.get_future().share();
  PortModel port({}, {}, {integer_register(0x100, [&] {
      const auto snapshot = value.load();
      if (first_read.exchange(false)) {
        reading.set_value();
        released.wait();
      }
      return snapshot;
    }, [&](std::uint64_t next) {
      value.store(next);
      return GenTL::GC_ERR_SUCCESS;
    })});
  auto write_byte = [&](std::uint64_t address, std::byte byte) {
    std::size_t size = 1;
    return port.write(address, &byte, &size);
  };
  auto first = std::async(std::launch::async, write_byte, 0x100, std::byte{0xaa});
  reading.get_future().wait();
  std::promise<void> second_started;
  auto second = std::async(std::launch::async, [&] {
    second_started.set_value();
    return write_byte(0x101, std::byte{0xbb});
  });
  second_started.get_future().wait();
  // Hold the first getter's old snapshot while the second writer tries access.
  // An unserialized second update completes here and is lost when first resumes.
  second.wait_for(std::chrono::milliseconds(100));
  release.set_value();
  EXPECT(first.get() == GenTL::GC_ERR_SUCCESS);
  EXPECT(second.get() == GenTL::GC_ERR_SUCCESS);
  const auto actual = value.load();
  std::array<std::byte, sizeof(actual)> expected{};
  expected[0] = std::byte{0xaa};
  expected[1] = std::byte{0xbb};
  EXPECT(std::memcmp(&actual, expected.data(), sizeof(actual)) == 0);
}

void test_failed_access_counts() {
  using namespace gentl_producer_core;
  PortModel port({}, {}, {
      integer_register(0x100, [] { return 1U; }),
      PortModel::RegisterRegion{.address = 0x200, .data = {}, .size = 8,
          .readable = true, .writable = true,
          .access = [](std::uint64_t, std::span<std::byte>, bool) {
            return GenTL::GC_ERR_IO;
          }}});
  std::uint64_t value = 0;
  std::size_t size = sizeof(value);
  EXPECT(port.read(0x900, &value, &size) == GenTL::GC_ERR_INVALID_ADDRESS);
  EXPECT(size == 0U);
  size = sizeof(value);
  EXPECT(port.write(0x100, &value, &size) == GenTL::GC_ERR_ACCESS_DENIED);
  EXPECT(size == 0U);
  size = sizeof(value);
  EXPECT(port.read(0x200, &value, &size) == GenTL::GC_ERR_IO);
  EXPECT(size == 0U);
  size = sizeof(value);
  EXPECT(port.write(0x200, &value, &size) == GenTL::GC_ERR_IO);
  EXPECT(size == 0U);
  size = sizeof(value);
  EXPECT(port.read(0x100, nullptr, &size) == GenTL::GC_ERR_INVALID_PARAMETER);
  EXPECT(size == 0U);
}

void test_invalid_backing_and_address_ranges() {
  using namespace gentl_producer_core;
  PortModel port({}, {}, {
      PortModel::RegisterRegion{.address = 0x100, .data = {std::byte{1}},
          .size = 8, .readable = true, .writable = true, .access = {}},
      integer_register(std::numeric_limits<std::uint64_t>::max() - 3,
                       [] { return 1U; }),
      PortModel::RegisterRegion{.address = std::numeric_limits<std::uint64_t>::max(),
          .data = {std::byte{0x42}}, .readable = true,
          .writable = true, .access = {}}});
  std::array<std::byte, 8> bytes{};
  std::size_t size = bytes.size();
  EXPECT(port.read(0x100, bytes.data(), &size) == GenTL::GC_ERR_INVALID_ADDRESS);
  EXPECT(size == 0U);
  size = bytes.size();
  EXPECT(port.write(0x100, bytes.data(), &size) == GenTL::GC_ERR_INVALID_ADDRESS);
  EXPECT(size == 0U);
  size = bytes.size();
  EXPECT(port.read(std::numeric_limits<std::uint64_t>::max() - 3,
                   bytes.data(), &size) == GenTL::GC_ERR_INVALID_ADDRESS);
  EXPECT(size == 0U);
  size = 1;
  EXPECT(port.read(std::numeric_limits<std::uint64_t>::max(),
                   bytes.data(), &size) == GenTL::GC_ERR_SUCCESS);
  EXPECT(size == 1U && bytes[0] == std::byte{0x42});
}

void test_file_urls() {
  using namespace gentl_producer_core;
  const auto urls = make_file_urls({
      {.relative_path = "path #?%.xml", .schema_major = 1, .schema_minor = 0},
      {.relative_path = "plain.xml", .schema_major = 1, .schema_minor = 1}});
  EXPECT(urls.size() == 2U);
  EXPECT(urls[0].url.ends_with("/path%20%23%3F%25.xml?SchemaVersion=1.0.0"));
  EXPECT(urls[0].filename == "path #?%.xml");
  EXPECT(urls[0].schema_major == 1 && urls[0].schema_minor == 0);
  EXPECT(urls[1].url.ends_with("/plain.xml?SchemaVersion=1.1.0"));
}

void test_empty_string_info() {
  using namespace gentl_producer_core;
  std::array<char, 1> buffer{'x'};
  std::size_t size = buffer.size();
  GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
  EXPECT(info_string(std::string_view{}, &type, buffer.data(), &size) ==
         GenTL::GC_ERR_SUCCESS);
  EXPECT(size == 1U && buffer[0] == '\0');
}

void test_runtime_path_after_directory_change() {
  // Load through a relative name, then query for the first time after chdir.
  const auto module = std::filesystem::path(GENTL_RUNTIME_PATH_TEST_MODULE);
  const auto original = std::filesystem::current_path();
  std::filesystem::current_path(module.parent_path());
  const auto relative = std::filesystem::path(".") / module.filename();
  void *handle = dlopen(relative.c_str(), RTLD_NOW | RTLD_LOCAL);
  EXPECT(handle != nullptr);
  if (handle == nullptr) {
    std::filesystem::current_path(original);
    return;
  }
  auto path = reinterpret_cast<const char *(*)()>(
      dlsym(handle, "core_test_runtime_path"));
  EXPECT(path != nullptr);
  if (path != nullptr) {
    const auto changed = original == original.root_path()
        ? std::filesystem::temp_directory_path() : original.root_path();
    std::filesystem::current_path(changed);
    EXPECT(std::filesystem::path(path()).lexically_normal() ==
           module.lexically_normal());
    std::filesystem::current_path(original);
    EXPECT(std::filesystem::path(path()).lexically_normal() ==
           module.lexically_normal());
  }
  dlclose(handle);
}

void test_runtime_path_is_absolute() {
  using namespace gentl_producer_core;
  EXPECT(cti_runtime_path("fallback.cti").is_absolute());
  EXPECT(cti_runtime_directory().is_absolute());
}

} // namespace

int main() {
  test_port_reads_and_writes();
  test_embedded_xml();
  test_info_sizing_and_copying();
  test_runtime_path_is_absolute();
  test_integer_partial_writes();
  test_concurrent_integer_partial_writes();
  test_failed_access_counts();
  test_file_urls();
  test_runtime_path_after_directory_change();
  test_empty_string_info();
  test_invalid_backing_and_address_ranges();
  return failures == 0 ? 0 : 1;
}
