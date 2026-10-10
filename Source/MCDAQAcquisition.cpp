
#include "MCDAQAcquisition.h"

#include <algorithm>
#include <atomic>
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
            self->scanError.store (
                static_cast<int> (eventData), std::memory_order_release);
            self->dataReady.signal();
        }
        else if (eventType == ON_END_OF_INPUT_SCAN)
        {
            // Wake the worker if acquisition ends unexpectedly.
            // Status/error handling will be hardened in the next milestone.
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

void MCDAQAcquisition::stop()
{
    if (! impl_->running)
        return;

    auto& result = impl_->lastStop;

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