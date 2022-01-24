// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "camera.h"

namespace camera_windows {
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

namespace {
// Camera channel events.
const char kCameraMethodChannelBaseName[] = "flutter.io/cameraPlugin/camera";
const char kVideoRecordedEvent[] = "video_recorded";
const char kErrorEvent[] = "error";

// Helper function for creating messaging channel for camera.
std::unique_ptr<flutter::MethodChannel<>> BuildChannelForCamera(
    flutter::BinaryMessenger *messenger, int64_t camera_id) {
  auto channel_name =
      std::string(kCameraMethodChannelBaseName) + std::to_string(camera_id);
  return std::make_unique<flutter::MethodChannel<>>(
      messenger, channel_name, &flutter::StandardMethodCodec::GetInstance());
}

}  // namespace

CameraImpl::CameraImpl(const std::string &device_id)
    : device_id_(device_id), Camera(device_id) {}

CameraImpl::~CameraImpl() {
  capture_controller_ = nullptr;

  SendErrorForPendingResults("Plugin disposed",
                             "Plugin disposed before request was handled");
}

void CameraImpl::InitCamera(flutter::TextureRegistrar *texture_registrar,
                            flutter::BinaryMessenger *messenger,
                            bool enable_audio,
                            ResolutionPreset resolution_preset) {
  auto capture_controller_factory =
      std::make_unique<CaptureControllerFactoryImpl>();
  InitCamera(std::move(capture_controller_factory), texture_registrar,
             messenger, enable_audio, resolution_preset);
}

void CameraImpl::InitCamera(
    std::unique_ptr<CaptureControllerFactory> capture_controller_factory,
    flutter::TextureRegistrar *texture_registrar,
    flutter::BinaryMessenger *messenger, bool enable_audio,
    ResolutionPreset resolution_preset) {
  assert(!device_id_.empty());
  messenger_ = messenger;
  capture_controller_ =
      capture_controller_factory->CreateCaptureController(this);
  capture_controller_->InitCaptureDevice(texture_registrar, device_id_,
                                         enable_audio, resolution_preset);
}

// Adds pending result to the pending_results map.
// Calls method result error handler, if result already exists.
bool CameraImpl::AddPendingResult(
    PendingResultType type, std::unique_ptr<flutter::MethodResult<>> result) {
  assert(result);

  auto it = pending_results_.find(type);
  if (it != pending_results_.end()) {
    result->Error("Duplicate request", "Method handler already called");
    return false;
  }

  pending_results_.insert(std::make_pair(type, std::move(result)));
  return true;
}

std::unique_ptr<flutter::MethodResult<>> CameraImpl::GetPendingResultByType(
    PendingResultType type) {
  if (pending_results_.empty()) {
    return nullptr;
  }
  auto it = pending_results_.find(type);
  if (it == pending_results_.end()) {
    return nullptr;
  }
  auto result = std::move(it->second);
  pending_results_.erase(it);
  return result;
}

bool CameraImpl::HasPendingResultByType(PendingResultType type) {
  if (pending_results_.empty()) {
    return false;
  }
  auto it = pending_results_.find(type);
  if (it == pending_results_.end()) {
    return false;
  }
  return it->second != nullptr;
}

void CameraImpl::SendErrorForPendingResults(const std::string &error_id,
                                            const std::string &descripion) {
  for (const auto &pending_result : pending_results_) {
    std::move(pending_result.second)->Error(error_id, descripion);
  }
  pending_results_.clear();
}

// From CaptureControllerListener.
void CameraImpl::OnCreateCaptureEngineSucceeded(int64_t texture_id) {
  // Use texture id as camera id
  camera_id_ = texture_id;
  auto pending_result =
      GetPendingResultByType(PendingResultType::CREATE_CAMERA);
  if (pending_result) {
    pending_result->Success(EncodableMap(
        {{EncodableValue("cameraId"), EncodableValue(texture_id)}}));
  }
}

// From CaptureControllerListener.
void CameraImpl::OnCreateCaptureEngineFailed(const std::string &error) {
  auto pending_result =
      GetPendingResultByType(PendingResultType::CREATE_CAMERA);
  if (pending_result) {
    pending_result->Error("camera_error", error);
  }
}

// From CaptureControllerListener.
void CameraImpl::OnStartPreviewSucceeded(int32_t width, int32_t height) {
  auto pending_result = GetPendingResultByType(PendingResultType::INITIALIZE);
  if (pending_result) {
    pending_result->Success(EncodableValue(EncodableMap({
        {EncodableValue("previewWidth"), EncodableValue((float)width)},
        {EncodableValue("previewHeight"), EncodableValue((float)height)},
    })));
  }
};

// From CaptureControllerListener.
void CameraImpl::OnStartPreviewFailed(const std::string &error) {
  auto pending_result = GetPendingResultByType(PendingResultType::INITIALIZE);
  if (pending_result) {
    pending_result->Error("camera_error", error);
  }
};

// From CaptureControllerListener.
void CameraImpl::OnResumePreviewSucceeded() {
  auto pending_result =
      GetPendingResultByType(PendingResultType::RESUME_PREVIEW);
  if (pending_result) {
    pending_result->Success();
  }
}

// From CaptureControllerListener.
void CameraImpl::OnResumePreviewFailed(const std::string &error) {
  auto pending_result =
      GetPendingResultByType(PendingResultType::RESUME_PREVIEW);
  if (pending_result) {
    pending_result->Error("camera_error", error);
  }
}

// From CaptureControllerListener.
void CameraImpl::OnPausePreviewSucceeded() {
  auto pending_result =
      GetPendingResultByType(PendingResultType::PAUSE_PREVIEW);
  if (pending_result) {
    pending_result->Success();
  }
}

// From CaptureControllerListener.
void CameraImpl::OnPausePreviewFailed(const std::string &error) {
  auto pending_result =
      GetPendingResultByType(PendingResultType::PAUSE_PREVIEW);
  if (pending_result) {
    pending_result->Error("camera_error", error);
  }
}

// From CaptureControllerListener.
void CameraImpl::OnStartRecordSucceeded() {
  auto pending_result = GetPendingResultByType(PendingResultType::START_RECORD);
  if (pending_result) {
    pending_result->Success();
  }
};

// From CaptureControllerListener.
void CameraImpl::OnStartRecordFailed(const std::string &error) {
  auto pending_result = GetPendingResultByType(PendingResultType::START_RECORD);
  if (pending_result) {
    pending_result->Error("camera_error", error);
  }
};

// From CaptureControllerListener.
void CameraImpl::OnStopRecordSucceeded(const std::string &filepath) {
  auto pending_result = GetPendingResultByType(PendingResultType::STOP_RECORD);
  if (pending_result) {
    pending_result->Success(EncodableValue(filepath));
  }
};

// From CaptureControllerListener.
void CameraImpl::OnStopRecordFailed(const std::string &error) {
  auto pending_result = GetPendingResultByType(PendingResultType::STOP_RECORD);
  if (pending_result) {
    pending_result->Error("camera_error", error);
  }
};

// From CaptureControllerListener.
void CameraImpl::OnTakePictureSucceeded(const std::string &filepath) {
  auto pending_result = GetPendingResultByType(PendingResultType::TAKE_PICTURE);
  if (pending_result) {
    pending_result->Success(EncodableValue(filepath));
  }
};

// From CaptureControllerListener.
void CameraImpl::OnTakePictureFailed(const std::string &error) {
  auto pending_take_picture_result =
      GetPendingResultByType(PendingResultType::TAKE_PICTURE);
  if (pending_take_picture_result) {
    pending_take_picture_result->Error("camera_error", error);
  }
};

// From CaptureControllerListener.
void CameraImpl::OnVideoRecordSucceeded(const std::string &filepath,
                                        int64_t video_duration) {
  if (messenger_ && camera_id_ >= 0) {
    auto channel = BuildChannelForCamera(messenger_, camera_id_);

    std::unique_ptr<EncodableValue> message_data =
        std::make_unique<EncodableValue>(
            EncodableMap({{EncodableValue("path"), EncodableValue(filepath)},
                          {EncodableValue("maxVideoDuration"),
                           EncodableValue(video_duration)}}));

    channel->InvokeMethod(kVideoRecordedEvent, std::move(message_data));
  }
}

// From CaptureControllerListener.
void CameraImpl::OnVideoRecordFailed(const std::string &error){};

void CameraImpl::OnCaptureError(const std::string &error) {
  if (messenger_ && camera_id_ >= 0) {
    auto channel = BuildChannelForCamera(messenger_, camera_id_);
    std::unique_ptr<EncodableValue> message_data =
        std::make_unique<EncodableValue>(EncodableMap(
            {{EncodableValue("description"), EncodableValue(error)}}));
    channel->InvokeMethod(kErrorEvent, std::move(message_data));
  }

  SendErrorForPendingResults("capture_error", error);
}

}  // namespace camera_windows