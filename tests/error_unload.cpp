#include <GenTL.h>
#include <cassert>
#include <dlfcn.h>
#include <latch>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace GenTL;

template <class Function> Function load(void *library, const char *name) {
  const auto function = reinterpret_cast<Function>(dlsym(library, name));
  assert(function);
  return function;
}

std::pair<GC_ERROR, std::string> get_error(PGCGetLastError get_last_error) {
  GC_ERROR code{};
  std::size_t size{};
  assert(get_last_error(&code, nullptr, &size) == GC_ERR_SUCCESS);
  assert(size > 0);
  std::string message(size, '\0');
  assert(get_last_error(&code, message.data(), &size) == GC_ERR_SUCCESS);
  assert(size == message.size() && message.back() == '\0');
  message.pop_back();
  return {code, std::move(message)};
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  void *const library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  assert(library);
  const auto init = load<PGCInitLib>(library, "GCInitLib");
  const auto close = load<PGCCloseLib>(library, "GCCloseLib");
  const auto read = load<PGCGetLastError>(library, "GCGetLastError");
  const auto close_interface = load<PIFClose>(library, "IFClose");
  assert(init() == GC_ERR_SUCCESS);
  const auto initial = get_error(read);
  assert(initial.first == GC_ERR_SUCCESS && initial.second == "No Error");
  assert(init() == GC_ERR_RESOURCE_IN_USE);

  std::latch calls_finished{1};
  std::latch may_exit{1};
  std::thread caller([&] {
    assert(get_error(read) == initial);
    assert(close_interface(nullptr) == GC_ERR_INVALID_HANDLE);
    const auto worker_error = get_error(read);
    assert(worker_error.first == GC_ERR_INVALID_HANDLE);
    assert(worker_error.second == "invalid or stale handle");
    calls_finished.count_down();
    may_exit.wait();
  });

  calls_finished.wait();
  const auto main_error = get_error(read);
  assert(main_error.first == GC_ERR_RESOURCE_IN_USE);
  assert(main_error.second == "GenTL library is already initialized");
  assert(close() == GC_ERR_SUCCESS);
  assert(dlclose(library) == 0);
  // A newer loader can defer unload while it still has a TLS destructor to
  // run. Check the mapping while the caller is alive on those systems too.
  void *const still_loaded = dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD);
  if (still_loaded)
    assert(dlclose(still_loaded) == 0);
  may_exit.count_down();
  caller.join();
  assert(still_loaded == nullptr);
}
