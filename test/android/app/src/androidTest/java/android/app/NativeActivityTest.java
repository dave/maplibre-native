package android.app;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner;
import androidx.test.filters.LargeTest;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.rule.ActivityTestRule;

import org.junit.Assert;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

@LargeTest
@RunWith(AndroidJUnit4ClassRunner.class)
public class NativeActivityTest {

    private static final String GTEST_FILTER = "gtest_filter";

    @Rule
    public ActivityTestRule<NativeActivity> mActivityTestRule = new ActivityTestRule<>(NativeActivity.class, false, false);

    @Test(timeout = 1200000L)
    public void runUnitTest() throws Exception {
        Log.v("UnitTestRunner", "Start the unit test");
        // `am instrument -e gtest_filter <filter>` runs only the matching tests.
        Intent intent = null;
        Bundle arguments = InstrumentationRegistry.getArguments();
        String filter = arguments.getString(GTEST_FILTER);
        if (filter != null) {
            intent = new Intent();
            intent.putExtra(GTEST_FILTER, filter);
        }
        mActivityTestRule.launchActivity(intent);
        while (TestState.running) {
            Log.v("UnitTestRunner", "Unit test is running...");
            Thread.sleep(1000L);
        }
        Log.v("UnitTestRunner", "All unit tests are finished!");
        mActivityTestRule.finishActivity();
        Assert.assertTrue("UnitTestRunner was not successfully finished", TestState.testResult);
    }
}
