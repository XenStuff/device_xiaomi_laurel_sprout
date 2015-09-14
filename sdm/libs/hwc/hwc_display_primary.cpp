/*
* Copyright (c) 2014 - 2015, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <cutils/properties.h>
#include <utils/constants.h>
#include <stdarg.h>
#include "hwc_display_primary.h"
#include "hwc_debugger.h"

#define __CLASS__ "HWCDisplayPrimary"

namespace sdm {

int HWCDisplayPrimary::Create(CoreInterface *core_intf, hwc_procs_t const **hwc_procs,
                              HWCDisplay **hwc_display) {
  int status = 0;
  uint32_t primary_width = 0;
  uint32_t primary_height = 0;

  HWCDisplay *hwc_display_primary = new HWCDisplayPrimary(core_intf, hwc_procs);
  status = hwc_display_primary->Init();
  if (status) {
    delete hwc_display_primary;
    return status;
  }

  hwc_display_primary->GetPanelResolution(&primary_width, &primary_height);
  int width = 0, height = 0;
  HWCDebugHandler::Get()->GetProperty("sdm.fb_size_width", &width);
  HWCDebugHandler::Get()->GetProperty("sdm.fb_size_height", &height);
  if (width > 0 && height > 0) {
    primary_width = width;
    primary_height = height;
  }

  status = hwc_display_primary->SetFrameBufferResolution(primary_width, primary_height);
  if (status) {
    Destroy(hwc_display_primary);
    return status;
  }

  *hwc_display = hwc_display_primary;

  return status;
}

void HWCDisplayPrimary::Destroy(HWCDisplay *hwc_display) {
  hwc_display->Deinit();
  delete hwc_display;
}

HWCDisplayPrimary::HWCDisplayPrimary(CoreInterface *core_intf, hwc_procs_t const **hwc_procs)
  : HWCDisplay(core_intf, hwc_procs, kPrimary, HWC_DISPLAY_PRIMARY, true), cpu_hint_(NULL) {
}

int HWCDisplayPrimary::Init() {
  cpu_hint_ = new CPUHint();
  if (cpu_hint_->Init(static_cast<HWCDebugHandler*>(HWCDebugHandler::Get())) != kErrorNone) {
    delete cpu_hint_;
    cpu_hint_ = NULL;
  }

  use_metadata_refresh_rate_ = true;
  int disable_metadata_dynfps = 0;
  HWCDebugHandler::Get()->GetProperty("persist.metadata_dynfps.disable", &disable_metadata_dynfps);
  if (disable_metadata_dynfps) {
    use_metadata_refresh_rate_ = false;
  }

  return HWCDisplay::Init();
}

void HWCDisplayPrimary::ProcessBootAnimCompleted() {
  char value[PROPERTY_VALUE_MAX];

  // Applying default mode after bootanimation is finished
  property_get("init.svc.bootanim", value, "running");
  if (!strncmp(value, "stopped", strlen("stopped"))) {
    boot_animation_completed_ = true;

    // one-shot action check if bootanimation completed then apply default display mode.
    if (display_intf_)
      display_intf_->ApplyDefaultDisplayMode();
  }
}

int HWCDisplayPrimary::Prepare(hwc_display_contents_1_t *content_list) {
  int status = 0;

  if (!boot_animation_completed_)
    ProcessBootAnimCompleted();

  if (display_paused_) {
    MarkLayersForGPUBypass(content_list);
    return status;
  }

  status = AllocateLayerStack(content_list);
  if (status) {
    return status;
  }

  status = PrepareLayerStack(content_list);
  if (status) {
    return status;
  }

  // Drop invalidate trigger to SurfaceFlinger
  // if all layers in previous draw cycle were composed using GPU
  // OR
  // if all layers were composed using GPU sometime in past and the cached content
  // is being used currently.
  handle_refresh_ = false;
  int app_layer_count = content_list->numHwLayers - 1;
  if (app_layer_count > 1) {
    int fb_layer_count = 0;
    for (int i = 0; i < app_layer_count; i++) {
      if (content_list->hwLayers[i].compositionType == HWC_FRAMEBUFFER) {
        fb_layer_count++;
      }
    }

    if ((fb_layer_count == 0) && !layer_stack_cache_.in_use) {
      handle_refresh_ = true;
    } else if (fb_layer_count > 0 && fb_layer_count < app_layer_count) {
      handle_refresh_ = true;
    }
  }

  return 0;
}

int HWCDisplayPrimary::Commit(hwc_display_contents_1_t *content_list) {
  int status = 0;
  if (display_paused_) {
    if (content_list->outbufAcquireFenceFd >= 0) {
      // If we do not handle the frame set retireFenceFd to outbufAcquireFenceFd,
      // which will make sure the framework waits on it and closes it.
      content_list->retireFenceFd = dup(content_list->outbufAcquireFenceFd);
      close(content_list->outbufAcquireFenceFd);
      content_list->outbufAcquireFenceFd = -1;
    }
    CloseAcquireFences(content_list);

    DisplayError error = display_intf_->Flush();
    if (error != kErrorNone) {
      DLOGE("Flush failed. Error = %d", error);
    }
    return status;
  }

  status = HWCDisplay::CommitLayerStack(content_list);
  if (status) {
    return status;
  }

  status = HWCDisplay::PostCommitLayerStack(content_list);
  if (status) {
    return status;
  }

  bool one_updating_layer = SingleLayerUpdating(content_list->numHwLayers - 1);

  SetMetadataRefreshRate(one_updating_layer);
  ToggleCPUHint(one_updating_layer);

  return 0;
}

int HWCDisplayPrimary::SetRefreshRate(uint32_t refresh_rate) {
  DisplayError error = kErrorNone;

  if (display_intf_) {
    error = display_intf_->SetRefreshRate(refresh_rate);
  }

  return (error ? -1 : 0);
}

int HWCDisplayPrimary::Perform(uint32_t operation, ...) {
  va_list args;
  va_start(args, operation);
  int val = va_arg(args, uint32_t);
  va_end(args);
  switch (operation) {
    case SET_METADATA_DYN_REFRESH_RATE:
      SetMetaDataRefreshRateFlag(val);
      break;
    case SET_BINDER_DYN_REFRESH_RATE:
      ForceRefreshRate(val);
      break;
    case SET_DISPLAY_MODE:
      SetDisplayMode(val);
      break;
    case SET_QDCM_SOLID_FILL_INFO:
      SetQDCMSolidFillInfo(true, val);
      break;
    case UNSET_QDCM_SOLID_FILL_INFO:
      SetQDCMSolidFillInfo(false, val);
      break;
    default:
      DLOGW("Invalid operation %d", operation);
      return -EINVAL;
  }

  return 0;
}

DisplayError HWCDisplayPrimary::SetDisplayMode(uint32_t mode) {
  DisplayError error = kErrorNone;

  if (display_intf_) {
    error = display_intf_->SetDisplayMode(mode);
  }

  return error;
}

void HWCDisplayPrimary::SetMetaDataRefreshRateFlag(bool enable) {
  int disable_metadata_dynfps = 0;

  HWCDebugHandler::Get()->GetProperty("persist.metadata_dynfps.disable", &disable_metadata_dynfps);
  if (disable_metadata_dynfps) {
    return;
  }
  use_metadata_refresh_rate_ = enable;
}

void HWCDisplayPrimary::SetQDCMSolidFillInfo(bool enable, uint32_t color) {
  solid_fill_enable_ = enable;
  solid_fill_color_  = color;
}

void HWCDisplayPrimary::ToggleCPUHint(bool set) {
  if (!cpu_hint_) {
    return;
  }

  if (set) {
    cpu_hint_->Set();
  } else {
    cpu_hint_->Reset();
  }
}

void HWCDisplayPrimary::SetSecureDisplay(bool secure_display_active) {
  if (secure_display_active_ != secure_display_active) {
    // Skip Prepare and call Flush for null commit
    DLOGI("SecureDisplay state changed from %d to %d Needs Flush!!", secure_display_active_,
           secure_display_active);
    secure_display_active_ = secure_display_active;
    skip_prepare_ = true;
  }
  return;
}

void HWCDisplayPrimary::SetMetadataRefreshRate(bool one_updating_layer) {
  // return if force_refresh_rate_ is set or metadata_refresh_rate is not enabled
  if (force_refresh_rate_) {
    return;
  }

  uint32_t refresh_rate = max_refresh_rate_;
  if (use_metadata_refresh_rate_ && one_updating_layer && metadata_refresh_rate_) {
    refresh_rate = metadata_refresh_rate_;
  }

  int status = SetRefreshRate(refresh_rate);
  if (status == 0) {
    // Set current refresh rate to metadata refresh rate
    current_refresh_rate_ = refresh_rate;
  } else {
    // Failed - Restore to max refresh rate
    current_refresh_rate_ = max_refresh_rate_;
  }
}

void HWCDisplayPrimary::ForceRefreshRate(uint32_t refresh_rate) {
  if (refresh_rate && (refresh_rate < min_refresh_rate_ || refresh_rate > max_refresh_rate_)) {
    // Cannot honor force refresh rate, as its beyond the range
    return;
  }

  uint32_t rate = refresh_rate;
  force_refresh_rate_ = refresh_rate;
  if (refresh_rate == 0) {
    // refresh rate request of 0 means the client is resetting the forced request rate
    rate = max_refresh_rate_;
  }

  int status = SetRefreshRate(rate);
  if (status) {
    // failed Reset force_refresh_rate to 0, and restore max refresh rate
    force_refresh_rate_ = 0;
    rate = max_refresh_rate_;
    DLOGE("Setting Refresh Rate = %d Failed", refresh_rate);
  }
  current_refresh_rate_ = rate;

  return;
}

DisplayError HWCDisplayPrimary::Refresh() {
  const hwc_procs_t *hwc_procs = *hwc_procs_;
  DisplayError error = kErrorNone;

  if (!hwc_procs) {
    return kErrorParameters;
  }

  if (!handle_refresh_) {
    error = kErrorNotSupported;
  }

  if (error == kErrorNone) {
    hwc_procs->invalidate(hwc_procs);
  }

  uint32_t refresh_rate = force_refresh_rate_ ? force_refresh_rate_ : min_refresh_rate_;

  int status = SetRefreshRate(refresh_rate);
  if (status) {
    DLOGE("Setting Refresh Rate = %d Failed", refresh_rate);
  } else {
    current_refresh_rate_ = refresh_rate;
  }

  return error;
}

}  // namespace sdm

