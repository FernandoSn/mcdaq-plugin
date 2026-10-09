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
    return true;
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
    return true;
}

bool DataThreadPlugin::updateBuffer()
{
    return true;
}

bool DataThreadPlugin::stopAcquisition()
{
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