
#include "MCDAQDeviceManager.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <unordered_set>
#include <utility>

// Keep the MCC header after standard C++ headers.
// This avoids exposing MCC macros to other headers.
#include <cbw.h>

namespace
{
int initializeUL()
{
    static const int result = cbIgnoreInstaCal();
    return result;
}
// Shared across MCDAQ instances in this plugin.
std::mutex& reservationMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<int>& reservedBoardNumbers()
{
    static std::unordered_set<int> boards;
    return boards;
}

// Convert fixed-size MCC character arrays safely.
std::string descriptorString (
    const char* text,
    std::size_t capacity)
{
    std::size_t length = 0;

    while (length < capacity && text[length] != '\0')
        ++length;

    return std::string (text, length);
}
} // namespace

// ============================================================
// Private implementation
// ============================================================

struct MCDAQDeviceManager::Impl
{
    // Original UL descriptors, needed for future hardware calls.
    std::vector<DaqDeviceDescriptor> descriptors;

    // MCC-independent device information.
    std::vector<DeviceInfo> devices;

    // Selection owned by this MCDAQ instance.
    int selectedBoardNumber = -1;
    std::string selectedProductName;
    std::string selectedUniqueId;
};

// ============================================================
// Constructor / Destructor
// ============================================================

MCDAQDeviceManager::MCDAQDeviceManager()
    : impl_ (std::make_unique<Impl>())
{
}

MCDAQDeviceManager::~MCDAQDeviceManager()
{
    releaseSelection();
}

// ============================================================
// Device discovery
// ============================================================

int MCDAQDeviceManager::refresh()
{
    std::lock_guard<std::mutex> lock (reservationMutex());

    const int initError = initializeUL();

    if (initError != 0)
        return initError;

    // A selected board must be released before refreshing.
    if (impl_->selectedBoardNumber >= 0)
        return -1;

    constexpr int capacity = 100;

    // Allocate descriptors on the heap, not the stack.
    std::vector<DaqDeviceDescriptor> inventory (capacity);

    int count = capacity;

    const int error = cbGetDaqDeviceInventory (
        USB_IFC,
        inventory.data(),
        &count);

    if (error != 0)
    {
        impl_->descriptors.clear();
        impl_->devices.clear();
        return error;
    }

    // Reject invalid inventory counts rather than silently
    // truncating the device list.
    if (count < 0 || count > capacity)
    {
        impl_->descriptors.clear();
        impl_->devices.clear();
        return -2;
    }

    inventory.resize (static_cast<std::size_t> (count));

    std::vector<DeviceInfo> discovered;
    discovered.reserve (inventory.size());

    for (const auto& descriptor : inventory)
    {
        DeviceInfo info;

        info.productName = descriptorString (
            descriptor.ProductName,
            sizeof (descriptor.ProductName));

        info.uniqueId = descriptorString (
            descriptor.UniqueID,
            sizeof (descriptor.UniqueID));

        info.productId = descriptor.ProductID;

        // Board number assigned by UL / InstaCal.
        // Returns -1 if not registered.
        info.boardNumber = -1;

        discovered.push_back (std::move (info));
    }

    impl_->descriptors = std::move (inventory);
    impl_->devices = std::move (discovered);

    return 0;
}

const std::vector<MCDAQDeviceManager::DeviceInfo>&
    MCDAQDeviceManager::getDevices() const
{
    return impl_->devices;
}

// ============================================================
// Board reservation
// ============================================================


bool MCDAQDeviceManager::selectFirstAvailableBoard()
{
    std::lock_guard<std::mutex> lock (reservationMutex());

    // Already selected by this instance.
    if (impl_->selectedBoardNumber >= 0)
        return true;

    for (std::size_t i = 0;
         i < impl_->descriptors.size() && i < impl_->devices.size();
         ++i)
    {
        const auto& descriptor = impl_->descriptors[i];
        auto& device = impl_->devices[i];

        // This source plugin is designed and validated only for these
        // two boards. Do not reserve unrelated MCC USB devices.
        if (device.productName != "USB-204" &&
            device.productName != "USB-1608FS-Plus")
            continue;

        // Another instance may have already registered it.
        if (cbGetBoardNumber (descriptor) >= 0)
            continue;

        // Find the first logical board number not reserved
        // by another MCDAQ instance.
        int boardNum = 0;

        while (boardNum <= 99 && reservedBoardNumbers().count (boardNum) != 0)
        {
            ++boardNum;
        }

        if (boardNum > 99)
            return false;

        // Register the physical device with Universal Library.
        const int error = cbCreateDaqDevice (
            boardNum,
            descriptor);

        if (error != 0)
        {
            // Do not reserve a board if creation failed.
            // Error reporting will be expanded next.
            continue;
        }

        // Device registration succeeded.
        reservedBoardNumbers().insert (boardNum);

        impl_->selectedBoardNumber = boardNum;
        impl_->selectedProductName = device.productName;
        impl_->selectedUniqueId = device.uniqueId;

        device.boardNumber = boardNum;

        return true;
    }

    return false;
}


void MCDAQDeviceManager::releaseSelection()
{
    std::lock_guard<std::mutex> lock (reservationMutex());

    const int boardNum = impl_->selectedBoardNumber;

    if (boardNum < 0)
        return;

    // Release the UL device that MCDAQ created.
    const int error = cbReleaseDaqDevice (boardNum);

    if (error != 0)
    {
        // Keep the reservation if UL release failed.
        // We must not let another instance reuse it.
        return;
    }

    reservedBoardNumbers().erase (boardNum);

    for (auto& device : impl_->devices)
    {
        if (device.boardNumber == boardNum)
            device.boardNumber = -1;
    }

    impl_->selectedBoardNumber = -1;
    impl_->selectedProductName.clear();
    impl_->selectedUniqueId.clear();
}

// ============================================================
// Selection information
// ============================================================

int MCDAQDeviceManager::getSelectedBoardNumber() const
{
    std::lock_guard<std::mutex> lock (reservationMutex());

    return impl_->selectedBoardNumber;
}

std::string
    MCDAQDeviceManager::getSelectedProductName() const
{
    std::lock_guard<std::mutex> lock (reservationMutex());

    return impl_->selectedProductName;
}

std::string
    MCDAQDeviceManager::getSelectedUniqueId() const
{
    std::lock_guard<std::mutex> lock (reservationMutex());

    return impl_->selectedUniqueId;
}


MCDAQDeviceManager::ScanCapabilities
MCDAQDeviceManager::getSelectedScanCapabilities() const
{
    const std::string product = getSelectedProductName();
    if (product == "USB-204")
        return { true, false, 8, 8, 500000, 500000 };
    if (product == "USB-1608FS-Plus")
        return { true, true, 8, 8, 100000, 400000 };
    return {};
}

MCDAQDeviceManager::SyncOutputResult
MCDAQDeviceManager::enableSelectedSyncOutput()
{
    SyncOutputResult result;
    std::lock_guard<std::mutex> lock (reservationMutex());

    // SYNC is a bidirectional pacer pin on the USB-1608FS-Plus.
    // Do not apply this configuration to the USB-204 or other boards.
    if (impl_->selectedBoardNumber < 0 ||
        impl_->selectedProductName != "USB-1608FS-Plus")
        return result;

    const int boardNum = impl_->selectedBoardNumber;

    // The plug-in uses cbIgnoreInstaCal(), so configure this setting
    // explicitly on the dynamically registered logical board.
    result.setError = cbSetConfig (
        BOARDINFO, boardNum, 0, BIINPUTPACEROUT, ENABLED);

    if (result.setError != 0)
        return result;

    result.getError = cbGetConfig (
        BOARDINFO, boardNum, 0, BIINPUTPACEROUT, &result.readback);

    result.enabled = (result.getError == 0 && result.readback == ENABLED);
    return result;
}

int MCDAQDeviceManager::configureSelectedDigitalInputs (std::uint8_t selectedMask) const
{
    const int boardNum = getSelectedBoardNumber();
    if (boardNum < 0) return -1;
    for (int channel = 0; channel < 8; ++channel)
    {
        if ((selectedMask & static_cast<std::uint8_t> (1u << channel)) == 0)
            continue;
        const int error = cbDConfigBit (boardNum, AUXPORT, channel, DIGITALIN);
        if (error != 0) return error;
    }
    return 0;
}

int MCDAQDeviceManager::readSelectedDigitalPort (std::uint8_t& portValue) const
{
    portValue = 0;
    const int boardNum = getSelectedBoardNumber();
    if (boardNum < 0) return -1;
    unsigned short value = 0;
    const int error = cbDIn (boardNum, AUXPORT, &value);
    if (error == 0) portValue = static_cast<std::uint8_t> (value & 0xffu);
    return error;
}

int MCDAQDeviceManager::readAnalogVoltage (
    int channel,
    float& volts) const
{
    volts = 0.0f;

    // Both initial target boards have 8 analog inputs.
    if (channel < 0 || channel >= 8)
        return -2;

    std::lock_guard<std::mutex> lock (reservationMutex());

    const int boardNum = impl_->selectedBoardNumber;

    if (boardNum < 0)
        return -1;

    unsigned short rawValue = 0;

    // Both USB-204 and USB-1608FS-Plus support +/-10 V.
    const int error = cbAIn (
        boardNum,
        channel,
        BIP10VOLTS,
        &rawValue);

    if (error != 0)
        return error;

    // Convert the raw ADC reading to volts.
    return cbToEngUnits (
        boardNum,
        BIP10VOLTS,
        rawValue,
        &volts);
}
