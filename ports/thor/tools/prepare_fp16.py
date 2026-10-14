"""Export the unchanged BT.709, edge-padded FP16 frame contract."""
import argparse
import hashlib
import json
from pathlib import Path
import cv2
import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('video', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--frames', type=int, default=104)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    capture = cv2.VideoCapture(str(args.video))
    if not capture.isOpened():
        raise RuntimeError('Cannot open input video')
    width, height = int(capture.get(3)), int(capture.get(4))
    if (width, height) != (1920, 1080):
        raise ValueError('This bundle expects 1920x1080 input')
    fps = capture.get(cv2.CAP_PROP_FPS)
    count = 0
    hashes = []
    while count < args.frames:
        ok, frame = capture.read()
        if not ok:
            break
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB).astype(np.float32) / np.float32(255)
        r, g, b = rgb[:, :, 0], rgb[:, :, 1], rgb[:, :, 2]
        y = np.clip(np.float32(.2126)*r + np.float32(.7152)*g + np.float32(.0722)*b, 0, 1)
        u = np.clip(np.float32(.5)*(b-y)/np.float32(1-.0722)+np.float32(.5), 0, 1)
        v = np.clip(np.float32(.5)*(r-y)/np.float32(1-.2126)+np.float32(.5), 0, 1)
        tensor = np.pad(np.stack((y, u, v)), ((0, 0), (0, 8), (0, 0)), mode='edge').astype('<f2')
        path = args.output / f'frame_{count}.fp16'
        if path.exists():
            raise FileExistsError(path)
        tensor.tofile(path)
        hashes.append(hashlib.sha256(tensor.tobytes()).hexdigest())
        count += 1
        if count % 16 == 0:
            print('Exported', count, 'frames', flush=True)
    capture.release()
    (args.output / 'source_info.txt').write_text(f'width={width}\nheight={height}\nfps={fps}\n')
    (args.output / 'frames.json').write_text(json.dumps({'video': str(args.video), 'frames': count,
        'shape': [1, 3, 1088, 1920], 'dtype': 'float16', 'colorspace': 'bt709-yuv444-full',
        'padding': 'edge', 'sha256': hashes}, indent=2))
    print('FP16 export complete:', count, flush=True)


if __name__ == '__main__':
    main()
