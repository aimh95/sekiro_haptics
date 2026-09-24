"""Offline PCM authoring, Python 3.10+, standard library only.

No game access, controller output, speaker audio, or device drivers.
Run: python generate.py --output regenerated
Recipes are authoring data, not the application's OutputPreset schema.
"""
from __future__ import annotations
import argparse
import array
import hashlib
import json
import math
import random
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TAU = 2.0 * math.pi


def envelope(i, count, attack, release, decay, rate):
    # Exact zero at both ends, unless rendering a periodic loop.
    t = i / rate
    a = min(1.0, i / max(1, round(attack * rate / 1000.0)))
    r = min(1.0, (count - 1 - i) / max(1, round(release * rate / 1000.0)))
    v = (0.5 - 0.5 * math.cos(math.pi * a)) * (0.5 - 0.5 * math.cos(math.pi * r))
    return v * (math.exp(-t / (decay / 1000.0)) if decay else 1.0)


def render(recipe, rate):
    frames = round(recipe['durationMs'] * rate / 1000)
    output = [[0.0, 0.0] for _ in range(frames)]
    periodic = recipe.get('loop', False)
    for layer in recipe['layers']:
        start = round(layer.get('startMs', 0) * rate / 1000)
        count = round(layer['durationMs'] * rate / 1000)
        assert start >= 0 and start + count <= frames
        freq = layer.get('frequencyHz', [0, 0])
        f0, f1 = freq if isinstance(freq, list) else (freq, freq)
        dur = count / rate
        pan0 = layer.get('gainsLR', [1, 1])
        pan1 = layer.get('endGainsLR', pan0)
        seed = layer.get('seed', 0)
        rng = random.Random(seed)
        components = []
        if layer['kind'] == 'grain':
            low, high = layer['bandHz']
            # A finite, repeatable sum of sinusoids, NOT recorded real-world noise.
            # Integer Hz on 1 s loops makes every component periodic.
            freqs = rng.sample(range(low, high + 1), min(12, high - low + 1))
            weights = [rng.uniform(0.5, 1.0) for _ in freqs]
            scale = sum(weights)
            components = [(f, rng.uniform(0, TAU), w / scale) for f, w in zip(freqs, weights)]
        if periodic:
            assert start == 0 and count == frames and f0 == f1
            assert layer.get('endGainsLR') is None
            if layer['kind'] != 'grain':
                assert abs(f0 * dur - round(f0 * dur)) < 1e-9
            if layer.get('amHz'):
                assert abs(layer['amHz'] * dur - round(layer['amHz'] * dur)) < 1e-9
        for i in range(count):
            t = i / rate
            u = i / max(1, count - 1)
            env = 1.0 if periodic else envelope(i, count, layer['attackMs'], layer['releaseMs'], layer.get('decayMs'), rate)
            if components:
                carrier = sum(w * math.sin(TAU * f * t + phase) for f, phase, w in components)
            else:
                # Integrated linear frequency sweep. Do not use sin(2*pi*f(t)*t).
                carrier = math.sin(TAU * (f0 * t + 0.5 * (f1 - f0) * t * t / dur))
            depth = layer.get('amDepth', 0.0)
            mod = 1.0 - depth / 2 + depth / 2 * math.sin(TAU * layer.get('amHz', 0) * t)
            v = layer['amplitude'] * env * carrier * mod
            for ch in (0, 1):
                g = pan0[ch] + (pan1[ch] - pan0[ch]) * u
                output[start + i][ch] += v * g
    # Remove the tiny finite-clip average without creating nonzero endpoints.
    # This correction is measured; it is not peak/RMS normalization.
    dc_before = [sum(x[ch] for x in output) / frames for ch in (0, 1)]
    if periodic:
        window = [1.0] * frames
    else:
        window = [math.sin(math.pi * i / (frames - 1)) ** 2 for i in range(frames)]
    window_sum = sum(window)
    correction = [d * frames / window_sum for d in dc_before]
    for i in range(frames):
        for ch in (0, 1):
            output[i][ch] -= correction[ch] * window[i]
    peak = max(abs(v) for x in output for v in x)
    if peak > 0.85:
        raise ValueError(f"{recipe['id']}: design peak {peak:.6f} exceeds 0.85; edit recipe, don't normalize")
    if any(not math.isfinite(v) for x in output for v in x):
        raise ValueError('non-finite sample')
    stats = {
        'id': recipe['id'], 'frames': frames, 'durationMs': frames / rate * 1000,
        'peakLR': [max(abs(x[ch]) for x in output) for ch in (0, 1)],
        'rmsLR': [math.sqrt(sum(x[ch] ** 2 for x in output) / frames) for ch in (0, 1)],
        'meanLR': [sum(x[ch] for x in output) / frames for ch in (0, 1)],
        'dcCorrectionPeakLR': correction,
        'firstLR': output[0], 'lastLR': output[-1],
        'maxAdjacentDeltaLR': [max(abs(output[i][ch] - output[i-1][ch]) for i in range(1, frames)) for ch in (0, 1)],
        'loopBoundaryDeltaLR': [abs(output[0][ch] - output[-1][ch]) for ch in (0, 1)] if periodic else None,
        'hardwareValidated': False,
    }
    if not periodic:
        assert max(abs(v) for v in output[0] + output[-1]) < 1e-10
    else:
        for ch in (0, 1):
            assert stats['loopBoundaryDeltaLR'][ch] <= stats['maxAdjacentDeltaLR'][ch] * 1.05 + 1e-7
    return output, stats


def write_wav(path, samples, rate, pcm16=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    if pcm16:
        values = array.array('h', (round(max(-1, min(1, v)) * 32767) for x in samples for v in x))
        fmt = struct.pack('<HHIIHH', 1, 2, rate, rate * 4, 4, 16)
        fact = b''
    else:
        values = array.array('f', (v for x in samples for v in x))
        fmt = struct.pack('<HHIIHHH', 3, 2, rate, rate * 8, 8, 32, 0)
        fact = b'fact' + struct.pack('<II', 4, len(samples))
    if sys.byteorder != 'little':
        values.byteswap()
    data = values.tobytes()
    chunks = b'fmt ' + struct.pack('<I', len(fmt)) + fmt + fact + b'data' + struct.pack('<I', len(data)) + data
    path.write_bytes(b'RIFF' + struct.pack('<I', 4 + len(chunks)) + b'WAVE' + chunks)
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_trigger_map(mapping):
    for family in mapping['families']:
        effect = family['readyTrigger']
        assert effect['side'] == 'r2'
        if effect['mode'] == 'weapon':
            assert 2 <= effect['start'] <= 7 and effect['start'] < effect['end'] <= 8
            assert 1 <= effect['strength'] <= 8
            assert set(effect) == {'side','mode','start','end','strength'}
        elif effect['mode'] == 'feedback':
            assert 0 <= effect['position'] <= 9 and 1 <= effect['strength'] <= 8
            assert set(effect) == {'side','mode','position','strength'}
        else:
            raise ValueError('Unsupported mode in this design')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True, help='New output directory; never overwrites assets')
    parser.add_argument('--pcm16', action='store_true', help='Optional loader-compatible representation, no normalization')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    data = json.loads((ROOT / 'recipes.json').read_text(encoding='utf-8'))
    mapping = json.loads((ROOT / 'preset_map.json').read_text(encoding='utf-8'))
    validate_trigger_map(mapping)
    ids = [r['id'] for r in data['recipes']]
    assert len(ids) == len(set(ids))
    referenced = {c['cueId'] for f in mapping['families'] for c in f['cues']}
    referenced |= {c['cueId'] for c in mapping['bladeCues']}
    assert referenced <= set(ids)
    stats = []
    for recipe in data['recipes']:
        samples, item = render(recipe, data['sampleRateHz'])
        item['sha256'] = write_wav(args.output / 'wav' / (recipe['id'] + '.wav'), samples, data['sampleRateHz'], args.pcm16)
        stats.append(item)
    report = {'kind':'offline_signal_validation_only','sampleRateHz':data['sampleRateHz'],
              'encoding':'PCM16' if args.pcm16 else 'IEEE_FLOAT32', 'channels':['haptic_left','haptic_right'],
              'hardwareValidated':False,'runtimeIntegrationValidated':False,'clips':stats}
    (args.output / 'validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"Generated {len(stats)} clips. Peak={max(max(s['peakLR']) for s in stats):.6f}; finite, DC and boundaries checked. No hardware tested.")


if __name__ == '__main__':
    main()
