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
    return deviceManager && deviceManager->getSelectedBoardNumber() >= 0;
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

    if (! foundInputSource())
        return;

    const std::string product =
        deviceManager->getSelectedProductName();

    const std::string uniqueId =
        deviceManager->getSelectedUniqueId();

    const String streamName =
        "MCDAQ " + String (product.c_str());

    const String streamIdentifier =
        "mcdaq-" + String (uniqueId.c_str());

    // Temporary fixed acquisition settings:
    // AI0, +/-10 V, 1000 samples/sec.
    constexpr float sampleRate = 1000.0f;

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
            : 20.0f / 65536.0f;

    ContinuousChannel::Settings channelSettings {
        ContinuousChannel::Type::ADC,
        "AI0",
        "MCC analog input channel 0",
        "mcdaq-ai0",
        bitVolts,
        stream
    };

    continuousChannels->add (
        new ContinuousChannel (channelSettings));

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

    LOGC ("MCDAQ: Registered AI0 at ",
          sampleRate,
          " Hz | Nominal bitVolts: ",
          bitVolts);
}

bool DataThreadPlugin::startAcquisition()
{
    LOGC ("MCDAQ DEBUG 1: Entered startAcquisition()");

    if (! foundInputSource())
    {
        LOGE ("MCDAQ: No board selected.");
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

    const int error = acquisition->start (1000);

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

    LOGC ("MCDAQ: Continuous scan started. Board: ",
          boardNum,
          " | Actual rate: ",
          stats.actualRate,
          " S/s");

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

    // Temporary diagnostic interval.
    Thread::sleep (20);

    if (! acquisition->consumeAvailable())
    {
        const auto stats =
            acquisition->getStatistics();

        LOGE ("MCDAQ: Acquisition integrity failure.");
        LOGE ("MCDAQ: Scan error: ", stats.scanError);
        LOGE ("MCDAQ: Read error: ", stats.readError);
        LOGE ("MCDAQ: Possible data loss: ",
              stats.possibleDataLoss);

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

        LOGC ("MCDAQ: Maximum unread backlog: ",
              stats.maxUnreadSamples,
              " ADC sample points");

        // If UL could not stop or disconnect callbacks,
        // keep the acquisition object for error recovery.
        if (stopped.stopError != 0 || stopped.disableEventError != 0)
        {
            LOGE ("MCDAQ: Hardware shutdown incomplete.");
            return false;
        }

        const bool clean =
            stopped.statusError == 0 && stopped.drainSucceeded && stopped.finalProduced == stopped.finalConsumed && stopped.finalConsumed == stats.totalSamplesDelivered && stats.scanError == 0 && stats.readError == 0 && ! stats.possibleDataLoss;

        if (! clean)
        {
            LOGE ("MCDAQ: Acquisition integrity not verified.");
        }

        acquisition.reset();

        return clean;
    }

    return true;
}

void DataThreadPlugin::resizeBuffers()
{
    sourceBuffers.clear();

    if (foundInputSource())
    {
        // One channel, 10,000 sampling frames.
        sourceBuffers.add (new DataBuffer (1, 10000));
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
    if (samples == nullptr || count == 0 || count > BLOCK_SIZE || sourceBuffers.size() == 0)
    {
        return 0;
    }

    // Temporary fixed configuration: +/-10 V.
    const std::string product =
        deviceManager->getSelectedProductName();

    const bool is12Bit = product == "USB-204";

    const int resolution = is12Bit ? 4096 : 65536;
    const int midpoint = resolution / 2;

    const float voltsPerCount =
        20.0f / static_cast<float> (resolution);

    constexpr double sampleRate = 1000.0;

    for (std::size_t i = 0; i < count; ++i)
    {
        const int centeredCode =
            static_cast<int> (samples[i]) - midpoint;

        voltageBlock[i] =
            centeredCode * voltsPerCount;

        const int64_t sampleNumber =
            static_cast<int64_t> (firstSample + i);

        sampleNumbers[i] = sampleNumber;

        // Preliminary timebase: seconds since scan start.
        timestamps[i] =
            static_cast<double> (sampleNumber) / sampleRate;

        // Digital inputs are not implemented yet.
        eventCodes[i] = 0;
    }

    const auto accepted =
        sourceBuffers[0]->addToBuffer (
            voltageBlock.data(),
            sampleNumbers.data(),
            timestamps.data(),
            eventCodes.data(),
            static_cast<int> (count));

    return static_cast<std::size_t> (accepted);
}