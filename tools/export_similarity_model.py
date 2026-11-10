"""Reproduce and verify the pinned local CLAP audio-only deployment resources.
Build-time tool only. It never ships Python, source audio, or a text encoder.
Use the pinned versions in docs/similarity-model-build.md.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path

MODEL = 'laion/clap-htsat-unfused'
REVISION = '8fa0f1c6d0433df6e97c127f64b2a1d6c0dcda8a'
HASHES = {
    'clap-audio.onnx': '50d03fb781f7bf520a5d424a3479871cfca4af7a31026892ac1e935d5303f677',
    'clap-mel-filters.f64': '256b8c586fb0f0ea44c6b2cafa44caa9bf288c1cfe1a05ff2527b73ae123d37a',
    'clap-window.f64': '0d9c0e8dfb7e477785eaaac6e2082e19a030bc113178a207cb4d1ab8d536a7a9',
}

def main(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    os.environ['HF_HOME'] = str(root / 'hf-cache')
    os.environ['HF_HUB_DISABLE_SYMLINKS_WARNING'] = '1'
    os.environ['HF_HUB_DISABLE_XET'] = '1'
    import numpy as np
    import torch
    import onnxruntime as ort
    from transformers import ClapAudioModelWithProjection, ClapFeatureExtractor
    from transformers.audio_utils import window_function
    torch.set_num_threads(4)
    processor = ClapFeatureExtractor.from_pretrained(MODEL, revision=REVISION)
    encoder = ClapAudioModelWithProjection.from_pretrained(MODEL, revision=REVISION).eval()
    class AudioEncoder(torch.nn.Module):
        def __init__(self, encoder):
            super().__init__()
            self.encoder = encoder
        def forward(self, mel):
            return self.encoder(input_features=mel, return_dict=False)[0]
    model = AudioEncoder(encoder).eval()
    t = np.arange(480000, dtype=np.float32) / 48000
    synthetic = (0.4 * np.sin(2 * np.pi * 440 * t) * np.exp(-np.remainder(t, 1.0) * 8)).astype(np.float32)
    example = processor(synthetic, sampling_rate=48000, return_tensors='pt',
                        truncation='rand_trunc', padding='repeatpad')['input_features']
    destination = root / 'clap-audio.onnx'
    if not args.verify_existing:
        torch.onnx.export(model, (example,), str(destination), input_names=['mel'], output_names=['embedding'],
                          opset_version=17, do_constant_folding=True, dynamo=False)
        processor.mel_filters_slaney.astype('<f8').tofile(root / 'clap-mel-filters.f64')
        window_function(1024, 'hann').astype('<f8').tofile(root / 'clap-window.f64')
    resources = {name: hashlib.sha256((root/name).read_bytes()).hexdigest() for name in HASHES}
    if resources != HASHES:
        raise RuntimeError('Exported resources differ from the pinned native deployment hashes; do not silently replace them.')
    options = ort.SessionOptions()
    options.intra_op_num_threads = 4
    session = ort.InferenceSession(str(destination), sess_options=options, providers=['CPUExecutionProvider'])
    def fixture(audio, prefix):
        mel = processor(audio, sampling_rate=48000, return_tensors='pt',
                        truncation='rand_trunc', padding='repeatpad')['input_features']
        with torch.no_grad():
            expected = model(mel).cpu().numpy()
        actual = session.run(None, {'mel': mel.cpu().numpy()})[0]
        cosine = float(np.dot(expected.ravel(), actual.ravel()) / (np.linalg.norm(expected) * np.linalg.norm(actual)))
        error = float(np.max(np.abs(expected - actual)))
        np.asarray(audio, dtype='<f4').tofile(root / (prefix+'audio.f32'))
        mel.cpu().numpy().astype('<f4').tofile(root / (prefix+'mel.f32'))
        expected.astype('<f4').tofile(root / (prefix+'embedding.f32'))
        return {'samples': len(audio), 'input_shape': list(mel.shape), 'cosine': cosine,
                'max_absolute_error': error, 'passed': cosine >= .9999 and error < .001}
    checks = {'synthetic': fixture(synthetic, 'parity-')}
    if args.audio:
        import soundfile as sf
        from scipy.signal import resample_poly
        with sf.SoundFile(args.audio) as source:
            rate = source.samplerate
            pcm = source.read(min(source.frames, rate*10), dtype='float32', always_2d=True).mean(axis=1)
        if rate != 48000:
            divisor = math.gcd(rate, 48000)
            pcm = resample_poly(pcm, 48000//divisor, rate//divisor).astype(np.float32)
        pcm = pcm[:480000]
        if not len(pcm) or not np.isfinite(pcm).all() or np.max(np.abs(pcm)) <= 1e-8:
            raise RuntimeError('Real reference clip has no finite audible samples')
        pcm *= .95 / np.max(np.abs(pcm))
        checks['real_audio'] = fixture(pcm, 'real-parity-')
        checks['real_audio']['source_sha256'] = hashlib.sha256(args.audio.read_bytes()).hexdigest()
    processor.save_pretrained(root / 'processor')
    report = {'model': MODEL, 'revision': REVISION, 'resources': resources, 'checks': checks,
              'versions': {name: importlib.metadata.version(name) for name in ('torch','transformers','onnx','onnxruntime','numpy','scipy','soundfile')},
              'scope': 'Official vs deployed inference parity, not human listening accuracy.'}
    (root/'export-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2), flush=True)
    if not all(check['passed'] for check in checks.values()):
        raise SystemExit(1)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--audio', type=Path, help='Optional private real clip for the native parity fixture; never committed')
    parser.add_argument('--verify-existing', action='store_true', help='Check existing pinned resources without exporting again')
    main(parser.parse_args())
