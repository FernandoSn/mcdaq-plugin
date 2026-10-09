
#pragma once

#include <cstdint>
#include <memory>

class MCDAQAcquisition
{
public:
    struct Statistics
    {
        uint32_t notifications = 0;
        uint32_t lastSampleCount = 0;
        int scanError = 0;
        long actualRate = 0;
    };

    explicit MCDAQAcquisition (int boardNumber);
    ~MCDAQAcquisition();

    MCDAQAcquisition (const MCDAQAcquisition&) = delete;
    MCDAQAcquisition& operator= (const MCDAQAcquisition&) = delete;

    // Diagnostic only: scan analog input channel 0.
    // Returns zero on success.
    int start (long requestedRate = 1000);

    // Stop acquisition and free MCC resources.
    void stop();

    Statistics getStatistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
