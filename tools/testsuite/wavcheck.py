#!/usr/bin/env python3
"""wavcheck.py <file.wav> <expected Hz>   (0 = expect silence)

Checks a Music 5000 recording: prints rate, RMS and the spectral peak;
exit 0 when the peak is within 3% of the expected frequency and the RMS is
well above the silence floor (or, for 0 Hz, when the RMS is at the floor)."""
import sys
import wave
import numpy as np

TONE_RMS, SILENCE_RMS = 300.0, 100.0


def main(path, expect):
    with wave.open(path, 'rb') as w:
        rate, ch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        print('unsupported sample width %d' % sw)
        return 1
    x = np.frombuffer(raw, dtype='<i2').astype(np.float64)
    if ch == 2:
        x = x.reshape(-1, 2).mean(axis=1)
    x -= x.mean()
    rms = float(np.sqrt(np.mean(x * x))) if len(x) else 0.0
    if expect == 0:
        print('rate %d ch %d frames %d rms %.0f (silence floor %.0f)' % (rate, ch, n, rms, SILENCE_RMS))
        return 0 if rms < SILENCE_RMS else 1
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    freqs = np.fft.rfftfreq(len(x), 1.0 / rate)
    lo = int(20 * len(x) / rate)                     # ignore DC and rumble
    peak = float(freqs[lo + int(np.argmax(spec[lo:]))])
    print('rate %d ch %d frames %d rms %.0f peak %.0f Hz (want %.0f)' % (rate, ch, n, rms, peak, expect))
    return 0 if rms > TONE_RMS and abs(peak - expect) / expect < 0.03 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], float(sys.argv[2])))
