#include "PluginProcessor.h"

NubeProcessor::NubeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout NubeProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto addFloat = [&layout] (const char* id, const char* name, float lo, float hi, float step, float def)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 },
                                                                 name,
                                                                 juce::NormalisableRange<float> (lo, hi, step),
                                                                 def));
    };

    addFloat ("mix",      "Mix (%)",                    0.0f,   100.0f,   1.0f,   50.0f);
    addFloat ("grain",    "Grano (ms)",                 10.0f,  500.0f,   1.0f,   150.0f);
    addFloat ("pitch",    "Pitch (semitonos)",          -24.0f, 24.0f,    0.01f,  0.0f);
    addFloat ("spread",   "Dispersion de pitch (%)",    0.0f,   100.0f,   1.0f,   50.0f);
    addFloat ("density",  "Densidad (%)",               0.0f,   100.0f,   1.0f,   50.0f);
    addFloat ("scatter",  "Scatter de tiempo (%)",      0.0f,   100.0f,   1.0f,   40.0f);
    addFloat ("feedback", "Feedback (%)",               0.0f,   92.0f,    1.0f,   35.0f);
    addFloat ("width",    "Ancho estereo (%)",          0.0f,   100.0f,   1.0f,   70.0f);
    addFloat ("lowpass",  "Filtro pasa-bajos (Hz)",     200.0f, 20000.0f, 1.0f,   12000.0f);
    addFloat ("highpass", "Filtro pasa-altos (Hz)",     20.0f,  2000.0f,  1.0f,   60.0f);
    addFloat ("reverb",   "Reverb (%)",                 0.0f,   100.0f,   1.0f,   30.0f);
    addFloat ("decay",    "Reverb decay (%)",           0.0f,   100.0f,   1.0f,   70.0f);

    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "mode", 1 },
                                                              "Modo de pitch aleatorio",
                                                              juce::StringArray { "Continuo", "Octavas y quintas", "Solo octavas" },
                                                              1));

    addFloat ("gain",     "Ganancia de salida (dB)",    -18.0f, 12.0f,    0.1f,   0.0f);

    return layout;
}

NubeEngine::Params NubeProcessor::readParams() const
{
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    NubeEngine::Params p;
    p.mixPct         = get ("mix");
    p.grainMs        = get ("grain");
    p.pitchSemi      = get ("pitch");
    p.spreadPct      = get ("spread");
    p.densityPct     = get ("density");
    p.scatterPct     = get ("scatter");
    p.feedbackPct    = get ("feedback");
    p.widthPct       = get ("width");
    p.lowpassHz      = get ("lowpass");
    p.highpassHz     = get ("highpass");
    p.reverbPct      = get ("reverb");
    p.reverbDecayPct = get ("decay");
    p.mode           = (int) (get ("mode") + 0.5f);
    p.outGainDb      = get ("gain");
    return p;
}

void NubeProcessor::prepareToPlay (double sampleRate, int)
{
    engine.setParams (readParams());
    engine.prepare (sampleRate);
}

bool NubeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void NubeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (buffer.getNumChannels() < 2)
        return;

    engine.setParams (readParams());
    engine.process (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples);
}

juce::AudioProcessorEditor* NubeProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void NubeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void NubeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NubeProcessor();
}
