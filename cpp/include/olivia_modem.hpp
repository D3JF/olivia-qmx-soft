#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace olivia {

class OliviaModem {
public:
    OliviaModem(int tones = 8, int bandwidth = 250, int sample_rate = 8000);

    std::vector<float> modulate(const std::string& text) const;
    std::string demodulate(const std::vector<float>& samples);

    int tones() const noexcept { return tones_; }
    int bandwidth() const noexcept { return bandwidth_; }
    int sample_rate() const noexcept { return sample_rate_; }

private:
    static int gray(int value) noexcept;
    static int degray(int value) noexcept;
    static std::vector<double> fwht(const std::vector<double>& data);
    static std::vector<double> ifwht(const std::vector<double>& data);

    std::vector<int> prepare_symbols(const std::vector<std::uint8_t>& piece) const;
    std::vector<float> modulate_piece(const std::vector<std::uint8_t>& piece) const;
    std::string demodulate_block(const std::vector<float>& block) const;

    int tones_;
    int bandwidth_;
    int sample_rate_;
    int bits_per_symbol_;
    int symbol_samples_;
    double tone_spacing_;
    std::vector<float> pending_samples_;
};

}  // namespace olivia
