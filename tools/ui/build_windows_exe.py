"""Build a one-file Windows launcher; MediaMTX remains an external service."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output", type=Path, default=REPO / "artifacts/ui-exe/dist"
    )
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("需要在 Windows 上构建")
    output = args.output.resolve()
    if (output / "SemanticFiveMonitor.exe").exists():
        parser.error("输出目录已有程序，请选择新的 --output 目录")
    work = REPO / "artifacts/ui-exe/build"
    spec = REPO / "artifacts/ui-exe/spec"
    env = os.environ.copy()
    conda = Path(sys.base_prefix) / "Library/bin"
    command = [
        sys.executable,
        "-m",
        "PyInstaller",
        "--onefile",
        "--windowed",
        "--noupx",
        "--name",
        "SemanticFiveMonitor",
        "--distpath",
        str(output),
        "--workpath",
        str(work),
        "--specpath",
        str(spec),
    ]
    if conda.is_dir():
        env["PATH"] = str(conda) + os.pathsep + env.get("PATH", "")
        for name in ("tcl86t.dll", "tk86t.dll"):
            if (conda / name).exists():
                command += ["--add-binary", str(conda / name) + ";."]
    for name in ("index.html", "app.js", "styles.css", "ui_config.json"):
        command += ["--add-data", str(ROOT / name) + ";."]
    command.append(str(ROOT / "launch_ui.py"))
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    shutil.copy2(ROOT / "ui_config.json", output / "ui_config.json")
    shutil.copy2(ROOT / "README.md", output / "README.md")
    exe = output / "SemanticFiveMonitor.exe"
    metadata = {
        "executable": exe.name,
        "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "bytes": exe.stat().st_size,
        "files": {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in ROOT.iterdir()
            if p.suffix in (".py", ".js", ".css", ".html", ".json")
        },
    }
    (output / "BUILD_INFO.json").write_text(
        json.dumps(metadata, indent=2), encoding="utf8"
    )
    print(exe)


if __name__ == "__main__":
    main()
