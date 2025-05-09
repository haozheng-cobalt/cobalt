// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/password_manager/core/browser/affiliation/mock_affiliated_match_helper.h"

#include <utility>

#include "components/password_manager/core/browser/password_form.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace password_manager {

MockAffiliatedMatchHelper::MockAffiliatedMatchHelper()
    : AffiliatedMatchHelper(nullptr) {}

MockAffiliatedMatchHelper::MockAffiliatedMatchHelper(
    AffiliationService* affiliation_service)
    : AffiliatedMatchHelper(affiliation_service) {}

MockAffiliatedMatchHelper::~MockAffiliatedMatchHelper() = default;

void MockAffiliatedMatchHelper::ExpectCallToGetAffiliatedAndroidRealms(
    const PasswordFormDigest& expected_observed_form,
    const std::vector<std::string>& results_to_return) {
  EXPECT_CALL(*this, OnGetAffiliatedAndroidRealmsCalled(expected_observed_form))
      .WillOnce(testing::Return(results_to_return));
}

void MockAffiliatedMatchHelper::ExpectCallToGetGroup(
    const PasswordFormDigest& expected_observed_form,
    const std::vector<std::string>& results_to_return) {
  EXPECT_CALL(*this, OnGetGroup(expected_observed_form))
      .WillOnce(testing::Return(results_to_return));
}

void MockAffiliatedMatchHelper::GetAffiliatedAndroidAndWebRealms(
    const PasswordFormDigest& observed_form,
    AffiliatedRealmsCallback result_callback) {
  std::vector<std::string> affiliated_android_realms =
      OnGetAffiliatedAndroidRealmsCalled(observed_form);
  std::move(result_callback).Run(affiliated_android_realms);
}

void MockAffiliatedMatchHelper::GetGroup(
    const PasswordFormDigest& observed_form,
    AffiliatedRealmsCallback result_callback) {
  std::vector<std::string> affiliated_android_realms =
      OnGetGroup(observed_form);
  std::move(result_callback).Run(affiliated_android_realms);
}

void MockAffiliatedMatchHelper::
    ExpectCallToInjectAffiliationAndBrandingInformation(
        const std::vector<AffiliationAndBrandingInformation>&
            results_to_inject) {
  EXPECT_CALL(*this, OnInjectAffiliationAndBrandingInformationCalled())
      .WillOnce(testing::Return(results_to_inject));
}

void MockAffiliatedMatchHelper::InjectAffiliationAndBrandingInformation(
    std::vector<std::unique_ptr<PasswordForm>> forms,
    PasswordFormsOrErrorCallback result_callback) {
  const std::vector<AffiliationAndBrandingInformation>& information =
      OnInjectAffiliationAndBrandingInformationCalled();
  if (information.empty()) {
    std::move(result_callback).Run(std::move(forms));
    return;
  }

  ASSERT_EQ(information.size(), forms.size());
  for (size_t i = 0; i < forms.size(); ++i) {
    forms[i]->affiliated_web_realm = information[i].affiliated_web_realm;
    forms[i]->app_display_name = information[i].app_display_name;
    forms[i]->app_icon_url = information[i].app_icon_url;
  }
  std::move(result_callback).Run(std::move(forms));
}

}  // namespace password_manager
