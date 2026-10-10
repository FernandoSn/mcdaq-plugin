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

#include "DataThreadPluginEditor.h"

DataThreadPluginEditor::DataThreadPluginEditor (GenericProcessor* parentNode, DataThreadPlugin* plugin)
    : GenericEditor (parentNode), thread (plugin)
{
    desiredWidth = 350;

    sampleRateLabel = std::make_unique<juce::Label>();
    sampleRateLabel->setText ("Hz per channel", juce::dontSendNotification);
    sampleRateLabel->setBounds (12, 25, 154, 19);
    addAndMakeVisible (*sampleRateLabel);

    channelLabel = std::make_unique<juce::Label>();
    channelLabel->setText ("AI channels", juce::dontSendNotification);
    channelLabel->setBounds (208, 25, 114, 19);
    addAndMakeVisible (*channelLabel);

    sampleRateSelector = std::make_unique<juce::ComboBox> ("MCDAQ sample rate");
    sampleRateSelector->setBounds (12, 46, 185, 22);
    sampleRateSelector->addListener (this);
    addAndMakeVisible (*sampleRateSelector);

    channelSelector = std::make_unique<juce::ComboBox> ("MCDAQ channel count");
    channelSelector->setBounds (208, 46, 113, 22);
    channelSelector->addListener (this);
    addAndMakeVisible (*channelSelector);

    samplingModeLabel = std::make_unique<juce::Label>();
    samplingModeLabel->setBounds (12, 73, 321, 17);
    addAndMakeVisible (*samplingModeLabel);
    refreshSelectors();
}

DataThreadPluginEditor::~DataThreadPluginEditor()
{
    sampleRateSelector->removeListener (this);
    channelSelector->removeListener (this);
}

void DataThreadPluginEditor::refreshSelectors()
{
    sampleRateSelector->clear (juce::dontSendNotification);
    for (long rate : thread->getAvailableRates())
    {
        juce::String name = juce::String (rate);
        // Explicitly disclose the empirically measured clock correction.
        if (rate == 30000 && thread->isSimultaneousSampling())
            name = "30000 (~30007 actual)";
        sampleRateSelector->addItem (name, static_cast<int> (rate));
    }
    sampleRateSelector->setSelectedId (
        static_cast<int> (thread->getSampleRateHz()), juce::dontSendNotification);

    channelSelector->clear (juce::dontSendNotification);
    for (int n : thread->getAvailableChannelCounts())
        channelSelector->addItem (n == 1 ? "1 (AI0)" :
            juce::String (n) + " (AI0-AI" + juce::String (n-1) + ")", n);
    channelSelector->setSelectedId (thread->getChannelCount(), juce::dontSendNotification);

    samplingModeLabel->setText (
        juce::String (thread->getSamplingModeDescription().c_str()) +
        " | " + juce::String (thread->getChannelCount()) + " ch",
        juce::dontSendNotification);
}

void DataThreadPluginEditor::comboBoxChanged (juce::ComboBox* box)
{
    bool changed = false;
    if (box == sampleRateSelector.get())
        changed = thread->setSampleRateHz (sampleRateSelector->getSelectedId());
    else if (box == channelSelector.get())
        changed = thread->setChannelCount (channelSelector->getSelectedId());
    else
        return;

    refreshSelectors();
    if (changed)
        CoreServices::updateSignalChain (this);
}

void DataThreadPluginEditor::saveCustomParametersToXml (XmlElement* xml)
{
    if (xml != nullptr)
    {
        xml->setAttribute ("sample_rate_hz", static_cast<int> (thread->getSampleRateHz()));
        xml->setAttribute ("channel_count", thread->getChannelCount());
    }
}

void DataThreadPluginEditor::loadCustomParametersFromXml (XmlElement* xml)
{
    if (xml == nullptr) return;
    const int rate = xml->getIntAttribute ("sample_rate_hz", 1000);
    const int channels = xml->getIntAttribute ("channel_count", 1);
    // A saved configuration may exceed the capabilities of a different
    // board. Keep the last valid setting rather than selecting an invalid pair.
    thread->setSampleRateHz (rate);
    thread->setChannelCount (channels);
    refreshSelectors();
}
