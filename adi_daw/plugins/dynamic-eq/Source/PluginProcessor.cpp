// Copyright (C) 2026 - zsliu98
// This file is part of ZLEqualizer
//
// ZLEqualizer is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License Version 3 as published by the Free Software Foundation.
//
// ZLEqualizer is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License along with ZLEqualizer. If not, see <https://www.gnu.org/licenses/>.

#include "PluginProcessor.hpp"
#include "PluginEditor.hpp"

namespace {
    // ADI adaptation of ZL 3468a3a: make its implicit double-to-float output
    // conversion explicit for /W4 /WX. Same conversion; no change to the DSP.
    void copyOutput(float* out, const double* in, size_t size) {
        for(size_t i=0;i<size;++i) out[i]=static_cast<float>(in[i]);
    }
    juce::ValueTree copyWithType(const juce::ValueTree& source, const juce::Identifier& type) {
        if (!source.isValid()) {
            return {};
        }

        juce::ValueTree result(type);
        result.copyPropertiesAndChildrenFrom(source, nullptr);
        return result;
    }

    juce::ValueTree getChildWithLegacyFallback(const juce::ValueTree& parent,
                                               const juce::Identifier& type,
                                               const juce::Identifier& legacy_type) {
        const auto child = parent.getChildWithName(type);
        return child.isValid() ? child : parent.getChildWithName(legacy_type);
    }
}

//==============================================================================
AdiEqProcessor::AdiEqProcessor() :
    AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withInput("Aux", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
        ),
    dummy_processor_(),
    parameters_(*this, nullptr,
                juce::Identifier(zlstate::schema::kParameterState),
                zlp::getParameterLayout()),
    parameters_NA_(dummy_processor_, nullptr,
                   juce::Identifier(zlstate::schema::kNonAutomatableState),
                   zlstate::getNAParameterLayout()),
    controller_(*this),
    chore_attachment_(*this, parameters_, controller_),
    ext_side_(*parameters_.getRawParameterValue(zlp::PExtSide::kID)),
    bypass_(*parameters_.getRawParameterValue(zlp::PBypass::kID)) {
    for (size_t i = 0; i < zlp::kBandNum; ++i) {
        filter_attachments_[i] = std::make_unique<zlp::FilterAttach>(*this, parameters_, controller_, i);
        filter_dynamic_attachments_[i] = std::make_unique<zlp::FilterDynamicAttach>(*this, parameters_, controller_, i);
        filter_side_attachments_[i] = std::make_unique<zlp::FilterSideAttach>(*this, parameters_, controller_, i);
    }
}

AdiEqProcessor::~AdiEqProcessor() = default;

//==============================================================================
const juce::String AdiEqProcessor::getName() const {
    return JucePlugin_Name;
}

bool AdiEqProcessor::acceptsMidi() const {
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool AdiEqProcessor::producesMidi() const {
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool AdiEqProcessor::isMidiEffect() const {
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double AdiEqProcessor::getTailLengthSeconds() const {
    return 0.0;
}

int AdiEqProcessor::getNumPrograms() {
    return 1;
}

int AdiEqProcessor::getCurrentProgram() {
    return 0;
}

void AdiEqProcessor::setCurrentProgram(int index) {
    juce::ignoreUnused(index);
}

const juce::String AdiEqProcessor::getProgramName(int index) {
    juce::ignoreUnused(index);
    return "Default";
}

void AdiEqProcessor::changeProgramName(int, const juce::String&) {
}

//==============================================================================
void AdiEqProcessor::prepareToPlay(const double sample_rate, const int samples_per_block) {
    for (size_t i = 0; i < 2; ++i) {
        main_buffer_[i].resize(static_cast<size_t>(samples_per_block));
        main_pointers_[i] = main_buffer_[i].data();
        side_buffer_[i].resize(static_cast<size_t>(samples_per_block));
        side_pointers_[i] = side_buffer_[i].data();
    }
    updateChannelLayout();
    const juce::PluginHostType hostType;
    update_channel_layout_per_call_ = hostType.isMaschine();
    controller_.prepare(sample_rate, static_cast<size_t>(samples_per_block));
    sample_rate_.store(sample_rate, std::memory_order::relaxed);
}

void AdiEqProcessor::releaseResources() {
}

bool AdiEqProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    if (layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo() &&
        layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() &&
        (layouts.getChannelSet(true, 1).isDisabled() ||
            layouts.getChannelSet(true, 1) == juce::AudioChannelSet::mono() ||
            layouts.getChannelSet(true, 1) == juce::AudioChannelSet::stereo())) {
        return true;
    }
    if (layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono() &&
        layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono() &&
        (layouts.getChannelSet(true, 1).isDisabled() ||
            layouts.getChannelSet(true, 1) == juce::AudioChannelSet::mono() ||
            layouts.getChannelSet(true, 1) == juce::AudioChannelSet::stereo())) {
        return true;
    }
    return false;
}

void AdiEqProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    if (bypass_.load(std::memory_order::relaxed) > .5f) {
        processBlockInternal<true>(buffer);
    } else {
        processBlockInternal<false>(buffer);
    }
}

void AdiEqProcessor::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) {
    if (bypass_.load(std::memory_order::relaxed) > .5f) {
        processBlockInternal<true>(buffer);
    } else {
        processBlockInternal<false>(buffer);
    }
}

void AdiEqProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    processBlockInternal<true>(buffer);
}

void AdiEqProcessor::processBlockBypassed(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) {
    processBlockInternal<true>(buffer);
}

template <bool bypass>
void AdiEqProcessor::processBlockInternal(juce::AudioBuffer<float>& buffer) {
    juce::ScopedNoDenormals no_denormals;
    if (buffer.getNumSamples() == 0) {
        return; // ignore empty blocks
    }
    if (update_channel_layout_per_call_) {
        updateChannelLayout();
    }
    const auto c_ext_side = ext_side_.load(std::memory_order::relaxed) > .5f;
    const auto num_samples = static_cast<size_t>(buffer.getNumSamples());
    switch (channel_layout_) {
    case kMain1Aux0: {
        zldsp::vector::copy(main_pointers_[0], buffer.getReadPointer(0), num_samples);
        zldsp::vector::copy(main_pointers_[1], main_pointers_[0], num_samples);
        zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
        zldsp::vector::copy(side_pointers_[1], side_pointers_[0], num_samples);
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        copyOutput(buffer.getWritePointer(0), main_pointers_[0], num_samples);
        break;
    }
    case kMain1Aux1: {
        zldsp::vector::copy(main_pointers_[0], buffer.getReadPointer(0), num_samples);
        zldsp::vector::copy(main_pointers_[1], main_pointers_[0], num_samples);
        if (c_ext_side) {
            zldsp::vector::copy(side_pointers_[0], buffer.getReadPointer(1), num_samples);
        } else {
            zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
        }
        zldsp::vector::copy(side_pointers_[1], side_pointers_[0], num_samples);
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        copyOutput(buffer.getWritePointer(0), main_pointers_[0], num_samples);
        break;
    }
    case kMain1Aux2: {
        zldsp::vector::copy(main_pointers_[0], buffer.getReadPointer(0), num_samples);
        zldsp::vector::copy(main_pointers_[1], main_pointers_[0], num_samples);
        if (c_ext_side) {
            zldsp::vector::copy(side_pointers_[0], buffer.getReadPointer(1), num_samples);
            zldsp::vector::copy(side_pointers_[1], buffer.getReadPointer(2), num_samples);
        } else {
            zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
            zldsp::vector::copy(side_pointers_[1], main_pointers_[0], num_samples);
        }

        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        copyOutput(buffer.getWritePointer(0), main_pointers_[0], num_samples);
        break;
    }
    case kMain2Aux0: {
        zldsp::vector::copy(main_pointers_[0], buffer.getReadPointer(0), num_samples);
        zldsp::vector::copy(main_pointers_[1], buffer.getReadPointer(1), num_samples);
        zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
        zldsp::vector::copy(side_pointers_[1], main_pointers_[1], num_samples);
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        copyOutput(buffer.getWritePointer(0), main_pointers_[0], num_samples);
        copyOutput(buffer.getWritePointer(1), main_pointers_[1], num_samples);
        break;
    }
    case kMain2Aux1: {
        zldsp::vector::copy(main_pointers_[0], buffer.getReadPointer(0), num_samples);
        zldsp::vector::copy(main_pointers_[1], buffer.getReadPointer(1), num_samples);
        if (c_ext_side) {
            zldsp::vector::copy(side_pointers_[0], buffer.getReadPointer(2), num_samples);
            zldsp::vector::copy(side_pointers_[1], buffer.getReadPointer(2), num_samples);
        } else {
            zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
            zldsp::vector::copy(side_pointers_[1], main_pointers_[1], num_samples);
        }
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        copyOutput(buffer.getWritePointer(0), main_pointers_[0], num_samples);
        copyOutput(buffer.getWritePointer(1), main_pointers_[1], num_samples);
        break;
    }
    case kMain2Aux2: {
        zldsp::vector::copy(main_pointers_[0], buffer.getReadPointer(0), num_samples);
        zldsp::vector::copy(main_pointers_[1], buffer.getReadPointer(1), num_samples);
        if (c_ext_side) {
            zldsp::vector::copy(side_pointers_[0], buffer.getReadPointer(2), num_samples);
            zldsp::vector::copy(side_pointers_[1], buffer.getReadPointer(3), num_samples);
        } else {
            zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
            zldsp::vector::copy(side_pointers_[1], main_pointers_[1], num_samples);
        }
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        copyOutput(buffer.getWritePointer(0), main_pointers_[0], num_samples);
        copyOutput(buffer.getWritePointer(1), main_pointers_[1], num_samples);
        break;
    }
    case kInvalid: {
        return;
    }
    }
}

template <bool bypass>
void AdiEqProcessor::processBlockInternal(juce::AudioBuffer<double>& buffer) {
    juce::ScopedNoDenormals no_denormals;
    if (buffer.getNumSamples() == 0) {
        return; // ignore empty blocks
    }
    if (update_channel_layout_per_call_) {
        updateChannelLayout();
    }
    const auto c_ext_side = ext_side_.load(std::memory_order::relaxed) > .5f;
    const auto num_samples = static_cast<size_t>(buffer.getNumSamples());
    switch (channel_layout_) {
    case kMain1Aux0: {
        main_pointers_[0] = buffer.getWritePointer(0);
        zldsp::vector::copy(main_pointers_[1], main_pointers_[0], num_samples);
        zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
        zldsp::vector::copy(side_pointers_[1], side_pointers_[0], num_samples);
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        break;
    }
    case kMain1Aux1: {
        main_pointers_[0] = buffer.getWritePointer(0);
        zldsp::vector::copy(main_pointers_[1], main_pointers_[0], num_samples);
        if (c_ext_side) {
            zldsp::vector::copy(side_pointers_[0], buffer.getReadPointer(1), num_samples);
        } else {
            zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
        }
        zldsp::vector::copy(side_pointers_[1], side_pointers_[0], num_samples);
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        break;
    }
    case kMain1Aux2: {
        main_pointers_[0] = buffer.getWritePointer(0);
        zldsp::vector::copy(main_pointers_[1], main_pointers_[0], num_samples);
        if (c_ext_side) {
            zldsp::vector::copy(side_pointers_[0], buffer.getReadPointer(1), num_samples);
            zldsp::vector::copy(side_pointers_[1], buffer.getReadPointer(2), num_samples);
        } else {
            zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
            zldsp::vector::copy(side_pointers_[1], side_pointers_[0], num_samples);
        }
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        break;
    }
    case kMain2Aux0: {
        main_pointers_[0] = buffer.getWritePointer(0);
        main_pointers_[1] = buffer.getWritePointer(1);
        zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
        zldsp::vector::copy(side_pointers_[1], main_pointers_[1], num_samples);
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        break;
    }
    case kMain2Aux1: {
        main_pointers_[0] = buffer.getWritePointer(0);
        main_pointers_[1] = buffer.getWritePointer(1);
        if (c_ext_side) {
            zldsp::vector::copy(side_pointers_[0], buffer.getReadPointer(2), num_samples);
            zldsp::vector::copy(side_pointers_[1], side_pointers_[0], num_samples);
        } else {
            zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
            zldsp::vector::copy(side_pointers_[1], main_pointers_[1], num_samples);
        }
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        break;
    }
    case kMain2Aux2: {
        main_pointers_[0] = buffer.getWritePointer(0);
        main_pointers_[1] = buffer.getWritePointer(1);
        if (c_ext_side) {
            zldsp::vector::copy(side_pointers_[0], buffer.getReadPointer(2), num_samples);
            zldsp::vector::copy(side_pointers_[1], buffer.getReadPointer(3), num_samples);
        } else {
            zldsp::vector::copy(side_pointers_[0], main_pointers_[0], num_samples);
            zldsp::vector::copy(side_pointers_[1], main_pointers_[1], num_samples);
        }
        controller_.template process<bypass>(main_pointers_, side_pointers_, num_samples);
        break;
    }
    case kInvalid: {
        return;
    }
    }
}

bool AdiEqProcessor::hasEditor() const {
    return true;
}

juce::AudioProcessorEditor* AdiEqProcessor::createEditor() {
    return new AdiEqEditor(*this);
    // return new juce::GenericAudioProcessorEditor(*this);
}

void AdiEqProcessor::getStateInformation(juce::MemoryBlock& dest_data) {
    auto temp_tree = juce::ValueTree(zlstate::schema::kProcessorState);
    temp_tree.appendChild(parameters_.copyState(), nullptr);
    temp_tree.appendChild(parameters_NA_.copyState(), nullptr);
    const std::unique_ptr<juce::XmlElement> xml(temp_tree.createXml());
    copyXmlToBinary(*xml, dest_data);
}

void AdiEqProcessor::setStateInformation(const void* data, const int size_in_bytes) {
    std::unique_ptr<juce::XmlElement> xml_state(getXmlFromBinary(data, size_in_bytes));
    if (xml_state == nullptr ||
        (!xml_state->hasTagName(zlstate::schema::kProcessorState) &&
            !xml_state->hasTagName("ZLCompressorParaState"))) {
        return;
    }

    const auto temp_tree = juce::ValueTree::fromXml(*xml_state);
    const auto parameter_state = getChildWithLegacyFallback(
        temp_tree,
        juce::Identifier(zlstate::schema::kParameterState),
        juce::Identifier(zlstate::schema::legacy::kParameterState));
    const auto non_automatable_state = getChildWithLegacyFallback(
        temp_tree,
        juce::Identifier(zlstate::schema::kNonAutomatableState),
        juce::Identifier(zlstate::schema::legacy::kNonAutomatableState));
    if (!parameter_state.isValid() || !non_automatable_state.isValid()) {
        return;
    }

    parameters_.replaceState(copyWithType(parameter_state,
                                          juce::Identifier(zlstate::schema::kParameterState)));
    parameters_NA_.replaceState(copyWithType(non_automatable_state,
                                             juce::Identifier(zlstate::schema::kNonAutomatableState)));
}

void AdiEqProcessor::updateChannelLayout() {
    // determine current channel layout
    const auto* main_bus = getBus(true, 0);
    const auto* aux_bus = getBus(true, 1);
    channel_layout_ = ChannelLayout::kInvalid;
    if (main_bus == nullptr) {
        return;
    }
    if (main_bus->getCurrentLayout() == juce::AudioChannelSet::mono()) {
        if (aux_bus == nullptr || !aux_bus->isEnabled()) {
            channel_layout_ = ChannelLayout::kMain1Aux0;
        } else if (aux_bus->getCurrentLayout() == juce::AudioChannelSet::mono()) {
            channel_layout_ = ChannelLayout::kMain1Aux1;
        } else if (aux_bus->getCurrentLayout() == juce::AudioChannelSet::stereo()) {
            channel_layout_ = ChannelLayout::kMain1Aux2;
        }
    } else if (main_bus->getCurrentLayout() == juce::AudioChannelSet::stereo()) {
        if (aux_bus == nullptr || !aux_bus->isEnabled()) {
            channel_layout_ = ChannelLayout::kMain2Aux0;
        } else if (aux_bus->getCurrentLayout() == juce::AudioChannelSet::mono()) {
            channel_layout_ = ChannelLayout::kMain2Aux1;
        } else if (aux_bus->getCurrentLayout() == juce::AudioChannelSet::stereo()) {
            channel_layout_ = ChannelLayout::kMain2Aux2;
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE

createPluginFilter() {
    return new AdiEqProcessor();
}
