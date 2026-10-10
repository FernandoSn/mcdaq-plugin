
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

class MCDAQAcquisition
{
public:
    struct DriverStatus
    {
        int error = -1;
        short status = -1;

        long currentCount = -1;
        long currentIndex = -1;
    };

    
    struct StopDiagnostics
    {
        int stopError = -1;
        int statusError = -1;
        int disableEventError = -1;

        short status = -1;
        long currentCount = -1;
        long currentIndex = -1;

        bool drainAttempted = false;
        bool drainSucceeded = false;

        uint64_t finalProduced = 0;
        uint64_t finalConsumed = 0;
    };


    // Latched on the acquisition worker; non-zero means data integrity
    // can no longer be verified. Codes are deliberately stable for logs.
    enum class WatchdogFault : int
    {
        None = 0,
        ScanErrorEvent = 1,
        UnexpectedScanEnd = 2,
        StatusQueryFailed = 3,
        ScanStoppedUnexpectedly = 4,
        DriverCountStalled = 5,
        NotificationsLagging = 6,
        DriverCountWentBackwards = 7
    };

    struct Statistics
    {
        uint32_t notifications = 0;
        uint32_t lastSampleCount = 0;

        uint64_t totalSamplesProduced = 0;
        uint64_t totalSamplesConsumed = 0;

        uint16_t minimumRaw = 0;
        uint16_t maximumRaw = 0;
        uint64_t rawChecksum = 0;

        int scanError = 0;
        int readError = 0;
        bool possibleDataLoss = false;

        long actualRate = 0;
        long bufferPoints = 0;
        unsigned notificationPoints = 0;

        uint64_t totalSamplesDelivered = 0;

        uint64_t maxUnreadSamples = 0;

        WatchdogFault watchdogFault = WatchdogFault::None;
        uint64_t watchdogChecks = 0;
        long watchdogDriverCount = -1;
        short watchdogDriverStatus = -1;
        int watchdogStatusQueryError = 0;
        int32_t watchdogNotificationLag = 0;
        bool scanErrorEventSeen = false;
        bool unexpectedEndEventSeen = false;
    };

    explicit MCDAQAcquisition (int boardNumber);
    ~MCDAQAcquisition();

    MCDAQAcquisition (const MCDAQAcquisition&) = delete;
    MCDAQAcquisition& operator= (const MCDAQAcquisition&) = delete;

    int start (long requestedRate = 1000);
    void stop();

    // Consume newly reported native ADC samples.
    // Returns false on an acquisition-integrity failure.
    bool consumeAvailable();

    // Periodic independent UL status check, called ONLY by the worker.
    // Does not alter the callback sample counter or copy buffer contents.
    // Returns false and latches a fault if continuous acquisition is unsafe.
    bool checkWatchdog();

    // Called by the Open Ephys DataThread worker (never by the MCC callback).
    // Wakes when MCC reports data/error, or when timeoutMilliseconds elapses.
    // The consumer must still check the cumulative counter after waking.
    void waitForData (double timeoutMilliseconds = 100.0);

    // Interrupt the worker's wait before joining it during shutdown.
    void wakeWorker();

    // Call from the acquisition worker, or after it stops.
    Statistics getStatistics() const;

    DriverStatus getDriverStatus() const;

    StopDiagnostics getStopDiagnostics() const;

    using RawBlockCallback = std::size_t (*) (
        const uint16_t* samples,
        std::size_t count,
        uint64_t firstSample,
        void* userData);

    void setRawBlockCallback (
        RawBlockCallback callback,
        void* userData);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
