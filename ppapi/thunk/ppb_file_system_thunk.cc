// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// From ppb_file_system.idl modified Thu Dec 20 13:10:26 2012.

#include "ppapi/c/pp_completion_callback.h"
#include "ppapi/c/pp_errors.h"
#include "ppapi/c/ppb_file_system.h"
#include "ppapi/shared_impl/tracked_callback.h"
#include "ppapi/thunk/enter.h"
#include "ppapi/thunk/ppb_file_system_api.h"
#include "ppapi/thunk/ppb_instance_api.h"
#include "ppapi/thunk/resource_creation_api.h"
#include "ppapi/thunk/thunk.h"

namespace ppapi {
namespace thunk {

namespace {

PP_Resource Create(PP_Instance instance, PP_FileSystemType type) {
  EnterResourceCreation enter(instance);
  if (enter.failed())
    return 0;
  return enter.functions()->CreateFileSystem(instance, type);
}

PP_Bool IsFileSystem(PP_Resource resource) {
  EnterResource<PPB_FileSystem_API> enter(resource, false);
  return PP_FromBool(enter.succeeded());
}

int32_t Open(PP_Resource file_system,
             int64_t expected_size,
             struct PP_CompletionCallback callback) {
  EnterResource<PPB_FileSystem_API> enter(file_system, callback, true);
  if (enter.failed())
    return enter.retval();
  return enter.SetResult(enter.object()->Open(expected_size, enter.callback()));
}

PP_FileSystemType GetType(PP_Resource file_system) {
  EnterResource<PPB_FileSystem_API> enter(file_system, true);
  if (enter.failed())
    return PP_FILESYSTEMTYPE_INVALID;
  return enter.object()->GetType();
}

const PPB_FileSystem_1_0 g_ppb_filesystem_thunk_1_0 = {
  &Create,
  &IsFileSystem,
  &Open,
  &GetType
};

}  // namespace

const PPB_FileSystem_1_0* GetPPB_FileSystem_1_0_Thunk() {
  return &g_ppb_filesystem_thunk_1_0;
}

}  // namespace thunk
}  // namespace ppapi
