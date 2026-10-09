
#pragma once

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

    // Call from the acquisition worker, or after it stops.
    Statistics getStatistics() const;

    DriverStatus getDriverStatus() const;

    StopDiagnostics getStopDiagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
