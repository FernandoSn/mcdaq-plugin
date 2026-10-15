/* MCDAQ Open Ephys editor: NI-style single-ended analog channel selection. */
#ifndef DATATHREADPLUGINEDITOR_H_DEFINED
#define DATATHREADPLUGINEDITOR_H_DEFINED

#include <EditorHeaders.h>
#include "DataThreadPlugin.h"
#include <array>

class DataThreadPluginEditor : public GenericEditor,
                               private juce::ComboBox::Listener,
                               private juce::Button::Listener
{
public:
    DataThreadPluginEditor (GenericProcessor* parentNode, DataThreadPlugin* thread);
    ~DataThreadPluginEditor() override;
    void saveCustomParametersToXml (XmlElement* xml) override;
    void loadCustomParametersFromXml (XmlElement* xml) override;

private:
    void comboBoxChanged (juce::ComboBox* box) override;
    void buttonClicked (juce::Button* button) override;
    void refreshControls();

    DataThreadPlugin* thread = nullptr;
    std::array<std::unique_ptr<juce::TextButton>, 8> analogButtons;
    std::array<std::unique_ptr<juce::TextButton>, 8> digitalButtons;
    std::unique_ptr<juce::ComboBox> sampleRateSelector;
    std::unique_ptr<juce::TextButton> diTimeButton;
    std::unique_ptr<juce::Label> diTimeLabel;
    std::unique_ptr<juce::Label> exactRateLabel;
    std::unique_ptr<juce::ComboBox> voltageRangeSelector;
    std::unique_ptr<juce::Label> sampleRateLabel;
    std::unique_ptr<juce::Label> voltageRangeLabel;
    std::unique_ptr<juce::Label> boardLabel;
    std::unique_ptr<juce::Label> architectureLabel;
};

#endif
