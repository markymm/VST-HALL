/*
  ==============================================================================
    AetherSpaceReverbVST3.cpp
    Kompletna wtyczka VST3 (Single-File JUCE Architecture) dla Steinberg Cubase.
    Emuluje: Concert Hall, Vintage Plate, Studio Room, Lush Chamber, Cathedral.
  ==============================================================================
*/

#include <JuceHeader.h>
#include <array>
#include <vector>
#include <cmath>

//==============================================================================
// 1. PROCESOR DSP WTYCZKI (AetherSpaceAudioProcessor)
//==============================================================================
class AetherSpaceAudioProcessor : public juce::AudioProcessor
{
public:
    AetherSpaceAudioProcessor()
        : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "Parameters", createParameterLayout())
    {
    }

    ~AetherSpaceAudioProcessor() override = default;

    // Definicja parametrów widocznych dla automatyki w Cubase (VST3 Parameter IDs)
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        params.push_back (std::make_unique<juce::AudioParameterChoice>(
            "ALGO", "Reverb Type",
            juce::StringArray { "Concert Hall", "Vintage Plate", "Studio Room", "Lush Chamber", "Cathedral" }, 0));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            "PREDELAY", "Pre-Delay (ms)",
            juce::NormalisableRange<float> (0.0f, 250.0f, 1.0f), 24.0f));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            "DECAY", "Decay Time (s)",
            juce::NormalisableRange<float> (0.2f, 12.0f, 0.05f, 0.5f), 2.8f));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            "SIZE", "Room Size (%)",
            juce::NormalisableRange<float> (10.0f, 100.0f, 1.0f), 78.0f));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            "DAMPING", "High Damping (%)",
            juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 35.0f));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            "LOWCUT", "Low Cut Frequency (Hz)",
            juce::NormalisableRange<float> (20.0f, 600.0f, 1.0f, 0.4f), 90.0f));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            "WIDTH", "Stereo Width (%)",
            juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            "MIX", "Dry/Wet Mix (%)",
            juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 35.0f));

        params.push_back (std::make_unique<juce::AudioParameterBool>(
            "FREEZE", "Freeze Tail", false));

        return { params.begin(), params.end() };
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        currentSampleRate = sampleRate;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
        spec.numChannels = 2;

        preDelayLine.reset();
        preDelayLine.setMaximumDelayInSamples (static_cast<int> (sampleRate * 0.5));
        preDelayLine.prepare (spec);

        lowCutFilter.prepare (spec);
        lowCutFilter.setType (juce::dsp::StateVariableTPTFilterType::highpass);

        highCutFilter.prepare (spec);
        highCutFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);

        reverbEngine.reset();
        reverbEngine.setSampleRate (sampleRate);

        wetBuffer.setSize (2, samplesPerBlock);
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
            return false;
        if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo()
            && layouts.getMainInputChannelSet() != juce::AudioChannelSet::mono())
            return false;
        return true;
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();

        // Pobranie aktualnych wartości z drzewa parametrów VST3
        const int algoIndex   = static_cast<int> (*apvts.getRawParameterValue ("ALGO"));
        const float preDelay  = *apvts.getRawParameterValue ("PREDELAY");
        const float decay     = *apvts.getRawParameterValue ("DECAY");
        const float sizePct   = *apvts.getRawParameterValue ("SIZE") * 0.01f;
        const float dampPct   = *apvts.getRawParameterValue ("DAMPING") * 0.01f;
        const float lowCutHz  = *apvts.getRawParameterValue ("LOWCUT");
        const float widthPct  = *apvts.getRawParameterValue ("WIDTH") * 0.01f;
        const float mixPct    = *apvts.getRawParameterValue ("MIX") * 0.01f;
        const bool freeze     = *apvts.getRawParameterValue ("FREEZE") > 0.5f;

        // Charakterystyka wybranego algorytmu (Hall, Plate, Room, Chamber, Cathedral)
        float algoSizeBias = 1.0f;
        float algoDampBias = 1.0f;
        float highCutHz    = 8500.0f;

        switch (algoIndex)
        {
            case 0: // Concert Hall
                algoSizeBias = 1.05f; algoDampBias = 0.85f; highCutHz = 7200.0f; break;
            case 1: // Vintage Plate
                algoSizeBias = 0.88f; algoDampBias = 0.45f; highCutHz = 12500.0f; break;
            case 2: // Studio Room
                algoSizeBias = 0.62f; algoDampBias = 1.15f; highCutHz = 5500.0f; break;
            case 3: // Lush Chamber
                algoSizeBias = 0.92f; algoDampBias = 0.70f; highCutHz = 8800.0f; break;
            case 4: // Cathedral
                algoSizeBias = 1.18f; algoDampBias = 0.95f; highCutHz = 4800.0f; break;
            default: break;
        }

        // Konfiguracja silnika pogłosowego
        juce::Reverb::Parameters revParams;
        const float normDecay = juce::jlimit (0.0f, 1.0f, (decay / 12.0f) * 0.55f + sizePct * 0.45f);
        revParams.roomSize   = juce::jlimit (0.05f, 0.99f, normDecay * algoSizeBias);
        revParams.damping    = juce::jlimit (0.02f, 0.98f, dampPct * algoDampBias);
        revParams.width      = widthPct;
        revParams.freezeMode = freeze ? 1.0f : 0.0f;
        revParams.wetLevel   = 1.0f;
        revParams.dryLevel   = 0.0f;
        reverbEngine.setParameters (revParams);

        // Konfiguracja Pre-Delay oraz filtrów zbiornika pogłosowego
        const float delaySamples = (preDelay * 0.001f) * static_cast<float> (currentSampleRate);
        preDelayLine.setDelay (delaySamples);
        lowCutFilter.setCutoffFrequency (lowCutHz);
        highCutFilter.setCutoffFrequency (highCutHz * (1.0f - dampPct * 0.5f));

        // Kopiowanie sygnału do bufora Wet
        if (wetBuffer.getNumSamples() < numSamples)
            wetBuffer.setSize (2, numSamples, false, false, true);

        wetBuffer.copyFrom (0, 0, buffer, 0, 0, numSamples);
        if (numChannels > 1)
            wetBuffer.copyFrom (1, 0, buffer, 1, 0, numSamples);
        else
            wetBuffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

        // Przetwarzanie ścieżki Wet: Pre-Delay -> LowCut -> HighCut -> Reverb Tank
        juce::dsp::AudioBlock<float> wetBlock (wetBuffer);
        juce::dsp::ProcessContextReplacing<float> wetContext (wetBlock);
        preDelayLine.process (wetContext);
        lowCutFilter.process (wetContext);
        highCutFilter.process (wetContext);

        reverbEngine.processStereo (wetBuffer.getWritePointer (0),
                                    wetBuffer.getWritePointer (1),
                                    numSamples);

        // Stało-mocowe miksowanie Dry / Wet
        const float dryGain = std::cos (mixPct * juce::MathConstants<float>::halfPi);
        const float wetGain = std::sin (mixPct * juce::MathConstants<float>::halfPi);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* out = buffer.getWritePointer (ch);
            const float* wet = wetBuffer.getReadPointer ( juce::jmin (ch, 1) );
            for (int s = 0; s < numSamples; ++s)
                out[s] = out[s] * dryGain + wet[s] * wetGain;
        }

        // Pomiar poziomu wyjściowego dla wizualizacji GUI
        currentOutputRMS.store (buffer.getRMSLevel (0, 0, numSamples));
    }

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AetherSpace Reverb"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 12.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default Hall"; }
    void changeProgramName (int, const juce::String&) override {}

    // Zapis i odczyt stanu sesji projektu Cubase (.cpr)
    void getStateInformation (juce::MemoryBlock& destData) override
    {
        auto state = apvts.copyState();
        std::unique_ptr<juce::XmlElement> xml (state.createXml());
        copyXmlToBinary (*xml, destData);
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
        if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
    }

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> currentOutputRMS { 0.0f };

private:
    double currentSampleRate = 48000.0;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> preDelayLine { 48000 };
    juce::dsp::StateVariableTPTFilter<float> lowCutFilter;
    juce::dsp::StateVariableTPTFilter<float> highCutFilter;
    juce::Reverb reverbEngine;
    juce::AudioBuffer<float> wetBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AetherSpaceAudioProcessor)
};

//==============================================================================
// 2. INTERFEJS GRAFICZNY WTYCZKI Z WIZUALIZACJĄ (AetherSpaceEditor)
//==============================================================================
class AetherSpaceEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AetherSpaceEditor (AetherSpaceAudioProcessor& p)
        : AudioProcessorEditor (&p), processorRef (p)
    {
        setSize (820, 500);

        // Wybór algorytmu pogłosu
        algoSelector.addItemList ({ "Concert Hall", "Vintage Plate", "Studio Room", "Lush Chamber", "Cathedral" }, 1);
        addAndMakeVisible (algoSelector);
        algoAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            processorRef.apvts, "ALGO", algoSelector);

        // Przycisk Freeze
        freezeButton.setButtonText ("FREEZE TAIL");
        freezeButton.setClickingTogglesState (true);
        addAndMakeVisible (freezeButton);
        freezeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processorRef.apvts, "FREEZE", freezeButton);

        // Inicjalizacja 6 głównych pokręteł
        setupKnob (preDelayKnob, preDelayLabel, "PREDELAY", "Pre-Delay", preDelayAttach);
        setupKnob (decayKnob,    decayLabel,    "DECAY",    "Decay RT60", decayAttach);
        setupKnob (sizeKnob,     sizeLabel,     "SIZE",     "Room Size",  sizeAttach);
        setupKnob (dampKnob,     dampLabel,     "DAMPING",  "Damping",    dampAttach);
        setupKnob (widthKnob,    widthLabel,    "WIDTH",    "Stereo Width", widthAttach);
        setupKnob (mixKnob,      mixLabel,      "MIX",      "Dry / Wet",  mixAttach);

        startTimerHz (30); // Odświeżanie wizualizatora 30 FPS
    }

    ~AetherSpaceEditor() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        // Tło główne wtyczki
        g.fillAll (juce::Colour (0xff111318));

        // Nagłówek
        g.setColour (juce::Colour (0xff1a1e29));
        g.fillRect (0, 0, getWidth(), 54);
        g.setColour (juce::Colours::cyan);
        g.setFont (juce::Font (19.0f, juce::Font::bold));
        g.drawText ("AETHERSPACE VST3 // ALGORITHMIC REVERB", 20, 12, 420, 30, juce::Justification::centredLeft);

        // Ramka wizualizatora obwiedni pogłosu
        juce::Rectangle<float> visArea (24.0f, 72.0f, static_cast<float>(getWidth() - 48), 210.0f);
        g.setColour (juce::Colour (0xff0b0d12));
        g.fillRoundedRectangle (visArea, 10.0f);
        g.setColour (juce::Colour (0xff262c3a));
        g.drawRoundedRectangle (visArea, 10.0f, 1.5f);

        // Rysowanie wizualizacji obwiedni RT60 + PreDelay
        const float preDelayNorm = *processorRef.apvts.getRawParameterValue ("PREDELAY") / 250.0f;
        const float decayNorm    = *processorRef.apvts.getRawParameterValue ("DECAY") / 12.0f;
        const float sizeNorm     = *processorRef.apvts.getRawParameterValue ("SIZE") / 100.0f;
        const float rms          = processorRef.currentOutputRMS.load();

        const float startX = visArea.getX() + 30.0f + preDelayNorm * 140.0f;
        const float tailW  = juce::jmax (60.0f, (visArea.getRight() - startX - 20.0f) * (0.25f + decayNorm * 0.75f));
        const float baseY  = visArea.getBottom() - 20.0f;

        juce::Path tailPath;
        tailPath.startNewSubPath (startX, baseY);
        for (int i = 0; i <= 60; ++i)
        {
            float t = static_cast<float>(i) / 60.0f;
            float env = std::exp (-t * 3.8f) * (0.5f + sizeNorm * 0.5f);
            float dynBoost = 1.0f + juce::jmin (0.4f, rms * 1.8f);
            float py = baseY - (env * 145.0f * dynBoost);
            tailPath.lineTo (startX + t * tailW, py);
        }
        tailPath.lineTo (startX + tailW, baseY);
        tailPath.closeSubPath();

        juce::ColourGradient grad (juce::Colours::cyan.withAlpha (0.55f), startX, visArea.getY(),
                                   juce::Colours::purple.withAlpha (0.05f), startX + tailW, baseY, false);
        g.setGradientFill (grad);
        g.fillPath (tailPath);

        g.setColour (juce::Colours::cyan);
        g.strokePath (tailPath, juce::PathStrokeType (2.0f));
    }

    void resized() override
    {
        algoSelector.setBounds (getWidth() - 320, 12, 180, 30);
        freezeButton.setBounds (getWidth() - 130, 12, 110, 30);

        const int knobWidth = (getWidth() - 48) / 6;
        const int knobY = 315;
        juce::Slider* knobs[] = { &preDelayKnob, &decayKnob, &sizeKnob, &dampKnob, &widthKnob, &mixKnob };
        juce::Label* labels[] = { &preDelayLabel, &decayLabel, &sizeLabel, &dampLabel, &widthLabel, &mixLabel };

        for (int i = 0; i < 6; ++i)
        {
            int x = 24 + i * knobWidth;
            labels[i]->setBounds (x, knobY - 22, knobWidth, 20);
            knobs[i]->setBounds (x, knobY, knobWidth, 145);
        }
    }

private:
    void timerCallback() override { repaint(); }

    void setupKnob (juce::Slider& slider, juce::Label& label, const juce::String& paramID,
                    const juce::String& title,
                    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attachment)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 20);
        addAndMakeVisible (slider);

        label.setText (title, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible (label);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processorRef.apvts, paramID, slider);
    }

    AetherSpaceAudioProcessor& processorRef;

    juce::ComboBox algoSelector;
    juce::TextButton freezeButton;
    juce::Slider preDelayKnob, decayKnob, sizeKnob, dampKnob, widthKnob, mixKnob;
    juce::Label  preDelayLabel, decayLabel, sizeLabel, dampLabel, widthLabel, mixLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> algoAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   freezeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   preDelayAttach, decayAttach,
                                                                            sizeAttach, dampAttach,
                                                                            widthAttach, mixAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AetherSpaceEditor)
};

juce::AudioProcessorEditor* AetherSpaceAudioProcessor::createEditor()
{
    return new AetherSpaceEditor (*this);
}

// Punkt wejścia wymagany przez standard Steinberg VST3 SDK / JUCE
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AetherSpaceAudioProcessor();
}