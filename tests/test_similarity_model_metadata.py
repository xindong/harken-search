from pathlib import Path
import json
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]

def test_staged_similarity_metadata_matches_configured_bundle(tmp_path):
    for enabled in (0, 1):
        target = tmp_path / str(enabled)
        result = subprocess.run([sys.executable, str(ROOT/'tools/stage_similarity_metadata.py'),
            '--source', str(ROOT/'THIRD_PARTY_COMPONENTS.json'), '--output', str(target),
            '--onnx', str(enabled), '--hnsw', str(enabled), '--model', str(enabled)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        manifest = json.loads((target/'THIRD_PARTY_COMPONENTS.json').read_text(encoding='utf-8'))
        names = {'ONNX Runtime','hnswlib','LAION CLAP HTSAT unfused audio encoder'}
        assert all(c['distributed'] == bool(enabled) for c in manifest['components'] if c['name'] in names)
        sbom = json.loads((target/'third-party-sbom.cdx.json').read_text(encoding='utf-8'))
        assert names <= {c['name'] for c in sbom['components']}


def test_alternate_macos_sdk_version_is_reported_in_manifest_and_sbom(tmp_path):
    result = subprocess.run([sys.executable, str(ROOT/'tools/stage_similarity_metadata.py'),
        '--source', str(ROOT/'THIRD_PARTY_COMPONENTS.json'), '--output', str(tmp_path),
        '--onnx', '1', '--hnsw', '0', '--model', '1', '--onnx-version', '1.16.3'],
        capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    manifest = json.loads((tmp_path/'THIRD_PARTY_COMPONENTS.json').read_text(encoding='utf-8'))
    runtime = next(component for component in manifest['components'] if component['name'] == 'ONNX Runtime')
    assert runtime['version'] == '1.16.3'
    assert runtime['source'].endswith('/v1.16.3')
    assert runtime['distribution_paths'] == ['libonnxruntime.1.16.3.dylib']
    sbom = json.loads((tmp_path/'third-party-sbom.cdx.json').read_text(encoding='utf-8'))
    runtime = next(component for component in sbom['components'] if component['name'] == 'ONNX Runtime')
    assert runtime['version'] == '1.16.3'


def test_sdk_version_override_rejects_disabled_or_malformed_versions(tmp_path):
    for enabled, version in [('0', '1.16.3'), ('1', '../1.16.3')]:
        output = tmp_path/enabled
        result = subprocess.run([sys.executable, str(ROOT/'tools/stage_similarity_metadata.py'),
            '--source', str(ROOT/'THIRD_PARTY_COMPONENTS.json'), '--output', str(output),
            '--onnx', enabled, '--hnsw', '0', '--model', '0', '--onnx-version', version],
            capture_output=True, text=True)
        assert result.returncode != 0
        assert not output.exists()
