// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/i18n/i18n_api.h"

#include <algorithm>
#include <string>
#include <vector>

#include "base/lazy_instance.h"
#include "base/prefs/pref_service.h"
#include "base/string_piece.h"
#include "base/string_split.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/extensions/api/i18n.h"
#include "chrome/common/extensions/api/i18n/default_locale_handler.h"
#include "chrome/common/extensions/extension_manifest_constants.h"
#include "chrome/common/extensions/manifest_handler.h"
#include "chrome/common/pref_names.h"

namespace GetAcceptLanguages = extensions::api::i18n::GetAcceptLanguages;

namespace extensions {

namespace {

// Errors.
static const char kEmptyAcceptLanguagesError[] = "accept-languages is empty.";

}

bool I18nGetAcceptLanguagesFunction::RunImpl() {
  std::string accept_languages =
      profile()->GetPrefs()->GetString(prefs::kAcceptLanguages);
  // Currently, there are 2 ways to set browser's accept-languages: through UI
  // or directly modify the preference file. The accept-languages set through
  // UI is guranteed to be valid, and the accept-languages string returned from
  // profile()->GetPrefs()->GetString(prefs::kAcceptLanguages) is guranteed to
  // be valid and well-formed, which means each accept-langauge is a valid
  // code, and accept-languages are seperatd by "," without surrrounding
  // spaces. But we do not do any validation (either the format or the validity
  // of the language code) on accept-languages set through editing preference
  // file directly. So, here, we're adding extra checks to be resistant to
  // crashes caused by data corruption.
  if (accept_languages.empty()) {
    error_ = kEmptyAcceptLanguagesError;
    return false;
  }

  std::vector<std::string> languages;
  base::SplitString(accept_languages, ',', &languages);
  languages.erase(std::remove(languages.begin(), languages.end(), ""),
                  languages.end());

  if (languages.empty()) {
    error_ = kEmptyAcceptLanguagesError;
    return false;
  }

  results_ = GetAcceptLanguages::Results::Create(languages);
  return true;
}

I18nAPI::I18nAPI(Profile* profile) {
  ManifestHandler::Register(extension_manifest_keys::kDefaultLocale,
                            make_linked_ptr(new DefaultLocaleHandler));
}

I18nAPI::~I18nAPI() {
}

static base::LazyInstance<ProfileKeyedAPIFactory<I18nAPI> >
    g_factory = LAZY_INSTANCE_INITIALIZER;

// static
ProfileKeyedAPIFactory<I18nAPI>* I18nAPI::GetFactoryInstance() {
  return &g_factory.Get();
}

}  // namespace extensions
