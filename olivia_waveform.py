#!/usr/bin/env python3
"""Render an Olivia 8/250 transmission as a JPEG image."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PyQt5.QtGui import QColor, QFont, QImage, QPainter
from PyQt5.QtWidgets import QApplication

from olivia_modem import OliviaModem


WIDTH = 1800
HEIGHT = 1000


def render_waveform(message: str, output: Path) -> None:
    """Generate and save a waveform and spectrogram for ``message``."""
    samples = OliviaModem(tones=8, bandwidth=250, sample_rate=8000).modulate(
        message
    )
    image = QImage(WIDTH, HEIGHT, QImage.Format_RGB32)
    image.fill(QColor("white"))
    painter = QPainter(image)
    painter.setPen(QColor("#111111"))
    painter.setFont(QFont("DejaVu Sans", 22, QFont.Bold))
    painter.drawText(60, 45, "Olivia MFSK 8/250 waveform")
    painter.setFont(QFont("DejaVu Sans", 14))
    painter.drawText(
        60,
        75,
        f'Message: "{message}"    Sample rate: 8000 Hz    '
        f"Center: 1500 Hz    Duration: {len(samples) / 8000:.2f} s",
    )

    left, right = 80, WIDTH - 40
    plot_top, plot_bottom = 115, 390
    painter.setPen(QColor("#aaaaaa"))
    painter.drawRect(left, plot_top, right - left, plot_bottom - plot_top)
    painter.setPen(QColor("#2255aa"))
    xs = np.linspace(left, right, len(samples), endpoint=True).astype(int)
    ys = ((plot_top + plot_bottom) / 2 - samples * 85).astype(int)
    for index in range(1, len(samples)):
        painter.drawLine(
            int(xs[index - 1]),
            int(ys[index - 1]),
            int(xs[index]),
            int(ys[index]),
        )
    painter.setPen(QColor("#555555"))
    painter.drawText(left, plot_bottom + 28, "Amplitude")

    spec_top, spec_bottom = 470, 900
    segment, hop = 512, 256
    frequencies = np.fft.rfftfreq(segment, 1 / 8000)
    mask = (frequencies >= 1300) & (frequencies <= 1700)
    windows = []
    for start in range(0, max(1, len(samples) - segment + 1), hop):
        chunk = samples[start : start + segment]
        if len(chunk) < segment:
            chunk = np.pad(chunk, (0, segment - len(chunk)))
        windows.append(
            np.abs(np.fft.rfft(chunk * np.hanning(segment)))[mask]
        )
    spectrum = np.array(windows).T
    spectrum /= max(float(spectrum.max()), 1e-12)
    for column in range(spectrum.shape[1]):
        x0 = left + column * (right - left) / spectrum.shape[1]
        x1 = left + (column + 1) * (right - left) / spectrum.shape[1] + 1
        for row in range(spectrum.shape[0]):
            value = float(spectrum[row, column])
            color = QColor(
                int(255 * value),
                int(80 * (1 - value)),
                int(255 * (1 - value)),
            )
            y0 = spec_bottom - (row + 1) * (spec_bottom - spec_top) / spectrum.shape[0]
            y1 = spec_bottom - row * (spec_bottom - spec_top) / spectrum.shape[0]
            painter.fillRect(
                int(x0),
                int(y0),
                max(1, int(x1 - x0)),
                max(1, int(y1 - y0) + 1),
                color,
            )
    painter.setPen(QColor("#555555"))
    painter.drawRect(left, spec_top, right - left, spec_bottom - spec_top)
    painter.drawText(left, spec_bottom + 32, "Spectrogram, 1300 to 1700 Hz")
    painter.drawText(right - 220, spec_bottom + 32, "Time, seconds")
    painter.end()

    if not image.save(str(output), "JPG", 95):
        raise RuntimeError(f"Could not save JPEG: {output}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render an Olivia 8/250 message as a JPEG waveform."
    )
    parser.add_argument(
        "message",
        nargs="?",
        help="message to render; reads stdin when omitted",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("olivia_waveform.jpg"),
        help="output JPEG path (default: olivia_waveform.jpg)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    message = args.message if args.message is not None else sys.stdin.read().rstrip("\n")
    if not message:
        raise SystemExit("A message argument or non-empty stdin is required.")
    app = QApplication.instance() or QApplication(sys.argv[:1])
    render_waveform(message, args.output)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
