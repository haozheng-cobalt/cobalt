// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.vr;

import static org.chromium.chrome.browser.vr.XrTestFramework.PAGE_LOAD_TIMEOUT_S;
import static org.chromium.chrome.browser.vr.XrTestFramework.POLL_TIMEOUT_LONG_MS;
import static org.chromium.chrome.browser.vr.XrTestFramework.POLL_TIMEOUT_SHORT_MS;

import androidx.test.filters.MediumTest;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.RuleChain;
import org.junit.runner.RunWith;

import org.chromium.base.test.params.ParameterAnnotations.ClassParameter;
import org.chromium.base.test.params.ParameterAnnotations.UseRunnerDelegate;
import org.chromium.base.test.params.ParameterSet;
import org.chromium.base.test.params.ParameterizedRunner;
import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.base.test.util.DisableIf;
import org.chromium.chrome.browser.flags.ChromeSwitches;
import org.chromium.chrome.browser.vr.rules.XrActivityRestriction;
import org.chromium.chrome.browser.vr.util.VrTestRuleUtils;
import org.chromium.chrome.test.ChromeActivityTestRule;
import org.chromium.chrome.test.ChromeJUnit4RunnerDelegate;

import java.util.List;
import java.util.concurrent.Callable;

/**
 * End-to-end tests for various scenarios around when the permission prompt is expected.
 */
@RunWith(ParameterizedRunner.class)
@UseRunnerDelegate(ChromeJUnit4RunnerDelegate.class)
// TODO(crbug.com/1192004): Remove --allow-pre-commit-input once the root cause of the
// failures has been fixed.
@CommandLineFlags.Add({ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE,
        "enable-features=WebXR,LogJsConsoleMessages", "allow-pre-commit-input"})
public class WebXrVrPermissionTest {
    @ClassParameter
    private static List<ParameterSet> sClassParams =
            VrTestRuleUtils.generateDefaultTestRuleParameters();
    @Rule
    public RuleChain mRuleChain;

    private ChromeActivityTestRule mTestRule;
    private WebXrVrPermissionTestFramework mWebXrVrPermissionTestFramework;

    public WebXrVrPermissionTest(Callable<ChromeActivityTestRule> callable) throws Exception {
        mTestRule = callable.call();
        mRuleChain = VrTestRuleUtils.wrapRuleInActivityRestrictionRule(mTestRule);
    }

    @Before
    public void setUp() {
        mWebXrVrPermissionTestFramework = new WebXrVrPermissionTestFramework(mTestRule);
    }

    /**
     * Tests that denying permission blocks the session from being created.
     */
    @Test
    @MediumTest
    @XrActivityRestriction({XrActivityRestriction.SupportedActivity.ALL})
    public void testPermissionDenyFailsSessionCreation() {
        mWebXrVrPermissionTestFramework.setPermissionPromptAction(
                WebXrVrTestFramework.PERMISSION_PROMPT_ACTION_DENY);
        mWebXrVrPermissionTestFramework.setPermissionPromptExpected(true);

        mWebXrVrPermissionTestFramework.loadFileAndAwaitInitialization(
                "test_webxr_permission", PAGE_LOAD_TIMEOUT_S);

        mWebXrVrPermissionTestFramework.enterSessionWithUserGesture();
        mWebXrVrPermissionTestFramework.pollJavaScriptBooleanOrFail(
                "sessionInfos[sessionTypes.IMMERSIVE].error != null", POLL_TIMEOUT_LONG_MS);
        mWebXrVrPermissionTestFramework.runJavaScriptOrFail(
                "verifyPermissionDeniedError(sessionTypes.IMMERSIVE)", POLL_TIMEOUT_SHORT_MS);
        mWebXrVrPermissionTestFramework.assertNoJavaScriptErrors();
    }

    /**
     * Tests that attempting to enter a session that requires the same permission level/type does
     * not reprompt.
     */
    @Test
    @MediumTest
    @XrActivityRestriction({XrActivityRestriction.SupportedActivity.ALL})
    public void testVrPermissionPersistance() {
        mWebXrVrPermissionTestFramework.loadFileAndAwaitInitialization(
                "generic_webxr_page", PAGE_LOAD_TIMEOUT_S);
        mWebXrVrPermissionTestFramework.enterSessionWithUserGestureOrFail();
        mWebXrVrPermissionTestFramework.endSession();

        mWebXrVrPermissionTestFramework.setPermissionPromptExpected(false);

        mWebXrVrPermissionTestFramework.enterSessionWithUserGestureOrFail();
    }

    /**
     * Tests that attempting to enter an inline session with no special features does not require
     * permission.
     */
    @Test
    @MediumTest
    @XrActivityRestriction({XrActivityRestriction.SupportedActivity.ALL})
    public void testPermissionNotNeededForInline() {
        mWebXrVrPermissionTestFramework.setPermissionPromptExpected(false);

        mWebXrVrPermissionTestFramework.loadFileAndAwaitInitialization(
                "test_webxr_permission", PAGE_LOAD_TIMEOUT_S);

        mWebXrVrPermissionTestFramework.runJavaScriptOrFail(
                "requestMagicWindowSession()", POLL_TIMEOUT_SHORT_MS);
        mWebXrVrPermissionTestFramework.pollJavaScriptBooleanOrFail(
                "sessionInfos[sessionTypes.MAGIC_WINDOW].currentSession != null",
                POLL_TIMEOUT_LONG_MS);
    }

    /**
     * Tests that granted permissions persist after a page reload.
     */
    @Test
    @MediumTest
    @XrActivityRestriction({XrActivityRestriction.SupportedActivity.ALL})
    @DisableIf.Build(message = "crbug.com/1429697", hardware_is = "marlin",
            sdk_is_greater_than = 28, sdk_is_less_than = 30)
    public void
    testPermissionPersistsAfterReload() {
        mWebXrVrPermissionTestFramework.loadFileAndAwaitInitialization(
                "generic_webxr_page", PAGE_LOAD_TIMEOUT_S);

        mWebXrVrPermissionTestFramework.enterSessionWithUserGestureOrFail();
        mWebXrVrPermissionTestFramework.endSession();

        mWebXrVrPermissionTestFramework.loadFileAndAwaitInitialization(
                "generic_webxr_page", PAGE_LOAD_TIMEOUT_S);

        mWebXrVrPermissionTestFramework.setPermissionPromptExpected(false);

        mWebXrVrPermissionTestFramework.enterSessionWithUserGestureOrFail();
    }
}
