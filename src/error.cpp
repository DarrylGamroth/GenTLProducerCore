#include "gentl_producer_core/error.hpp"
namespace gentl_producer_core {
namespace {
thread_local GenTL::GC_ERROR error_code = GenTL::GC_ERR_SUCCESS;
thread_local std::string error_text = "No Error";
} // namespace
void set_last_error(GenTL::GC_ERROR code, std::string text) {
  error_code = code;
  error_text = std::move(text);
}
std::pair<GenTL::GC_ERROR, std::string> last_error() {
  return {error_code, error_text};
}
} // namespace gentl_producer_core
