// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture_controller.h"

#include <atlbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cassert>
#include <chrono>
#include <system_error>

#include "photo_handler.h"
#include "preview_handler.h"
#include "record_handler.h"
#include "string_utils.h"

namespace camera_windows {

using Microsoft::WRL::ComPtr;

CaptureControllerImpl::CaptureControllerImpl(
    CaptureControllerListener* listener)
    : capture_controller_listener_(listener), CaptureController(){};

CaptureControllerImpl::~CaptureControllerImpl() {
  ResetCaptureController();
  capture_controller_listener_ = nullptr;
};

// static
bool CaptureControllerImpl::EnumerateVideoCaptureDeviceSources(
    IMFActivate*** devices, UINT32* count) {
  ComPtr<IMFAttributes> attributes;

  HRESULT hr = MFCreateAttributes(&attributes, 1);
  if (FAILED(hr)) {
    return false;
  }

  hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) {
    return false;
  }

  hr = MFEnumDeviceSources(attributes.Get(), devices, count);
  if (FAILED(hr)) {
    return false;
  }

  return true;
}

HRESULT CaptureControllerImpl::CreateDefaultAudioCaptureSource() {
  audio_source_ = nullptr;
  CComHeapPtr<IMFActivate*> devices;
  UINT32 count = 0;

  ComPtr<IMFAttributes> attributes;
  HRESULT hr = MFCreateAttributes(&attributes, 1);

  if (SUCCEEDED(hr)) {
    hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                             MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);
  }

  if (SUCCEEDED(hr)) {
    hr = MFEnumDeviceSources(attributes.Get(), &devices, &count);
  }

  if (SUCCEEDED(hr) && count > 0) {
    CComHeapPtr<wchar_t> audio_device_id;
    UINT32 audio_device_id_size;

    // Use first audio device.
    hr = devices[0]->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, &audio_device_id,
        &audio_device_id_size);

    if (SUCCEEDED(hr)) {
      ComPtr<IMFAttributes> audio_capture_source_attributes;
      hr = MFCreateAttributes(&audio_capture_source_attributes, 2);

      if (SUCCEEDED(hr)) {
        hr = audio_capture_source_attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);
      }

      if (SUCCEEDED(hr)) {
        hr = audio_capture_source_attributes->SetString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID,
            audio_device_id);
      }

      if (SUCCEEDED(hr)) {
        hr = MFCreateDeviceSource(audio_capture_source_attributes.Get(),
                                  audio_source_.GetAddressOf());
      }
    }
  }

  return hr;
}

HRESULT CaptureControllerImpl::CreateVideoCaptureSourceForDevice(
    const std::string& video_device_id) {
  video_source_ = nullptr;

  ComPtr<IMFAttributes> video_capture_source_attributes;

  HRESULT hr = MFCreateAttributes(&video_capture_source_attributes, 2);
  if (FAILED(hr)) {
    return hr;
  }

  hr = video_capture_source_attributes->SetGUID(
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) {
    return hr;
  }

  hr = video_capture_source_attributes->SetString(
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
      Utf16FromUtf8(video_device_id).c_str());
  if (FAILED(hr)) {
    return hr;
  }

  hr = MFCreateDeviceSource(video_capture_source_attributes.Get(),
                            video_source_.GetAddressOf());
  return hr;
}

HRESULT CaptureControllerImpl::CreateD3DManagerWithDX11Device() {
  // TODO: Use existing ANGLE device

  HRESULT hr = S_OK;
  hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                         D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                         D3D11_SDK_VERSION, &dx11_device_, nullptr, nullptr);
  if (FAILED(hr)) {
    return hr;
  }

  // Enable multithread protection
  ComPtr<ID3D10Multithread> multi_thread;
  hr = dx11_device_.As(&multi_thread);
  if (FAILED(hr)) {
    return hr;
  }

  multi_thread->SetMultithreadProtected(TRUE);

  hr = MFCreateDXGIDeviceManager(&dx_device_reset_token_,
                                 dxgi_device_manager_.GetAddressOf());
  if (FAILED(hr)) {
    return hr;
  }

  hr = dxgi_device_manager_->ResetDevice(dx11_device_.Get(),
                                         dx_device_reset_token_);
  return hr;
}

HRESULT CaptureControllerImpl::CreateCaptureEngine() {
  assert(!video_device_id_.empty());

  HRESULT hr = S_OK;
  ComPtr<IMFAttributes> attributes;

  // Creates capture engine only if not already initialized by test framework
  if (!capture_engine_) {
    ComPtr<IMFCaptureEngineClassFactory> capture_engine_factory;

    hr = CoCreateInstance(CLSID_MFCaptureEngineClassFactory, nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&capture_engine_factory));
    if (FAILED(hr)) {
      return hr;
    }

    // Creates CaptureEngine.
    hr = capture_engine_factory->CreateInstance(CLSID_MFCaptureEngine,
                                                IID_PPV_ARGS(&capture_engine_));
    if (FAILED(hr)) {
      return hr;
    }
  }

  hr = CreateD3DManagerWithDX11Device();

  if (FAILED(hr)) {
    return hr;
  }

  if (!video_source_) {
    hr = CreateVideoCaptureSourceForDevice(video_device_id_);
    if (FAILED(hr)) {
      return hr;
    }
  }

  if (record_audio_ && !audio_source_) {
    hr = CreateDefaultAudioCaptureSource();
    if (FAILED(hr)) {
      return hr;
    }
  }

  if (!capture_engine_callback_handler_) {
    capture_engine_callback_handler_ =
        ComPtr<CaptureEngineListener>(new CaptureEngineListener(this));
  }

  hr = MFCreateAttributes(&attributes, 2);
  if (FAILED(hr)) {
    return hr;
  }

  hr = attributes->SetUnknown(MF_CAPTURE_ENGINE_D3D_MANAGER,
                              dxgi_device_manager_.Get());
  if (FAILED(hr)) {
    return hr;
  }

  hr = attributes->SetUINT32(MF_CAPTURE_ENGINE_USE_VIDEO_DEVICE_ONLY,
                             !record_audio_);
  if (FAILED(hr)) {
    return hr;
  }

  hr = capture_engine_->Initialize(capture_engine_callback_handler_.Get(),
                                   attributes.Get(), audio_source_.Get(),
                                   video_source_.Get());
  return hr;
}

void CaptureControllerImpl::ResetCaptureController() {
  if (record_handler_) {
    if (record_handler_->IsContinuousRecording()) {
      StopRecord();
    } else if (record_handler_->IsTimedRecording()) {
      StopTimedRecord();
    }
  }

  if (preview_handler_) {
    StopPreview();
  }

  // Shuts down the media foundation platform object.
  // Releases all resources including threads.
  // Application should call MFShutdown the same number of times as MFStartup
  if (media_foundation_started_) {
    MFShutdown();
  }

  // States
  media_foundation_started_ = false;
  capture_engine_state_ = CaptureEngineState::CAPTURE_ENGINE_NOT_INITIALIZED;
  record_handler_ = nullptr;
  preview_handler_ = nullptr;
  photo_handler_ = nullptr;
  preview_frame_width_ = 0;
  preview_frame_height_ = 0;
  capture_engine_callback_handler_ = nullptr;
  capture_engine_ = nullptr;
  audio_source_ = nullptr;
  video_source_ = nullptr;
  base_preview_media_type_ = nullptr;
  base_capture_media_type_ = nullptr;

  if (dxgi_device_manager_) {
    dxgi_device_manager_->ResetDevice(dx11_device_.Get(),
                                      dx_device_reset_token_);
  }
  dxgi_device_manager_ = nullptr;
  dx11_device_ = nullptr;

  // Texture
  if (texture_registrar_ && texture_id_ > -1) {
    texture_registrar_->UnregisterTexture(texture_id_);
  }
  texture_id_ = -1;
  texture_ = nullptr;
}

void CaptureControllerImpl::InitCaptureDevice(
    flutter::TextureRegistrar* texture_registrar, const std::string& device_id,
    bool record_audio, ResolutionPreset resolution_preset) {
  assert(capture_controller_listener_);

  if (capture_engine_state_ == CaptureEngineState::CAPTURE_ENGINE_INITIALIZED &&
      texture_id_ >= 0) {
    return capture_controller_listener_->OnCreateCaptureEngineFailed(
        "Capture device already initialized");
  } else if (capture_engine_state_ ==
             CaptureEngineState::CAPTURE_ENGINE_INITIALIZING) {
    return capture_controller_listener_->OnCreateCaptureEngineFailed(
        "Capture device already initializing");
  }

  capture_engine_state_ = CaptureEngineState::CAPTURE_ENGINE_INITIALIZING;
  resolution_preset_ = resolution_preset;
  record_audio_ = record_audio;
  texture_registrar_ = texture_registrar;
  video_device_id_ = device_id;

  // MFStartup must be called before using Media Foundation.
  if (!media_foundation_started_) {
    HRESULT hr = MFStartup(MF_VERSION);

    if (FAILED(hr)) {
      capture_controller_listener_->OnCreateCaptureEngineFailed(
          "Failed to create camera");
      ResetCaptureController();
      return;
    }

    media_foundation_started_ = true;
  }

  HRESULT hr = CreateCaptureEngine();
  if (FAILED(hr)) {
    capture_controller_listener_->OnCreateCaptureEngineFailed(
        "Failed to create camera");
    ResetCaptureController();
    return;
  }
}

const FlutterDesktopPixelBuffer*
CaptureControllerImpl::ConvertPixelBufferForFlutter(size_t target_width,
                                                    size_t target_height) {
  if (this->source_buffer_data_ && this->source_buffer_size_ > 0 &&
      this->preview_frame_width_ > 0 && this->preview_frame_height_ > 0) {
    uint32_t pixels_total =
        this->preview_frame_width_ * this->preview_frame_height_;
    dest_buffer_ = std::make_unique<uint8_t[]>(pixels_total * 4);

    MFVideoFormatRGB32Pixel* src =
        (MFVideoFormatRGB32Pixel*)this->source_buffer_data_.get();
    FlutterDesktopPixel* dst = (FlutterDesktopPixel*)dest_buffer_.get();

    for (uint32_t i = 0; i < pixels_total; i++) {
      dst[i].r = src[i].r;
      dst[i].g = src[i].g;
      dst[i].b = src[i].b;
      dst[i].a = 255;
    }

    // TODO: add release_callback and clear dest_buffer after each frame.
    this->flutter_desktop_pixel_buffer_.buffer = dest_buffer_.get();
    this->flutter_desktop_pixel_buffer_.width = this->preview_frame_width_;
    this->flutter_desktop_pixel_buffer_.height = this->preview_frame_height_;
    return &this->flutter_desktop_pixel_buffer_;
  }
  return nullptr;
}

void CaptureControllerImpl::TakePicture(const std::string file_path) {
  assert(capture_engine_callback_handler_);
  assert(capture_engine_);

  if (capture_engine_state_ != CaptureEngineState::CAPTURE_ENGINE_INITIALIZED) {
    return OnPicture(false, "Not initialized");
  }

  if (!base_capture_media_type_) {
    // Enumerates mediatypes and finds media type for video capture.
    if (FAILED(FindBaseMediaTypes())) {
      return OnPicture(false, "Failed to initialize photo capture");
    }
  }

  if (!photo_handler_) {
    photo_handler_ = std::make_unique<PhotoHandler>();
  } else if (photo_handler_->IsTakingPhoto()) {
    return OnPicture(false, "Photo already requested");
  }

  // Check MF_CAPTURE_ENGINE_PHOTO_TAKEN event handling
  // for response process.
  if (!photo_handler_->TakePhoto(file_path, capture_engine_.Get(),
                                 base_capture_media_type_.Get())) {
    // Destroy photo handler on error cases to make sure state is resetted.
    photo_handler_ = nullptr;
    return OnPicture(false, "Failed to take photo");
  }
}

uint32_t CaptureControllerImpl::GetMaxPreviewHeight() {
  switch (resolution_preset_) {
    case RESOLUTION_PRESET_LOW:
      return 240;
      break;
    case RESOLUTION_PRESET_MEDIUM:
      return 480;
      break;
    case RESOLUTION_PRESET_HIGH:
      return 720;
      break;
    case RESOLUTION_PRESET_VERY_HIGH:
      return 1080;
      break;
    case RESOLUTION_PRESET_ULTRA_HIGH:
      return 2160;
      break;
    case RESOLUTION_PRESET_AUTO:
    default:
      // no limit.
      return 0xffffffff;
      break;
  }
}

// Finds best mediat type for given source stream index and max height;
bool FindBestMediaType(DWORD source_stream_index, IMFCaptureSource* source,
                       IMFMediaType** target_media_type, uint32_t max_height,
                       uint32_t* target_frame_width,
                       uint32_t* target_frame_height) {
  assert(source);
  ComPtr<IMFMediaType> media_type;

  uint32_t best_width = 0;
  uint32_t best_height = 0;

  // Loop native media types.
  for (int i = 0;; i++) {
    if (FAILED(source->GetAvailableDeviceMediaType(
            source_stream_index, i, media_type.GetAddressOf()))) {
      break;
    }

    uint32_t frame_width;
    uint32_t frame_height;
    if (SUCCEEDED(MFGetAttributeSize(media_type.Get(), MF_MT_FRAME_SIZE,
                                     &frame_width, &frame_height))) {
      // Update target mediatype
      if (frame_height <= max_height &&
          (best_width < frame_width || best_height < frame_height)) {
        media_type.CopyTo(target_media_type);
        best_width = frame_width;
        best_height = frame_height;
      }
    }
  }

  if (target_frame_width && target_frame_height) {
    *target_frame_width = best_width;
    *target_frame_height = best_height;
  }

  return *target_media_type != nullptr;
}

HRESULT CaptureControllerImpl::FindBaseMediaTypes() {
  if (capture_engine_state_ != CaptureEngineState::CAPTURE_ENGINE_INITIALIZED) {
    return E_FAIL;
  }

  ComPtr<IMFCaptureSource> source;
  HRESULT hr = capture_engine_->GetSource(&source);
  if (FAILED(hr)) {
    return hr;
  }

  // Find base media type for previewing.
  uint32_t max_preview_height = GetMaxPreviewHeight();
  if (!FindBestMediaType(
          (DWORD)MF_CAPTURE_ENGINE_PREFERRED_SOURCE_STREAM_FOR_VIDEO_PREVIEW,
          source.Get(), base_preview_media_type_.GetAddressOf(),
          max_preview_height, &preview_frame_width_, &preview_frame_height_)) {
    return E_FAIL;
  }

  // Find base media type for record and photo capture.
  if (!FindBestMediaType(
          (DWORD)MF_CAPTURE_ENGINE_PREFERRED_SOURCE_STREAM_FOR_VIDEO_RECORD,
          source.Get(), base_capture_media_type_.GetAddressOf(),
          (uint32_t)0xffffffff, nullptr, nullptr)) {
    return E_FAIL;
  }

  return S_OK;
}

void CaptureControllerImpl::StartRecord(const std::string& file_path,
                                        int64_t max_video_duration_ms) {
  assert(capture_engine_);

  if (capture_engine_state_ != CaptureEngineState::CAPTURE_ENGINE_INITIALIZED) {
    return OnRecordStarted(false,
                           "Camera not initialized. Camera should be "
                           "disposed and reinitialized.");
  }

  if (!base_capture_media_type_) {
    // Enumerates mediatypes and finds media type for video capture.
    if (FAILED(FindBaseMediaTypes())) {
      return OnRecordStarted(false, "Failed to initialize video recording");
    }
  }

  if (!record_handler_) {
    record_handler_ = std::make_unique<RecordHandler>(record_audio_);
  } else if (!record_handler_->CanStart()) {
    return OnRecordStarted(
        false,
        "Recording cannot be started. Previous recording must be stopped "
        "first.");
  }

  // Check MF_CAPTURE_ENGINE_RECORD_STARTED event handling for response
  // process.
  if (!record_handler_->StartRecord(file_path, max_video_duration_ms,
                                    capture_engine_.Get(),
                                    base_capture_media_type_.Get())) {
    // Destroy record handler on error cases to make sure state is resetted.
    record_handler_ = nullptr;
    return OnRecordStarted(false, "Failed to start video recording");
  }
}

void CaptureControllerImpl::StopRecord() {
  assert(capture_controller_listener_);

  if (capture_engine_state_ != CaptureEngineState::CAPTURE_ENGINE_INITIALIZED) {
    return OnRecordStopped(false,
                           "Camera not initialized. Camera should be "
                           "disposed and reinitialized.");
  }

  if (!record_handler_ && !record_handler_->CanStop()) {
    return OnRecordStopped(false, "Recording cannot be stopped.");
  }

  // Check MF_CAPTURE_ENGINE_RECORD_STOPPED event handling for response
  // process.
  if (!record_handler_->StopRecord(capture_engine_.Get())) {
    // Destroy record handler on error cases to make sure state is resetted.
    record_handler_ = nullptr;
    return OnRecordStopped(false, "Failed to stop video recording");
  }
}

// Stops timed recording. Called internally when requested time is passed.
// Check MF_CAPTURE_ENGINE_RECORD_STOPPED event handling for response process.
void CaptureControllerImpl::StopTimedRecord() {
  assert(capture_controller_listener_);
  if (!record_handler_ || !record_handler_->IsTimedRecording()) {
    return;
  }

  if (!record_handler_->StopRecord(capture_engine_.Get())) {
    // Destroy record handler on error cases to make sure state is resetted.
    record_handler_ = nullptr;
    return capture_controller_listener_->OnVideoRecordFailed(
        "Failed to record video");
  }
}

// Starts capturing preview frames using preview handler
// After first frame is captured, OnPreviewStarted is called
void CaptureControllerImpl::StartPreview() {
  assert(capture_engine_callback_handler_);
  assert(capture_engine_);

  if (capture_engine_state_ != CaptureEngineState::CAPTURE_ENGINE_INITIALIZED) {
    return OnPreviewStarted(false,
                            "Camera not initialized. Camera should be "
                            "disposed and reinitialized.");
  }

  if (!base_preview_media_type_) {
    // Enumerates mediatypes and finds media type for video capture.
    if (FAILED(FindBaseMediaTypes())) {
      return OnPreviewStarted(false, "Failed to initialize video preview");
    }
  }

  if (!preview_handler_) {
    preview_handler_ = std::make_unique<PreviewHandler>();
  } else if (preview_handler_->IsInitialized()) {
    return OnPreviewStarted(true, "");
  } else {
    return OnPreviewStarted(false, "Preview already exists");
  }

  // Check MF_CAPTURE_ENGINE_PREVIEW_STARTED event handling for response
  // process.
  if (!preview_handler_->StartPreview(capture_engine_.Get(),
                                      base_capture_media_type_.Get(),
                                      capture_engine_callback_handler_.Get())) {
    // Destroy preview handler on error cases to make sure state is resetted.
    preview_handler_ = nullptr;
    return OnPreviewStarted(false, "Failed to start video preview");
  }
}

// Stops preview. Called by destructor
// Use PausePreview and ResumePreview methods to for
// pausing and resuming the preview.
// Check MF_CAPTURE_ENGINE_PREVIEW_STOPPED event handling for response
// process.
void CaptureControllerImpl::StopPreview() {
  assert(capture_engine_);

  if (capture_engine_state_ != CaptureEngineState::CAPTURE_ENGINE_INITIALIZED &&
      !preview_handler_) {
    return;
  }

  // Requests to stop preview.
  preview_handler_->StopPreview(capture_engine_.Get());
}

// Marks preview as paused.
// When preview is paused, captured frames are not processed for preview
// and flutter texture is not updated
void CaptureControllerImpl::PausePreview() {
  assert(capture_controller_listener_);

  if (!preview_handler_ && !preview_handler_->IsInitialized()) {
    return capture_controller_listener_->OnPausePreviewFailed(
        "Preview not started");
  }

  if (preview_handler_->PausePreview()) {
    capture_controller_listener_->OnPausePreviewSucceeded();
  } else {
    capture_controller_listener_->OnPausePreviewFailed(
        "Failed to pause preview");
  }
}

// Marks preview as not paused.
// When preview is not paused, captured frames are processed for preview
// and flutter texture is updated.
void CaptureControllerImpl::ResumePreview() {
  assert(capture_controller_listener_);

  if (!preview_handler_ && !preview_handler_->IsInitialized()) {
    return capture_controller_listener_->OnResumePreviewFailed(
        "Preview not started");
  }

  if (preview_handler_->ResumePreview()) {
    capture_controller_listener_->OnResumePreviewSucceeded();
  } else {
    capture_controller_listener_->OnResumePreviewFailed(
        "Failed to pause preview");
  }
}

// Handles capture engine events.
// Called via IMFCaptureEngineOnEventCallback implementation.
// Implements CaptureEngineObserver::OnEvent.
void CaptureControllerImpl::OnEvent(IMFMediaEvent* event) {
  if (capture_engine_state_ != CaptureEngineState::CAPTURE_ENGINE_INITIALIZED &&
      capture_engine_state_ !=
          CaptureEngineState::CAPTURE_ENGINE_INITIALIZING) {
    return;
  }

  GUID extended_type_guid;
  if (SUCCEEDED(event->GetExtendedType(&extended_type_guid))) {
    std::string error;

    HRESULT event_hr;
    if (FAILED(event->GetStatus(&event_hr))) {
      return;
    }

    if (FAILED(event_hr)) {
      // Reads system error
      error = std::system_category().message(event_hr);
    }

    if (extended_type_guid == MF_CAPTURE_ENGINE_ERROR) {
      OnCaptureEngineError(event_hr, error);
    } else if (extended_type_guid == MF_CAPTURE_ENGINE_INITIALIZED) {
      OnCaptureEngineInitialized(SUCCEEDED(event_hr), error);
    } else if (extended_type_guid == MF_CAPTURE_ENGINE_PREVIEW_STARTED) {
      // Preview is marked as started after first frame is captured.
      // This is because, CaptureEngine might inform that preview is started
      // even if error is thrown right after.
    } else if (extended_type_guid == MF_CAPTURE_ENGINE_PREVIEW_STOPPED) {
      OnPreviewStopped(SUCCEEDED(event_hr), error);
    } else if (extended_type_guid == MF_CAPTURE_ENGINE_RECORD_STARTED) {
      OnRecordStarted(SUCCEEDED(event_hr), error);
    } else if (extended_type_guid == MF_CAPTURE_ENGINE_RECORD_STOPPED) {
      OnRecordStopped(SUCCEEDED(event_hr), error);
    } else if (extended_type_guid == MF_CAPTURE_ENGINE_PHOTO_TAKEN) {
      OnPicture(SUCCEEDED(event_hr), error);
    } else if (extended_type_guid == MF_CAPTURE_ENGINE_CAMERA_STREAM_BLOCKED) {
      // TODO: Inform capture state to flutter.
    } else if (extended_type_guid ==
               MF_CAPTURE_ENGINE_CAMERA_STREAM_UNBLOCKED) {
      // TODO: Inform capture state to flutter.
    }
  }
}

// Handles Picture event and informs CaptureControllerListener.
void CaptureControllerImpl::OnPicture(bool success, const std::string& error) {
  if (success && photo_handler_) {
    if (capture_controller_listener_) {
      std::string path = photo_handler_->GetPhotoPath();
      capture_controller_listener_->OnTakePictureSucceeded(path);
    }
    photo_handler_->OnPhotoTaken();
  } else {
    if (capture_controller_listener_) {
      capture_controller_listener_->OnTakePictureFailed(error);
    }
    // Destroy photo handler on error cases to make sure state is resetted.
    photo_handler_ = nullptr;
  }
}

// Handles CaptureEngineInitialized event and informs
// CaptureControllerListener.
void CaptureControllerImpl::OnCaptureEngineInitialized(
    bool success, const std::string& error) {
  if (capture_controller_listener_) {
    // Create flutter desktop pixelbuffer texture;
    texture_ =
        std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
            [this](size_t width,
                   size_t height) -> const FlutterDesktopPixelBuffer* {
              return this->ConvertPixelBufferForFlutter(width, height);
            }));

    auto new_texture_id = texture_registrar_->RegisterTexture(texture_.get());

    if (new_texture_id >= 0) {
      texture_id_ = new_texture_id;
      capture_controller_listener_->OnCreateCaptureEngineSucceeded(texture_id_);
      capture_engine_state_ = CaptureEngineState::CAPTURE_ENGINE_INITIALIZED;
    } else {
      capture_controller_listener_->OnCreateCaptureEngineFailed(
          "Failed to create texture_id");
      // Reset state
      ResetCaptureController();
    }
  }
}

// Handles CaptureEngineError event and informs CaptureControllerListener.
void CaptureControllerImpl::OnCaptureEngineError(HRESULT hr,
                                                 const std::string& error) {
  if (capture_controller_listener_) {
    capture_controller_listener_->OnCaptureError(error);
  }

  // TODO: If MF_CAPTURE_ENGINE_ERROR is returned,
  // should capture controller be reinitialized automatically?
}

// Handles PreviewStarted event and informs CaptureControllerListener.
// This should be called only after first frame has been received or
// in error cases.
void CaptureControllerImpl::OnPreviewStarted(bool success,
                                             const std::string& error) {
  if (preview_handler_ && success) {
    preview_handler_->OnPreviewStarted();
  } else {
    // Destroy preview handler on error cases to make sure state is resetted.
    preview_handler_ = nullptr;
  }

  if (capture_controller_listener_) {
    if (success && preview_frame_width_ > 0 && preview_frame_height_ > 0) {
      capture_controller_listener_->OnStartPreviewSucceeded(
          preview_frame_width_, preview_frame_height_);
    } else {
      capture_controller_listener_->OnStartPreviewFailed(error);
    }
  }
};

// Handles PreviewStopped event.
void CaptureControllerImpl::OnPreviewStopped(bool success,
                                             const std::string& error) {
  // Preview handler is destroyed if preview is stopped as it
  // does not have any use anymore.
  preview_handler_ = nullptr;
};

// Handles RecordStarted event and informs CaptureControllerListener.
void CaptureControllerImpl::OnRecordStarted(bool success,
                                            const std::string& error) {
  if (success && record_handler_) {
    record_handler_->OnRecordStarted();
    if (capture_controller_listener_) {
      capture_controller_listener_->OnStartRecordSucceeded();
    }
  } else {
    if (capture_controller_listener_) {
      capture_controller_listener_->OnStartRecordFailed(error);
    }

    // Destroy record handler on error cases to make sure state is resetted.
    record_handler_ = nullptr;
  }
};

// Handles RecordStopped event and informs CaptureControllerListener.
void CaptureControllerImpl::OnRecordStopped(bool success,
                                            const std::string& error) {
  if (capture_controller_listener_ && record_handler_) {
    // Always calls OnStopRecord listener methods
    // to handle separate stop record request for timed records.

    if (success) {
      std::string path = record_handler_->GetRecordPath();
      capture_controller_listener_->OnStopRecordSucceeded(path);
      if (record_handler_->IsTimedRecording()) {
        capture_controller_listener_->OnVideoRecordSucceeded(
            path, (record_handler_->GetRecordedDuration() / 1000));
      }
    } else {
      capture_controller_listener_->OnStopRecordFailed(error);
      if (record_handler_->IsTimedRecording()) {
        capture_controller_listener_->OnVideoRecordFailed(error);
      }
    }
  }

  if (success && record_handler_) {
    record_handler_->OnRecordStopped();
  } else {
    // Destroy record handler on error cases to make sure state is resetted.
    record_handler_ = nullptr;
  }
}

// Returns pointer to databuffer.
// Called via IMFCaptureEngineOnSampleCallback implementation.
// Implements CaptureEngineObserver::GetFrameBuffer.
uint8_t* CaptureControllerImpl::GetFrameBuffer(uint32_t new_length) {
  if (this->source_buffer_data_ == nullptr ||
      this->source_buffer_size_ != new_length) {
    // Update source buffer size.
    this->source_buffer_data_ = nullptr;
    this->source_buffer_data_ = std::make_unique<uint8_t[]>(new_length);
    this->source_buffer_size_ = new_length;
  }
  return this->source_buffer_data_.get();
}

// Marks texture frame available after buffer is updated.
// Called via IMFCaptureEngineOnSampleCallback implementation.
// Implements CaptureEngineObserver::OnBufferUpdated.
void CaptureControllerImpl::OnBufferUpdated() {
  if (this->texture_registrar_ && this->texture_id_ >= 0) {
    this->texture_registrar_->MarkTextureFrameAvailable(this->texture_id_);
  }
}

// Handles capture time update from each processed frame.
// Stops timed recordings if requested recording duration has passed.
// Called via IMFCaptureEngineOnSampleCallback implementation.
// Implements CaptureEngineObserver::UpdateCaptureTime.
void CaptureControllerImpl::UpdateCaptureTime(uint64_t capture_time_us) {
  if (capture_engine_state_ != CaptureEngineState::CAPTURE_ENGINE_INITIALIZED) {
    return;
  }

  if (preview_handler_ && preview_handler_->IsStarting()) {
    // Informs that first frame is captured succeffully and preview has
    // started.
    OnPreviewStarted(true, "");
  }

  // Checks if max_video_duration_ms is passed.
  if (record_handler_) {
    record_handler_->UpdateRecordingTime(capture_time_us);
    if (record_handler_->ShouldStopTimedRecording()) {
      StopTimedRecord();
    }
  }
}

}  // namespace camera_windows
