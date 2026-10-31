// Shared GenTL ABI. Included in the selected ownership runtime translation unit.

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCGetLastError(GC_ERROR *piErrorCode,
                                                     char *sErrText,
                                                     size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  if (piErrorCode == nullptr || piSize == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }

  const auto [code, text] =
      gentl_producer_core::runtime::Runtime::instance().last_error();
  *piErrorCode = code;
  return gentl_producer_core::runtime::copy_string(
      text, INFO_DATATYPE_STRING, INFO_DATATYPE_STRING, sErrText, piSize);
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCInitLib(void) {
  return gentl_producer_core::runtime::Runtime::instance().init();
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCCloseLib(void) {
  return gentl_producer_core::runtime::Runtime::instance().close();
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE TLOpen(TL_HANDLE *phTL) {
  GENTL_REQUIRE_INITIALIZED();
  if (phTL == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  if (!gentl_producer_core::runtime::Runtime::instance().claim_system_open()) {
    return gentl_producer_core::runtime::resource_in_use(
        "System module is already open");
  }
  *phTL = new gentl_producer_core::runtime::TlHandleData();
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE TLGetInfo(TL_HANDLE hTL,
                                                TL_INFO_CMD iInfoCmd,
                                                INFO_DATATYPE *piType,
                                                void *pBuffer, size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  if (gentl_producer_core::runtime::Runtime::as_tl(hTL) == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  return GCGetInfo(iInfoCmd, piType, pBuffer, piSize);
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
TLGetNumInterfaces(TL_HANDLE hTL, uint32_t *piNumIfaces) {
  GENTL_REQUIRE_INITIALIZED();
  if (gentl_producer_core::runtime::Runtime::as_tl(hTL) == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (piNumIfaces == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  *piNumIfaces = 1;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE TLGetInterfaceID(TL_HANDLE hTL,
                                                       uint32_t iIndex,
                                                       char *sID,
                                                       size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  if (gentl_producer_core::runtime::Runtime::as_tl(hTL) == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (iIndex != 0U) {
    return GC_ERR_INVALID_INDEX;
  }
  return gentl_producer_core::runtime::copy_string(
      gentl_producer_core::runtime::Runtime::instance()
          .backend_info()
          .interface_id,
      INFO_DATATYPE_STRING, INFO_DATATYPE_STRING, sID, piSize);
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE TLGetInterfaceInfo(
    TL_HANDLE hTL, const char *sIfaceID, INTERFACE_INFO_CMD iInfoCmd,
    INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  if (gentl_producer_core::runtime::Runtime::as_tl(hTL) == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  const auto &backend_info =
      gentl_producer_core::runtime::Runtime::instance().backend_info();
  if (sIfaceID == nullptr ||
      std::string(sIfaceID) != backend_info.interface_id) {
    return gentl_producer_core::runtime::invalid_id("unknown interface id");
  }

  switch (iInfoCmd) {
  case INTERFACE_INFO_ID:
    return gentl_producer_core::runtime::info_string(backend_info.interface_id,
                                                     piType, pBuffer, piSize);
  case INTERFACE_INFO_DISPLAYNAME:
    return gentl_producer_core::runtime::info_string(
        backend_info.interface_display_name, piType, pBuffer, piSize);
  case INTERFACE_INFO_TLTYPE:
    return gentl_producer_core::runtime::info_string(backend_info.tl_type,
                                                     piType, pBuffer, piSize);
  default:
    return GC_ERR_INVALID_ID;
  }
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE TLUpdateInterfaceList(TL_HANDLE hTL,
                                                            bool8_t *pbChanged,
                                                            uint64_t) {
  GENTL_REQUIRE_INITIALIZED();
  if (gentl_producer_core::runtime::Runtime::as_tl(hTL) == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (pbChanged != nullptr) {
    *pbChanged = 0;
  }
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE IFGetInfo(IF_HANDLE hIface,
                                                INTERFACE_INFO_CMD iInfoCmd,
                                                INFO_DATATYPE *piType,
                                                void *pBuffer, size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::IfHandleData *iface =
      gentl_producer_core::runtime::Runtime::as_if(hIface);
  if (iface == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  switch (iInfoCmd) {
  case INTERFACE_INFO_ID:
    return gentl_producer_core::runtime::info_string(iface->id, piType, pBuffer,
                                                     piSize);
  case INTERFACE_INFO_DISPLAYNAME:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::Runtime::instance()
            .backend_info()
            .interface_display_name,
        piType, pBuffer, piSize);
  case INTERFACE_INFO_TLTYPE:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::Runtime::instance()
            .backend_info()
            .tl_type,
        piType, pBuffer, piSize);
  default:
    return GC_ERR_INVALID_ID;
  }
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE IFGetNumDevices(IF_HANDLE hIface,
                                                      uint32_t *piNumDevices) {
  GENTL_REQUIRE_INITIALIZED();
  if (gentl_producer_core::runtime::Runtime::as_if(hIface) == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (piNumDevices == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }

  GC_ERROR error_code = GC_ERR_SUCCESS;
  const auto cameras =
      gentl_producer_core::runtime::Runtime::instance().snapshot_cameras(
          &error_code);
  if (error_code != GC_ERR_SUCCESS) {
    return error_code;
  }

  *piNumDevices = static_cast<uint32_t>(cameras.size());
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE IFGetDeviceID(IF_HANDLE hIface,
                                                    uint32_t iIndex,
                                                    char *sIDeviceID,
                                                    size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  if (gentl_producer_core::runtime::Runtime::as_if(hIface) == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  GC_ERROR error_code = GC_ERR_SUCCESS;
  const auto cameras =
      gentl_producer_core::runtime::Runtime::instance().snapshot_cameras(
          &error_code);
  if (error_code != GC_ERR_SUCCESS) {
    return error_code;
  }
  if (iIndex >= cameras.size()) {
    return GC_ERR_INVALID_INDEX;
  }

  return gentl_producer_core::runtime::copy_string(
      cameras[iIndex].id, INFO_DATATYPE_STRING, INFO_DATATYPE_STRING,
      sIDeviceID, piSize);
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE IFUpdateDeviceList(IF_HANDLE hIface,
                                                         bool8_t *pbChanged,
                                                         uint64_t) {
  GENTL_REQUIRE_INITIALIZED();
  if (gentl_producer_core::runtime::Runtime::as_if(hIface) == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  return gentl_producer_core::runtime::Runtime::instance().refresh_cameras(
      pbChanged);
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DevGetPort(DEV_HANDLE hDevice,
                                                 PORT_HANDLE *phRemoteDevice) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DevHandleData *device =
      gentl_producer_core::runtime::Runtime::as_dev(hDevice);
  if (device == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (phRemoteDevice == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  *phRemoteDevice = device->remote_port.get();
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
DevGetNumDataStreams(DEV_HANDLE hDevice, uint32_t *piNumDataStreams) {
  GENTL_REQUIRE_INITIALIZED();
  if (gentl_producer_core::runtime::Runtime::as_dev(hDevice) == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (piNumDataStreams == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  *piNumDataStreams = 1;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DevGetDataStreamID(DEV_HANDLE hDevice,
                                                         uint32_t iIndex,
                                                         char *sDataStreamID,
                                                         size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DevHandleData *device =
      gentl_producer_core::runtime::Runtime::as_dev(hDevice);
  if (device == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (iIndex != 0U) {
    return GC_ERR_INVALID_INDEX;
  }
  return gentl_producer_core::runtime::copy_string(
      device->device.id + "::stream0", INFO_DATATYPE_STRING,
      INFO_DATATYPE_STRING, sDataStreamID, piSize);
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DevGetInfo(DEV_HANDLE hDevice,
                                                 DEVICE_INFO_CMD iInfoCmd,
                                                 INFO_DATATYPE *piType,
                                                 void *pBuffer,
                                                 size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DevHandleData *device =
      gentl_producer_core::runtime::Runtime::as_dev(hDevice);
  if (device == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  return gentl_producer_core::runtime::device_info_response(
      device->device,
      gentl_producer_core::runtime::access_status_from_flags(device->access),
      iInfoCmd, piType, pBuffer, piSize);
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferID(DS_HANDLE hDataStream,
                                                    uint32_t iIndex,
                                                    BUFFER_HANDLE *phBuffer) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DsHandleData *stream =
      gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  if (stream == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (phBuffer == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  std::scoped_lock lock(stream->mutex);
  if (iIndex >= stream->buffers.size()) {
    return GC_ERR_INVALID_INDEX;
  }
  *phBuffer = stream->buffers[iIndex].get();
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
GCReadPortStacked(PORT_HANDLE hPort, PORT_REGISTER_STACK_ENTRY *pEntries,
                  size_t *piNumEntries) {
  GENTL_REQUIRE_INITIALIZED();
  if (pEntries == nullptr || piNumEntries == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }

  std::size_t processed = 0;
  for (; processed < *piNumEntries; ++processed) {
    size_t size = pEntries[processed].Size;
    const auto result = GCReadPort(hPort, pEntries[processed].Address,
                                   pEntries[processed].pBuffer, &size);
    pEntries[processed].Size = size;
    if (result != GC_ERR_SUCCESS) {
      *piNumEntries = processed;
      return result;
    }
  }

  *piNumEntries = processed;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
GCWritePortStacked(PORT_HANDLE hPort, PORT_REGISTER_STACK_ENTRY *pEntries,
                   size_t *piNumEntries) {
  GENTL_REQUIRE_INITIALIZED();
  if (pEntries == nullptr || piNumEntries == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }

  std::size_t processed = 0;
  for (; processed < *piNumEntries; ++processed) {
    size_t size = pEntries[processed].Size;
    const auto result = GCWritePort(hPort, pEntries[processed].Address,
                                    pEntries[processed].pBuffer, &size);
    pEntries[processed].Size = size;
    if (result != GC_ERR_SUCCESS) {
      *piNumEntries = processed;
      return result;
    }
  }

  *piNumEntries = processed;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferChunkData(DS_HANDLE,
                                                           BUFFER_HANDLE,
                                                           SINGLE_CHUNK_DATA *,
                                                           size_t *) {
  GENTL_REQUIRE_INITIALIZED();
  return GC_ERR_NOT_AVAILABLE;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE IFGetParentTL(IF_HANDLE hIface,
                                                    TL_HANDLE *phSystem) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::IfHandleData *iface =
      gentl_producer_core::runtime::Runtime::as_if(hIface);
  if (iface == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (phSystem == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  *phSystem = iface->parent;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DevGetParentIF(DEV_HANDLE hDevice,
                                                     IF_HANDLE *phIface) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DevHandleData *device =
      gentl_producer_core::runtime::Runtime::as_dev(hDevice);
  if (device == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (phIface == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  *phIface = device->parent;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSGetParentDev(DS_HANDLE hDataStream,
                                                     DEV_HANDLE *phDevice) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::runtime::DsHandleData *stream =
      gentl_producer_core::runtime::Runtime::as_ds(hDataStream);
  if (stream == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (phDevice == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  *phDevice = stream->parent;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
DSGetNumBufferParts(DS_HANDLE, BUFFER_HANDLE, uint32_t *piNumParts) {
  GENTL_REQUIRE_INITIALIZED();
  if (piNumParts == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  *piNumParts = 1;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
DSGetBufferPartInfo(DS_HANDLE, BUFFER_HANDLE, uint32_t iPartIndex,
                    BUFFER_PART_INFO_CMD, INFO_DATATYPE *, void *, size_t *) {
  GENTL_REQUIRE_INITIALIZED();
  if (iPartIndex != 0U) {
    return GC_ERR_INVALID_INDEX;
  }
  return gentl_producer_core::runtime::not_implemented(
      "buffer-part metadata is not implemented yet");
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSAnnounceCompositeBuffer(
    DS_HANDLE, size_t, void **, size_t *, void *, BUFFER_HANDLE *) {
  GENTL_REQUIRE_INITIALIZED();
  return gentl_producer_core::runtime::not_implemented(
      "composite buffers are not implemented in the initial scaffold");
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferInfoStacked(
    DS_HANDLE, BUFFER_HANDLE, DS_BUFFER_INFO_STACKED *, size_t) {
  GENTL_REQUIRE_INITIALIZED();
  return gentl_producer_core::runtime::not_implemented(
      "stacked buffer info is not implemented in the initial scaffold");
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferPartInfoStacked(
    DS_HANDLE, BUFFER_HANDLE, DS_BUFFER_PART_INFO_STACKED *, size_t) {
  GENTL_REQUIRE_INITIALIZED();
  return gentl_producer_core::runtime::not_implemented(
      "stacked buffer part info is not implemented in the initial scaffold");
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSGetNumFlows(DS_HANDLE,
                                                    uint32_t *piNumFlows) {
  GENTL_REQUIRE_INITIALIZED();
  if (piNumFlows == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  *piNumFlows = 0;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE DSGetFlowInfo(DS_HANDLE, uint32_t,
                                                    FLOW_INFO_CMD,
                                                    INFO_DATATYPE *, void *,
                                                    size_t *) {
  GENTL_REQUIRE_INITIALIZED();
  return GC_ERR_NOT_AVAILABLE;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
DSGetNumBufferSegments(DS_HANDLE, BUFFER_HANDLE, uint32_t *piNumSegments) {
  GENTL_REQUIRE_INITIALIZED();
  if (piNumSegments == nullptr) {
    return GC_ERR_INVALID_PARAMETER;
  }
  *piNumSegments = 1;
  return GC_ERR_SUCCESS;
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
DSGetBufferSegmentInfo(DS_HANDLE, BUFFER_HANDLE, uint32_t iSegmentIndex,
                       SEGMENT_INFO_CMD, INFO_DATATYPE *, void *, size_t *) {
  GENTL_REQUIRE_INITIALIZED();
  if (iSegmentIndex != 0U) {
    return GC_ERR_INVALID_INDEX;
  }
  return gentl_producer_core::runtime::not_implemented(
      "buffer segment info is not implemented in the initial scaffold");
}


GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCGetInfo(TL_INFO_CMD iInfoCmd,
                                                INFO_DATATYPE *piType,
                                                void *pBuffer, size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  switch (iInfoCmd) {
  case TL_INFO_ID:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::Runtime::instance()
            .backend_info()
            .system_id,
        piType, pBuffer, piSize);
  case TL_INFO_VENDOR:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::Runtime::instance()
            .backend_info()
            .system_vendor,
        piType, pBuffer, piSize);
  case TL_INFO_MODEL:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::Runtime::instance()
            .backend_info()
            .system_model,
        piType, pBuffer, piSize);
  case TL_INFO_VERSION:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::producer_definition().version, piType,
        pBuffer, piSize);
  case TL_INFO_TLTYPE:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::Runtime::instance()
            .backend_info()
            .tl_type,
        piType, pBuffer, piSize);
  case TL_INFO_NAME:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::cti_runtime_path(
            gentl_producer_core::runtime::producer_definition().cti_filename)
            .filename()
            .string(),
        piType, pBuffer, piSize);
  case TL_INFO_PATHNAME:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::cti_runtime_path(
            gentl_producer_core::runtime::producer_definition().cti_filename)
            .string(),
        piType, pBuffer, piSize);
  case TL_INFO_DISPLAYNAME:
    return gentl_producer_core::runtime::info_string(
        gentl_producer_core::runtime::Runtime::instance()
            .backend_info()
            .system_display_name,
        piType, pBuffer, piSize);
  case TL_INFO_CHAR_ENCODING:
    return gentl_producer_core::runtime::info_scalar<int32_t>(
        TL_CHAR_ENCODING_ASCII, INFO_DATATYPE_INT32, piType, pBuffer, piSize);
  case TL_INFO_GENTL_VER_MAJOR:
    return gentl_producer_core::runtime::info_scalar<uint32_t>(
        GenTLMajorVersion, INFO_DATATYPE_UINT32, piType, pBuffer, piSize);
  case TL_INFO_GENTL_VER_MINOR:
    return gentl_producer_core::runtime::info_scalar<uint32_t>(
        GenTLMinorVersion, INFO_DATATYPE_UINT32, piType, pBuffer, piSize);
  default:
    return GC_ERR_INVALID_ID;
  }
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCReadPort(PORT_HANDLE hPort,
                                                 uint64_t iAddress,
                                                 void *pBuffer,
                                                 size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::PortModel *port_model =
      gentl_producer_core::runtime::port_model_from_handle(hPort);
  if (port_model == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  const auto result = port_model->read(iAddress, pBuffer, piSize);
  switch (result) {
  case GC_ERR_SUCCESS:
    return GC_ERR_SUCCESS;
  case GC_ERR_INVALID_ADDRESS:
    return gentl_producer_core::runtime::invalid_address();
  case GC_ERR_ACCESS_DENIED:
    return gentl_producer_core::runtime::access_denied("port is not readable");
  default:
    return result;
  }
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCWritePort(PORT_HANDLE hPort,
                                                  uint64_t iAddress,
                                                  const void *pBuffer,
                                                  size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  gentl_producer_core::PortModel *port_model =
      gentl_producer_core::runtime::port_model_from_handle(hPort);
  if (port_model == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  auto *port = gentl_producer_core::runtime::Runtime::as_port(hPort);
  if (port != nullptr &&
      port->port_kind ==
          gentl_producer_core::runtime::PortKind::kRemoteDevice &&
      port->owner->access == DEVICE_ACCESS_READONLY) {
    return gentl_producer_core::runtime::access_denied(
        "remote port is read-only for this device handle");
  }
  if (port != nullptr &&
      port->port_kind ==
          gentl_producer_core::runtime::PortKind::kRemoteDevice &&
      piSize != nullptr &&
      gentl_producer_core::runtime::Runtime::instance()
          .remote_write_requires_idle(iAddress, *piSize)) {
    std::scoped_lock lock(port->owner->mutex);
    if (port->owner->acquisition_active) {
      return gentl_producer_core::runtime::access_denied(
          "remote feature writes are blocked while acquisition is active");
    }
  }

  const auto result =
      port_model->write(iAddress, const_cast<void *>(pBuffer), piSize);
  switch (result) {
  case GC_ERR_SUCCESS:
    if (port != nullptr &&
        port->port_kind ==
            gentl_producer_core::runtime::PortKind::kRemoteDevice &&
        piSize != nullptr) {
      auto features = gentl_producer_core::runtime::Runtime::instance()
                          .remote_invalidated_features(iAddress, *piSize);
      if (!features.empty()) {
        gentl_producer_core::runtime::publish_port_feature_invalidations(
            port, features);
      }
    }
    return GC_ERR_SUCCESS;
  case GC_ERR_INVALID_ADDRESS:
    return gentl_producer_core::runtime::invalid_address();
  case GC_ERR_ACCESS_DENIED:
    return gentl_producer_core::runtime::access_denied("port is not writable");
  default:
    return result;
  }
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCGetPortURL(PORT_HANDLE hPort,
                                                   char *sURL, size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  const gentl_producer_core::PortModel *port_model =
      gentl_producer_core::runtime::port_model_from_handle(hPort);
  if (port_model == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (port_model->urls().empty()) {
    return GC_ERR_NOT_AVAILABLE;
  }
  return gentl_producer_core::runtime::copy_string(
      port_model->urls().front().url, INFO_DATATYPE_STRING,
      INFO_DATATYPE_STRING, sURL, piSize);
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCGetPortInfo(PORT_HANDLE hPort,
                                                    PORT_INFO_CMD iInfoCmd,
                                                    INFO_DATATYPE *piType,
                                                    void *pBuffer,
                                                    size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  const gentl_producer_core::PortModel *port_model =
      gentl_producer_core::runtime::port_model_from_handle(hPort);
  if (port_model == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }

  const auto &descriptor = port_model->descriptor();
  switch (iInfoCmd) {
  case PORT_INFO_ID:
    return gentl_producer_core::runtime::info_string(descriptor.id, piType,
                                                     pBuffer, piSize);
  case PORT_INFO_VENDOR:
    return gentl_producer_core::runtime::info_string(descriptor.vendor, piType,
                                                     pBuffer, piSize);
  case PORT_INFO_MODEL:
    return gentl_producer_core::runtime::info_string(descriptor.model, piType,
                                                     pBuffer, piSize);
  case PORT_INFO_TLTYPE:
    return gentl_producer_core::runtime::info_string(descriptor.tl_type, piType,
                                                     pBuffer, piSize);
  case PORT_INFO_MODULE:
    return gentl_producer_core::runtime::info_string(descriptor.module_name,
                                                     piType, pBuffer, piSize);
  case PORT_INFO_LITTLE_ENDIAN:
    return gentl_producer_core::runtime::info_scalar<bool8_t>(
        descriptor.little_endian ? 1 : 0, INFO_DATATYPE_BOOL8, piType, pBuffer,
        piSize);
  case PORT_INFO_BIG_ENDIAN:
    return gentl_producer_core::runtime::info_scalar<bool8_t>(
        descriptor.little_endian ? 0 : 1, INFO_DATATYPE_BOOL8, piType, pBuffer,
        piSize);
  case PORT_INFO_ACCESS_READ:
    return gentl_producer_core::runtime::info_scalar<bool8_t>(
        descriptor.read_access ? 1 : 0, INFO_DATATYPE_BOOL8, piType, pBuffer,
        piSize);
  case PORT_INFO_ACCESS_WRITE:
    return gentl_producer_core::runtime::info_scalar<bool8_t>(
        descriptor.write_access ? 1 : 0, INFO_DATATYPE_BOOL8, piType, pBuffer,
        piSize);
  case PORT_INFO_ACCESS_NA:
    return gentl_producer_core::runtime::info_scalar<bool8_t>(
        0, INFO_DATATYPE_BOOL8, piType, pBuffer, piSize);
  case PORT_INFO_ACCESS_NI:
    return gentl_producer_core::runtime::info_scalar<bool8_t>(
        0, INFO_DATATYPE_BOOL8, piType, pBuffer, piSize);
  case PORT_INFO_VERSION:
    return gentl_producer_core::runtime::info_string(descriptor.version, piType,
                                                     pBuffer, piSize);
  case PORT_INFO_PORTNAME:
    return gentl_producer_core::runtime::info_string(descriptor.port_name,
                                                     piType, pBuffer, piSize);
  default:
    return GC_ERR_INVALID_ID;
  }
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE GCGetNumPortURLs(PORT_HANDLE hPort,
                                                       uint32_t *piNumURLs) {
  GENTL_REQUIRE_INITIALIZED();
  if (const gentl_producer_core::PortModel *port_model =
          gentl_producer_core::runtime::port_model_from_handle(hPort);
      port_model != nullptr) {
    if (piNumURLs == nullptr) {
      return GC_ERR_INVALID_PARAMETER;
    }
    *piNumURLs = static_cast<uint32_t>(port_model->urls().size());
    return GC_ERR_SUCCESS;
  }
  return gentl_producer_core::runtime::invalid_handle();
}

GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE
GCGetPortURLInfo(PORT_HANDLE hPort, uint32_t iURLIndex, URL_INFO_CMD iInfoCmd,
                 INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
  GENTL_REQUIRE_INITIALIZED();
  const gentl_producer_core::PortModel *port_model =
      gentl_producer_core::runtime::port_model_from_handle(hPort);
  if (port_model == nullptr) {
    return gentl_producer_core::runtime::invalid_handle();
  }
  if (iURLIndex >= port_model->urls().size()) {
    return GC_ERR_INVALID_INDEX;
  }

  const auto &url = port_model->urls()[iURLIndex];
  switch (iInfoCmd) {
  case URL_INFO_URL:
    return gentl_producer_core::runtime::info_string(url.url, piType, pBuffer,
                                                     piSize);
  case URL_INFO_SCHEMA_VER_MAJOR:
    return gentl_producer_core::runtime::info_scalar<int32_t>(
        url.schema_major, INFO_DATATYPE_INT32, piType, pBuffer, piSize);
  case URL_INFO_SCHEMA_VER_MINOR:
    return gentl_producer_core::runtime::info_scalar<int32_t>(
        url.schema_minor, INFO_DATATYPE_INT32, piType, pBuffer, piSize);
  case URL_INFO_FILE_VER_MAJOR:
    return gentl_producer_core::runtime::info_scalar<int32_t>(
        url.file_major, INFO_DATATYPE_INT32, piType, pBuffer, piSize);
  case URL_INFO_FILE_VER_MINOR:
    return gentl_producer_core::runtime::info_scalar<int32_t>(
        url.file_minor, INFO_DATATYPE_INT32, piType, pBuffer, piSize);
  case URL_INFO_FILE_VER_SUBMINOR:
    return gentl_producer_core::runtime::info_scalar<int32_t>(
        url.file_subminor, INFO_DATATYPE_INT32, piType, pBuffer, piSize);
  case URL_INFO_FILE_REGISTER_ADDRESS:
    return gentl_producer_core::runtime::info_scalar<uint64_t>(
        url.file_register_address, INFO_DATATYPE_UINT64, piType, pBuffer,
        piSize);
  case URL_INFO_FILE_SIZE:
    return gentl_producer_core::runtime::info_scalar<uint64_t>(
        url.file_size, INFO_DATATYPE_UINT64, piType, pBuffer, piSize);
  case URL_INFO_SCHEME:
    return gentl_producer_core::runtime::info_scalar<int32_t>(
        url.scheme, INFO_DATATYPE_INT32, piType, pBuffer, piSize);
  case URL_INFO_FILENAME:
    return gentl_producer_core::runtime::info_string(url.filename, piType,
                                                     pBuffer, piSize);
  case URL_INFO_FILE_SHA1_HASH:
    return GC_ERR_NOT_AVAILABLE;
  default:
    return GC_ERR_INVALID_ID;
  }
}
