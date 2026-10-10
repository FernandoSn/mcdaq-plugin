
#include "MCDAQAcquisition.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

// Include JUCE before MCC's cbw.h to keep MCC macros out of JUCE headers.
#include <DataThreadHeaders.h>
#include <cbw.h>

namespace
{
// Number of 16-bit ADC points, not bytes.
constexpr long BUFFER_POINTS = 65536;

constexpr unsigned NOTIFICATION_POINTS = 32; //256

constexpr size_t COPY_CHUNK_POINTS = 4096;
} // namespace

struct MCDAQAcquisition::Impl
{
    explicit Impl (int number)
        : boardNumber (number),
          scratch (COPY_CHUNK_POINTS)
    {
    }

    RawBlockCallback rawBlockCallback = nullptr;
    void* rawBlockUserData = nullptr;

    uint64_t delivered = 0;

    int boardNumber;

    StopDiagnostics lastStop;

    HGLOBAL buffer = nullptr;

    bool running = false;
    bool eventsEnabled = false;

    std::vector<unsigned short> scratch;

    // Written by the MCC callback.
    std::atomic<uint32_t> notifications { 0 };
    std::atomic<uint32_t> reportedCount { 0 };
    std::atomic<int> scanError { 0 };
    std::atomic<bool> scanErrorEventSeen { false };
    std::atomic<bool> unexpectedEndEventSeen { false };
    std::atomic<bool> intentionalStop { false };

    // All watchdog state below belongs to the worker thread, except that
    // start() resets it before launching the worker and stop() reads it
    // after joining that worker.
    using WatchClock = std::chrono::steady_clock;
    WatchClock::time_point lastWatchdogCheck {};
    WatchClock::time_point lastDriverProgress {};
    uint32_t previousDriverCount = 0;
    bool haveDriverCount = false;
    unsigned consecutiveLagChecks = 0;
    uint64_t watchdogChecks = 0;
    long watchdogDriverCount = -1;
    short watchdogDriverStatus = -1;
    int watchdogStatusQueryError = 0;
    int32_t watchdogNotificationLag = 0;
    WatchdogFault watchdogFault = WatchdogFault::None;

    // Auto-reset: multiple notifications may be coalesced, which is fine.
    // The callback count remains the source of truth for produced samples.
    juce::WaitableEvent dataReady { false };

    // Owned by the acquisition worker thread.
    uint32_t previousReportedCount = 0;

    uint64_t produced = 0;
    uint64_t consumed = 0;

    uint64_t maxUnreadSamples = 0;

    uint64_t checksum = 0;

    uint16_t minimumRaw =
        (std::numeric_limits<uint16_t>::max)();

    uint16_t maximumRaw = 0;

    bool hasSamples = false;
    bool possibleDataLoss = false;

    int readError = 0;
    long actualRate = 0;

    static void __stdcall onEvent (
        int board,
        unsigned eventType,
        unsigned eventData,
        void* userData)
    {
        auto* self = static_cast<Impl*> (userData);

        if (eventType == ON_DATA_AVAILABLE)
        {
            self->reportedCount.store (
                static_cast<uint32_t> (eventData),
                std::memory_order_release);

            self->notifications.fetch_add (
                1, std::memory_order_relaxed);

            self->dataReady.signal();
        }
        else if (eventType == ON_SCAN_ERROR)
        {
            // A zero eventData must not hide the fact that an error
            // notification occurred.
            self->scanError.store (
                static_cast<int> (eventData), std::memory_order_release);
            self->scanErrorEventSeen.store (true, std::memory_order_release);
            self->dataReady.signal();
        }
        else if (eventType == ON_END_OF_INPUT_SCAN)
        {
            if (! self->intentionalStop.load (std::memory_order_acquire))
                self->unexpectedEndEventSeen.store (true, std::memory_order_release);

            self->dataReady.signal();
        }
    }

    void updateProducedCount()
    {
        const uint32_t current =
            reportedCount.load (
                std::memory_order_acquire);

        // Unsigned subtraction handles 32-bit rollover,
        // provided progress is checked sufficiently often.
        const uint32_t delta =
            current - previousReportedCount;

        previousReportedCount = current;

        produced += static_cast<uint64_t> (delta);

        // Track the largest unread backlog observed by the worker.
        const uint64_t unread = produced - consumed;

        if (unread > maxUnreadSamples)
        {
            maxUnreadSamples = unread;
        }
    }
};

MCDAQAcquisition::MCDAQAcquisition (int boardNumber)
    : impl_ (std::make_unique<Impl> (boardNumber))
{
}

MCDAQAcquisition::~MCDAQAcquisition()
{
    stop();
}

void MCDAQAcquisition::setRawBlockCallback (
    RawBlockCallback callback,
    void* userData)
{
    // Configure only while acquisition is stopped.
    if (impl_->running)
        return;

    impl_->rawBlockCallback = callback;
    impl_->rawBlockUserData = userData;
}

int MCDAQAcquisition::start (long requestedRate)
{
    if (impl_->running)
        return 0;

    impl_->lastStop = {};

    impl_->delivered = 0;

    impl_->maxUnreadSamples = 0;

    impl_->notifications.store (0);
    impl_->reportedCount.store (0);
    impl_->scanError.store (0);
    impl_->scanErrorEventSeen.store (false);
    impl_->unexpectedEndEventSeen.store (false);
    impl_->intentionalStop.store (false);
    impl_->watchdogFault = WatchdogFault::None;
    impl_->watchdogChecks = 0;
    impl_->watchdogDriverCount = -1;
    impl_->watchdogDriverStatus = -1;
    impl_->watchdogStatusQueryError = 0;
    impl_->watchdogNotificationLag = 0;
    impl_->previousDriverCount = 0;
    impl_->haveDriverCount = false;
    impl_->consecutiveLagChecks = 0;
    impl_->dataReady.reset(); // Acquisition is stopped; no callback is active yet.

    impl_->previousReportedCount = 0;

    impl_->produced = 0;
    impl_->consumed = 0;
    impl_->checksum = 0;

    impl_->minimumRaw =
        (std::numeric_limits<uint16_t>::max)();

    impl_->maximumRaw = 0;
    impl_->hasSamples = false;

    impl_->possibleDataLoss = false;
    impl_->readError = 0;

    // Native 16-bit ADC sample buffer.
    impl_->buffer = cbWinBufAlloc (BUFFER_POINTS);

    if (impl_->buffer == nullptr)
        return -1;

    const unsigned eventMask =
        ON_DATA_AVAILABLE | ON_SCAN_ERROR | ON_END_OF_INPUT_SCAN;

    int error = cbEnableEvent (
        impl_->boardNumber,
        eventMask,
        NOTIFICATION_POINTS,
        &Impl::onEvent,
        impl_.get());

    if (error != 0)
    {
        cbWinBufFree (impl_->buffer);
        impl_->buffer = nullptr;
        return error;
    }

    impl_->eventsEnabled = true;

    long rate = requestedRate;

    // No SCALEDATA, SINGLEIO, or BLOCKIO.
    // CONVERTDATA aligns native ADC codes when needed.
    const int options =
        BACKGROUND | CONTINUOUS | CONVERTDATA;

    error = cbAInScan (
        impl_->boardNumber,
        0,
        0,
        BUFFER_POINTS,
        &rate,
        BIP10VOLTS,
        impl_->buffer,
        options);

    if (error != 0)
    {
        cbDisableEvent (
            impl_->boardNumber,
            ALL_EVENT_TYPES);

        impl_->eventsEnabled = false;

        cbWinBufFree (impl_->buffer);
        impl_->buffer = nullptr;

        return error;
    }

    impl_->actualRate = rate;
    impl_->running = true;
    impl_->lastWatchdogCheck = Impl::WatchClock::now();
    impl_->lastDriverProgress = impl_->lastWatchdogCheck;

    return 0;
}

void MCDAQAcquisition::waitForData (double timeoutMilliseconds)
{
    if (! impl_->running)
        return;

    // Avoid sleeping if a callback arrived while the worker was processing
    // the previous block, or an error was already reported.
    if (impl_->scanError.load (std::memory_order_acquire) != 0 ||
        impl_->reportedCount.load (std::memory_order_acquire) !=
            impl_->previousReportedCount)
        return;

    // An event arriving just before wait() is retained by the auto-reset
    // event. Even if a notification is missed, the timeout lets us recheck.
    impl_->dataReady.wait (timeoutMilliseconds);
}

void MCDAQAcquisition::wakeWorker()
{
    impl_->dataReady.signal();
}

bool MCDAQAcquisition::consumeAvailable()
{
    if (! impl_->running || impl_->buffer == nullptr)
        return false;

    if (impl_->scanErrorEventSeen.load (std::memory_order_acquire))
    {
        impl_->watchdogFault = WatchdogFault::ScanErrorEvent;
        return false;
    }

    if (impl_->unexpectedEndEventSeen.load (std::memory_order_acquire))
    {
        impl_->watchdogFault = WatchdogFault::UnexpectedScanEnd;
        return false;
    }

    if (impl_->scanError.load() != 0)
        return false;

    if (impl_->possibleDataLoss || impl_->readError != 0)
        return false;

    impl_->updateProducedCount();

    // Check whether unconsumed samples may have
    // been overwritten in the MCC circular buffer.
    if (impl_->produced - impl_->consumed >= static_cast<uint64_t> (BUFFER_POINTS))
    {
        impl_->possibleDataLoss = true;
        return false;
    }

    while (impl_->consumed < impl_->produced)
    {
        const uint64_t available =
            impl_->produced - impl_->consumed;

        const size_t firstPoint =
            static_cast<size_t> (
                impl_->consumed % BUFFER_POINTS);

        // Never request a copy across the physical
        // end of the circular buffer.
        const size_t untilWrap =
            static_cast<size_t> (BUFFER_POINTS) - firstPoint;

        const size_t count = static_cast<size_t> (
            std::min<uint64_t> (
                available,
                (std::min)(
                    COPY_CHUNK_POINTS,
                    untilWrap)));

        const int error = cbWinBufToArray (
            impl_->buffer,
            impl_->scratch.data(),
            static_cast<long> (firstPoint),
            static_cast<long> (count));

        if (error != 0)
        {
            impl_->readError = error;
            return false;
        }

        // Producer may have advanced while copying.
        // Check again before accepting this block.
        impl_->updateProducedCount();

        if (impl_->produced - impl_->consumed >= static_cast<uint64_t> (BUFFER_POINTS))
        {
            impl_->possibleDataLoss = true;
            return false;
        }

        // Diagnostic processing of native samples.
        // Later this block will be passed to Open Ephys.
        for (size_t i = 0; i < count; ++i)
        {
            const uint16_t value =
                impl_->scratch[i];

            impl_->minimumRaw =
                (std::min)(impl_->minimumRaw, value);


            impl_->maximumRaw =
                (std::max)(impl_->maximumRaw, value);

            impl_->checksum += value;
        }
        
        impl_->hasSamples = true;

        // These samples were successfully copied out of UL.
        const uint64_t firstSample = impl_->consumed;

        impl_->consumed += count;

        // Forward the raw block to Open Ephys through our callback.
        if (impl_->rawBlockCallback != nullptr)
        {
            const std::size_t accepted =
                impl_->rawBlockCallback (
                    impl_->scratch.data(),
                    count,
                    firstSample,
                    impl_->rawBlockUserData);

            // Protect against an invalid callback result.
            if (accepted > count)
            {
                impl_->readError = -3;
                impl_->possibleDataLoss = true;
                return false;
            }

            impl_->delivered += accepted;

            if (accepted != count)
            {
                // Open Ephys did not accept the entire block.
                impl_->readError = -3;
                impl_->possibleDataLoss = true;
                return false;
            }
        }

    }

    return true;
}


bool MCDAQAcquisition::checkWatchdog()
{
    if (! impl_->running || impl_->buffer == nullptr)
        return false;

    // Even if no new samples have arrived, check callback error flags now.
    // Do not rely on eventData being nonzero for ON_SCAN_ERROR.
    if (impl_->scanErrorEventSeen.load (std::memory_order_acquire))
    {
        impl_->watchdogFault = WatchdogFault::ScanErrorEvent;
        return false;
    }

    if (impl_->unexpectedEndEventSeen.load (std::memory_order_acquire))
    {
        impl_->watchdogFault = WatchdogFault::UnexpectedScanEnd;
        return false;
    }

    if (impl_->watchdogFault != WatchdogFault::None)
        return false;

    const auto now = Impl::WatchClock::now();

    // Check at most twice/second at 1 kHz, and more often as the rate
    // increases so one watchdog interval is a small fraction of the
    // UL ring-buffer duration. The scan is currently single-channel.
    const double actualRate = static_cast<double> (
        (std::max)(1L, impl_->actualRate));
    const double intervalMs = (std::max)(
        50.0, (std::min)(500.0, 125.0 * BUFFER_POINTS / actualRate));

    if (std::chrono::duration<double, std::milli> (
            now - impl_->lastWatchdogCheck).count() < intervalMs)
        return true;

    impl_->lastWatchdogCheck = now;
    ++impl_->watchdogChecks;

    const DriverStatus driver = getDriverStatus();
    impl_->watchdogDriverCount = driver.currentCount;
    impl_->watchdogDriverStatus = driver.status;
    impl_->watchdogStatusQueryError = driver.error;

    if (driver.error != 0)
    {
        impl_->watchdogFault = WatchdogFault::StatusQueryFailed;
        return false;
    }

    // MCC: 1 is RUNNING; 0 means IDLE. A continuous scan must
    // remain RUNNING until MCDAQ explicitly requests shutdown.
    if (driver.status != 1)
    {
        impl_->watchdogFault = WatchdogFault::ScanStoppedUnexpectedly;
        return false;
    }

    // Windows 'long' and UL's CurCount are signed 32-bit. Compare modulo
    // 2^32 to tolerate the documented counter rollover.
    const uint32_t current = static_cast<uint32_t> (driver.currentCount);

    if (! impl_->haveDriverCount)
    {
        impl_->previousDriverCount = current;
        impl_->haveDriverCount = true;
        impl_->lastDriverProgress = now;
    }
    else
    {
        const int32_t delta = static_cast<int32_t> (
            current - impl_->previousDriverCount);

        if (delta < 0)
        {
            impl_->watchdogFault = WatchdogFault::DriverCountWentBackwards;
            return false;
        }

        if (delta > 0)
        {
            impl_->lastDriverProgress = now;
            impl_->previousDriverCount = current;
        }
    }

    // A continuously clocked 1 kHz (or faster) scan should not stop
    // advancing for two seconds while UL still claims it is running.
    if (std::chrono::duration<double> (
            now - impl_->lastDriverProgress).count() >= 2.0)
    {
        impl_->watchdogFault = WatchdogFault::DriverCountStalled;
        return false;
    }

    // Driver count and callback count are sampled at different instants.
    // A small positive difference is normal between USB notifications.
    const uint32_t callbackCount = impl_->reportedCount.load (
        std::memory_order_acquire);
    const int32_t notificationLag = static_cast<int32_t> (
        current - callbackCount);
    impl_->watchdogNotificationLag = notificationLag;

    // A lag of at least half the UL buffer capacity is unsafe NOW.
    if (notificationLag >= BUFFER_POINTS / 2)
    {
        impl_->watchdogFault = WatchdogFault::NotificationsLagging;
        return false;
    }

    // Otherwise require two consecutive unusually large differences
    // to avoid reacting to normal packet batching or scheduling jitter.
    const int64_t rateQuarterSecond = static_cast<int64_t> (actualRate / 4.0);
    const int64_t lagLimit = (std::min)(
        static_cast<int64_t> (BUFFER_POINTS / 4),
        (std::max)(static_cast<int64_t> (NOTIFICATION_POINTS * 8),
                   rateQuarterSecond));

    if (notificationLag > lagLimit)
    {
        if (++impl_->consecutiveLagChecks >= 2)
        {
            impl_->watchdogFault = WatchdogFault::NotificationsLagging;
            return false;
        }
    }
    else
    {
        impl_->consecutiveLagChecks = 0;
    }

    return true;
}

void MCDAQAcquisition::stop()
{
    if (! impl_->running)
        return;

    auto& result = impl_->lastStop;

    // The hardware may emit ON_END_OF_INPUT_SCAN during an intentional
    // stop. Never interpret that event as an unexpected scan failure.
    impl_->intentionalStop.store (true, std::memory_order_release);

    // 1. Stop hardware acquisition.
    result.stopError = cbStopBackground (
        impl_->boardNumber,
        AIFUNCTION);

    if (result.stopError != 0)
    {
        impl_->scanError.store (result.stopError);

        // Do not free a buffer while hardware may
        // still be writing into it.
        return;
    }

    // 2. Get the final driver count before freeing
    // the UL buffer.
    result.statusError = cbGetStatus (
        impl_->boardNumber,
        &result.status,
        &result.currentCount,
        &result.currentIndex,
        AIFUNCTION);

    // 3. Disable MCC callbacks before reconciling
    // the final cumulative count.
    if (impl_->eventsEnabled)
    {
        result.disableEventError = cbDisableEvent (
            impl_->boardNumber,
            ALL_EVENT_TYPES);

        if (result.disableEventError != 0)
        {
            impl_->scanError.store (
                result.disableEventError);
            return;
        }

        impl_->eventsEnabled = false;
    }
    else
    {
        result.disableEventError = 0;
    }

    // 4. Verify that the scan stopped and that the
    // reported buffer index matches our sample count.
    if (result.statusError == 0 && result.status == 0 && result.currentIndex >= -1)
    {
        // UL's cumulative count has 32-bit semantics.
        // Use unsigned subtraction to handle rollover.
        const uint32_t finalCount =
            static_cast<uint32_t> (
                result.currentCount);

        const uint32_t delta =
            finalCount - impl_->previousReportedCount;

        const uint64_t finalProduced =
            impl_->produced + static_cast<uint64_t> (delta);

        const long expectedIndex =
            finalProduced == 0
                ? -1
                : static_cast<long> (
                      (finalProduced - 1) % BUFFER_POINTS);

        const bool validCount =
            finalProduced >= impl_->consumed && finalProduced - impl_->consumed < static_cast<uint64_t> (BUFFER_POINTS);

        const bool validIndex =
            result.currentIndex == expectedIndex;

        if (validCount && validIndex)
        {
            // 5. Publish the final hardware count to
            // the existing consumer.
            impl_->reportedCount.store (
                finalCount,
                std::memory_order_release);

            result.drainAttempted = true;

            // The hardware is stopped but 'running'
            // remains true until the final copy finishes.
            result.drainSucceeded =
                consumeAvailable();
        }
        else
        {
            impl_->possibleDataLoss = true;
        }
    }
    else
    {
        impl_->possibleDataLoss = true;
    }

    // 6. Capture final accounting before cleanup.
    result.finalProduced = impl_->produced;
    result.finalConsumed = impl_->consumed;

    impl_->running = false;

    // 7. Release the acquisition memory only
    // after attempting the final drain.
    if (impl_->buffer != nullptr)
    {
        cbWinBufFree (impl_->buffer);
        impl_->buffer = nullptr;
    }
}

MCDAQAcquisition::Statistics
    MCDAQAcquisition::getStatistics() const
{
    Statistics result;

    result.notifications =
        impl_->notifications.load();

    result.lastSampleCount =
        impl_->reportedCount.load();

    result.totalSamplesProduced = impl_->produced;
    result.totalSamplesConsumed = impl_->consumed;

    result.minimumRaw =
        impl_->hasSamples ? impl_->minimumRaw : 0;

    result.maximumRaw =
        impl_->hasSamples ? impl_->maximumRaw : 0;

    result.rawChecksum = impl_->checksum;

    result.scanError = impl_->scanError.load();
    result.readError = impl_->readError;
    result.possibleDataLoss = impl_->possibleDataLoss;

    result.actualRate = impl_->actualRate;

    result.totalSamplesDelivered = impl_->delivered;

    result.maxUnreadSamples = impl_->maxUnreadSamples;
    result.watchdogFault = impl_->watchdogFault;
    result.watchdogChecks = impl_->watchdogChecks;
    result.watchdogDriverCount = impl_->watchdogDriverCount;
    result.watchdogDriverStatus = impl_->watchdogDriverStatus;
    result.watchdogStatusQueryError = impl_->watchdogStatusQueryError;
    result.watchdogNotificationLag = impl_->watchdogNotificationLag;
    result.scanErrorEventSeen = impl_->scanErrorEventSeen.load();
    result.unexpectedEndEventSeen = impl_->unexpectedEndEventSeen.load();

    return result;
}

MCDAQAcquisition::DriverStatus
    MCDAQAcquisition::getDriverStatus() const
{
    DriverStatus result;

    // Only query an active background scan.
    if (! impl_->running || impl_->buffer == nullptr)
        return result;

    result.error = cbGetStatus (
        impl_->boardNumber,
        &result.status,
        &result.currentCount,
        &result.currentIndex,
        AIFUNCTION);

    return result;
}

MCDAQAcquisition::StopDiagnostics
    MCDAQAcquisition::getStopDiagnostics() const
{
    return impl_->lastStop;
}