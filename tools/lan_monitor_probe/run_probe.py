"""Compare one SDK's two LAN targets with isolated SDK workers, using saved Orca pairing."""

import argparse
import datetime
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time


PREFIX = "ORCA_LAN_MONITOR "
ROOT = Path(__file__).resolve().parents[2]


def read_config(path):
    # AppConfig appends a checksum comment after the JSON document.
    return json.JSONDecoder().raw_decode(path.read_text(encoding="utf-8-sig"))[0]


def paired_printers(config):
    printers = []
    for device_id, saved in config.get("local_machines", {}).items():
        if saved.get("printer_type") not in {"C11", "C12"}:
            continue
        code = (saved.get("access_code") or config.get("access_code", {}).get(device_id)
                or config.get("user_access_code", {}).get(device_id))
        if not saved.get("dev_ip") or not code:
            continue
        printers.append({"id": device_id, "host": saved["dev_ip"], "code": code,
                         "name": saved.get("dev_name", "P1 printer")})
    if len(printers) != 2:
        raise ValueError("The diagnostic requires exactly two paired P1 LAN printers")
    return printers


def selection(config):
    result = {key: value for key, value in config.items() if "selected_machine" in key}
    result["app"] = {key: value for key, value in config.get("app", {}).items()
                     if "selected_machine" in key}
    return result


def start_worker(executable, settings, events, index, generation=0):
    process = subprocess.Popen(
        [str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace",
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )

    def receive():
        try:
            for line in process.stdout:
                if line.startswith(PREFIX):
                    try:
                        event = json.loads(line[len(PREFIX):])
                        if isinstance(event, dict) and event.get("kind") in {
                            "start", "connection", "connect_request", "sample", "finished", "error"
                        }:
                            events.put((index, generation, event))
                    except json.JSONDecodeError:
                        pass
        finally:
            events.put((index, generation, {"kind": "exited"}))

    threading.Thread(target=receive, daemon=True).start()
    # Pairing credentials travel only through the private stdin pipe, never command arguments.
    process.stdin.write(json.dumps(settings) + "\n")
    if settings["seconds"] != 0:
        process.stdin.close()
    else:
        process.stdin.flush()
    return process


def run(args):
    data = Path(args.data_dir).resolve()
    config_path = data / "OrcaSlicer.conf"
    config = read_config(config_path)
    printers = paired_printers(config)
    sdk = args.sdk_version or config.get("app", {}).get("network_plugin_version")
    if not sdk:
        raise ValueError("No configured networking module version")
    if os.name == "nt":
        module = data / "plugins" / f"bambu_networking_{sdk}.dll"
    else:
        suffix = "dylib" if sys.platform == "darwin" else "so"
        module = data / "plugins" / f"libbambu_networking_{sdk}.{suffix}"
    if not module.is_file():
        raise ValueError("The selected installed networking module is missing")
    executable = Path(args.worker).resolve()
    if not executable.is_file():
        raise ValueError("Build lan_monitor_worker in Release before running the diagnostic")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output = ROOT / "build" / "lan-monitor-probe" / f"{args.mode}-{stamp}"
    output.mkdir(parents=True)
    groups = [printers] if args.mode == "shared" else [[printer] for printer in printers]
    events = queue.Queue()
    processes = []
    settings_list = []
    generations = [0] * len(groups)
    restarts = []
    records = []
    latest = {}
    finished = set()
    before = selection(config)
    began = time.monotonic()
    try:
        for index, group in enumerate(groups):
            directory = output / f"worker-{index}"
            directory.mkdir()
            for printer in group:
                source = data / "printers" / printer["id"]
                if source.is_dir():
                    for certificate in source.rglob("*.cer"):
                        target = directory / "printers" / printer["id"] / certificate.relative_to(source)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(certificate, target)
            settings = {"module": str(module), "directory": str(directory),
                        "certificate": str(ROOT / "resources" / "cert"),
                        "country": config.get("app", {}).get("country_code", "US"),
                        "printers": group, "seconds": 0 if args.application_mode else args.seconds,
                        "second_delay": args.second_delay}
            settings_list.append(settings)
            processes.append(start_worker(executable, settings, events, index))
        next_progress = 0
        stop_sent = False
        while len(finished) < len(processes):
            elapsed = time.monotonic() - began
            if elapsed > args.seconds + 20:
                break
            if args.application_mode and elapsed >= args.seconds and not stop_sent:
                for process in processes:
                    process.stdin.write('{"command":"stop"}\n')
                    process.stdin.flush()
                stop_sent = True
            if args.restart_after is not None and not restarts and elapsed >= args.restart_after:
                peer = latest.get((1, 0), {})
                restarts.append({"elapsed_seconds": int(elapsed), "peer_reports_before": peer.get("reports", 0)})
                processes[0].terminate()
                processes[0].wait(timeout=3)
                generations[0] += 1
                finished.discard(0)
                latest.pop((0, 0), None)
                settings_list[0]["seconds"] = max(1, args.seconds - int(elapsed))
                processes[0] = start_worker(executable, settings_list[0], events, 0, generations[0])
                print(json.dumps({"kind": "worker_restarted", "worker": 0}), flush=True)
            try:
                worker, generation, event = events.get(timeout=1)
            except queue.Empty:
                continue
            if generation != generations[worker]:
                continue
            event["worker"] = worker
            event["generation"] = generation
            records.append(event)
            if event["kind"] == "sample":
                latest[(worker, event["target"])] = event
            elif event["kind"] == "exited":
                finished.add(worker)
            elif event["kind"] in {"start", "connect_request", "connection", "error"}:
                print(json.dumps(event), flush=True)
            if elapsed >= next_progress:
                print(json.dumps({"elapsed_seconds": int(elapsed), "samples": [
                    {"worker": key[0], "target": key[1], "reports": value["reports"],
                     "printer": groups[key[0]][key[1]]["name"], "status": value["status"],
                     "connection": value["connection"], "age_seconds": value.get("age_seconds")}
                    for key, value in latest.items()]}), flush=True)
                next_progress = elapsed + 15
    finally:
        for process in processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    final = [{"worker": key[0], "target": key[1], **value} for key, value in sorted(latest.items())]
    selection_unchanged = selection(read_config(config_path)) == before
    isolation_passed = (latest.get((1, 0), {}).get("reports", 0) > restarts[0]["peer_reports_before"]
                        if restarts else None)
    freshness_passed = all(event.get("age_seconds", 0) <= 30 for event in records
                           if event["kind"] == "sample" and event["reports"] > 0)
    passed = (len(final) == 2 and len(finished) == len(processes) and selection_unchanged
              and freshness_passed and (not restarts or isolation_passed)
              and all(process.returncode == 0 for process in processes)
              and all(value["reports"] > 0 and value["connection"] == 0
                      and value["status"].get("gcode_state")
                      and value.get("age_seconds", 31) <= 30 for value in final))
    result = {"mode": args.mode, "sdk_requested": sdk, "duration_seconds": args.seconds,
              "passed": passed, "selection_unchanged": selection_unchanged,
              "freshness_passed": freshness_passed,
              "restart_isolation_passed": isolation_passed, "restarts": restarts,
              "worker_exit_codes": [process.returncode for process in processes],
              "final": final, "records": records}
    report = output / "result.json"
    report.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"passed": passed, "selection_unchanged": result["selection_unchanged"],
                      "worker_exit_codes": result["worker_exit_codes"], "report": str(report)}), flush=True)
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=["shared", "workers"], required=True)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--second-delay", type=int, default=20)
    parser.add_argument("--sdk-version")
    parser.add_argument("--application-mode", action="store_true", help="Keep stdin open and stop workers through the application protocol")
    parser.add_argument("--restart-after", type=int, help="Restart worker 0 to verify failure isolation")
    parser.add_argument("--data-dir", default=str(Path(os.environ.get("APPDATA", "~")) / "OrcaSlicer"))
    parser.add_argument("--worker", default=str(ROOT / "build" / "lan-monitor-probe" / "Release" / "lan_monitor_worker.exe"))
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or (args.mode == "shared" and not 0 <= args.second_delay < args.seconds):
        parser.error("duration must be 1..3600 seconds, with second-delay less than duration")
    if args.restart_after is not None and (args.mode != "workers" or not 1 <= args.restart_after < args.seconds - 10):
        parser.error("restart-after requires workers mode and at least ten seconds to reconnect")
    try:
        return run(args)
    except (OSError, ValueError, KeyError):
        print("Cannot read paired printers, prepare SDK files, or start the diagnostic", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
