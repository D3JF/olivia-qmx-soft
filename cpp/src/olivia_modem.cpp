#include "olivia_modem.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace olivia {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double center_frequency = 1500.0;
constexpr double key[64] = {
    1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1,
    1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0,
    0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1,
    0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0,
};

std::vector<double> rolled_key(int offset) {
    std::vector<double> result(64);
    for (int index = 0; index < 64; ++index) {
        result[index] = key[(index + offset) % 64];
    }
    return result;
}

}  // namespace

OliviaModem::OliviaModem(int tones, int bandwidth, int sample_rate)
    : tones_(tones),
      bandwidth_(bandwidth),
      sample_rate_(sample_rate),
      bits_per_symbol_(0),
      symbol_samples_(0),
      tone_spacing_(0.0) {
    if (tones != 2 && tones != 4 && tones != 8 && tones != 16 &&
        tones != 32 && tones != 64 && tones != 128 && tones != 256) {
        throw std::invalid_argument("Unsupported Olivia tone count.");
    }
    if (bandwidth != 125 && bandwidth != 250 && bandwidth != 500 &&
        bandwidth != 1000 && bandwidth != 2000) {
        throw std::invalid_argument("Unsupported Olivia bandwidth.");
    }
    if (sample_rate <= 0) {
        throw std::invalid_argument("Sample rate must be positive.");
    }
    bits_per_symbol_ = static_cast<int>(std::log2(tones));
    tone_spacing_ = static_cast<double>(bandwidth) / tones;
    symbol_samples_ = static_cast<int>(
        std::ceil(static_cast<double>(sample_rate) / tone_spacing_));
}

int OliviaModem::gray(int value) noexcept { return value ^ (value >> 1); }

int OliviaModem::degray(int value) noexcept {
    for (int mask = value >> 1; mask != 0; mask >>= 1) {
        value ^= mask;
    }
    return value;
}

std::vector<double> OliviaModem::ifwht(const std::vector<double>& data) {
    auto result = data;
    for (int step = 32; step; step /= 2) {
        for (int start = 0; start < 64; start += 2 * step) {
            for (int index = 0; index < step; ++index) {
                const double left = result[start + index];
                const double right = result[start + step + index];
                result[start + index] = left - right;
                result[start + step + index] = left + right;
            }
        }
    }
    return result;
}

std::vector<double> OliviaModem::fwht(const std::vector<double>& data) {
    auto result = data;
    for (int step = 1; step < 64; step *= 2) {
        for (int start = 0; start < 64; start += 2 * step) {
            for (int index = 0; index < step; ++index) {
                const double left = result[start + index];
                const double right = result[start + step + index];
                result[start + index] = left + right;
                result[start + step + index] = right - left;
            }
        }
    }
    return result;
}

std::vector<int> OliviaModem::prepare_symbols(
    const std::vector<std::uint8_t>& piece) const {
    std::vector<std::vector<double>> transformed(
        bits_per_symbol_, std::vector<double>(64, 0.0));
    for (int row = 0; row < bits_per_symbol_; ++row) {
        const int value = piece[row] < 128 ? piece[row] : 0;
        transformed[row][value % 64] = value < 64 ? 1.0 : -1.0;
        transformed[row] = ifwht(transformed[row]);
        const auto row_key = rolled_key(13 * row);
        for (int index = 0; index < 64; ++index) {
            transformed[row][index] *= -2.0 * row_key[index] + 1.0;
        }
    }

    std::vector<int> result(64, 0);
    for (int index = 0; index < 64; ++index) {
        for (int bit = bits_per_symbol_ - 1; bit >= 0; --bit) {
            const int source =
                (100 * bits_per_symbol_ + bit - index) % bits_per_symbol_;
            result[index] <<= 1;
            result[index] |= transformed[source][index] < 0.0 ? 1 : 0;
        }
    }
    return result;
}

std::vector<float> OliviaModem::modulate_piece(
    const std::vector<std::uint8_t>& piece) const {
    const auto symbols = prepare_symbols(piece);
    std::vector<float> waveform(65 * symbol_samples_, 0.0F);
    const int tone_count = static_cast<int>(symbols.size());
    const int tone_samples = static_cast<int>(
        std::ceil(2.0 / tone_spacing_ * sample_rate_));
    for (int index = 0; index < tone_count; ++index) {
        const double frequency =
            center_frequency - bandwidth_ / 2.0 + tone_spacing_ / 2.0 +
            tone_spacing_ * gray(symbols[index]);
        for (int sample = 0; sample < tone_samples; ++sample) {
            const double time = static_cast<double>(sample) / sample_rate_;
            const double x = -pi + 2.0 * pi * sample / (tone_samples - 1);
            const double shape =
                1.0 + 1.1913785723 * std::cos(x) -
                0.0793018558 * std::cos(2 * x) -
                0.2171442026 * std::cos(3 * x) -
                0.0014526076 * std::cos(4 * x);
            waveform[index * symbol_samples_ + sample] += static_cast<float>(
                std::sin(2.0 * pi * frequency * time + pi / 2.0) * shape);
        }
    }
    waveform.resize(64 * symbol_samples_);
    return waveform;
}

std::vector<float> OliviaModem::modulate(const std::string& text) const {
    std::vector<std::uint8_t> encoded(text.begin(), text.end());
    std::vector<float> result;
    if (encoded.empty()) {
        encoded.push_back(0);
    }
    for (std::size_t index = 0; index < encoded.size();
         index += bits_per_symbol_) {
        std::vector<std::uint8_t> piece(bits_per_symbol_, 0);
        const auto end = std::min(encoded.size(), index + piece.size());
        std::copy(encoded.begin() + static_cast<std::ptrdiff_t>(index),
                  encoded.begin() + static_cast<std::ptrdiff_t>(end),
                  piece.begin());
        const auto waveform = modulate_piece(piece);
        result.insert(result.end(), waveform.begin(), waveform.end());
    }
    return result;
}

std::string OliviaModem::demodulate_block(
    const std::vector<float>& block) const {
    std::vector<int> symbols;
    const double start_frequency =
        center_frequency - bandwidth_ / 2.0 + tone_spacing_ / 2.0;
    for (int index = 0; index < 64; ++index) {
        int best_tone = 0;
        double best_magnitude = -1.0;
        for (int tone = 0; tone < tones_; ++tone) {
            const double frequency = start_frequency + tone_spacing_ * tone;
            const int bin = static_cast<int>(
                frequency * symbol_samples_ / sample_rate_);
            const double bin_frequency =
                static_cast<double>(bin) * sample_rate_ / symbol_samples_;
            std::complex<double> sum(0.0, 0.0);
            for (int sample = 0; sample < symbol_samples_; ++sample) {
                const double angle = -2.0 * pi * bin_frequency * sample /
                                     static_cast<double>(sample_rate_);
                sum += static_cast<double>(
                           block[index * symbol_samples_ + sample]) *
                       std::complex<double>(std::cos(angle), std::sin(angle));
            }
            const double magnitude = std::abs(sum);
            if (magnitude > best_magnitude) {
                best_magnitude = magnitude;
                best_tone = tone;
            }
        }
        symbols.push_back(degray(best_tone));
    }

    std::string output;
    for (int row = 0; row < bits_per_symbol_; ++row) {
        std::vector<double> values(64);
        const auto row_key = rolled_key(13 * row);
        for (int index = 0; index < 64; ++index) {
            values[index] =
                (symbols[index] >> ((row + index) % bits_per_symbol_)) & 1
                    ? -1.0
                    : 1.0;
            values[index] *= -2.0 * row_key[index] + 1.0;
        }
        const auto transformed = fwht(values);
        int best_index = 0;
        for (int index = 1; index < 64; ++index) {
            if (std::abs(transformed[index]) >
                std::abs(transformed[best_index])) {
                best_index = index;
            }
        }
        const int value =
            best_index + (transformed[best_index] < 0.0 ? 64 : 0);
        output.push_back(value == 0 ? '\0' : static_cast<char>(value));
    }
    return output;
}

std::string OliviaModem::demodulate(const std::vector<float>& samples) {
    pending_samples_.insert(pending_samples_.end(), samples.begin(),
                            samples.end());
    const std::size_t block_size =
        static_cast<std::size_t>(64 * symbol_samples_);
    std::string decoded;
    while (pending_samples_.size() >= block_size) {
        std::vector<float> block(pending_samples_.begin(),
                                 pending_samples_.begin() + block_size);
        pending_samples_.erase(pending_samples_.begin(),
                               pending_samples_.begin() + block_size);
        decoded += demodulate_block(block);
    }
    while (!decoded.empty() && decoded.back() == '\0') {
        decoded.pop_back();
    }
    return decoded;
}

}  // namespace olivia
