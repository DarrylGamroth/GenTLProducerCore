#include "gentl_producer_core/detail/runtime.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace gentl_producer_core::runtime {
namespace {
enum class Failure { none, queue, wait, fifo, stopped, finite };
struct Faults {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<void *> borrowed;
  Failure failure = Failure::none;
  bool release_wait = false;
  bool host_stopped = false;
  bool retain = true;
  bool operational_success = false;
  bool close_success = true;
  bool camera_started = false;
  bool hold_wait = false;
  bool wait_entered = false;
  unsigned completed = 0;
  unsigned completed_at_stop = 0;
  unsigned stops = 0;
  unsigned queued = 0;
  void await(auto predicate) {
    std::unique_lock lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(5), predicate));
  }
} faults;
class FaultBackend final : public QueuedBackend {
public:
  ~FaultBackend() override { assert(faults.borrowed.empty()); }
  const BackendInfo &info() const override {
    static const BackendInfo value = [] {
      BackendInfo v; v.system_id = "system"; v.interface_id = "interface"; return v;
    }();
    return value;
  }
  bool initialize(std::string *) override { return true; }
  std::vector<DeviceDescriptor> enumerate_devices(std::string *) override {
    DeviceDescriptor v; v.id = "device"; v.payload_size_bytes = 16; return {v};
  }
  std::unique_ptr<PortModel> make_remote_port_model(const DeviceDescriptor &) override {
    return std::make_unique<PortModel>(PortDescriptor{}, std::vector<PortUrlInfo>{});
  }
  bool get_stream_description(const DeviceDescriptor &, StreamDescription *v,
                              std::string *) override {
    *v = {.buffer_alignment = 1, .payload_size_bytes = 16}; return true;
  }
  bool open_device(DeviceDescriptor *, std::string *) override { return true; }
  bool close_device(const DeviceDescriptor &, std::string *error) override {
    if (!faults.close_success) *error = "native close failed";
    return faults.close_success;
  }
  bool start_stream(const DeviceDescriptor &, std::string *error) override {
    std::scoped_lock lock(faults.mutex);
    if (faults.camera_started) { *error = "camera still started"; return false; }
    faults.camera_started = true;
    assert(faults.borrowed.empty());
    faults.host_stopped = false; faults.release_wait = false;
    faults.stops = 0; faults.queued = 0; faults.wait_entered = false;
    faults.completed = 0; faults.completed_at_stop = 0; return true;
  }
  StopResult stop_stream(const DeviceDescriptor &, std::string *error) override {
    std::scoped_lock lock(faults.mutex);
    ++faults.stops; faults.host_stopped = true;
    faults.completed_at_stop = faults.completed;
    if (faults.operational_success) faults.camera_started = false;
    if (!faults.retain) faults.borrowed.clear();
    *error = faults.operational_success ? "" : "injected stop failure";
    faults.condition.notify_all();
    return {faults.operational_success, !faults.retain};
  }
  bool queue_buffer(const DeviceDescriptor &, void *base, std::size_t,
                    std::size_t, std::string *error) override {
    std::scoped_lock lock(faults.mutex);
    ++faults.queued;
    if (faults.failure == Failure::queue && faults.queued == 2) {
      *error = "queue failed"; faults.condition.notify_all(); return false;
    }
    faults.borrowed.push_back(base); faults.condition.notify_all(); return true;
  }
  bool wait_frame(const DeviceDescriptor &, std::uint64_t timeout_ms, AcquireFrameResult *v,
                  std::string *error) override {
    std::unique_lock lock(faults.mutex);
    auto *completed_buffer = faults.borrowed.front();
    faults.wait_entered = true;
    faults.condition.notify_all();
    const auto ready = [] { return faults.host_stopped || faults.release_wait; };
    if (faults.hold_wait) {
      // The in-flight test explicitly releases this wait after observing stop.
      assert(faults.condition.wait_for(lock, std::chrono::seconds(5), ready));
    } else if (!faults.condition.wait_for(lock,
                   std::chrono::milliseconds(std::min(timeout_ms, std::uint64_t{20})), ready)) {
      v->status = AcquireFrameStatus::kTimeout; return true;
    }
    if ((!faults.hold_wait && faults.host_stopped) || faults.failure == Failure::stopped) {
      v->status = AcquireFrameStatus::kStopped; return true;
    }
    if (faults.failure == Failure::wait) { *error = "wait failed"; return false; }
    v->status = AcquireFrameStatus::kSuccess;
    v->buffer = faults.failure == Failure::fifo ? this : completed_buffer;
    v->bytes_filled = 16;
    ++faults.completed;
    if (faults.failure != Failure::fifo && !faults.borrowed.empty())
      faults.borrowed.erase(faults.borrowed.begin());
    return true;
  }
  bool remote_write_requires_idle(std::uint64_t, std::size_t) const override { return true; }
  std::vector<std::string> remote_invalidated_features(std::uint64_t, std::size_t) const override { return {}; }
  std::vector<std::string> acquisition_invalidated_features() const override { return {}; }
};
std::unique_ptr<RuntimeBackend> make_backend() { return std::make_unique<FaultBackend>(); }
} // namespace
const ProducerDefinition<RuntimeBackend> &producer_definition() {
  static const ProducerDefinition<RuntimeBackend> v{
    .version = "test", .cti_filename = "test.cti", .tl = {"tl.xml", "test"},
    .interface = {"interface.xml", "test"}, .device = {"device.xml", "test"},
    .stream = {"stream.xml", "test"}, .make_backend = make_backend};
  return v;
}
} // namespace gentl_producer_core::runtime

using namespace GenTL;
using namespace gentl_producer_core::runtime;
struct Session {
  TL_HANDLE tl{}; IF_HANDLE iface{}; DEV_HANDLE dev{}; DS_HANDLE ds{};
  BUFFER_HANDLE buffers[2]{};
  Session(Failure failure = Failure::none, std::uint64_t count = GENTL_INFINITE,
          bool hold_wait = false) {
    faults.hold_wait = hold_wait;
    faults.failure = failure; faults.retain = true; faults.operational_success = false;
    assert(GCInitLib() == GC_ERR_SUCCESS);
    assert(TLOpen(&tl) == GC_ERR_SUCCESS);
    assert(TLOpenInterface(tl, "interface", &iface) == GC_ERR_SUCCESS);
    assert(IFOpenDevice(iface, "device", DEVICE_ACCESS_CONTROL, &dev) == GC_ERR_SUCCESS);
    assert(DevOpenDataStream(dev, "device::stream0", &ds) == GC_ERR_SUCCESS);
    for (auto &buffer : buffers) {
      assert(DSAllocAndAnnounceBuffer(ds, 16, nullptr, &buffer) == GC_ERR_SUCCESS);
      assert(DSQueueBuffer(ds, buffer) == GC_ERR_SUCCESS);
    }
    assert(DSStartAcquisition(ds, ACQ_START_FLAGS_DEFAULT, count) == GC_ERR_SUCCESS);
    faults.await([&] { return faults.queued >= (failure == Failure::finite ? 1U : 2U); });
  }
  void allow_retirement(bool success = true) {
    std::scoped_lock lock(faults.mutex);
    faults.retain = false; faults.operational_success = success;
  }
  void assert_retained() {
    auto *stream = Runtime::as_ds(ds);
    {
      std::scoped_lock lock(stream->mutex);
      assert(stream->acquisition_session_active);
      assert(!stream->stop_result.buffers_released);
      for (auto *buffer : stream->sdk_queue) {
        assert(buffer->is_sdk_queued);
        assert(buffer->is_acquiring == (buffer == stream->sdk_queue.front()));
        assert(buffer->is_queued == (buffer != stream->sdk_queue.front()));
      }
    }
    {
      std::scoped_lock lock(stream->parent->mutex);
      assert(stream->parent->acquisition_active);
    }
    // ASan verifies producer-owned storage survives every failed close cascade.
    std::scoped_lock lock(stream->mutex, faults.mutex);
    assert(stream->sdk_queue.size() == faults.borrowed.size());
    for (auto *base : faults.borrowed) {
      assert(std::ranges::any_of(stream->sdk_queue,
          [base](auto *buffer) { return buffer->base == base && buffer->is_sdk_queued; }));
      auto *memory = static_cast<unsigned char *>(base);
      memory[0] = 0x5a; memory[15] = 0xa5;
      assert(memory[0] == 0x5a && memory[15] == 0xa5);
    }
  }
  void guards() {
    assert_retained();
    assert(DSStartAcquisition(ds, ACQ_START_FLAGS_DEFAULT, 1) == GC_ERR_RESOURCE_IN_USE);
    assert(DSQueueBuffer(ds, buffers[1]) == GC_ERR_RESOURCE_IN_USE);
    assert(GCCloseLib() == GC_ERR_RESOURCE_IN_USE);
    for (auto operation : {ACQ_QUEUE_ALL_TO_INPUT, ACQ_QUEUE_UNQUEUED_TO_INPUT,
                           ACQ_QUEUE_INPUT_TO_OUTPUT, ACQ_QUEUE_ALL_DISCARD,
                           ACQ_QUEUE_OUTPUT_DISCARD})
      assert(DSFlushQueue(ds, operation) == GC_ERR_BUSY);
  }
  void finish() {
    assert(TLClose(tl) == GC_ERR_SUCCESS);
    assert(GCCloseLib() == GC_ERR_SUCCESS);
  }
};
void await_stop_request(DS_HANDLE handle) {
  auto *stream = Runtime::as_ds(handle);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    {
      std::scoped_lock lock(stream->mutex);
      if (stream->stop_requested) return;
    }
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}
int main() {
  for (auto flag : {ACQ_STOP_FLAGS_DEFAULT, ACQ_STOP_FLAGS_KILL}) {
    Session s(Failure::none, GENTL_INFINITE, true);
    s.allow_retirement();
    EVENT_HANDLE event{};
    assert(GCRegisterEvent(s.ds, EVENT_NEW_BUFFER, &event) == GC_ERR_SUCCESS);
    faults.await([] { return faults.wait_entered; });
    GC_ERROR stopped = GC_ERR_ERROR;
    std::thread stopper([&] { stopped = DSStopAcquisition(s.ds, flag); });
    if (flag == ACQ_STOP_FLAGS_DEFAULT) {
      await_stop_request(s.ds);
      std::scoped_lock lock(faults.mutex);
      assert(faults.stops == 0); // DEFAULT must not abort the in-flight wait.
      faults.release_wait = true;
      faults.condition.notify_all();
    }
    stopper.join();
    assert(stopped == GC_ERR_SUCCESS);
    assert(faults.stops == 1);
    assert(faults.completed_at_stop == (flag == ACQ_STOP_FLAGS_DEFAULT ? 1U : 0U));
    EVENT_NEW_BUFFER_DATA data{};
    std::size_t size = sizeof(data);
    assert(EventGetData(event, &data, &size, 1000) == GC_ERR_SUCCESS);
    auto *buffer = Runtime::as_buffer(data.BufferHandle);
    assert(buffer->is_incomplete == (flag == ACQ_STOP_FLAGS_KILL));
    assert(buffer->size_filled == (flag == ACQ_STOP_FLAGS_DEFAULT ? 16U : 0U));
    assert(EventKill(event) == GC_ERR_SUCCESS);
    s.finish();
  }
  for (auto flag : {ACQ_STOP_FLAGS_DEFAULT, ACQ_STOP_FLAGS_KILL}) {
    Session s;
    EVENT_HANDLE event{};
    assert(GCRegisterEvent(s.ds, EVENT_NEW_BUFFER, &event) == GC_ERR_SUCCESS);
    assert(DSStopAcquisition(s.ds, flag) == GC_ERR_IO);
    assert(faults.stops == 1); // Worker must not silently retry the failed host stop.
    std::uint64_t fired = 99;
    std::size_t size = sizeof(fired);
    INFO_DATATYPE type{};
    assert(EventGetInfo(event, EVENT_NUM_FIRED, &type, &fired, &size) == GC_ERR_SUCCESS);
    assert(fired == 0); // Failed kill must not publish the still-borrowed buffer.
    s.guards();
    assert(DSFlushQueue(s.ds, ACQ_QUEUE_ALL_DISCARD) == GC_ERR_BUSY);
    assert(DSRevokeBuffer(s.ds, s.buffers[0], nullptr, nullptr) == GC_ERR_BUSY);
    assert(DSClose(s.ds) == GC_ERR_IO); s.assert_retained();
    assert(DevClose(s.dev) == GC_ERR_IO); s.assert_retained();
    assert(IFClose(s.iface) == GC_ERR_IO); s.assert_retained();
    assert(TLClose(s.tl) == GC_ERR_IO); s.assert_retained();
    s.allow_retirement();
    assert(DSStopAcquisition(s.ds, flag) == GC_ERR_SUCCESS);
    assert(DSFlushQueue(s.ds, ACQ_QUEUE_ALL_TO_INPUT) == GC_ERR_SUCCESS);
    assert(DSStartAcquisition(s.ds, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE) == GC_ERR_SUCCESS);
    faults.await([] { return faults.queued == 2; });
    assert(DSStopAcquisition(s.ds, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
    assert(GCUnregisterEvent(s.ds, EVENT_NEW_BUFFER) == GC_ERR_SUCCESS);
    assert(EventKill(event) == GC_ERR_SUCCESS);
    s.finish();
  }
  for (auto failure : {Failure::queue, Failure::wait, Failure::fifo, Failure::stopped, Failure::finite}) {
    Session s(failure, failure == Failure::finite ? 1 : GENTL_INFINITE);
    {
      std::scoped_lock lock(faults.mutex);
      faults.release_wait = true; faults.condition.notify_all();
    }
    faults.await([] { return faults.stops == 1; });
    Runtime::as_ds(s.ds)->worker.join();
    s.guards();
    assert(DSClose(s.ds) == GC_ERR_IO); s.assert_retained();
    s.allow_retirement();
    const auto result = DSStopAcquisition(s.ds, ACQ_STOP_FLAGS_DEFAULT);
    assert(result == ((failure == Failure::queue || failure == Failure::wait || failure == Failure::fifo)
                     ? GC_ERR_IO : GC_ERR_SUCCESS));
    s.finish();
  }
  for (auto failure : {Failure::wait, Failure::finite}) {
    Session s(failure, failure == Failure::finite ? 1 : GENTL_INFINITE);
    s.allow_retirement(false);
    {
      std::scoped_lock lock(faults.mutex);
      faults.release_wait = true; faults.condition.notify_all();
    }
    faults.await([] { return faults.stops == 1; });
    Runtime::as_ds(s.ds)->worker.join();
    assert(faults.borrowed.empty());
    assert(faults.camera_started);
    assert(Runtime::as_dev(s.dev)->acquisition_active);
    assert(DSStartAcquisition(s.ds, ACQ_START_FLAGS_DEFAULT, 1) == GC_ERR_RESOURCE_IN_USE);
    assert(DSStopAcquisition(s.ds, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_IO);
    assert(faults.stops == 2);
    assert(Runtime::as_ds(s.ds)->acquisition_session_active);
    assert(Runtime::as_dev(s.dev)->acquisition_active);
    s.allow_retirement();
    assert(DSStopAcquisition(s.ds, ACQ_STOP_FLAGS_DEFAULT) ==
           (failure == Failure::wait ? GC_ERR_IO : GC_ERR_SUCCESS));
    assert(faults.stops == 3 && !faults.camera_started);
    assert(DSFlushQueue(s.ds, ACQ_QUEUE_ALL_TO_INPUT) == GC_ERR_SUCCESS);
    assert(DSStartAcquisition(s.ds, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE) == GC_ERR_SUCCESS);
    faults.await([] { return faults.queued == 2; });
    assert(DSStopAcquisition(s.ds, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
    s.finish();
  }
  {
    Session s;
    s.allow_retirement(false);
    assert(DSStopAcquisition(s.ds, ACQ_STOP_FLAGS_KILL) == GC_ERR_IO);
    assert(faults.borrowed.empty());
    assert(DSFlushQueue(s.ds, ACQ_QUEUE_ALL_DISCARD) == GC_ERR_SUCCESS);
    assert(DSRevokeBuffer(s.ds, s.buffers[0], nullptr, nullptr) == GC_ERR_SUCCESS);
    assert(DSStartAcquisition(s.ds, ACQ_START_FLAGS_DEFAULT, 1) == GC_ERR_RESOURCE_IN_USE);
    assert(faults.camera_started);
    const auto prior_stops = faults.stops;
    s.allow_retirement();
    assert(DSStopAcquisition(s.ds, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
    assert(faults.stops == prior_stops + 1);
    assert(!faults.camera_started);
    assert(DSQueueBuffer(s.ds, s.buffers[1]) == GC_ERR_SUCCESS);
    assert(DSStartAcquisition(s.ds, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE) == GC_ERR_SUCCESS);
    faults.await([] { return faults.queued == 1; });
    assert(DSStopAcquisition(s.ds, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
    s.finish();
  }
  {
    Session s;
    {
      std::scoped_lock lock(faults.mutex);
      faults.operational_success = true; // Command success does not retire memory.
    }
    assert(DSStopAcquisition(s.ds, ACQ_STOP_FLAGS_KILL) == GC_ERR_IO);
    assert(faults.stops == 1);
    s.guards();
    s.allow_retirement();
    assert(DSStopAcquisition(s.ds, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
    s.finish();
  }
  {
    Session s;
    s.allow_retirement(false);
    assert(DSClose(s.ds) == GC_ERR_IO);
    assert(faults.borrowed.empty());
    s.allow_retirement();
    assert(DSClose(s.ds) == GC_ERR_SUCCESS);
    faults.close_success = false;
    assert(DevClose(s.dev) == GC_ERR_IO);
    assert(GCCloseLib() == GC_ERR_RESOURCE_IN_USE);
    faults.close_success = true;
    s.finish();
  }
}
