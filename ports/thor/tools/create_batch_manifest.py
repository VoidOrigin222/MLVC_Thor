"""Audit a batch engine bundle while retaining the N=1 stream adapter contract."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("workspace", type=Path)
    args = p.parse_args()
    root = args.workspace.resolve()
    manifest = json.loads((root / "models/manifest_thor.json").read_text())
    manifest["execution_batch_size"] = 5
    manifest["execution_adapter"] = "thor_batch_stage_n1_to_n5"
    shapes = {"batch_size": 5, "logical_stream_batch": 1, "models": {}}
    for record in manifest["models"]:
        name = record["name"]
        file = root / "engines/batch5" / (name + ".engine")
        if file.stat().st_size == 0:
            raise ValueError("Incomplete engine: " + str(file))
        record.update(file="../engines/batch5/" + file.name,
                      bytes=file.stat().st_size,
                      sha256=hashlib.sha256(file.read_bytes()).hexdigest())
        shapes["models"][name] = {}
        for direction in ("inputs", "outputs"):
            shapes["models"][name][direction] = [
                {**s, "shape": [5] + s["shape"][1:]} for s in record[direction]]
        onnx = root / "models/batch5" / (name + ".onnx")
        manifest["runtime_artifacts"].append({
            "name": "batch5-" + onnx.name,
            "file": "batch5/" + onnx.name,
            "bytes": onnx.stat().st_size,
            "sha256": hashlib.sha256(onnx.read_bytes()).hexdigest()})
    metadata = root / "models/batch5/execution.json"
    metadata.write_text(json.dumps(shapes, indent=2) + "\n")
    manifest["runtime_artifacts"].append({
        "name": "batch5-execution.json", "file": "batch5/execution.json",
        "bytes": metadata.stat().st_size,
        "sha256": hashlib.sha256(metadata.read_bytes()).hexdigest()})
    target = root / "models/manifest_thor_batch5.json"
    target.write_text(json.dumps(manifest, indent=2) + "\n")
    print(target)


if __name__ == "__main__":
    main()
