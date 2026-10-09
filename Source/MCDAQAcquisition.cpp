
#include "MCDAQAcquisition.h"

#include <atomic>
#include <cbw.h>

struct MCDAQAcquisition::Impl
{
    explicit Impl (int number) : boardNumber (number) {}

    int boardNumber;

    HGLOBAL buffer = nullptr;
    bool running = false;
    bool eventsEnabled = false;

    std::atomic<uint32_t> notifications { 0 };
    std::atomic<uint32_t> lastSampleCount { 0 };
    std::atomic<int> scanError { 0 };

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
            self->lastSampleCount.store (eventData);
            self->notifications.fetch_add (1);
        }
        else if (eventType == ON_SCAN_ERROR)
        {
            self->scanError.store (
                static_cast<int> (eventData));
        }

        // No logging, allocation, conversion, or buffer copies
        // inside the hardware notification callback.
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

    constexpr long bufferPoints = 32768;
    constexpr unsigned notificationPoints = 256;

    impl_->notifications.store (0);
    impl_->lastSampleCount.store (0);
    impl_->scanError.store (0);

    impl_->buffer = cbScaledWinBufAlloc (bufferPoints);

    if (impl_->buffer == nullptr)
        return -1;

    const unsigned eventMask =
        ON_DATA_AVAILABLE | ON_SCAN_ERROR | ON_END_OF_INPUT_SCAN;

    int error = cbEnableEvent (
        impl_->boardNumber,
        eventMask,
        notificationPoints,
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

    error = cbAInScan (
        impl_->boardNumber,
        0, // Low channel
        0, // High channel
        bufferPoints,
        &rate,
        BIP10VOLTS,
        impl_->buffer,
        BACKGROUND | CONTINUOUS | SCALEDATA);

    if (error != 0)
    {
        cbDisableEvent (impl_->boardNumber, eventMask);
        impl_->eventsEnabled = false;

        cbWinBufFree (impl_->buffer);
        impl_->buffer = nullptr;

        return error;
    }

    impl_->actualRate = rate;
    impl_->running = true;

    return 0;
}

void MCDAQAcquisition::stop()
{
    if (impl_->running)
    {
        cbStopBackground (impl_->boardNumber, AIFUNCTION);
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

    result.notifications = impl_->notifications.load();
    result.lastSampleCount = impl_->lastSampleCount.load();
    result.scanError = impl_->scanError.load();
    result.actualRate = impl_->actualRate;

    return result;
}
