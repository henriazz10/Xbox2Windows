"""Exercise slot 86 on WARP, then translate the request with the offline CLI."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/startup"))
import gfx2hlsl


def container(stage):
    # One straight-line export (pos0 or mrt0) followed by s_endpgm and section padding.
    target = 12 if stage == 1 else 0
    code = struct.pack("<III", 0xf800080f | target << 4, 0, 0xbf810000)
    image = bytearray(312)
    image[:4] = b"FSSM"
    struct.pack_into("<I", image, 4, 0x101)
    struct.pack_into("<QQ", image, 192, 248, 64)
    struct.pack_into("<Q", image, 232, 312)
    image[288:] = code + struct.pack("<3I", *([0xbf9f0000] * 3))
    native = b"XBCH" + bytes(12) + struct.pack("<II", len(image), 0) + zlib.compress(image)
    program = (struct.pack("<HH", 0, stage) + bytes(20) + b"XDAT"
               + struct.pack("<5I", 1, 0, 13, 48, len(native)) + native)
    section = b"DXIL" + struct.pack("<I", len(program)) + program
    return b"DXBC" + bytes(16) + struct.pack("<4I", 1, 36 + len(section), 1, 36) + section


def run(directory, consumer):
    cache = directory / "cache"
    cache.mkdir()
    vs, ps = container(1), container(0)
    for name, data in (("vertex", vs), ("pixel", ps)):
        (directory / name).write_bytes(data)
    root = directory / "root"
    args = [consumer, directory / "vertex", directory / "pixel", root]
    env = dict(os.environ, X2W_D3D12_PERMISSIVE="1", X2W_D3D12_RENDER="1", X2W_SHADER_CACHE=str(cache))
    subprocess.run(args, env=dict(env, X2W_TEST_TRUNCATED_ONLY="1"), check=True)
    assert not list(cache.iterdir()), "truncated stream wrote a request"
    subprocess.run(args, env=env, check=True)
    key = "%016x" % gfx2hlsl.pipeline_key(vs, ps, root.read_bytes())
    name = "req_" + key
    for ext, want in ((".a", vs), (".b", ps), (".root", root.read_bytes())):
        assert (cache / (name + ext)).read_bytes() == want

    def translate():
        subprocess.run([sys.executable, "-B", gfx2hlsl.__file__, "--pending", cache], check=True)

    translate()
    assert "targets 0" in (cache / (key + ".txt")).read_text()
    for stage in ("vs", "ps"):
        assert (cache / f"{key}.{stage}.cso").read_bytes()[:4] == b"DXBC"
    replay = subprocess.run(args, env=env, check=True, capture_output=True, text=True)
    assert f"pipeline {key} translated:" in replay.stderr, replay.stderr
    # Swap the containers: their explicit stages no longer agree with the stream slots.
    bad = "%016x" % gfx2hlsl.pipeline_key(ps, vs, root.read_bytes())
    for ext, data in ((".a", ps), (".b", vs), (".root", root.read_bytes())):
        (cache / ("req_" + bad + ext)).write_bytes(data)
    translate()
    assert "Unexpected shader stage" in (cache / (bad + ".fail")).read_text()
    assert not (cache / (bad + ".txt")).exists()
    print("WARP stream request, stage order, compiled VS/PS, truncated and swapped controls passed")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        run(Path(temporary), sys.argv[1])
