#include "DataThreadPluginEditor.h"

DataThreadPluginEditor::DataThreadPluginEditor (GenericProcessor* parentNode, DataThreadPlugin* plugin)
    : GenericEditor (parentNode), thread (plugin)
{
    // Two columns of analog buttons, as in the NI-DAQ editor. All eight
    // inputs are single-ended; there is deliberately no DIFF control.
    desiredWidth = 405;

    for (int ch = 0; ch < 8; ++ch)
    {
        auto button = std::make_unique<juce::TextButton> ("AI" + juce::String (ch));
        button->setButtonText ("AI" + juce::String (ch));
        button->setBounds (30 + (ch / 4) * 84, 31 + (ch % 4) * 26, 68, 20);
        button->setClickingTogglesState (true);
        button->setColour (juce::TextButton::buttonOnColourId, juce::Colours::forestgreen);
        button->setColour (juce::TextButton::buttonColourId, juce::Colours::darkgrey);
        button->setTooltip ("Toggle analog input AI" + juce::String (ch));
        button->addListener (this);
        addAndMakeVisible (*button);
        analogButtons[static_cast<std::size_t> (ch)] = std::move (button);
    }

    boardLabel = std::make_unique<juce::Label>();
    boardLabel->setBounds (211, 15, 190, 16);
    boardLabel->setFont (juce::Font (11.0f));
    addAndMakeVisible (*boardLabel);

    sampleRateLabel = std::make_unique<juce::Label>();
    sampleRateLabel->setText ("Sample rate / channel", juce::dontSendNotification);
    sampleRateLabel->setBounds (211, 31, 180, 16);
    sampleRateLabel->setFont (juce::Font (11.0f));
    addAndMakeVisible (*sampleRateLabel);

    sampleRateSelector = std::make_unique<juce::ComboBox> ("MCDAQ sample rate");
    sampleRateSelector->setBounds (215, 49, 174, 21);
    sampleRateSelector->addListener (this);
    addAndMakeVisible (*sampleRateSelector);

    voltageRangeLabel = std::make_unique<juce::Label>();
    voltageRangeLabel->setText ("Analog input voltage range", juce::dontSendNotification);
    voltageRangeLabel->setBounds (211, 73, 180, 16);
    voltageRangeLabel->setFont (juce::Font (11.0f));
    addAndMakeVisible (*voltageRangeLabel);

    voltageRangeSelector = std::make_unique<juce::ComboBox> ("MCDAQ voltage range");
    voltageRangeSelector->setBounds (215, 91, 174, 21);
    for (const int range : { 10, 5, 2, 1 })
        voltageRangeSelector->addItem (
            "-" + juce::String (range) + " to +" + juce::String (range) + " V", range);
    voltageRangeSelector->addListener (this);
    addAndMakeVisible (*voltageRangeSelector);

    architectureLabel = std::make_unique<juce::Label>();
    architectureLabel->setBounds (209, 116, 190, 17);
    architectureLabel->setFont (juce::Font (10.0f));
    addAndMakeVisible (*architectureLabel);

    refreshControls();
}

DataThreadPluginEditor::~DataThreadPluginEditor()
{
    sampleRateSelector->removeListener (this);
    voltageRangeSelector->removeListener (this);
    for (auto& button : analogButtons)
        button->removeListener (this);
}

void DataThreadPluginEditor::refreshControls()
{
    for (int ch = 0; ch < 8; ++ch)
        analogButtons[static_cast<std::size_t> (ch)]->setToggleState (
            thread->isChannelEnabled (ch), juce::dontSendNotification);

    sampleRateSelector->clear (juce::dontSendNotification);
    for (const long rate : thread->getAvailableRates())
    {
        juce::String title = juce::String (rate) + " Hz";
        if (rate == 30000 && thread->getDeclaredSampleRateHz() == 30007)
            title = "30000 Hz (actual 30007)";
        sampleRateSelector->addItem (title, static_cast<int> (rate));
    }
    sampleRateSelector->setSelectedId (
        static_cast<int> (thread->getSampleRateHz()), juce::dontSendNotification);

    voltageRangeSelector->setSelectedId (
        thread->getVoltageRangeVolts(), juce::dontSendNotification);
    voltageRangeSelector->setEnabled (thread->canSelectVoltageRange());
    voltageRangeSelector->setTooltip (thread->canSelectVoltageRange()
        ? "Select the common range for all enabled USB-1608FS-Plus analog inputs"
        : "USB-204 analog input range is fixed at -10 to +10 V");

    boardLabel->setText (juce::String (thread->getBoardName().c_str()),
                         juce::dontSendNotification);
    architectureLabel->setText (
        juce::String (thread->isSimultaneousSampling()
            ? "Simultaneous ADCs" : "Multiplexed (sequential)") +
        " | " + juce::String (thread->getChannelCount()) + " AI",
        juce::dontSendNotification);
}

void DataThreadPluginEditor::comboBoxChanged (juce::ComboBox* box)
{
    bool accepted = false;
    if (box == sampleRateSelector.get())
        accepted = thread->setSampleRateHz (sampleRateSelector->getSelectedId());
    else if (box == voltageRangeSelector.get())
        accepted = thread->setVoltageRangeVolts (voltageRangeSelector->getSelectedId());
    else
        return;

    refreshControls();
    if (accepted)
        CoreServices::updateSignalChain (this);
}

void DataThreadPluginEditor::buttonClicked (juce::Button* button)
{
    for (int ch = 0; ch < 8; ++ch)
    {
        if (button != analogButtons[static_cast<std::size_t> (ch)].get())
            continue;
        const auto nextMask = static_cast<std::uint8_t> (
            thread->getChannelMask() ^ static_cast<std::uint8_t> (1u << ch));
        const bool accepted = thread->setChannelMask (nextMask);
        refreshControls(); // Reject attempts to disable the last channel.
        if (accepted)
            CoreServices::updateSignalChain (this);
        return;
    }
}

void DataThreadPluginEditor::saveCustomParametersToXml (XmlElement* xml)
{
    if (! xml) return;
    xml->setAttribute ("sample_rate_hz", static_cast<int> (thread->getSampleRateHz()));
    xml->setAttribute ("ai_channel_mask", static_cast<int> (thread->getChannelMask()));
    xml->setAttribute ("input_range_volts", thread->getVoltageRangeVolts());
}

void DataThreadPluginEditor::loadCustomParametersFromXml (XmlElement* xml)
{
    if (! xml) return;
    // Backward compatibility with older configs containing channel_count.
    int channelMask = xml->getIntAttribute ("ai_channel_mask", -1);
    if (channelMask < 0)
    {
        const int previousCount = xml->getIntAttribute ("channel_count", 1);
        channelMask = (previousCount >= 1 && previousCount <= 8)
            ? ((1 << previousCount) - 1) : 1;
    }
    thread->setChannelMask (static_cast<std::uint8_t> (channelMask));
    thread->setSampleRateHz (xml->getIntAttribute ("sample_rate_hz", 1000));
    thread->setVoltageRangeVolts (xml->getIntAttribute ("input_range_volts", 10));
    refreshControls();
}
