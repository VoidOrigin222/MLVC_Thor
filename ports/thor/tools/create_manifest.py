"""Create an integrity-checked engine bundle from the audited ONNX bundle."""
import argparse
import copy
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('workspace', type=Path)
    args = parser.parse_args()
    root = args.workspace.resolve()
    source = root / 'models/onnx'
    manifest = copy.deepcopy(json.loads((source / 'manifest_motion_ascend310p1.json').read_text()))
    manifest.update(runtime='tensorrt', soc_version='NVIDIA-Thor')
    manifest['sidecar']['file'] = 'onnx/' + manifest['sidecar']['file']
    for record in manifest['runtime_artifacts']:
        record['file'] = 'onnx/' + record['file']
    for record in manifest['models']:
        name = record['name']
        engine = root / 'engines' / (name + '.engine')
        record.update(file='../engines/' + engine.name, backend='tensorrt',
                      bytes=engine.stat().st_size, sha256=hashlib.sha256(engine.read_bytes()).hexdigest(),
                      onnx_file='onnx/' + name + '.onnx')
        onnx = source / (name + '.onnx')
        manifest['runtime_artifacts'].append({'name': onnx.name, 'file': 'onnx/' + onnx.name,
            'bytes': onnx.stat().st_size, 'sha256': hashlib.sha256(onnx.read_bytes()).hexdigest()})
    target = root / 'models' / 'manifest_thor.json'
    target.write_text(json.dumps(manifest, indent=2) + '\n')
    print(target)


if __name__ == '__main__':
    main()
