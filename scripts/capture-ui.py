#!/usr/bin/env python3
"""Run synthetic Qt UI fixtures at separate display scales and save review images."""
import argparse
import html
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-executable', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--platform', default='offscreen')
    parser.add_argument('--scales', default='1,1.25,1.5')
    args = parser.parse_args()
    executable = args.test_executable.resolve()
    if not executable.is_file():
        parser.error('Build test_ui first')
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('Choose a fresh output directory to keep screenshot runs separate')
    scales = args.scales.split(',')
    if any(float(scale) <= 0 or float(scale) > 4 for scale in scales):
        parser.error('Display scales must be greater than 0 and at most 4')
    output.mkdir(parents=True, exist_ok=True)
    manifest = []
    base_dpr = None
    for scale in scales:
        folder = output / ('scale-' + scale.replace('.', '_'))
        folder.mkdir()
        env = dict(os.environ, QT_QPA_PLATFORM=args.platform,
                   QT_ENABLE_HIGHDPI_SCALING='1', QT_AUTO_SCREEN_SCALE_FACTOR='0',
                   QT_SCALE_FACTOR=scale, QT_SCALE_FACTOR_ROUNDING_POLICY='PassThrough',
                   CLIENT_QT_SCREENSHOT_DIR=str(folder))
        result = subprocess.run(
            [str(executable), 'rendersAssetViewsAndOptionalScreenshots',
             'loginLogoRemainsComplete', 'versionFeedbackOnlyUsesStatusBar',
             'connectionValidationClearsAndSmallWindowScrolls', 'visualLayoutMatrix'],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        (folder / 'test.log').write_text(result.stdout)
        print('Display scale {}: {}'.format(scale, 'passed' if result.returncode == 0 else 'failed'))
        if result.returncode:
            raise RuntimeError('UI verification failed; inspect {}'.format(folder / 'test.log'))
        geometry = [json.loads(line) for line in (folder / 'geometry.jsonl').read_text().splitlines()]
        if not geometry:
            raise RuntimeError('No screenshot geometry was recorded')
        effective_dpr = geometry[0]['devicePixelRatio']
        if base_dpr is None:
            base_dpr = effective_dpr / float(scale)
        if any(abs(item['devicePixelRatio'] - base_dpr * float(scale)) > 0.05 for item in geometry):
            raise RuntimeError('Requested display scale was not applied consistently')
        images = sorted(folder.glob('*.png'))
        if not images:
            raise RuntimeError('No UI screenshots were produced')
        for image in images:
            with image.open('rb') as stream:
                header = stream.read(24)
            if header[:8] != b'\x89PNG\r\n\x1a\n':
                raise RuntimeError('Invalid PNG: {}'.format(image))
            width, height = struct.unpack('>II', header[16:24])
            manifest.append({'scale': scale, 'device_pixel_ratio': effective_dpr,
                             'image': str(image.relative_to(output)),
                             'pixel_width': width, 'pixel_height': height})
    (output / 'images.json').write_text(json.dumps(manifest, indent=2) + '\n')
    cards = []
    for item in manifest:
        path = html.escape(item['image'], quote=True)
        label = html.escape('{} — {} × {}'.format(item['image'], item['pixel_width'], item['pixel_height']))
        cards.append('<figure><a href="{0}"><img src="{0}" loading="lazy"></a><figcaption>{1}</figcaption></figure>'.format(path, label))
    (output / 'index.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>Qt UI validation</title>'
        '<style>body{font:14px system-ui;background:#eee;color:#222;padding:20px}'
        'main{display:grid;grid-template-columns:repeat(auto-fit,minmax(400px,1fr));gap:20px}'
        'figure{margin:0;background:white;padding:10px}img{width:100%;height:auto}'
        'figcaption{margin-top:8px}</style><h1>Synthetic UI validation</h1><main>'
        + ''.join(cards) + '</main>')
    print('Screenshot index: {}'.format(output / 'index.html'))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print('UI capture failed: {}'.format(error), file=sys.stderr)
        sys.exit(1)
