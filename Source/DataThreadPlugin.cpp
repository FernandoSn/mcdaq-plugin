/*
 ------------------------------------------------------------------

 This file is part of the Open Ephys GUI
 Copyright (C) 2022 Open Ephys

 ------------------------------------------------------------------

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#include "DataThreadPlugin.h"
#include "DataThreadPluginEditor.h"

// Include MCC-specific definitions after Open Ephys headers
#include "MCDAQDeviceManager.h"
#include "MCDAQAcquisition.h"

// Set to 1 when detailed acquisition diagnostics are needed.
#define MCDAQ_VERBOSE_ACQUISITION_LOGS 0

struct PluginSettingsObject
{
    // Store settings for the plugin here
};


DataThreadPlugin::DataThreadPlugin (SourceNode* sn)
    : DataThread (sn),
      deviceManager (std::make_unique<MCDAQDeviceManager>())
{
    LOGC ("MCDAQ: Scanning for MCC USB devices...");

    const int error = deviceManager->refresh();

    if (error != 0)
    {
        LOGE ("MCDAQ: Device discovery failed. Error: ", error);
        return;
    }

    const auto& devices = deviceManager->getDevices();

    LOGC ("MCDAQ: Found ", (int) devices.size(), " device(s).");

    for (size_t i = 0; i < devices.size(); ++i)
    {
        const auto& device = devices[i];

        LOGC ("MCDAQ: [", (int) i, "] ", device.productName.c_str(), " | Unique ID: ", device.uniqueId.c_str(), " | Board number: ", device.boardNumber);
    }

    // Automatically reserve one available board.
    if (deviceManager->selectFirstAvailableBoard())
    {
        const auto name =
            deviceManager->getSelectedProductName();

        const auto id =
            deviceManager->getSelectedUniqueId();

        LOGC ("MCDAQ: Selected ",
              name.c_str(),
              " | Board number: ",
              deviceManager->getSelectedBoardNumber(),
              " | Unique ID: ",
              id.c_str());

        float voltage = 0.0f;

        const int readError =
            deviceManager->readAnalogVoltage (0, voltage);

        if (readError == 0)
        {
            LOGC ("MCDAQ: AI0 voltage = ", voltage, " V");
        }
        else
        {
            LOGE ("MCDAQ: Analog input test failed. UL error: ",
                  readError);
        }

    }
    else
    {
        LOGE ("MCDAQ: No unassigned, registered MCC "
              "board is available.");
    }
}


DataThreadPlugin::~DataThreadPlugin()
{
}

bool DataThreadPlugin::foundInputSource()
{
    return deviceManager && deviceManager->getSelectedBoardNumber() >= 0 &&
           deviceManager->getSelectedScanCapabilities().supported;
}

namespace
{
constexpr long allowedRates[] = { 500, 1000, 2000, 5000, 10000, 25000, 30000 };
bool allowedRate (long hz)
{
    for (const long rate : allowedRates)
        if (rate == hz) return true;
    return false;
}
}

std::vector<long> DataThreadPlugin::getAvailableRates() const
{
    std::vector<long> result;
    if (! deviceManager) return result;
    const auto cap = deviceManager->getSelectedScanCapabilities();
    if (! cap.supported) return result;
    for (const long rate : allowedRates)
        if (rate <= cap.maxPerChannelHz &&
            static_cast<int64_t> (rate) * selectedChannelCount <= cap.maxAggregateHz)
            result.push_back (rate);
    return result;
}

bool DataThreadPlugin::isChannelEnabled (int channel) const noexcept
{
    return channel >= 0 && channel < 8 &&
           (selectedChannelMask & static_cast<std::uint8_t> (1u << channel)) != 0;
}

std::vector<int> DataThreadPlugin::getSelectedChannels() const
{
    std::vector<int> result;
    for (int channel = 0; channel < 8; ++channel)
        if (isChannelEnabled (channel))
            result.push_back (channel);
    return result;
}

bool DataThreadPlugin::setChannelMask (std::uint8_t mask)
{
    if (CoreServices::getAcquisitionStatus() || acquisition || isThreadRunning() || mask == 0 || ! deviceManager)
        return false;

    const auto cap = deviceManager->getSelectedScanCapabilities();
    if (! cap.supported || (mask >> cap.analogChannels) != 0)
        return false;

    int count = 0;
    for (int channel = 0; channel < 8; ++channel)
        if (mask & (1u << channel)) ++count;

    if (selectedSampleRateHz > cap.maxPerChannelHz ||
        static_cast<int64_t> (selectedSampleRateHz) * count > cap.maxAggregateHz)
        return false;

    selectedChannelMask = mask;
    selectedChannelCount = count;
    rateNegotiated = false;
    return true;
}

std::string DataThreadPlugin::getBoardName() const
{
    return deviceManager ? deviceManager->getSelectedProductName() : std::string();
}

bool DataThreadPlugin::canSelectVoltageRange() const
{
    return getBoardName() == "USB-1608FS-Plus";
}

bool DataThreadPlugin::setVoltageRangeVolts (int fullScaleVolts)
{
    if (CoreServices::getAcquisitionStatus() || acquisition || isThreadRunning())
        return false;

    if (fullScaleVolts != 10 && fullScaleVolts != 5 &&
        fullScaleVolts != 2 && fullScaleVolts != 1)
        return false;

    if (! canSelectVoltageRange() && fullScaleVolts != 10)
        return false;

    selectedRangeVolts = fullScaleVolts;
    rateNegotiated = false;
    return true;
}

bool DataThreadPlugin::setSampleRateHz (long rateHz)
{
    if (CoreServices::getAcquisitionStatus() || acquisition || isThreadRunning() ||
        ! allowedRate (rateHz))
        return false;
    const auto cap = deviceManager->getSelectedScanCapabilities();
    if (! cap.supported || rateHz > cap.maxPerChannelHz ||
        static_cast<int64_t> (rateHz) * selectedChannelCount > cap.maxAggregateHz)
        return false;
    selectedSampleRateHz = rateHz;
    rateNegotiated = false;
    return true;
}

bool DataThreadPlugin::isSimultaneousSampling() const
{
    return deviceManager && deviceManager->getSelectedScanCapabilities().simultaneous;
}

std::string DataThreadPlugin::getSamplingModeDescription() const
{
    const auto cap = deviceManager->getSelectedScanCapabilities();
    if (! cap.supported) return "Unknown board: unsupported";
    return cap.simultaneous ? "Simultaneous ADCs" : "Multiplexed (sequential ADC)";
}

long DataThreadPlugin::getDeclaredSampleRateHz() const
{
    if (! deviceManager || ! rateNegotiated ||
        negotiatedRequestHz != selectedSampleRateHz ||
        negotiatedChannelMask != selectedChannelMask ||
        negotiatedRangeVolts != selectedRangeVolts ||
        negotiatedBoardNum != deviceManager->getSelectedBoardNumber())
        return 0; // Unknown: never advertise an assumed hardware rate.

    return negotiatedRateHz;
}

bool DataThreadPlugin::negotiateSelectedRate()
{
    if (getDeclaredSampleRateHz() > 0)
        return true;
    if (! deviceManager || ! foundInputSource() ||
        CoreServices::getAcquisitionStatus() || acquisition || isThreadRunning())
        return false;

    const int boardNum = deviceManager->getSelectedBoardNumber();
    long actualRate = 0;
    const int error = MCDAQAcquisition::probeActualRate (
        boardNum, selectedSampleRateHz, getSelectedChannels(),
        selectedRangeVolts, actualRate);

    if (error != 0 || actualRate <= 0)
    {
        rateNegotiated = false;
        negotiatedRateHz = 0;
        LOGE ("MCDAQ: Hardware clock preflight failed for ",
              getBoardName().c_str(), " [", selectedChannelCount,
              " channels] at nominal ", selectedSampleRateHz,
              " Hz/channel. UL error: ", error,
              ". Stream registration blocked to protect timestamps.");
        return false;
    }

    negotiatedBoardNum = boardNum;
    negotiatedRequestHz = selectedSampleRateHz;
    negotiatedChannelMask = selectedChannelMask;
    negotiatedRangeVolts = selectedRangeVolts;
    negotiatedRateHz = actualRate;
    rateNegotiated = true;
    LOGC ("MCDAQ: Hardware clock negotiated: requested ",
          selectedSampleRateHz, " Hz/channel | Actual ", actualRate,
          " Hz/channel | Channels: ", selectedChannelCount,
          " | Board: ", getBoardName().c_str());
    return true;
}

void DataThreadPlugin::updateSettings (
    OwnedArray<ContinuousChannel>* continuousChannels,
    OwnedArray<EventChannel>* eventChannels,
    OwnedArray<SpikeChannel>* spikeChannels,
    OwnedArray<DataStream>* dataStreams,
    OwnedArray<DeviceInfo>* devices,
    OwnedArray<ConfigurationObject>* configurationObjects)
{
    continuousChannels->clear();
    eventChannels->clear();
    spikeChannels->clear();
    dataStreams->clear();
    devices->clear();
    configurationObjects->clear();

    if (! foundInputSource() || ! negotiateSelectedRate())
        return;

    const std::string product =
        deviceManager->getSelectedProductName();

    const std::string uniqueId =
        deviceManager->getSelectedUniqueId();

    const String streamName =
        "MCDAQ " + String (product.c_str());

    const String streamIdentifier =
        "mcdaq-" + String (uniqueId.c_str());

    // One synchronous stream; channel order follows the ascending MCC queue.
    const float sampleRate = static_cast<float> (getDeclaredSampleRateHz());

    DataStream::Settings streamSettings {
        streamName,
        "Analog input from MCC Universal Library",
        streamIdentifier,
        sampleRate,
        true
    };

    auto* stream = new DataStream (streamSettings);
    dataStreams->add (stream);

    // Nominal voltage resolution for the two boards.
    // We will validate scaling before recording.
    const float bitVolts =
        product == "USB-204"
            ? 20.0f / 4096.0f
            : (2.0f * selectedRangeVolts) / 65536.0f;

    for (const int channel : getSelectedChannels())
    {
        const String number (channel);
        ContinuousChannel::Settings channelSettings {
            ContinuousChannel::Type::ADC,
            "AI" + number,
            "MCC analog input channel " + number,
            "mcdaq-ai" + number,
            bitVolts,
            stream
        };
        continuousChannels->add (new ContinuousChannel (channelSettings));
    }

    // Placeholder event channel. Digital acquisition
    // is not yet implemented.
    EventChannel::Settings eventSettings {
        EventChannel::Type::TTL,
        "MCDAQ Digital Inputs",
        "Reserved for future MCC digital acquisition",
        "mcdaq-ttl",
        stream,
        8
    };

    eventChannels->add (
        new EventChannel (eventSettings));

    juce::String channelNames;
    for (const int channel : getSelectedChannels())
    {
        if (channelNames.isNotEmpty()) channelNames += ",";
        channelNames += "AI" + String (channel);
    }
    LOGC ("MCDAQ: Registered [", channelNames,
          "] (", selectedChannelCount, " channels) at ", sampleRate,
          " Hz/channel | Aggregate nominal: ",
          sampleRate * selectedChannelCount,
          " points/s | Sampling: ", getSamplingModeDescription().c_str(),
          " | Input range: +/-", selectedRangeVolts,
          " V | Nominal bitVolts: ", bitVolts);
}

bool DataThreadPlugin::startAcquisition()
{
    LOGC ("MCDAQ DEBUG 1: Entered startAcquisition()");

    if (! foundInputSource() || getDeclaredSampleRateHz() <= 0)
    {
        LOGE ("MCDAQ: No supported board or validated stream rate. "
              "Change a setting to retry hardware clock negotiation.");
        return false;
    }

    const int boardNum =
        deviceManager->getSelectedBoardNumber();

    LOGC ("MCDAQ DEBUG 2: Creating acquisition object");

    acquisition =
        std::make_unique<MCDAQAcquisition> (boardNum);

    acquisition->setRawBlockCallback (
        &DataThreadPlugin::receiveRawBlock,
        this);

    LOGC ("MCDAQ DEBUG 3: Calling MCC scan start");

    const int error = acquisition->start (selectedSampleRateHz, getSelectedChannels(), selectedRangeVolts);

    LOGC ("MCDAQ DEBUG 4: MCC returned ", error);

    if (error != 0)
    {
        LOGE ("MCDAQ: Failed to start scan. UL error: ",
              error);

        acquisition.reset();
        return false;
    }

    diagnosticTicks = 0;

    const auto stats = acquisition->getStatistics();

    // Metadata has already been registered with Open Ephys. Require
    // the driver-reported rate to match it exactly; do not relax this
    // safety check just because MCC rounds its hardware clock.
    const long declaredRate = getDeclaredSampleRateHz();
    if (stats.actualRate != declaredRate)
    {
        LOGE ("MCDAQ: UL selected ", stats.actualRate,
              " Hz; Open Ephys stream declares ", declaredRate,
              " Hz (nominal request: ", selectedSampleRateHz,
              " Hz). Acquisition cancelled to protect timestamps.");
        acquisition->stop();
        const auto stopped = acquisition->getStopDiagnostics();
        if (stopped.stopError == 0 && stopped.disableEventError == 0)
            acquisition.reset();
        else
            LOGE ("MCDAQ: Shutdown incomplete after clock mismatch.");
        return false;
    }

    activeSampleRateHz = static_cast<double> (stats.actualRate);

    if (selectedSampleRateHz != declaredRate)
        LOGC ("MCDAQ: Nominal selection ", selectedSampleRateHz,
              " Hz matches the preflight-negotiated and start-validated MCC rate ",
              declaredRate, " Hz on this board.");

    LOGC ("MCDAQ: Continuous scan started. Board: ",
          boardNum,
          " | Actual rate: ",
          stats.actualRate,
          " Hz/channel | Channels: ", stats.channelCount,
          " | Aggregate: ", stats.actualRate * stats.channelCount,
          " ADC points/s | Mode: ", getSamplingModeDescription().c_str(),
          " | UL buffer: ", stats.bufferPoints,
          " points | MCC event: ", stats.notificationPoints,
          " points");

    LOGC ("MCDAQ DEBUG 5: Starting DataThread");

    if (sourceBuffers.size() > 0)
        sourceBuffers[0]->clear();

    startThread();

    LOGC ("MCDAQ DEBUG 6: DataThread started");

    return true;
}


bool DataThreadPlugin::updateBuffer()
{
    if (! acquisition)
        return false;

    // Wait for an MCC notification instead of polling every 20 ms.
    // Timeout also allows the DataThread to recheck its exit condition.
    acquisition->waitForData (100.0);

    if (threadShouldExit())
        return true;

    if (! acquisition->consumeAvailable() || ! acquisition->checkWatchdog())
    {
        const auto stats =
            acquisition->getStatistics();

        LOGE ("MCDAQ: Acquisition integrity failure.");
        LOGE ("MCDAQ: Scan error: ", stats.scanError);
        LOGE ("MCDAQ: Read error: ", stats.readError);
        LOGE ("MCDAQ: Possible data loss: ",
              stats.possibleDataLoss);
        LOGE ("MCDAQ: Watchdog fault: ",
              static_cast<int> (stats.watchdogFault),
              " | Scan error event: ", stats.scanErrorEventSeen,
              " | Unexpected scan end: ", stats.unexpectedEndEventSeen);
        LOGE ("MCDAQ: Last watchdog driver status/count: ",
              stats.watchdogDriverStatus, " / ", stats.watchdogDriverCount,
              " | UL query error: ", stats.watchdogStatusQueryError,
              " | Driver-callback lag: ", stats.watchdogNotificationLag);

        return false;
    }

    #if MCDAQ_VERBOSE_ACQUISITION_LOGS
    // Approximately once per second.
    if (++diagnosticTicks >= 50)
    {
        diagnosticTicks = 0;

        const auto stats =
            acquisition->getStatistics();

        const auto driver =
            acquisition->getDriverStatus();

        LOGC ("MCDAQ: Produced: ",
              stats.totalSamplesProduced,
              " | Consumed: ",
              stats.totalSamplesConsumed,
              " | Delivered: ",
              stats.totalSamplesDelivered,
              " | Callback count: ",
              stats.lastSampleCount);

        if (driver.error == 0)
        {
            LOGC ("MCDAQ: UL status: ",
                  driver.status,
                  " | CurCount: ",
                  driver.currentCount,
                  " | CurIndex: ",
                  driver.currentIndex);
        }
        else
        {
            LOGE ("MCDAQ: cbGetStatus failed: ",
                  driver.error);
        }
    }
    #endif

    return true;
}

bool DataThreadPlugin::stopAcquisition()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();

        // Do not wait for the next MCC notification to exit promptly.
        if (acquisition)
            acquisition->wakeWorker();

        if (! waitForThreadToExit (2000))
        {
            LOGE ("MCDAQ: Acquisition worker did not exit.");
            return false;
        }
    }


    if (acquisition)
    {
        acquisition->stop();

        // Retrieve statistics AFTER the final drain.
        const auto stopped =
            acquisition->getStopDiagnostics();

        const auto stats =
            acquisition->getStatistics();

        LOGC ("MCDAQ: Stop error: ",
              stopped.stopError,
              " | Disable-event error: ",
              stopped.disableEventError);

        LOGC ("MCDAQ: Post-stop CurCount: ",
              stopped.currentCount,
              " | CurIndex: ",
              stopped.currentIndex);

        LOGC ("MCDAQ: Final Produced: ",
              stopped.finalProduced,
              " | Final Consumed: ",
              stopped.finalConsumed,
              " | Final Delivered: ",
              stats.totalSamplesDelivered);

        LOGC ("MCDAQ: Final drain attempted: ",
              stopped.drainAttempted,
              " | Successful: ",
              stopped.drainSucceeded);

        LOGC ("MCDAQ: Scan error: ",
              stats.scanError,
              " | Read error: ",
              stats.readError,
              " | Possible data loss: ",
              stats.possibleDataLoss);

        LOGC ("MCDAQ: Channels: ", stats.channelCount,
              " | Final frames delivered: ",
              stats.channelCount > 0 ? stats.totalSamplesDelivered / stats.channelCount : 0,
              " | Trailing partial ADC points: ", stats.trailingPartialPoints);

        if (stats.trailingPartialPoints > 0)
            LOGC ("MCDAQ WARNING: UL stopped with ", stats.trailingPartialPoints,
                  " point(s) in an incomplete terminal ", stats.channelCount,
                  "-channel scan. These points were NOT written as a frame; ",
                  "complete-frame delivery is checked separately.");

        LOGC ("MCDAQ: Maximum unread backlog: ",
              stats.maxUnreadSamples,
              " ADC sample points");

        LOGC ("MCDAQ: Watchdog checks: ",
              stats.watchdogChecks,
              " | Fault: ", static_cast<int> (stats.watchdogFault),
              " | Driver status/count: ",
              stats.watchdogDriverStatus, " / ",
              stats.watchdogDriverCount);

        // If UL could not stop or disconnect callbacks,
        // keep the acquisition object for error recovery.
        if (stopped.stopError != 0 || stopped.disableEventError != 0)
        {
            LOGE ("MCDAQ: Hardware shutdown incomplete.");
            return false;
        }

        // A valid trailing terminal fragment is NOT an incomplete delivered
        // frame. Reconcile every ADC point and verify the complete frames
        // separately. Do not require the hardware to stop on a frame edge.
        const bool completeFramesVerified =
            stopped.statusError == 0 && stopped.status == 0 &&
            stopped.drainSucceeded &&
            stats.channelCount > 0 &&
            stats.trailingPartialPoints < static_cast<uint64_t> (stats.channelCount) &&
            stopped.finalConsumed % static_cast<uint64_t> (stats.channelCount) == 0 &&
            stopped.finalProduced == stopped.finalConsumed + stats.trailingPartialPoints &&
            stopped.finalConsumed == stats.totalSamplesDelivered &&
            stats.scanError == 0 && stats.readError == 0 &&
            ! stats.possibleDataLoss &&
            stats.watchdogFault == MCDAQAcquisition::WatchdogFault::None;

        if (! completeFramesVerified)
        {
            LOGC ("MCDAQ: Acquisition integrity not verified.");
        }
        else if (stats.trailingPartialPoints > 0)
        {
            LOGC ("MCDAQ: All complete frames reconciled; terminal partial scan separately accounted for.");
        }

        acquisition.reset();

        return completeFramesVerified;
    }

    return true;
}

void DataThreadPlugin::resizeBuffers()
{
    sourceBuffers.clear();

    if (foundInputSource())
    {
        // A minimum of 10,000 frames, or four seconds at the chosen rate.
        // Separate from the MCC driver-owned circular buffer.
        const int frames = (std::max) (
            10000, static_cast<int> (getDeclaredSampleRateHz() * 4));
        sourceBuffers.add (new DataBuffer (selectedChannelCount, frames));
    }
}

std::unique_ptr<GenericEditor> DataThreadPlugin::createEditor (SourceNode* sn)
{
    std::unique_ptr<DataThreadPluginEditor> editor = std::make_unique<DataThreadPluginEditor> (sn, this);

    return editor;
}

void DataThreadPlugin::handleBroadcastMessage (const String& msg, const int64 messageTimestmpMilliseconds)
{
}

String DataThreadPlugin::handleConfigMessage (const String& msg)
{
    return "";
}

void DataThreadPlugin::registerParameters()
{
    // Register parameters for the plugin here (e.g. addParameter())
}

void DataThreadPlugin::parameterValueChanged (Parameter* parameter)
{
    // Handle parameter value changes here (e.g. update settings)
}


std::size_t DataThreadPlugin::receiveRawBlock (
    const uint16_t* samples,
    std::size_t count,
    uint64_t firstSample,
    void* userData)
{
    auto* plugin =
        static_cast<DataThreadPlugin*> (userData);

    if (plugin == nullptr)
        return 0;

    return plugin->forwardRawBlock (
        samples,
        count,
        firstSample);
}

std::size_t DataThreadPlugin::forwardRawBlock (
    const uint16_t* samples,
    std::size_t count,
    uint64_t firstSample)
{
    const size_t n = static_cast<size_t> (selectedChannelCount);
    if (samples == nullptr || n == 0 || count == 0 || count > BLOCK_SIZE ||
        count % n != 0 || firstSample % n != 0 || sourceBuffers.size() == 0)
        return 0;

    // UL layout follows the configured ascending channel queue.
    // Example mask AI0+AI6+AI7 -> [AI0,AI6,AI7,AI0,AI6,AI7,...].
    // Open Ephys v1.x expects channel-major blocks:
    // [all AI0 frames, all AI1 frames, ...].
    const size_t frames = count / n;
    const uint64_t firstFrame = firstSample / n;
    const bool is12Bit = deviceManager->getSelectedProductName() == "USB-204";
    const int resolution = is12Bit ? 4096 : 65536;
    const int midpoint = resolution / 2;
    const float voltsPerCount =
        (2.0f * (is12Bit ? 10.0f : static_cast<float> (selectedRangeVolts))) /
        static_cast<float> (resolution);
    const double sampleRate = activeSampleRateHz;

    for (size_t frame = 0; frame < frames; ++frame)
    {
        const int64_t sampleNumber = static_cast<int64_t> (firstFrame + frame);
        sampleNumbers[frame] = sampleNumber;
        timestamps[frame] = static_cast<double> (sampleNumber) / sampleRate;
        eventCodes[frame] = 0; // Digital inputs not yet implemented.

        for (size_t channel = 0; channel < n; ++channel)
        {
            const int centeredCode =
                static_cast<int> (samples[frame * n + channel]) - midpoint;
            voltageBlock[channel * frames + frame] =
                static_cast<float> (centeredCode) * voltsPerCount;
        }
    }

    const auto accepted = sourceBuffers[0]->addToBuffer (
        voltageBlock.data(), sampleNumbers.data(), timestamps.data(),
        eventCodes.data(), static_cast<int> (frames));

    if (accepted < 0 || static_cast<size_t> (accepted) != frames)
        return 0; // The acquisition engine latches this as a delivery failure.

    return count; // Report accepted ADC points, not frames.
}
