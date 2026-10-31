// Shared runtime state operations; included inside the runtime namespace.

GenTL::GC_ERROR Runtime::init() {
  std::scoped_lock lock(mutex_);
  if (initialized_) {
    set_last_error(GenTL::GC_ERR_RESOURCE_IN_USE,
                   "GenTL library is already initialized");
    return GenTL::GC_ERR_RESOURCE_IN_USE;
  }

  std::string error_text;
  if (backend_->initialize(&error_text)) {
    initialized_ = true;
    return GenTL::GC_ERR_SUCCESS;
  }

  set_last_error(GenTL::GC_ERR_NOT_AVAILABLE, error_text);
  return GenTL::GC_ERR_NOT_AVAILABLE;
}

GenTL::GC_ERROR Runtime::close() {
  std::scoped_lock lock(mutex_);
  if (!initialized_) {
    set_last_error(GenTL::GC_ERR_NOT_INITIALIZED,
                   "GenTL library is not initialized");
    return GenTL::GC_ERR_NOT_INITIALIZED;
  }
  if (system_open_) {
    set_last_error(GenTL::GC_ERR_RESOURCE_IN_USE, "system module is still open");
    return GenTL::GC_ERR_RESOURCE_IN_USE;
  }
  initialized_ = false;
  system_open_ = false;
  cached_cameras_.clear();
  camera_list_valid_ = false;
  device_opens_.clear();
  backend_ = producer_definition().make_backend();
  return GenTL::GC_ERR_SUCCESS;
}

bool Runtime::is_initialized() const {
  std::scoped_lock lock(mutex_);
  return initialized_;
}

bool Runtime::claim_system_open() {
  std::scoped_lock lock(mutex_);
  if (system_open_) {
    return false;
  }
  system_open_ = true;
  return true;
}

void Runtime::release_system_open() {
  std::scoped_lock lock(mutex_);
  system_open_ = false;
}

void Runtime::set_last_error(GenTL::GC_ERROR code, std::string text) {
  gentl_producer_core::set_last_error(code, std::move(text));
}

std::pair<GenTL::GC_ERROR, std::string> Runtime::last_error() const {
  return gentl_producer_core::last_error();
}

std::unique_ptr<PortModel>
Runtime::make_remote_port_model(const DeviceDescriptor &device) {
  return backend_->make_remote_port_model(device);
}

bool Runtime::get_stream_description(const DeviceDescriptor &device,
                                     StreamDescription *description,
                                     std::string *error_text) {
  if (!backend_->get_stream_description(device, description, error_text)) {
    return false;
  }
  const auto alignment = description->buffer_alignment;
  if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
    if (error_text != nullptr) {
      *error_text = "backend buffer alignment must be a positive power of two";
    }
    return false;
  }
  return true;
}

bool Runtime::start_stream(const DeviceDescriptor &device,
                           std::string *error_text) {
  return backend_->start_stream(device, error_text);
}

GenTL::GC_ERROR Runtime::refresh_cameras(bool8_t *changed) {
  std::scoped_lock lock(mutex_);
  return refresh_cameras_locked(changed);
}

Runtime::DeviceList Runtime::snapshot_cameras(GenTL::GC_ERROR *error_code) {
  std::scoped_lock lock(mutex_);

  if (!camera_list_valid_) {
    const auto result = refresh_cameras_locked(nullptr);
    if (result != GenTL::GC_ERR_SUCCESS) {
      if (error_code != nullptr) {
        *error_code = result;
      }
      return {};
    }
  }

  if (error_code != nullptr) {
    *error_code = GenTL::GC_ERR_SUCCESS;
  }
  return cached_cameras_;
}

std::optional<DeviceDescriptor>
Runtime::find_camera_by_id(std::string_view device_id,
                           GenTL::GC_ERROR *error_code) {
  std::scoped_lock lock(mutex_);

  if (!camera_list_valid_) {
    const auto result = refresh_cameras_locked(nullptr);
    if (result != GenTL::GC_ERR_SUCCESS) {
      if (error_code != nullptr) {
        *error_code = result;
      }
      return std::nullopt;
    }
  }

  if (error_code != nullptr) {
    *error_code = GenTL::GC_ERR_SUCCESS;
  }

  const auto it =
      std::ranges::find(cached_cameras_, device_id, &DeviceDescriptor::id);
  if (it == cached_cameras_.end()) {
    return std::nullopt;
  }
  return *it;
}

bool Runtime::claim_device_open(std::string_view device_id,
                                GenTL::DEVICE_ACCESS_FLAGS access) {
  std::scoped_lock lock(mutex_);
  auto &state = device_opens_[std::string(device_id)];
  if (state.readonly_count != 0 || state.control_open || state.exclusive_open) {
    return false;
  }
  if (access == GenTL::DEVICE_ACCESS_READONLY) {
    ++state.readonly_count;
    return true;
  }
  if (access == GenTL::DEVICE_ACCESS_CONTROL) {
    state.control_open = true;
    return true;
  }
  if (access == GenTL::DEVICE_ACCESS_EXCLUSIVE) {
    state.exclusive_open = true;
    return true;
  }
  return false;
}

void Runtime::release_device_open(std::string_view device_id,
                                  GenTL::DEVICE_ACCESS_FLAGS access) {
  std::scoped_lock lock(mutex_);
  const auto it = device_opens_.find(std::string(device_id));
  if (it == device_opens_.end())
    return;
  auto &state = it->second;
  if (access == GenTL::DEVICE_ACCESS_READONLY && state.readonly_count != 0) {
    --state.readonly_count;
  } else if (access == GenTL::DEVICE_ACCESS_CONTROL) {
    state.control_open = false;
  } else if (access == GenTL::DEVICE_ACCESS_EXCLUSIVE) {
    state.exclusive_open = false;
  }
  if (state.readonly_count == 0 && !state.control_open &&
      !state.exclusive_open) {
    device_opens_.erase(it);
  }
}
