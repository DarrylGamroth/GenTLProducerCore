#include "gentl_producer_core/error.hpp"

#include <algorithm>
#include <type_traits>

namespace gentl_producer_core {
namespace {
struct ErrorState {
  GenTL::GC_ERROR code = GenTL::GC_ERR_SUCCESS;
  std::size_t size = sizeof("No Error") - 1;
  char text[4096] = "No Error";
};

// Older Linux loaders can unload a CTI before a caller's thread exits.
// Keep its thread-local state free of callbacks into the CTI at thread exit.
static_assert(std::is_trivially_destructible_v<ErrorState>);
constinit thread_local ErrorState error;
} // namespace

void set_last_error(GenTL::GC_ERROR code, std::string text) {
  error.code = code;
  error.size = std::min(text.size(), sizeof(error.text) - 1);
  std::copy_n(text.data(), error.size, error.text);
  error.text[error.size] = '\0';
}

std::pair<GenTL::GC_ERROR, std::string> last_error() {
  return {error.code, std::string(error.text, error.size)};
}
} // namespace gentl_producer_core
