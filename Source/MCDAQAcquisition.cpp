
#include "MCDAQAcquisition.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

#include <cbw.h>

namespace
{
// Number of 16-bit ADC points, not bytes.
constexpr long BUFFER_POINTS = 65536;

constexpr unsigned NOTIFICATION_POINTS = 256;

constexpr size_t COPY_CHUNK_POINTS = 4096;
} // namespace

struct MCDAQAcquisition::Impl
{
    explicit Impl (int number)
        : boardNumber (number),
          scratch (COPY_CHUNK_POINTS)
    {
    }

    int boardNumber;

    HGLOBAL buffer = nullptr;

    bool running = false;
    bool eventsEnabled = false;

    std::vector<unsigned short> scratch;

    // Written by the MCC callback.
    std::atomic<uint32_t> notifications { 0 };
    std::atomic<uint32_t> reportedCount { 0 };
    std::atomic<int> scanError { 0 };

    // Owned by the acquisition worker thread.
    uint32_t previousReportedCount = 0;

    uint64_t produced = 0;
    uint64_t consumed = 0;

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
        }
        else if (eventType == ON_SCAN_ERROR)
        {
            self->scanError.store (
                static_cast<int> (eventData));
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

int MCDAQAcquisition::start (long requestedRate)
{
    if (impl_->running)
        return 0;

    impl_->notifications.store (0);
    impl_->reportedCount.store (0);
    impl_->scanError.store (0);

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
        impl_->consumed += count;
    }

    return true;
}

void MCDAQAcquisition::stop()
{
    if (impl_->running)
    {
        cbStopBackground (
            impl_->boardNumber,
            AIFUNCTION);

        impl_->running = false;
    }

    if (impl_->eventsEnabled)
    {
        cbDisableEvent (
            impl_->boardNumber,
            ALL_EVENT_TYPES);

        impl_->eventsEnabled = false;
    }

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

    return result;
}
