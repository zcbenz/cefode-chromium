// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_DRIVE_DRIVE_DOWNLOAD_HANDLER_H_
#define CHROME_BROWSER_CHROMEOS_DRIVE_DRIVE_DOWNLOAD_HANDLER_H_

#include "base/callback_forward.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/chromeos/drive/drive_file_error.h"
#include "chrome/browser/download/all_download_item_notifier.h"
#include "content/public/browser/download_manager_delegate.h"

class Profile;

namespace content {
class DownloadItem;
class DownloadManager;
}

namespace drive {

class DriveEntryProto;
class DriveFileSystemInterface;
class FileWriteHelper;

// Observes downloads to temporary local drive folder. Schedules these
// downloads for upload to drive service.
class DriveDownloadHandler : public AllDownloadItemNotifier::Observer {
 public:
  DriveDownloadHandler(FileWriteHelper* file_write_helper,
                       DriveFileSystemInterface* file_system);
  virtual ~DriveDownloadHandler();

  // Utility method to get DriveDownloadHandler with profile.
  static DriveDownloadHandler* GetForProfile(Profile* profile);

  // Become an observer of DownloadManager.
  void Initialize(content::DownloadManager* download_manager,
                  const base::FilePath& drive_tmp_download_path);

  // Callback used to return results from SubstituteDriveDownloadPath.
  // TODO(hashimoto): Report error with a DriveFileError. crbug.com/171345
  typedef base::Callback<void(const base::FilePath&)>
      SubstituteDriveDownloadPathCallback;

  void SubstituteDriveDownloadPath(
      const base::FilePath& drive_path,
      content::DownloadItem* download,
      const SubstituteDriveDownloadPathCallback& callback);

  // Sets drive path, for example, '/special/drive/MyFolder/MyFile',
  // to external data in |download|. Also sets display name and
  // makes |download| a temporary.
  void SetDownloadParams(const base::FilePath& drive_path,
                         content::DownloadItem* download);

  // Gets the target drive path from external data in |download|.
  base::FilePath GetTargetPath(const content::DownloadItem* download);

  // Checks if there is a Drive upload associated with |download|
  bool IsDriveDownload(const content::DownloadItem* download);

  // Checks a file corresponding to the download item exists in Drive.
  void CheckForFileExistence(
      const content::DownloadItem* download,
      const content::CheckForFileExistenceCallback& callback);

 private:
  // AllDownloadItemNotifier::Observer overrides:
  virtual void OnDownloadCreated(content::DownloadManager* manager,
                                 content::DownloadItem* download) OVERRIDE;
  virtual void OnDownloadUpdated(content::DownloadManager* manager,
                                 content::DownloadItem* download) OVERRIDE;

  // Removes the download.
  void RemoveDownload(int id);

  // Callback for DriveFileSystem::GetEntryInfoByPath().
  // Used to implement SubstituteDriveDownloadPath().
  void OnEntryFound(const base::FilePath& drive_dir_path,
                    const SubstituteDriveDownloadPathCallback& callback,
                    DriveFileError error,
                    scoped_ptr<DriveEntryProto> entry_proto);

  // Callback for DriveFileSystem::CreateDirectory().
  // Used to implement SubstituteDriveDownloadPath().
  void OnCreateDirectory(const SubstituteDriveDownloadPathCallback& callback,
                         DriveFileError error);

  // Starts the upload of a downloaded/downloading file.
  void UploadDownloadItem(content::DownloadItem* download);

  FileWriteHelper* file_write_helper_;
  // The file system owned by DriveSystemService.
  DriveFileSystemInterface* file_system_;
  // Observe the DownloadManager for new downloads.
  scoped_ptr<AllDownloadItemNotifier> notifier_;

  // Temporary download location directory.
  base::FilePath drive_tmp_download_path_;

  // Note: This should remain the last member so it'll be destroyed and
  // invalidate its weak pointers before any other members are destroyed.
  base::WeakPtrFactory<DriveDownloadHandler> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(DriveDownloadHandler);
};

}  // namespace drive

#endif  // CHROME_BROWSER_CHROMEOS_DRIVE_DRIVE_DOWNLOAD_HANDLER_H_
