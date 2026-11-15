#pragma once

#include <array>
#include <cstddef>
#include <memory>

class ADISharedEqSpectralDynamics
{
public:
    static constexpr std::size_t bandCount = 24;
    static constexpr std::size_t displayBinCount = 128;
    enum class Routing { Stereo, Left, Right, Mid, Side };
    enum class Shape { Bell, LowShelf, HighShelf };
    enum class Resolution { Low, Medium, High };

    struct Band
    {
        bool enabled = false;
        bool bypassed = false;
        bool externalSidechain = false;
        bool automaticThreshold = true;
        float frequencyHz = 1000.0f;
        float q = 1.0f;
        float staticGainDb = 0.0f;
        float rangeDb = -12.0f;
        float thresholdDb = -36.0f;
        float density = 0.5f;
        float tiltDbPerOctave = 3.0f;
        float attackMs = 10.0f;
        float releaseMs = 100.0f;
        Routing routing = Routing::Stereo;
        Shape shape = Shape::Bell;
        bool operator==(const Band&) const = default;
    };

    ADISharedEqSpectralDynamics();
    ~ADISharedEqSpectralDynamics();
    ADISharedEqSpectralDynamics(const ADISharedEqSpectralDynamics&) = delete;
    ADISharedEqSpectralDynamics& operator=(const ADISharedEqSpectralDynamics&) = delete;

    void prepare(double sampleRate, int maximumBlockSize);
    void reset() noexcept;
    void setBands(const std::array<Band, bandCount>& bands) noexcept;
    void setResolution(Resolution resolution) noexcept;
    void process(double* const main[2], const double* const sidechain[2],
                 int channels, int samples, bool bypass = false) noexcept;
    int latencySamples() const noexcept;
    int fftSize() const noexcept;
    int preparedFftSize(Resolution resolution) const noexcept;
    bool isEnabled() const noexcept;
    void setDisplayEnabled(bool enabled) noexcept;
    void copyDisplay(std::array<float, displayBinCount>& dynamicDb,
                     std::array<float, bandCount>& bandPeakChangeDb) const noexcept;
    static float displayFrequency(std::size_t index, double sampleRate) noexcept;
    static void calculateStaticResponse(const Band& band, double sampleRate,
                                        const float* frequencies, float* decibels, int count) noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation;
};
