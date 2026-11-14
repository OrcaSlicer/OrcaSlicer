"""Read frames from both paired P1 cameras without changing Device selection."""
import json
import argparse
import os
from pathlib import Path
import subprocess
from run_probe import paired_printers, read_config, selection, ROOT

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--worker", type=Path, default=ROOT / "build/lan-monitor-dev/Release/lan_camera_probe.exe")
parser.add_argument("--seconds", type=int, choices=range(1, 181), default=30)
args = parser.parse_args()
data = Path(os.environ["APPDATA"]) / "OrcaSlicer"
config_path = data / "OrcaSlicer.conf"
config = read_config(config_path)
output = ROOT / "build" / "lan-camera-probe"
output.mkdir(exist_ok=True)
settings = {"module": str(data / "plugins" / "BambuSource.dll"), "seconds": args.seconds,
            "printers": paired_printers(config), "output": str(output)}
process = subprocess.run([str(args.worker.resolve())], cwd=ROOT / "build/src/Release",
                         input=json.dumps(settings) + "\n", capture_output=True,
                         text=True, timeout=args.seconds + 20, creationflags=subprocess.CREATE_NO_WINDOW)
for line in process.stdout.splitlines():
    if line.startswith("ORCA_CAMERA_PROBE "):
        try:
            (output / "result.json").write_text(json.dumps(json.loads(line[len("ORCA_CAMERA_PROBE "):]), indent=2), encoding="utf-8")
        except json.JSONDecodeError:
            pass
        print(line)
print(json.dumps({"exit_code": process.returncode,
                  "device_selection_unchanged": selection(config) == selection(read_config(config_path))}))
raise SystemExit(process.returncode)
