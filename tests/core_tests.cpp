#include "gentl_producer_core/info.hpp"
#include "gentl_producer_core/port_model.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
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
  return failures == 0 ? 0 : 1;
}
