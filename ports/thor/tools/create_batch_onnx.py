#!/usr/bin/env python3
"""Build a fixed batch model without changing the per-lane image geometry."""
import argparse
from pathlib import Path

import onnx


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--batch", type=int, default=5)
    args = parser.parse_args()
    args.destination.mkdir(parents=True, exist_ok=True)
    for name in ("MLVCEncoder", "MLVCDecoder", "MLVCReferenceFromFrame"):
        model = onnx.load(args.source / f"{name}.onnx")
        for value in list(model.graph.input) + list(model.graph.output):
            dims = value.type.tensor_type.shape.dim
            if dims[0].dim_value != 1:
                raise ValueError(f"Unexpected source batch: {value.name}")
            dims[0].dim_value = args.batch
        # Inferred intermediate shapes from the original export describe N=1.
        # Drop them and infer again from the new graph boundary.
        del model.graph.value_info[:]
        model = onnx.shape_inference.infer_shapes(model, strict_mode=True)
        onnx.checker.check_model(model)
        onnx.save(model, args.destination / f"{name}.onnx")
        for value in list(model.graph.input) + list(model.graph.output):
            print(name, value.name,
                  [d.dim_value for d in value.type.tensor_type.shape.dim])


if __name__ == "__main__":
    main()
