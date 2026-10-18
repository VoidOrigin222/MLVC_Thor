#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
for app in bin/encoder bin/decoder bin/mlvc_encode_thor bin/mlvc_decode_thor bin/mlvc_multi_thor; do
  [[ -x "$app" ]] || { echo "Missing executable: $app" >&2; exit 1; }
  result=$(ldd "$app")
  if [[ "$result" == *'not found'* ]]; then echo "$result" >&2; exit 1; fi
done
python3 - <<'PY'
import ctypes,ctypes.util,hashlib,json
from pathlib import Path
root=Path('models')
for name in ('manifest_thor.json', 'manifest_thor_batch5.json'):
    manifest=json.loads((root/name).read_text())
    assert manifest['runtime']=='tensorrt'
    for record in manifest['models']+[manifest['sidecar']]+manifest['runtime_artifacts']:
        path=root/record['file']
        assert path.stat().st_size==record['bytes'],f'Wrong size: {path}'
        assert hashlib.sha256(path.read_bytes()).hexdigest()==record['sha256'],f'Wrong hash: {path}'
print('Thor runtime and model bundle: OK')
ctypes.CDLL('libnvcuvid.so.1')
codec=ctypes.CDLL(ctypes.util.find_library('avcodec'))
codec.avcodec_find_decoder_by_name.argtypes=[ctypes.c_char_p]
codec.avcodec_find_decoder_by_name.restype=ctypes.c_void_p
for name in (b'h264_cuvid',b'hevc_cuvid'):
    assert codec.avcodec_find_decoder_by_name(name),f'FFmpeg NVDEC decoder missing: {name.decode()}'
codec.avcodec_find_encoder_by_name.argtypes=[ctypes.c_char_p]
codec.avcodec_find_encoder_by_name.restype=ctypes.c_void_p
assert codec.avcodec_find_encoder_by_name(b'h264_nvenc'),'FFmpeg NVENC encoder missing'
print('NVIDIA NVDEC driver and FFmpeg H.264/HEVC decoders: OK')
PY
