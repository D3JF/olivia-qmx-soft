"""Small Olivia MFSK 8/250 codec used by the QMX+ application."""

from __future__ import annotations

import numpy as np


_KEY = np.flip(
    np.array(
        [
            1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1,
            1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0,
            0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1,
            0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0,
        ],
        dtype=np.float64,
    )
)


def _ifwht(data: np.ndarray) -> np.ndarray:
    result = data.copy()
    step = 32
    while step:
        for start in range(0, 64, 2 * step):
            left = result[start : start + step].copy()
            right = result[start + step : start + 2 * step].copy()
            result[start : start + step] = left - right
            result[start + step : start + 2 * step] = left + right
        step //= 2
    return result


def _fwht(data: np.ndarray) -> np.ndarray:
    result = data.copy()
    step = 1
    while step < 64:
        for start in range(0, 64, 2 * step):
            left = result[start : start + step].copy()
            right = result[start + step : start + 2 * step].copy()
            result[start : start + step] = left + right
            result[start + step : start + 2 * step] = right - left
        step *= 2
    return result


def _gray(value: int) -> int:
    return value ^ (value >> 1)


def _degray(value: int) -> int:
    mask = value
    while mask:
        mask >>= 1
        value ^= mask
    return value


class OliviaModem:
    """Encode and incrementally decode Olivia MFSK frames."""

    def __init__(self, tones: int = 8, bandwidth: int = 250, sample_rate: int = 8000):
        if tones != 8 or bandwidth != 250:
            raise ValueError("This codec supports Olivia 8/250 only.")
        self.tones = tones
        self.bandwidth = bandwidth
        self.sample_rate = sample_rate
        self.center_frequency = 1500
        self._bits_per_symbol = 3
        self._tone_spacing = bandwidth / tones
        self._symbol_samples = int(np.ceil(sample_rate / self._tone_spacing))
        self._samples = np.empty(0, dtype=np.float32)

    def modulate(self, text: str) -> np.ndarray:
        """Return waveform samples for text, including complete Olivia blocks."""
        encoded = text.encode("latin-1", errors="replace")
        pieces = [
            encoded[index : index + self._bits_per_symbol].ljust(
                self._bits_per_symbol, b"\0"
            )
            for index in range(0, len(encoded), self._bits_per_symbol)
        ]
        if not pieces:
            pieces = [b"\0" * self._bits_per_symbol]

        blocks = [self._modulate_piece(piece) for piece in pieces]
        return np.concatenate(blocks).astype(np.float32, copy=False)

    def demodulate(self, samples: np.ndarray) -> str:
        """Consume audio samples and return any completely decoded text."""
        self._samples = np.concatenate(
            (self._samples, np.asarray(samples, dtype=np.float32).reshape(-1))
        )
        block_size = 64 * self._symbol_samples
        decoded: list[str] = []
        while self._samples.size >= block_size:
            block = self._samples[:block_size]
            self._samples = self._samples[block_size:]
            decoded.append(self._demodulate_block(block))
        return "".join(decoded).rstrip("\0")

    def _modulate_piece(self, piece: bytes) -> np.ndarray:
        symbols = self._prepare_symbols(piece)
        waveform = np.zeros(65 * self._symbol_samples, dtype=np.float64)
        for index, symbol in enumerate(symbols):
            frequency = (
                self.center_frequency
                - self.bandwidth / 2
                + self._tone_spacing / 2
                + self._tone_spacing * _gray(int(symbol))
            )
            time = np.arange(2 / self._tone_spacing, step=1 / self.sample_rate)
            tone = np.sin(2 * np.pi * frequency * time + np.pi / 2)
            x = np.linspace(-np.pi, np.pi, tone.size)
            shape = (
                1
                + 1.1913785723 * np.cos(x)
                - 0.0793018558 * np.cos(2 * x)
                - 0.2171442026 * np.cos(3 * x)
                - 0.0014526076 * np.cos(4 * x)
            )
            start = index * self._symbol_samples
            waveform[start : start + tone.size] += tone * shape
        return waveform[: 64 * self._symbol_samples]

    def _prepare_symbols(self, piece: bytes) -> np.ndarray:
        transformed = np.zeros((self._bits_per_symbol, 64), dtype=np.float64)
        for row, value in enumerate(piece):
            value = value if value < 128 else 0
            transformed[row, value % 64] = 1 if value < 64 else -1
            transformed[row] = _ifwht(transformed[row])
            transformed[row] *= -2 * np.roll(_KEY, -13 * row) + 1

        symbols = np.zeros((64, self._bits_per_symbol), dtype=np.float64)
        for bit in range(self._bits_per_symbol):
            for index in range(64):
                source = (
                    100 * self._bits_per_symbol + bit - index
                ) % self._bits_per_symbol
                if transformed[source, index] < 0:
                    symbols[index, bit] = 1
        result = np.zeros(64, dtype=np.int64)
        for index in range(64):
            for bit in np.flip(symbols[index]).astype(int):
                result[index] = (result[index] << 1) | bit
        return result

    def _demodulate_block(self, block: np.ndarray) -> str:
        symbols = []
        for index in range(64):
            chunk = block[
                index * self._symbol_samples : (index + 1) * self._symbol_samples
            ]
            spectrum = np.abs(np.fft.fft(chunk))
            start_frequency = (
                self.center_frequency
                - self.bandwidth / 2
                + self._tone_spacing / 2
            )
            bins = [
                int(
                    (start_frequency + self._tone_spacing * (tone + 1))
                    * self._symbol_samples
                    / self.sample_rate
                )
                for tone in range(self.tones)
            ]
            symbols.append(_degray(int(np.argmax(spectrum[bins]))))

        output = []
        for row in range(self._bits_per_symbol):
            values = np.array(
                [
                    -1 if (symbol >> ((row + index) % self._bits_per_symbol)) & 1 else 1
                    for index, symbol in enumerate(symbols)
                ],
                dtype=np.float64,
            )
            values *= -2 * np.roll(_KEY, -13 * row) + 1
            transformed = _fwht(values)
            index = int(np.argmax(np.abs(transformed)))
            value = index + (64 if transformed[index] < 0 else 0)
            output.append("\0" if value == 0 else chr(value))
        return "".join(output)
