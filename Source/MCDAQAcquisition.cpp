
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
// Sizes are in native ADC sample points (not bytes or frames).
// Keep the validated 65536-point minimum, then grow with throughput.
constexpr long MIN_BUFFER_POINTS = 65536;
constexpr long MAX_BUFFER_POINTS = 4194304;
constexpr unsigned MIN_NOTIFICATION_POINTS = 32;
constexpr size_t COPY_CHUNK_POINTS = 4096;

// Request approximately 32 ms of samples per event. MCC USB transfers
// are packetized; a multiple of 32 is appropriate for these two boards.
// Round up to a multiple of both MCC USB packet sizes (64 points) and
// the number of enabled analog inputs. This prevents an arbitrary channel
// queue (e.g., 3, 5, 7 inputs) from straddling the ring wrap mid-frame.
uint64_t gcd (uint64_t a, uint64_t b)
{
    while (b != 0) { const uint64_t rem = a % b; a = b; b = rem; }
    return a;
}

uint64_t commonMultiple (uint64_t points, uint64_t channels)
{
    return points / gcd (points, channels) * channels;
}

uint64_t roundUp (uint64_t count, uint64_t alignment)
{
    return ((count + alignment - 1u) / alignment) * alignment;
}

unsigned chooseNotificationPoints (long rate, int channels)
{
    const uint64_t desired =
        (static_cast<uint64_t> (rate) * 32u + 999u) / 1000u;
    const uint64_t alignment = commonMultiple (32u, static_cast<uint64_t> (channels));
    const uint64_t rounded = roundUp ((std::max<uint64_t>) (desired, MIN_NOTIFICATION_POINTS), alignment);
    return rounded > 8192u ? 0u : static_cast<unsigned> (rounded);
}

// Preserve tested powers of two for 1,2,4,8 channels. For other counts,
// round slightly upward to a ring capacity divisible by 64 and channel count.
int configureChannelQueue (int boardNumber, const std::vector<int>& channels,
                           int rangeVolts)
{
    const int n = static_cast<int> (channels.size());
    if (n < 1 || n > 8 ||
        (rangeVolts != 10 && rangeVolts != 5 &&
         rangeVolts != 2 && rangeVolts != 1))
        return -4;

    short channelQueue[8] {};
    short gainQueue[8] {};
    const short rangeCode = rangeVolts == 10 ? BIP10VOLTS :
        rangeVolts == 5 ? BIP5VOLTS :
        rangeVolts == 2 ? BIP2VOLTS : BIP1VOLTS;

    for (int i = 0; i < n; ++i)
    {
        if (channels[i] < 0 || channels[i] >= 8 ||
            (i && channels[i] <= channels[i-1]))
            return -4;
        channelQueue[i] = static_cast<short> (channels[i]);
        gainQueue[i] = rangeCode;
    }
    return cbALoadQueue (boardNumber, channelQueue, gainQueue, n);
}

long chooseBufferPoints (long rate, int channels)
{
    const uint64_t desired = (std::max<uint64_t>) (
        MIN_BUFFER_POINTS, static_cast<uint64_t> (rate) * 4u);
    uint64_t capacity = MIN_BUFFER_POINTS;
    while (capacity < desired && capacity < MAX_BUFFER_POINTS)
        capacity *= 2u;
    capacity = roundUp (capacity, commonMultiple (64u, static_cast<uint64_t> (channels)));
    return capacity > MAX_BUFFER_POINTS ? 0L : static_cast<long> (capacity);
}
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

    // Optional digital-only callback timestamp mailbox. No locks, memory
    // allocations or extra UL functions are executed in the MCC callback.
    // Atomic flag prevents concurrent callbacks from publishing a torn pair.
    std::atomic<bool> timingObservationsEnabled { false };
    std::atomic_flag timingWriterBusy = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> timingVersion { 0 }; // even = stable
    std::atomic<std::uint32_t> timingCount { 0 };
    std::atomic<std::int64_t> timingClockTicks { 0 };
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
    long actualRate = 0; // per channel
    int channelCount = 1;
    uint64_t trailingPartialPoints = 0;
    long bufferPoints = MIN_BUFFER_POINTS;
    unsigned notificationPoints = MIN_NOTIFICATION_POINTS;

    static void __stdcall onEvent (
        int board,
        unsigned eventType,
        unsigned eventData,
        void* userData)
    {
        auto* self = static_cast<Impl*> (userData);

        if (eventType == ON_DATA_AVAILABLE)
        {
            if (self->timingObservationsEnabled.load (std::memory_order_relaxed))
            {
                // One monotonic host-clock read, never a driver query.
                const auto callbackTime = WatchClock::now().time_since_epoch().count();
                if (!self->timingWriterBusy.test_and_set (std::memory_order_acquire))
                {
                    self->timingVersion.fetch_add (1, std::memory_order_acq_rel); // odd
                    self->timingClockTicks.store (callbackTime, std::memory_order_relaxed);
                    self->timingCount.store (eventData, std::memory_order_relaxed);
                    self->timingVersion.fetch_add (1, std::memory_order_release); // even
                    self->timingWriterBusy.clear (std::memory_order_release);
                }
            }
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

void MCDAQAcquisition::enableTimingObservations (bool enable) noexcept
{
    // Enable only during plugin setup before cbAInScan has been called.
    impl_->timingObservationsEnabled.store (enable, std::memory_order_release);
}

bool MCDAQAcquisition::latestTimingObservation (TimingObservation& out) const noexcept
{
    // Do not spin on the MCC callback. A missed observation is harmless for
    // the long-term fit; retry on the next digital poll.
    const auto startVersion = impl_->timingVersion.load (std::memory_order_acquire);
    if (startVersion == 0 || (startVersion & 1u) != 0) return false;
    const auto count = impl_->timingCount.load (std::memory_order_relaxed);
    const auto ticks = impl_->timingClockTicks.load (std::memory_order_relaxed);
    const auto finishVersion = impl_->timingVersion.load (std::memory_order_acquire);
    if (startVersion != finishVersion || ticks == 0) return false;
    out.sequence = finishVersion / 2;
    out.aggregatePointCount = count;
    out.receivedAt = std::chrono::steady_clock::time_point (
        std::chrono::steady_clock::duration (ticks));
    return true;
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

int MCDAQAcquisition::probeActualRate (int boardNumber, long requestedRate,
                                       const std::vector<int>& channels,
                                       int rangeVolts, long& actualRate)
{
    actualRate = 0;
    const int n = static_cast<int> (channels.size());
    if (requestedRate < 500 || requestedRate > 100000 || n < 1 || n > 8)
        return -4;

    const long bufferPoints = chooseBufferPoints (requestedRate * n, n);
    if (bufferPoints <= 0 || bufferPoints % n != 0)
        return -4;

    const int queueError = configureChannelQueue (boardNumber, channels, rangeVolts);
    if (queueError != 0)
        return queueError;

    // Use the same configuration, buffering and scan options as a real
    // acquisition. No event handler and no Open Ephys delivery are active.
    HGLOBAL probeBuffer = cbWinBufAlloc (bufferPoints);
    if (! probeBuffer)
        return -1;

    long rate = requestedRate;
    const int error = cbAInScan (
        boardNumber, channels.front(), channels.back(), bufferPoints, &rate,
        rangeVolts == 10 ? BIP10VOLTS :
            rangeVolts == 5 ? BIP5VOLTS :
            rangeVolts == 2 ? BIP2VOLTS : BIP1VOLTS,
        probeBuffer, BACKGROUND | CONTINUOUS | CONVERTDATA);

    if (error != 0)
    {
        cbWinBufFree (probeBuffer);
        return error;
    }

    const int stopError = cbStopBackground (boardNumber, AIFUNCTION);
    if (stopError != 0)
    {
        // Do not free memory that an unsuccessfully stopped background scan
        // may still be writing to. Fail closed rather than risk use-after-free.
        return stopError;
    }

    cbWinBufFree (probeBuffer);
    if (rate <= 0)
        return -4;
    actualRate = rate;
    return 0;
}

int MCDAQAcquisition::start (long requestedRate, const std::vector<int>& channels, int rangeVolts)
{
    if (impl_->running)
        return 0;

    // These are intentionally the first checks: avoid unbounded allocations
    // and never start with an invalid or unsupported rate.
    const int channelCount = static_cast<int> (channels.size());
    if (requestedRate < 500 || requestedRate > 100000 ||
        channelCount < 1 || channelCount > 8 ||
        (rangeVolts != 10 && rangeVolts != 5 && rangeVolts != 2 && rangeVolts != 1))
        return -4;
    for (int index = 0; index < channelCount; ++index)
        if (channels[index] < 0 || channels[index] >= 8 ||
            (index > 0 && channels[index] <= channels[index - 1]))
            return -4;

    // Both scan counters and circular-buffer positions are ADC points,
    // not timestamps/frames. Make all scan boundaries whole frames.
    const long aggregateRate = requestedRate * channelCount;
    impl_->bufferPoints = chooseBufferPoints (aggregateRate, channelCount);
    impl_->notificationPoints = chooseNotificationPoints (aggregateRate, channelCount);
    impl_->channelCount = channelCount;
    impl_->trailingPartialPoints = 0;
    if (impl_->bufferPoints == 0 ||
        impl_->notificationPoints == 0 ||
        impl_->bufferPoints % channelCount != 0 ||
        impl_->notificationPoints % static_cast<unsigned> (channelCount) != 0)
        return -4;

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

    // Configure the hardware channel/gain queue on EVERY scan start.
    // This also replaces any previous queue when the channel set changes.
    // Both supported boards require unique, ascending channel numbers.
    // USB-204 ignores gain because its input range is fixed at +/-10 V.
    const short rangeCode = rangeVolts == 10 ? BIP10VOLTS :
        rangeVolts == 5 ? BIP5VOLTS :
        rangeVolts == 2 ? BIP2VOLTS : BIP1VOLTS;
    const int queueError = configureChannelQueue (
        impl_->boardNumber, channels, rangeVolts);
    if (queueError != 0)
        return queueError;

    // Native 16-bit ADC sample buffer.
    impl_->buffer = cbWinBufAlloc (impl_->bufferPoints);

    if (impl_->buffer == nullptr)
        return -1;

    const unsigned eventMask =
        ON_DATA_AVAILABLE | ON_SCAN_ERROR | ON_END_OF_INPUT_SCAN;

    int error = cbEnableEvent (
        impl_->boardNumber,
        eventMask,
        impl_->notificationPoints,
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
        channels.front(),
        channels.back(),
        impl_->bufferPoints,
        &rate,
        rangeCode,
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
    if (impl_->produced - impl_->consumed >= static_cast<uint64_t> (impl_->bufferPoints))
    {
        impl_->possibleDataLoss = true;
        return false;
    }

    // MCC data follow the ascending hardware channel queue. Do not deliver a
    // partial frame (even if the hardware count happens to end there).
    const uint64_t channels = static_cast<uint64_t> (impl_->channelCount);
    while (impl_->produced - impl_->consumed >= channels)
    {
        const uint64_t available =
            impl_->produced - impl_->consumed;

        const size_t firstPoint =
            static_cast<size_t> (
                impl_->consumed % impl_->bufferPoints);

        // Never request a copy across the physical
        // end of the circular buffer.
        const size_t untilWrap =
            static_cast<size_t> (impl_->bufferPoints) - firstPoint;

        const size_t wholeFrames = static_cast<size_t> (
            std::min<uint64_t> (
                available / channels,
                (std::min) (
                    static_cast<uint64_t> (COPY_CHUNK_POINTS / impl_->channelCount),
                    static_cast<uint64_t> (untilWrap / impl_->channelCount))));
        const size_t count = wholeFrames * impl_->channelCount;
        if (count == 0 || impl_->consumed % channels != 0)
        {
            impl_->readError = -5; // Invalid frame alignment.
            impl_->possibleDataLoss = true;
            return false;
        }

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

        if (impl_->produced - impl_->consumed >= static_cast<uint64_t> (impl_->bufferPoints))
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
    // UL ring-buffer duration. Rate here is aggregate ADC points/s.
    const double actualRate = static_cast<double> (
        (std::max)(1L, impl_->actualRate)) * impl_->channelCount;
    const double intervalMs = (std::max)(
        50.0, (std::min)(500.0, 125.0 * impl_->bufferPoints / actualRate));

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

    // A continuously clocked scan should not stop
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
    if (notificationLag >= impl_->bufferPoints / 2)
    {
        impl_->watchdogFault = WatchdogFault::NotificationsLagging;
        return false;
    }

    // Otherwise require two consecutive unusually large differences
    // to avoid reacting to normal packet batching or scheduling jitter.
    const int64_t rateQuarterSecond = static_cast<int64_t> (actualRate / 4.0);
    const int64_t lagLimit = (std::min)(
        static_cast<int64_t> (impl_->bufferPoints / 4),
        (std::max)(static_cast<int64_t> (impl_->notificationPoints * 8),
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

        // UL's CurIndex points to the FIRST ADC point of the last
        // completed channel scan, not to the last individual ADC point.
        // With N channels and K complete frames, that means the first
        // point of frame K-1. The old (finalProduced-1) expression was
        // correct only for single-channel acquisition and incorrectly
        // rejected every clean multichannel stop.
        //
        // Deliberately floor to complete frames here: any trailing
        // incomplete frame is separately rejected below, not hidden.
        const uint64_t channels = static_cast<uint64_t> (impl_->channelCount);
        const uint64_t completedFrames = finalProduced / channels;
        const long expectedIndex =
            completedFrames == 0
                ? -1
                : static_cast<long> (
                      ((completedFrames - 1) * channels) % impl_->bufferPoints);

        const bool validCount =
            finalProduced >= impl_->consumed && finalProduced - impl_->consumed < static_cast<uint64_t> (impl_->bufferPoints);

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
            result.drainSucceeded = consumeAvailable();
            impl_->trailingPartialPoints =
                impl_->produced - impl_->consumed;
            // A multiplexed acquisition can stop after a partial final
            // scan: CurCount includes the unfinished ADC points while
            // CurIndex still identifies the last COMPLETE channel scan.
            //
            // Those points cannot form a synchronized multichannel frame.
            // Account for them explicitly as a terminal fragment, but do
            // not misclassify them as missing complete frames. The complete
            // frames must already have been copied successfully, the final
            // CurIndex must agree with the last complete scan, and the
            // remainder must be strictly smaller than one frame.
            //
            // All other loss detectors (buffer overrun, bad CurIndex,
            // mismatched counters, callback rejection, read errors, etc.)
            // remain enabled; none is cleared or bypassed here.
            if (impl_->trailingPartialPoints >= channels ||
                impl_->consumed % channels != 0 ||
                impl_->consumed + impl_->trailingPartialPoints != finalProduced)
            {
                impl_->possibleDataLoss = true;
                result.drainSucceeded = false;
            }
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
    result.bufferPoints = impl_->bufferPoints;
    result.notificationPoints = impl_->notificationPoints;
    result.channelCount = impl_->channelCount;
    result.trailingPartialPoints = impl_->trailingPartialPoints;

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