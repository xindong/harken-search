"""Stage hash-pinned ONNX Runtime and CLAP resources for release builds."""
import argparse
import hashlib
import shutil
import urllib.request
import zipfile
from pathlib import Path

SDK_URL = 'https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip'
SDK_SHA256 = '78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581'
PINNED = {
    'onnxruntime.dll': '4cb41e89b8bf30578e1dd95e9c40292d61974a4bfcd666409302c4f0c5aa8ce0',
    'clap-audio.onnx': '50d03fb781f7bf520a5d424a3479871cfca4af7a31026892ac1e935d5303f677',
    'clap-mel-filters.f64': '256b8c586fb0f0ea44c6b2cafa44caa9bf288c1cfe1a05ff2527b73ae123d37a',
    'clap-window.f64': '0d9c0e8dfb7e477785eaaac6e2082e19a030bc113178a207cb4d1ab8d536a7a9',
}

def verify(path, expected):
    with path.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    if digest != expected:
        raise ValueError(f'wrong SHA256: {path}: {digest}')

def fetch(url, target):
    if not url.startswith('https://'):
        raise ValueError(f'HTTPS required: {url}')
    urllib.request.urlretrieve(url, target)

def stage(output, model_source_dir=None, model_base_url=None, sdk_archive=None):
    if bool(model_source_dir) == bool(model_base_url):
        raise ValueError('provide exactly one model source')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    archive = output / 'onnxruntime-win-x64-1.20.1.zip'
    if sdk_archive:
        shutil.copyfile(sdk_archive, archive)
    else:
        fetch(SDK_URL, archive)
    verify(archive, SDK_SHA256)
    sdk_root = output / 'onnxruntime-win-x64-1.20.1'
    with zipfile.ZipFile(archive) as package:
        for item in package.infolist():
            target = (output / item.filename).resolve()
            if not target.is_relative_to(output.resolve()):
                raise ValueError(f'unsafe SDK archive entry: {item.filename}')
        package.extractall(output)
    for path in (sdk_root / 'include', sdk_root / 'lib' / 'onnxruntime.lib', sdk_root / 'lib' / 'onnxruntime.dll', sdk_root / 'LICENSE', sdk_root / 'ThirdPartyNotices.txt'):
        if not path.exists():
            raise ValueError(f'SDK member missing: {path}')
    verify(sdk_root / 'lib' / 'onnxruntime.dll', PINNED['onnxruntime.dll'])
    model_dir = output / 'models' / 'clap'
    model_dir.mkdir(parents=True, exist_ok=True)
    if model_base_url and not model_base_url.startswith('https://'):
        raise ValueError('model base URL must use HTTPS')
    for name in ('clap-audio.onnx', 'clap-mel-filters.f64', 'clap-window.f64'):
        target = model_dir / name
        if model_source_dir:
            shutil.copyfile(Path(model_source_dir) / name, target)
        else:
            fetch(model_base_url.rstrip('/') + '/' + name, target)
        verify(target, PINNED[name])
    return sdk_root, model_dir

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    sources = parser.add_mutually_exclusive_group(required=True)
    sources.add_argument('--model-source-dir')
    sources.add_argument('--model-base-url')
    parser.add_argument('--sdk-archive')
    args = parser.parse_args()
    sdk, model = stage(args.output, args.model_source_dir, args.model_base_url, args.sdk_archive)
    print(f'ONNX SDK: {sdk}; CLAP model: {model}')
