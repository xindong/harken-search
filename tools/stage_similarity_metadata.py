"""Stage factual dependency metadata for the configured optional similarity bundle."""
from pathlib import Path
import argparse
import json
import re
from generate_third_party_sbom import generate_sbom, write_json

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--onnx-version')
    for name in ('onnx','hnsw','model'):
        parser.add_argument('--'+name, type=int, choices=(0,1), required=True)
    args = parser.parse_args()
    if args.model and not args.onnx:
        parser.error('A bundled CLAP model requires ONNX Runtime')
    if args.onnx_version and (not args.onnx or not re.fullmatch(r'\d+\.\d+\.\d+', args.onnx_version)):
        parser.error('ONNX Runtime version requires an enabled SDK and a numeric major.minor.patch')
    payload = json.loads(args.source.read_text(encoding='utf-8-sig'))
    configured = {'ONNX Runtime': bool(args.onnx), 'hnswlib': bool(args.hnsw),
                  'LAION CLAP HTSAT unfused audio encoder': bool(args.model)}
    for component in payload['components']:
        if component['name'] == 'ONNX Runtime' and args.onnx_version:
            component['version'] = args.onnx_version
            component['source'] = 'https://github.com/microsoft/onnxruntime/tree/v' + args.onnx_version
            component['distribution_paths'] = ['libonnxruntime.' + args.onnx_version + '.dylib']
            component['load_mode'] = 'Bundled macOS CPU inference runtime loaded beside HarkenSearch'
        if component['name'] in configured:
            component['distributed'] = configured.pop(component['name'])
    if configured:
        raise ValueError('Missing similarity components: '+', '.join(configured))
    args.output.mkdir(parents=True, exist_ok=True)
    write_json(args.output/'THIRD_PARTY_COMPONENTS.json', payload)
    write_json(args.output/'third-party-sbom.cdx.json', generate_sbom(payload))

if __name__ == '__main__':
    main()
