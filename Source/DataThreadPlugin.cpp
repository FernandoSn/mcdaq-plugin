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

void DataThreadPlugin::updateSettings (OwnedArray<ContinuousChannel>* continuousChannels,
                                       OwnedArray<EventChannel>* eventChannels,
                                       OwnedArray<SpikeChannel>* spikeChannels,
                                       OwnedArray<DataStream>* sourceStreams,
                                       OwnedArray<DeviceInfo>* devices,
                                       OwnedArray<ConfigurationObject>* configurationObjects)
{
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

    // Approximately once per second.
    if (++diagnosticTicks >= 50)
    {
        diagnosticTicks = 0;

        const auto stats =
            acquisition->getStatistics();

        LOGC ("MCDAQ: Produced: ",
              stats.totalSamplesProduced,
              " | Consumed: ",
              stats.totalSamplesConsumed,
              " | Raw min/max: ",
              stats.minimumRaw,
              "/",
              stats.maximumRaw);
    }

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
        const auto stats = acquisition->getStatistics();

        acquisition->stop();

        LOGC ("MCDAQ: Scan stopped.");
        LOGC ("MCDAQ: Total notifications: ",
              stats.notifications);
        LOGC ("MCDAQ: Final reported sample count: ",
              stats.lastSampleCount);
        LOGC ("MCDAQ: Scan error: ",
              stats.scanError);

        acquisition.reset();
    }

    return true;
}

void DataThreadPlugin::resizeBuffers()
{
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