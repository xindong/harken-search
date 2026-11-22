"""Stage hash-pinned ONNX Runtime and CLAP resources for release builds."""
import argparse
import hashlib
import shutil
import urllib.request
import zipfile
from pathlib import Path

SDK_URL = 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.20.1/microsoft.ml.onnxruntime.1.20.1.nupkg'
SDK_SHA256 = '9c32108e9a1c78b090a95c2a219f244dd5f067d4caf68fb0f967103f3f20839c'
SDK_SIZE = 108611147
SDK_ROOT_NAME = 'onnxruntime-win-x64-1.20.1'
PINNED = {
    'onnxruntime.dll': '4cb41e89b8bf30578e1dd95e9c40292d61974a4bfcd666409302c4f0c5aa8ce0',
    'clap-audio.onnx': '50d03fb781f7bf520a5d424a3479871cfca4af7a31026892ac1e935d5303f677',
    'clap-mel-filters.f64': '256b8c586fb0f0ea44c6b2cafa44caa9bf288c1cfe1a05ff2527b73ae123d37a',
    'clap-window.f64': '0d9c0e8dfb7e477785eaaac6e2082e19a030bc113178a207cb4d1ab8d536a7a9',
}
SDK_FILE_SHA256 = {
    'include/onnxruntime_cxx_api.h': 'fa7f64d7717a0d57bf8fe65d55b80038a476dae2132bd4b9c2eb6119490ef445',
    'include/onnxruntime_cxx_inline.h': '0c53f13b655830c381b559a6841eab2e8e6682d17191844c813eb58834a2be47',
    'include/onnxruntime_float16.h': 'f6885fe69ca80eb9c46f2f0b951138bbef81646407705fdf2d6c23aa50ce632e',
    'include/onnxruntime_lite_custom_op.h': '4748811e61bf5f11a58f2c11fd0b05b409651bd81fee58c039a32ec8933c477c',
    'include/onnxruntime_run_options_config_keys.h': 'c990d43696b316f51a6b5b0caf241adce5f1e9de3d63e13f1ffca2c615286a9b',
    'include/onnxruntime_session_options_config_keys.h': '673fa5d83d69ddd8d6da4b25a87d5d56d48325ca1248e5741150b65760b604cb',
    'include/onnxruntime_c_api.h': 'b0176725a7cc6d44c9de973330422433684ae3144e6b300ec2df755a2e6bcf64',
    'include/provider_options.h': '74f8accd5b30fbfe8abb24ca279e634657b446bf5b9be520badaaebcea63cade',
    'include/cpu_provider_factory.h': 'e2f658eeace78d79a8df82f233dcdeea135db498a77766156054bfc0d1d06c21',
    'lib/onnxruntime.dll': '4cb41e89b8bf30578e1dd95e9c40292d61974a4bfcd666409302c4f0c5aa8ce0',
    'lib/onnxruntime.lib': 'c5a4f0b1af1e8f1edd11391e1a15a7d52832d94a1a67cf6b8f22ed29f863a06c',
    'LICENSE': 'c250d6278f0b47a6439fb7592b08b58a55eb9f535aa49a1db63211c3f982b674',
    'ThirdPartyNotices.txt': 'fac5bb85f568b38fd8f3a6aea32737985841f23a1a43e9ce952f79474f883aa9',
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


def _safe_target(output, name):
    target = (output / name).resolve()
    if not target.is_relative_to(output.resolve()):
        raise ValueError(f'unsafe SDK archive entry: {name}')
    return target


def _stage_sdk_package(package, output):
    sdk_root = output / SDK_ROOT_NAME
    names = {item.filename for item in package.infolist()}
    nuget_layout = {
        'build/native/include/onnxruntime_c_api.h',
        'runtimes/win-x64/native/onnxruntime.dll',
        'runtimes/win-x64/native/onnxruntime.lib',
    } <= names
    if nuget_layout:
        for item in package.infolist():
            if item.is_dir():
                continue
            if item.filename.startswith('build/native/include/'):
                relative = Path(item.filename).relative_to('build/native/include')
                target = _safe_target(sdk_root / 'include', relative)
            elif item.filename == 'runtimes/win-x64/native/onnxruntime.dll':
                target = _safe_target(sdk_root / 'lib', 'onnxruntime.dll')
            elif item.filename == 'runtimes/win-x64/native/onnxruntime.lib':
                target = _safe_target(sdk_root / 'lib', 'onnxruntime.lib')
            elif item.filename in {'LICENSE', 'ThirdPartyNotices.txt'}:
                target = _safe_target(sdk_root, item.filename)
            else:
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with package.open(item) as source, target.open('wb') as destination:
                shutil.copyfileobj(source, destination)
        return sdk_root

    for item in package.infolist():
        _safe_target(output, item.filename)
    package.extractall(output)
    return sdk_root


def stage(output, model_source_dir=None, model_base_url=None, sdk_archive=None):
    if bool(model_source_dir) == bool(model_base_url):
        raise ValueError('provide exactly one model source')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    archive = output / 'onnxruntime-win-x64-1.20.1.nupkg'
    if sdk_archive:
        shutil.copyfile(sdk_archive, archive)
    else:
        fetch(SDK_URL, archive)
    if archive.stat().st_size != SDK_SIZE:
        raise ValueError(f'wrong SDK package size: {archive}: {archive.stat().st_size}')
    verify(archive, SDK_SHA256)
    with zipfile.ZipFile(archive) as package:
        sdk_root = _stage_sdk_package(package, output)
    for path in (sdk_root / 'include', sdk_root / 'lib' / 'onnxruntime.lib', sdk_root / 'lib' / 'onnxruntime.dll', sdk_root / 'LICENSE', sdk_root / 'ThirdPartyNotices.txt'):
        if not path.exists():
            raise ValueError(f'SDK member missing: {path}')
    for relative, expected in SDK_FILE_SHA256.items():
        verify(sdk_root / relative, expected)
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
