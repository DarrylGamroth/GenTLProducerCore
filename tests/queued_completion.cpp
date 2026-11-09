#include "gentl_producer_core/detail/runtime.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace gentl_producer_core::runtime {
namespace {
struct Control {
  std::mutex mutex;
  std::condition_variable condition;
  BufferCompletionOrder order = BufferCompletionOrder::kAny;
  std::vector<void *> borrowed;
  std::deque<AcquireFrameResult> ready;
  std::size_t submissions = 0;
  std::size_t waits = 0;
  std::uint64_t last_timeout = 0;
  unsigned stops = 0;
  bool stopped = false;
  bool retain = false;
  bool notifications = false;
  bool fail_start = false;
  bool fault_at_start = false;
  bool fault = false;
  void *publish_during_poll = nullptr;
  std::function<void()> callback;

  void await(auto predicate) {
    std::unique_lock lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(5), predicate));
  }
  void complete(void *base, std::uint64_t stamp = 100,
                bool incomplete = false) {
    std::scoped_lock lock(mutex);
    ready.push_back({.status = AcquireFrameStatus::kSuccess,
                     .buffer = base,
                     .bytes_filled = 16,
                     .timestamp_ns = stamp,
                     .incomplete = incomplete});
    if (callback)
      callback();
    condition.notify_all();
  }
} control;

class Backend final : public QueuedBackend {
public:
  const BackendInfo &info() const override {
    static const BackendInfo value = [] {
      BackendInfo result;
      result.system_id = "system";
      result.interface_id = "interface";
      return result;
    }();
    return value;
  }
  bool initialize(std::string *) override { return true; }
  std::vector<DeviceDescriptor> enumerate_devices(std::string *) override {
    DeviceDescriptor device;
    device.id = "device";
    device.width = 4;
    device.height = 4;
    device.payload_size_bytes = 16;
    return {device};
  }
  std::unique_ptr<PortModel>
  make_remote_port_model(const DeviceDescriptor &) override {
    return std::make_unique<PortModel>(PortDescriptor{},
                                       std::vector<PortUrlInfo>{});
  }
  bool get_stream_description(const DeviceDescriptor &, StreamDescription *s,
                              std::string *) override {
    *s = {.width = 4,
          .height = 4,
          .payload_size_bytes = 16,
          .pixel_format = 0x01080001,
          .pixel_size_bits = 8};
    return true;
  }
  BufferCompletionOrder
  completion_order(const DeviceDescriptor &d) const override {
    return control.order == BufferCompletionOrder::kFifo
               ? QueuedBackend::completion_order(d)
               : control.order;
  }
  bool set_completion_callback(const DeviceDescriptor &,
                               std::function<void()> callback) override {
    std::scoped_lock lock(control.mutex);
    if (!control.notifications)
      return false;
    control.callback = std::move(callback);
    return true;
  }
  bool open_device(DeviceDescriptor *, std::string *) override { return true; }
  bool close_device(const DeviceDescriptor &, std::string *) override {
    return true;
  }
  bool start_stream(const DeviceDescriptor &, std::string *error) override {
    std::scoped_lock lock(control.mutex);
    assert(control.borrowed.empty());
    if (control.fail_start) {
      *error = "start failed";
      return false;
    }
    control.fault = control.fault_at_start;
    if (control.fault && control.callback)
      control.callback();
    control.ready.clear();
    control.submissions = control.stops = 0;
    control.waits = 0;
    control.stopped = false;
    return true;
  }
  StopResult stop_stream(const DeviceDescriptor &,
                         std::string *error) override {
    std::scoped_lock lock(control.mutex);
    ++control.stops;
    control.stopped = true;
    if (!control.retain)
      control.borrowed.clear();
    else
      *error = "retirement failed";
    if (control.callback)
      control.callback();
    control.condition.notify_all();
    return {!control.retain, !control.retain};
  }
  bool queue_buffer(const DeviceDescriptor &, void *base,
                    std::size_t allocation, std::size_t transfer,
                    std::string *) override {
    std::scoped_lock lock(control.mutex);
    assert(allocation == 16 && transfer == 16);
    assert(std::ranges::find(control.borrowed, base) == control.borrowed.end());
    control.borrowed.push_back(base);
    ++control.submissions;
    control.condition.notify_all();
    return true;
  }
  bool wait_frame(const DeviceDescriptor &, std::uint64_t timeout,
                  AcquireFrameResult *r, std::string *error) override {
    std::unique_lock lock(control.mutex);
    ++control.waits;
    control.last_timeout = timeout;
    control.condition.notify_all();
    if (control.fault) {
      *error = "asynchronous native fault";
      return false;
    }
    if (control.publish_during_poll) {
      control.ready.push_back({.status = AcquireFrameStatus::kSuccess,
                               .buffer = control.publish_during_poll,
                               .bytes_filled = 16,
                               .timestamp_ns = 100});
      control.publish_during_poll = nullptr;
      assert(control.callback);
      control.callback();
      *r = {}; // Completion arrived immediately after the readiness check.
      return true;
    }
    if (!control.condition.wait_for(
            lock,
            std::chrono::milliseconds(std::min(timeout, std::uint64_t{20})),
            [] { return control.stopped || !control.ready.empty(); })) {
      *r = {};
      return true;
    }
    if (control.stopped) {
      *r = {.status = AcquireFrameStatus::kStopped};
      return true;
    }
    *r = control.ready.front();
    control.ready.pop_front();
    const auto it = std::ranges::find(control.borrowed, r->buffer);
    if (it != control.borrowed.end())
      control.borrowed.erase(it);
    return true;
  }
  bool remote_write_requires_idle(std::uint64_t, std::size_t) const override {
    return true;
  }
  std::vector<std::string>
  remote_invalidated_features(std::uint64_t, std::size_t) const override {
    return {};
  }
  std::vector<std::string> acquisition_invalidated_features() const override {
    return {};
  }
};
std::unique_ptr<RuntimeBackend> make_backend() {
  return std::make_unique<Backend>();
}
} // namespace
const ProducerDefinition<RuntimeBackend> &producer_definition() {
  static const ProducerDefinition<RuntimeBackend> value{
      .version = "test",
      .cti_filename = "test.cti",
      .tl = {"tl.xml", "test"},
      .interface = {"interface.xml", "test"},
      .device = {"device.xml", "test"},
      .stream = {"stream.xml", "test"},
      .make_backend = make_backend};
  return value;
}
} // namespace gentl_producer_core::runtime

using namespace GenTL;
using namespace gentl_producer_core;
using namespace gentl_producer_core::runtime;
namespace {
struct Session {
  TL_HANDLE tl{};
  IF_HANDLE iface{};
  DEV_HANDLE device{};
  DS_HANDLE stream{};
  EVENT_HANDLE event{};
  std::array<BUFFER_HANDLE, 4> buffers{};
  std::array<void *, 4> bases{};
  explicit Session(BufferCompletionOrder order = BufferCompletionOrder::kAny,
                   bool notifications = false) {
    assert(!control.callback);
    control.notifications = notifications;
    control.fail_start = control.fault_at_start = control.fault = false;
    control.publish_during_poll = nullptr;
    control.order = order;
    control.retain = false;
    assert(GCInitLib() == GC_ERR_SUCCESS);
    assert(TLOpen(&tl) == GC_ERR_SUCCESS);
    assert(TLOpenInterface(tl, "interface", &iface) == GC_ERR_SUCCESS);
    assert(IFOpenDevice(iface, "device", DEVICE_ACCESS_CONTROL, &device) ==
           GC_ERR_SUCCESS);
    assert(DevOpenDataStream(device, "device::stream0", &stream) ==
           GC_ERR_SUCCESS);
    assert(GCRegisterEvent(stream, EVENT_NEW_BUFFER, &event) == GC_ERR_SUCCESS);
    for (std::size_t i = 0; i < buffers.size(); ++i) {
      assert(DSAllocAndAnnounceBuffer(stream, 16, &buffers[i], &buffers[i]) ==
             GC_ERR_SUCCESS);
      bases[i] = Runtime::as_buffer(buffers[i])->base;
    }
  }
  void start(std::uint64_t count = GENTL_INFINITE) {
    for (std::size_t i = 0; i < 3; ++i)
      assert(DSQueueBuffer(stream, buffers[i]) == GC_ERR_SUCCESS);
    assert(DSStartAcquisition(stream, ACQ_START_FLAGS_DEFAULT, count) ==
           GC_ERR_SUCCESS);
    control.await([] { return control.submissions >= 3; });
  }
  void receive(std::size_t index, std::uint64_t id, std::uint64_t stamp = 100,
               bool incomplete = false) {
    EVENT_NEW_BUFFER_DATA data{};
    std::size_t length = sizeof(data);
    assert(EventGetData(event, &data, &length, 2000) == GC_ERR_SUCCESS);
    assert(data.BufferHandle == buffers[index] &&
           data.pUserPointer == &buffers[index]);
    auto *stream_data = Runtime::as_ds(stream);
    std::scoped_lock lock(stream_data->mutex);
    auto *buffer = Runtime::as_buffer(data.BufferHandle);
    assert(buffer->frame_id == id && buffer->timestamp == stamp);
    assert(buffer->size_filled == 16 && buffer->width == 4 &&
           buffer->height == 4);
    assert(buffer->is_incomplete == incomplete);
    assert(!buffer->is_sdk_queued && !buffer->is_acquiring &&
           !buffer->awaiting_delivery);
  }
  void join_worker() {
    control.await([] { return control.stops != 0; });
    auto *s = Runtime::as_ds(stream);
    if (s->worker.joinable())
      s->worker.join();
  }
  ~Session() {
    assert(GCUnregisterEvent(stream, EVENT_NEW_BUFFER) == GC_ERR_SUCCESS);
    assert(EventKill(event) == GC_ERR_SUCCESS);
    assert(TLClose(tl) == GC_ERR_SUCCESS);
    assert(GCCloseLib() == GC_ERR_SUCCESS);
    assert(!control.callback);
  }
};

void reordered_completion(bool notifications) {
  Session s(BufferCompletionOrder::kAny, notifications);
  s.start();
  control.await([] { return control.waits != 0; });
  {
    std::scoped_lock lock(control.mutex);
    assert(control.last_timeout == 0);
  }
  control.complete(s.bases[1], 201, true);
  s.receive(1, 1, 201, true);
  // Requeue the middle completion while its former neighbours remain borrowed.
  assert(DSQueueBuffer(s.stream, s.buffers[1]) == GC_ERR_SUCCESS);
  control.await([] { return control.submissions == 4; });
  assert(DSQueueBuffer(s.stream, s.buffers[1]) == GC_ERR_RESOURCE_IN_USE);
  assert(DSRevokeBuffer(s.stream, s.buffers[0], nullptr, nullptr) ==
         GC_ERR_BUSY);
  control.complete(s.bases[2], 202);
  s.receive(2, 2, 202);
  control.complete(s.bases[0], 203);
  s.receive(0, 3, 203);
  control.complete(s.bases[1], 204);
  s.receive(1, 4, 204);
  assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
}

void returned_order_differs_from_capture(bool notifications) {
  Session s(BufferCompletionOrder::kAny, notifications);
  // A finite run with fewer remaining deliveries than the pool must still
  // forward every return; a fixed hardware ring may need the final return
  // to grant credits for its first buffer.
  s.start(5);
  for (std::size_t i = 0; i < 3; ++i) {
    control.complete(s.bases[i]);
    s.receive(i, i + 1);
  }
  for (std::size_t i : {2U, 1U, 0U})
    assert(DSQueueBuffer(s.stream, s.buffers[i]) == GC_ERR_SUCCESS);
  control.await([] { return control.submissions == 6; });
  control.complete(s.bases[0]);
  s.receive(0, 4);
  control.complete(s.bases[1]);
  s.receive(1, 5);
  s.join_worker();
  assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
  auto *stream = Runtime::as_ds(s.stream);
  std::scoped_lock lock(stream->mutex);
  assert(stream->delivered_count == 5 && stream->sdk_queue.empty());
  assert(stream->input_queue.size() == 1 &&
         stream->input_queue.front()->base == s.bases[2]);
}

void finite_shorter_than_pool(bool notifications) {
  Session s(BufferCompletionOrder::kAny, notifications);
  s.start(1);
  control.complete(s.bases[2]);
  s.receive(2, 1);
  s.join_worker();
  assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
}

void empty_input_wakeup(bool notifications) {
  for (bool close_while_idle : {false, true}) {
    Session s(BufferCompletionOrder::kAny, notifications);
    assert(DSStartAcquisition(s.stream, ACQ_START_FLAGS_DEFAULT,
                              GENTL_INFINITE) == GC_ERR_SUCCESS);
    if (close_while_idle)
      continue; // Session close must also wake a worker with no queued input.
    for (std::size_t i = 0; i < 2; ++i) {
      assert(DSQueueBuffer(s.stream, s.buffers[i]) == GC_ERR_SUCCESS);
      control.await([&] { return control.submissions == i + 1; });
      control.complete(s.bases[i]);
      s.receive(i, i + 1);
    }
    assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_DEFAULT) ==
           GC_ERR_SUCCESS);
  }
}

void bad_completions(bool notifications) {
  // Default FIFO behaviour must continue rejecting a valid non-head address.
  {
    Session s(BufferCompletionOrder::kFifo);
    s.start();
    control.await([] { return control.waits != 0; });
    {
      std::scoped_lock lock(control.mutex);
      assert(control.last_timeout == 1000);
    }
    control.complete(s.bases[1]);
    s.join_worker();
    assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_IO);
    assert(Runtime::instance().last_error().second.find("FIFO") !=
           std::string::npos);
  }
  int foreign{};
  for (int kind = 0; kind < 4; ++kind) {
    Session s(BufferCompletionOrder::kAny, notifications);
    s.start();
    if (kind == 3) {
      control.complete(s.bases[0]);
      s.receive(0, 1);
    }
    void *base = kind == 0   ? nullptr
                 : kind == 1 ? &foreign
                 : kind == 2 ? s.bases[3]
                             : s.bases[0];
    control.complete(base);
    s.join_worker();
    // Invalid identities must not produce an event or increase delivery count.
    assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_IO);
    EVENT_NEW_BUFFER_DATA data{};
    std::size_t length = sizeof(data);
    assert(EventGetData(s.event, &data, &length, 0) == GC_ERR_TIMEOUT);
    assert(Runtime::as_ds(s.stream)->delivered_count == (kind == 3 ? 1U : 0U));
  }
}

void failed_retirement_after_reorder(bool notifications) {
  Session s(BufferCompletionOrder::kAny, notifications);
  s.start();
  control.complete(s.bases[1]);
  s.receive(1, 1);
  {
    std::scoped_lock lock(control.mutex);
    control.retain = true;
  }
  assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_KILL) == GC_ERR_IO);
  auto *stream = Runtime::as_ds(s.stream);
  {
    std::scoped_lock lock(stream->mutex);
    assert(stream->sdk_queue.size() == 2);
    assert(stream->sdk_queue[0]->base == s.bases[0]);
    assert(stream->sdk_queue[1]->base == s.bases[2]);
  }
  assert(DSRevokeBuffer(s.stream, s.buffers[0], nullptr, nullptr) ==
         GC_ERR_BUSY);
  assert(DSFlushQueue(s.stream, ACQ_QUEUE_ALL_DISCARD) == GC_ERR_BUSY);
  {
    std::scoped_lock lock(control.mutex);
    control.retain = false;
  }
  assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_KILL) == GC_ERR_SUCCESS);
  EVENT_NEW_BUFFER_DATA data{};
  std::size_t length = sizeof(data);
  // Any-order backends expose no single active head to synthesize on KILL.
  assert(EventGetData(s.event, &data, &length, 0) == GC_ERR_TIMEOUT);
  assert(DSRevokeBuffer(s.stream, s.buffers[1], nullptr, nullptr) ==
         GC_ERR_SUCCESS);
}
void completion_before_park() {
  Session s(BufferCompletionOrder::kAny, true);
  control.publish_during_poll = s.bases[1];
  s.start();
  s.receive(1, 1);
  assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
}

void polling_fallback_is_preserved() {
  Session s;
  s.start();
  control.await([] { return control.waits != 0; });
  {
    std::unique_lock lock(control.mutex);
    assert(!control.callback);
    const auto initial = control.waits;
    assert(control.condition.wait_for(lock, std::chrono::seconds(2), [&] {
      return control.waits >= initial + 3;
    }));
  }
  assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_SUCCESS);
}

void notification_idle_burst_fault_and_lifetime() {
  std::function<void()> late;
  {
    Session s(BufferCompletionOrder::kAny, true);
    s.start();
    control.await([] { return control.waits != 0; });
    {
      std::unique_lock lock(control.mutex);
      const auto initial = control.waits;
      // Idle call count must remain stable without a completion/input/stop.
      assert(
          !control.condition.wait_for(lock, std::chrono::milliseconds(20), [&] {
            return control.waits != initial;
          }));
      assert(control.callback);
      late = control.callback;
      control.callback(); // A spurious/coalesced wake must be harmless.
    }
    control.await([] { return control.waits >= 2; });
    {
      std::scoped_lock lock(control.mutex);
      // One notification can represent a burst; readiness remains level state.
      for (auto index : {2U, 0U, 1U})
        control.ready.push_back({.status = AcquireFrameStatus::kSuccess,
                                 .buffer = s.bases[index],
                                 .bytes_filled = 16,
                                 .timestamp_ns = 100});
      control.callback();
    }
    s.receive(2, 1);
    s.receive(0, 2);
    s.receive(1, 3);
    {
      std::scoped_lock lock(control.mutex);
      assert(control.borrowed.empty());
      control.fault = true; // All images consumer-held; no SDK queue entries.
      control.callback();
    }
    s.join_worker();
    assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_IO);
  }
  // A retained in-flight copy owns only shared wake state, never the stream.
  late();
}

void startup_notifications() {
  for (bool failed_start : {true, false}) {
    Session s(BufferCompletionOrder::kAny, true);
    control.fail_start = failed_start;
    control.fault_at_start = !failed_start;
    const auto result =
        DSStartAcquisition(s.stream, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE);
    if (failed_start) {
      assert(result == GC_ERR_IO);
      assert(!control.callback);
    } else {
      assert(result == GC_ERR_SUCCESS);
      s.join_worker(); // Fault callback was inline at start, with no
                       // submissions.
      assert(DSStopAcquisition(s.stream, ACQ_STOP_FLAGS_DEFAULT) == GC_ERR_IO);
    }
  }
}

} // namespace
int main() {
  for (bool notifications : {false, true}) {
    reordered_completion(notifications);
    returned_order_differs_from_capture(notifications);
    finite_shorter_than_pool(notifications);
    empty_input_wakeup(notifications);
    bad_completions(notifications);
    failed_retirement_after_reorder(notifications);
  }
  completion_before_park();
  polling_fallback_is_preserved();
  notification_idle_burst_fault_and_lifetime();
  startup_notifications();
}
