#include <mln/test.hpp>
#include "test_runner_common.hpp"

#include <unistd.h>
#include <mutex>
#include <thread>
#include <gtest/gtest.h>
#include <android/log.h>

using namespace mln;
using namespace mln::android;

std::string testResultToStr(const ::testing::TestResult* testResult) {
    if (testResult->Passed()) return " (PASSED)";
    if (testResult->Skipped()) return " (SKIPPED)";
    if (testResult->Failed()) return " (FAILED)";
    return "";
}

class GTestAndroidLogger : public ::testing::EmptyTestEventListener {
public:
    void OnTestProgramStart(const ::testing::UnitTest&) override {
        __android_log_print(ANDROID_LOG_INFO, tag, "Test program started.");
    }

    void OnTestProgramEnd(const ::testing::UnitTest& unit_test) override {
        __android_log_print(
            ANDROID_LOG_INFO, tag, "Test program ended with %d tests run.", unit_test.total_test_count());
    }

    void OnTestStart(const ::testing::TestInfo& test_info) override {
        __android_log_print(ANDROID_LOG_INFO, tag, "Starting: %s.%s", test_info.test_case_name(), test_info.name());
    }

    void OnTestEnd(const ::testing::TestInfo& test_info) override {
        __android_log_print(ANDROID_LOG_INFO,
                            tag,
                            "Finished: %s.%s%s",
                            test_info.test_case_name(),
                            test_info.name(),
                            testResultToStr(test_info.result()).c_str());
    }

    void OnTestPartResult(const ::testing::TestPartResult& test_part_result) override {
        __android_log_print(ANDROID_LOG_INFO,
                            tag,
                            "%s: %s",
                            test_part_result.failed() ? "FAILED" : "PASSED",
                            test_part_result.summary());
    }

    const char* tag = "GTest";
};

std::atomic<bool> running{true};
std::atomic<bool> success{false};
std::once_flag done;

// The runner thread belongs to the process, not to an activity. The system can
// destroy the activity while the tests run (a configuration or overlay change),
// and its android_main must then return at once: the activity's onDestroy waits
// for it. A joinable std::thread destroyed on that return would end the process.
// The activity made in its place finds the thread running and waits for it.
std::thread* runnerThread = nullptr; // never freed: a thread may still run at exit

// The looper of the activity that waits for the runner; null while there is none.
std::mutex looperMutex;
ALooper* looper = nullptr;

void wakeLooper() {
    const std::scoped_lock lock(looperMutex);
    if (looper != nullptr) {
        ALooper_wake(looper);
    }
}

void setLooper(ALooper* newLooper) {
    const std::scoped_lock lock(looperMutex);
    if (looper != nullptr) {
        ALooper_release(looper);
    }
    looper = newLooper;
    if (looper != nullptr) {
        ALooper_acquire(looper);
    }
}

// The tests the runner runs when the instrumentation names none.
const std::string defaultGtestFilter = "-StringIndexer.GetOOBIdentity";

void runner(const std::string& gtestOutput, const std::string& gtestFilter) {
    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
    listeners.Append(new GTestAndroidLogger); // takes ownership

    std::vector<std::string> arguments = {
        "mbgl-test-runner", "--gtest_output=xml:" + gtestOutput, "--gtest_filter=" + gtestFilter};
    std::vector<char*> argv;
    for (const auto& arg : arguments) {
        argv.push_back((char*)arg.data());
    }
    argv.push_back(nullptr);

    mln::Log::Info(mln::Event::General, "Start TestRunner");
    int status = mln::runTests(argv.size(), argv.data());
    mln::Log::Info(mln::Event::General, "TestRunner finished with status: '" + std::to_string(status) + "'");
    success = (status == 0);
    running = false;
    wakeLooper();
}

void android_main(struct android_app* app) {
    mln::android::theJVM = app->activity->vm;
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, NULL);
    setLooper(ALooper_forThread());

    std::string storagePath(app->activity->internalDataPath);
    std::string zipFile = storagePath + "/data.zip";
    const std::string gtestOutput = storagePath + "/results.xml";

    if (runnerThread != nullptr) {
        mln::Log::Info(mln::Event::General, "The activity was made again; the tests are still running");
    } else if (copyFile(env, app->activity->assetManager, zipFile, storagePath, "data.zip")) {
        if (chdir(storagePath.c_str())) {
            mln::Log::Error(mln::Event::General, "Failed to change the directory to storage dir");
            changeState(env, app, success);
        } else {
            unZipFile(env, zipFile, storagePath);
            // `am instrument -e gtest_filter <filter>` (NativeActivityTest) passes the filter on.
            std::string gtestFilter = intentStringExtra(env, app, "gtest_filter");
            if (gtestFilter.empty()) {
                gtestFilter = defaultGtestFilter;
            }
            runnerThread = new std::thread(runner, gtestOutput, gtestFilter);
        }
    } else {
        mln::Log::Error(mln::Event::General, "Failed to copy zip file '" + zipFile + "' to external storage");
        changeState(env, app, success);
    }

    int outFd, outEvents;
    struct android_poll_source* source = nullptr;

    while (true) {
        ALooper_pollOnce(-1, &outFd, &outEvents, reinterpret_cast<void**>(&source));
        if (source != nullptr) {
            source->process(app, source);
        }

        if (!running) {
            std::call_once(done, [&] {
                mln::Log::Info(mln::Event::General, "TestRunner done");
                runnerThread->join();
                changeState(env, app, success);
            });
        }

        if (app->destroyRequested != 0) {
            setLooper(nullptr);
            app->activity->vm->DetachCurrentThread();
            mln::Log::Info(mln::Event::General, "Close the App!");
            return;
        }
    }
}
