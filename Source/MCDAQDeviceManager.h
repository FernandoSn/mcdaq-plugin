
#pragma once

#include <memory>
#include <string>
#include <vector>

class MCDAQDeviceManager
{
public:
    // Public representation of an MCC device.
    // No MCC Universal Library types are exposed here.
    struct DeviceInfo
    {
        std::string productName;
        std::string uniqueId;

        unsigned int productId = 0;
        int boardNumber = -1;
    };

    // Known-board continuous scan capabilities (hardware-paced; not BURSTIO).
    // No generic fallback: unknown boards are not treated as compatible.
    struct ScanCapabilities
    {
        bool supported = false;
        bool simultaneous = false;
        int analogChannels = 0;
        long maxPerChannelHz = 0;
        long maxAggregateHz = 0;
    };

    MCDAQDeviceManager();
    ~MCDAQDeviceManager();

    // A manager belongs to one MCDAQ instance.
    MCDAQDeviceManager (const MCDAQDeviceManager&) = delete;
    MCDAQDeviceManager& operator= (const MCDAQDeviceManager&) = delete;

    // Discover connected USB DAQ devices.
    // Returns 0 on success, an error code otherwise.
    // Must not be called while a device is selected.
    int refresh();

    // Available devices discovered during refresh().
    const std::vector<DeviceInfo>& getDevices() const;

    // Reserve the first available, registered MCC board.
    // Returns false if no suitable unreserved board exists.
    bool selectFirstAvailableBoard();

    // Release this instance's internal board reservation.
    void releaseSelection();

    // Information about the selected board.
    int getSelectedBoardNumber() const;

    std::string getSelectedProductName() const;
    std::string getSelectedUniqueId() const;
    ScanCapabilities getSelectedScanCapabilities() const;

    // Diagnostic: read one analog input in volts.
    // Not intended for continuous acquisition.
    int readAnalogVoltage (int channel, float& volts) const;

private:
    // Hides MCC-specific data and implementation details.
    struct Impl;

    std::unique_ptr<Impl> impl_;
};
