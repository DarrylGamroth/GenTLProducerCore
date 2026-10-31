#include "gentl_producer_core/port_model.hpp"

extern "C" const char *core_test_runtime_path() {
  static std::string value;
  value = gentl_producer_core::cti_runtime_path().string();
  return value.c_str();
}
