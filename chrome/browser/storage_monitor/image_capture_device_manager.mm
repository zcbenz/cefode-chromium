// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/storage_monitor/image_capture_device_manager.h"

#import <ImageCaptureCore/ImageCaptureCore.h>

#include "base/file_util.h"
#import "chrome/browser/storage_monitor/image_capture_device.h"
#include "chrome/browser/storage_monitor/media_storage_util.h"
#include "content/public/browser/browser_thread.h"

namespace {

chrome::ImageCaptureDeviceManager* g_image_capture_device_manager = NULL;

}  // namespace

// This class is the surface for the Mac ICDeviceBrowser ImageCaptureCore API.
// Owned by the ChromeBrowserParts and has browser process lifetime. Upon
// creation, it gets a list of attached media volumes (asynchronously) which
// it will eventually forward to RemovableStorageNotifications. It will also
// set up an ImageCaptureCore listener to be told when new devices/volumes
// are discovered and existing ones are removed.
@interface ImageCaptureDeviceManagerImpl
    : NSObject<ICDeviceBrowserDelegate> {
 @private
  scoped_nsobject<ICDeviceBrowser> deviceBrowser_;
  scoped_nsobject<NSMutableArray> cameras_;

  // Guaranteed to outlive this class.
  // TODO(gbillock): Update when ownership chains go up through
  // a RemovableStorageNotifications subclass.
  chrome::RemovableStorageNotifications::Receiver* notifications_;
}

- (void)setNotifications:(chrome::RemovableStorageNotifications::Receiver*)
                         notifications;
- (void)close;

// The UUIDs passed here are available in the device attach notifications.
// They're gotten by cracking the device ID and taking the unique ID output.
- (ImageCaptureDevice*)deviceForUUID:(const std::string&)uuid;

@end

@implementation ImageCaptureDeviceManagerImpl

- (id)init {
  if ((self = [super init])) {
    cameras_.reset([[NSMutableArray alloc] init]);
    notifications_ = NULL;

    deviceBrowser_.reset([[ICDeviceBrowser alloc] init]);
    [deviceBrowser_ setDelegate:self];
    [deviceBrowser_ setBrowsedDeviceTypeMask:
        [deviceBrowser_ browsedDeviceTypeMask] |
        ICDeviceTypeMaskCamera | ICDeviceLocationTypeMaskLocal];
    [deviceBrowser_ start];
  }
  return self;
}

- (void)setNotifications:(chrome::RemovableStorageNotifications::Receiver*)
                         notifications {
  notifications_ = notifications;
}

- (void)close {
  [deviceBrowser_ setDelegate:nil];
  [deviceBrowser_ stop];
  deviceBrowser_.reset();
  cameras_.reset();
}

- (ImageCaptureDevice*) deviceForUUID:(const std::string&)uuid {
  for (ICCameraDevice* camera in cameras_.get()) {
    NSString* camera_id = [camera UUIDString];
    if (base::SysNSStringToUTF8(camera_id) == uuid) {
      return [[[ImageCaptureDevice alloc]
          initWithCameraDevice:camera] autorelease];
    }
  }
  return nil;
}

- (void)deviceBrowser:(ICDeviceBrowser*)browser
         didAddDevice:(ICDevice*)addedDevice
           moreComing:(BOOL)moreComing {
  if (!(addedDevice.type & ICDeviceTypeCamera))
    return;

  ICCameraDevice* cameraDevice =
      base::mac::ObjCCastStrict<ICCameraDevice>(addedDevice);

  [cameras_ addObject:addedDevice];

  // TODO(gbillock): use [cameraDevice mountPoint] here when possible.
  notifications_->ProcessAttach(
      chrome::MediaStorageUtil::MakeDeviceId(
          chrome::MediaStorageUtil::MAC_IMAGE_CAPTURE,
          base::SysNSStringToUTF8([cameraDevice UUIDString])),
      base::SysNSStringToUTF16([cameraDevice name]), "");
}

- (void)deviceBrowser:(ICDeviceBrowser*)browser
      didRemoveDevice:(ICDevice*)device
            moreGoing:(BOOL)moreGoing {
  if (!(device.type & ICDeviceTypeCamera))
    return;

  std::string uuid = base::SysNSStringToUTF8([device UUIDString]);

  // May delete |device|.
  [cameras_ removeObject:device];

  notifications_->ProcessDetach(
      chrome::MediaStorageUtil::MakeDeviceId(
          chrome::MediaStorageUtil::MAC_IMAGE_CAPTURE, uuid));
}

@end  // ImageCaptureDeviceManagerImpl

namespace chrome {

ImageCaptureDeviceManager::ImageCaptureDeviceManager() {
  device_browser_.reset([[ImageCaptureDeviceManagerImpl alloc] init]);
  g_image_capture_device_manager = this;
}

ImageCaptureDeviceManager::~ImageCaptureDeviceManager() {
  g_image_capture_device_manager = NULL;
  [device_browser_ close];
}

void ImageCaptureDeviceManager::SetNotifications(
    RemovableStorageNotifications::Receiver* notifications) {
  [device_browser_ setNotifications:notifications];
}

// static
ImageCaptureDevice* ImageCaptureDeviceManager::deviceForUUID(
    const std::string& uuid) {
  ImageCaptureDeviceManagerImpl* manager =
      g_image_capture_device_manager->device_browser_;
  return [manager deviceForUUID:uuid];
}

id<ICDeviceBrowserDelegate> ImageCaptureDeviceManager::device_browser() {
  return device_browser_.get();
}

}  // namespace chrome
