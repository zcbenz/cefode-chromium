// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/developer_private/entry_picker.h"

#include "base/bind.h"
#include "base/file_util.h"
#include "base/files/file_path.h"
#include "base/string_util.h"
#include "chrome/browser/extensions/api/developer_private/developer_private_api.h"
#include "chrome/browser/extensions/shell_window_registry.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/ui/chrome_select_file_policy.h"
#include "chrome/browser/ui/extensions/shell_window.h"
#include "content/public/browser/web_contents.h"
#include "ui/shell_dialogs/select_file_dialog.h"

namespace {

bool g_skip_picker_for_test = false;
base::FilePath* g_path_to_be_picked_for_test = NULL;

}  // namespace

namespace extensions {

namespace api {

EntryPicker::EntryPicker(EntryPickerClient* client,
                         content::WebContents* web_contents,
                         ui::SelectFileDialog::Type picker_type,
                         const base::FilePath& last_directory,
                         const string16& select_title,
                         const ui::SelectFileDialog::FileTypeInfo& info,
                         int file_type_index)
    : client_(client) {
  select_file_dialog_ = ui::SelectFileDialog::Create(
      this, new ChromeSelectFilePolicy(web_contents));

  gfx::NativeWindow owning_window = web_contents ?
      platform_util::GetTopLevel(web_contents->GetNativeView()) : NULL;

  if (g_skip_picker_for_test) {
    if (g_path_to_be_picked_for_test) {
      content::BrowserThread::PostTask(content::BrowserThread::UI, FROM_HERE,
          base::Bind(
              &EntryPicker::FileSelected,
              base::Unretained(this), *g_path_to_be_picked_for_test, 1,
              static_cast<void*>(NULL)));
    } else {
      content::BrowserThread::PostTask(content::BrowserThread::UI, FROM_HERE,
          base::Bind(
              &EntryPicker::FileSelectionCanceled,
              base::Unretained(this), static_cast<void*>(NULL)));
    }
    return;
  }

  select_file_dialog_->SelectFile(picker_type,
                                  select_title,
                                  last_directory,
                                  &info,
                                  file_type_index,
                                  FILE_PATH_LITERAL(""),
                                  owning_window, NULL);
}

EntryPicker::~EntryPicker() {}

void EntryPicker::FileSelected(const base::FilePath& path,
                               int index,
                               void* params) {
  client_->FileSelected(path);
  delete this;
}

void EntryPicker::FileSelectionCanceled(void* params) {
  client_->FileSelectionCanceled();
  delete this;
}

// static
void EntryPicker::SkipPickerAndAlwaysSelectPathForTest(
    base::FilePath* path) {
  g_skip_picker_for_test = true;
  g_path_to_be_picked_for_test = path;
}

// static
void EntryPicker::SkipPickerAndAlwaysCancelForTest() {
  g_skip_picker_for_test = true;
  g_path_to_be_picked_for_test = NULL;
}

// static
void EntryPicker::StopSkippingPickerForTest() {
  g_skip_picker_for_test = false;
}

}  // namespace api

}  // namespace extensions
