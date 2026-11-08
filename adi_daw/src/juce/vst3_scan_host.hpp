// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The scanner uses JUCE's headless discovery/description layer, not device/UI
// hosting. Shared by the child and the parent format validator.
#include "adi/plugin_scanner.hpp"
#include "adi/audio/io_audit.hpp"
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <stdexcept>

namespace adi::device {
#if defined(ADI_SCAN_WITH_VST3)
// Only the dedicated helper target defines this. Parent adapters have no
// callable discovery factory probe, only the metadata decoder/client below.
inline std::vector<scan::Plugin> probeVst3Bundle(const std::string& path) {
    if(audio::onAudioThread)throw std::runtime_error("VST3 scan refuses callback");
    juce::ScopedJuceInitialiser_GUI initialize;
    juce::VST3PluginFormatHeadless format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    const auto requested=juce::String::fromUTF8(path.c_str());
    format.findAllTypesForFile(descriptions,requested);
    if(descriptions.size()<=0 || static_cast<std::size_t>(descriptions.size())>scan::kMaximumPlugins)
        throw std::runtime_error("VST3 scanner descriptor bound");
    // JUCE binds each description to the library it enumerated: the bundle on
    // macOS and Linux, but on Windows the DLL inside a bundle directory
    // (getLibraryPaths), unless a moduleinfo.json bound the bundle first. The
    // protocol identity is the requested canonical path, and the owner refuses
    // any other spelling, so bind every description to it before its
    // identifier string and XML are serialized.
    std::vector<scan::Plugin> result;
    for(const auto* found:descriptions) {
        if(found==nullptr)throw std::runtime_error("missing VST3 scanner descriptor");
        juce::PluginDescription description=*found;description.fileOrIdentifier=requested;
        scan::Plugin value;value.id=description.createIdentifierString().toStdString();
        value.name=description.name.toStdString();value.vendor=description.manufacturerName.toStdString();
        value.version=description.version.toStdString();value.features=description.category.toStdString();
        value.instrument=description.isInstrument;value.data=description.createXml()->toString().toStdString();
        result.push_back(std::move(value));
    }
    return result;
}
#endif

/// Whole format validation before quarantine is cleared. Context is an owner
/// vector of descriptions; consume it only when Scanner::probe returns Ready.
inline bool decodeVst3Metadata(const std::vector<scan::Plugin>& plugins,const std::string& path,
                               void* context,std::string& error) {
    if(audio::onAudioThread)return false;
    try {
        if(context==nullptr)throw std::runtime_error("missing VST3 metadata destination");
        std::vector<juce::PluginDescription> result;result.reserve(plugins.size());
        for(const auto& value:plugins) {
            // JUCE writes a simple PLUGIN element with attributes. No DTD,
            // entity declarations/comments or nested data is part of this wire
            // format; refuse those before invoking an XML parser.
            if(value.data.find("<!")!=value.data.npos)throw std::runtime_error("unexpected VST3 XML declaration");
            auto xml=juce::XmlDocument::parse(juce::String::fromUTF8(value.data.c_str()));
            juce::PluginDescription description;
            if(!xml || !xml->hasTagName("PLUGIN") || xml->getFirstChildElement()!=nullptr ||
                !description.loadFromXml(*xml) || description.pluginFormatName!="VST3" ||
                description.fileOrIdentifier.toStdString()!=path ||
                description.createIdentifierString().toStdString()!=value.id ||
                description.name.toStdString()!=value.name || description.manufacturerName.toStdString()!=value.vendor ||
                description.version.toStdString()!=value.version || description.category.toStdString()!=value.features ||
                description.isInstrument!=value.instrument || description.numInputChannels<0 || description.numOutputChannels<0)
                throw std::runtime_error("inconsistent VST3 scanner description");
            result.push_back(std::move(description));
        }
        *static_cast<std::vector<juce::PluginDescription>*>(context)=std::move(result);
        return true;
    } catch(const std::exception&) {error="invalid VST3 scanner description";return false;}
}

inline scan::Result scanVst3File(scan::Scanner& scanner,const juce::String& file,
                                std::vector<juce::PluginDescription>& destination) {
    if(audio::onAudioThread){scan::Result result;result.state=scan::State::CallbackRefused;return result;}
    const auto original=juce::File(file).getFullPathName();const auto bytes=original.toStdString();
    std::vector<juce::PluginDescription> staged;
    auto result=scanner.probe("vst3",std::filesystem::path(std::u8string(bytes.begin(),bytes.end())),decodeVst3Metadata,&staged);
    if(result.state==scan::State::Ready) {
        // JUCE hashes this field into the stored UID. The native canonical
        // path binds the protocol/quarantine; retain original absolute JUCE
        // spelling for saved identifiers after validating the child response.
        for(auto& description:staged)description.fileOrIdentifier=original;
        destination=std::move(staged);
    }
    return result;
}
} // namespace adi::device
