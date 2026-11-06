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
