"""Merge a completed ESP-IDF build into one image for flashing at address zero."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = root / "build"
dist = root / "dist"
dist.mkdir(exist_ok=True)
args = json.loads((build / "flasher_args.json").read_text())
command = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge-bin",
           "--output", str(dist / "keychain-full.bin")] + args["write_flash_args"]
for offset, name in sorted(args["flash_files"].items(), key=lambda entry: int(entry[0], 0)):
    command += [offset, str(build / name)]
subprocess.run(command, check=True)
image = dist / "keychain-full.bin"
digest = hashlib.sha256(image.read_bytes()).hexdigest()
(dist / "SHA256SUMS.txt").write_text(f"{digest}  keychain-full.bin\n", encoding="ascii")
print(f"Packaged {image.stat().st_size} bytes. SHA-256: {digest}")
