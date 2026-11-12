"""Exercise a compiled BodySlide MCP against isolated synthetic game assets.

python tests/BodySlideMcpSmokeTest.py --bodyslide build/mcp/Release/BodySlide.exe \
    --tests build/mcp/Release/BSOSTests.exe --work-dir build/mcp-smoke
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time
import urllib.request
import uuid
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bodyslide", required=True, type=Path)
    parser.add_argument("--tests", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    root = (args.work_dir / uuid.uuid4().hex[:10]).resolve()
    root.mkdir(parents=True)
    shutil.copy2(args.bodyslide, root / "BodySlide.exe")
    if args.bodyslide.with_suffix(".pdb").exists():
        shutil.copy2(args.bodyslide.with_suffix(".pdb"), root / "BodySlide.pdb")
    shutil.copytree(repo / "res", root / "res")
    env = dict(os.environ, BSOS_MCP_FIXTURE_DIR=str(root))
    subprocess.run([str(args.tests.resolve()), "[.bodyslide-fixture]"], env=env, check=True)
    for folder in ("Data", "Output", "SliderPresets", "SliderGroups"):
        (root / folder).mkdir(exist_ok=True)
    broken = root / "ShapeData/Smoke/broken.nif"
    broken.write_bytes(b"Deliberately invalid NIF for per-outfit failure reporting")
    (root / "SliderPresets/Smoke.xml").write_text(
        '<SliderPresets><Preset name="Smoke Preset" set="Smoke">'
        '<Group name="Smoke Group"/><SetSlider name="Grow" size="both" value="50"/>'
        '</Preset></SliderPresets>', encoding="utf-8")
    (root / "SliderGroups/Smoke.xml").write_text(
        '<SliderGroups><Group name="Smoke Group"><Member name="Smoke"/>'
        '<Member name="Smoke Physics"/></Group></SliderGroups>', encoding="utf-8")
    (root / "BuildSelection.xml").write_text('<BuildSelection/>', encoding="utf-8")
    initial_selection = (root / "BuildSelection.xml").read_bytes()
    config = ET.parse(repo / "Config.xml")
    for key, value in {"TargetGame": "4", "GameDataPath": str(root / "Data"),
                       "OutputDataPath": str(root / "Output"), "ProjectPath": str(root),
                       "WarnMissingGamePath": "false", "LogLevel": "3", "Language": "0"}.items():
        element = config.getroot().find(key)
        if element is None:
            element = ET.SubElement(config.getroot(), key)
        element.text = value
    config.write(root / "Config.xml", encoding="utf-8")
    (root / "BodySlide.xml").write_text(
        '<BodySlideConfig><SelectedOutfit>Smoke</SelectedOutfit><SelectedPreset>Smoke Preset</SelectedPreset>'
        '<BodySlideFrame previewVisible="false"/></BodySlideConfig>', encoding="utf-8")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    proc = subprocess.Popen([str(root / "BodySlide.exe"), "--mcp", "--mcp-port", str(port)],
                            cwd=root, startupinfo=startup)
    session = None
    request_id = 0
    progress = []

    def rpc(method, params):
        nonlocal session, request_id
        request_id += 1
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if session:
            headers.update({"Mcp-Session-Id": session, "MCP-Protocol-Version": "2025-03-26"})
        payload = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}
        req = urllib.request.Request(f"http://127.0.0.1:{port}/mcp", json.dumps(payload).encode(), headers)
        with urllib.request.urlopen(req, timeout=90) as response:
            session = response.headers.get("Mcp-Session-Id", session)
            body = response.read().decode()
            if response.headers.get("Content-Type", "").startswith("text/event-stream"):
                messages = [json.loads(line[5:].strip()) for line in body.splitlines() if line.startswith("data:")]
                progress.extend(m for m in messages if m.get("method") == "notifications/progress")
                message = next(m for m in messages if m.get("id") == request_id)
            else:
                message = json.loads(body)
        assert "error" not in message, message
        return message["result"]

    def call(name, arguments=None, error=False, with_progress=False):
        params = {"name": name, "arguments": arguments or {}}
        if with_progress:
            params["_meta"] = {"progressToken": "smoke-build"}
        result = rpc("tools/call", params)
        assert bool(result.get("isError")) == error, result
        return result.get("structuredContent") or json.loads(result["content"][0]["text"])

    try:
        deadline = time.monotonic() + 45
        while True:
            try:
                rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                                   "clientInfo": {"name": "BodySlideSmoke", "version": "1"}})
                break
            except OSError:
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"BodySlide did not start; inspect {root / 'Log_BS.txt'}")
                time.sleep(0.2)
        time.sleep(0.5)
        tools = {t["name"] for t in rpc("tools/list", {})["tools"]}
        assert {"plan_build", "resolve_conflicts", "build_batch", "get_build_report"} <= tools
        assert call("get_status")["server"] == "bodyslide"
        assert call("list_outfits")["total"] == 3
        assert call("list_outfits", {"filter": "Physics", "limit": 1})["total"] == 1
        assert call("list_presets")["presets"][0]["name"] == "Smoke Preset"
        assert call("list_groups")["groups"][0]["name"] == "Smoke Group"
        call("plan_build", {"outfits": ["Missing"]}, error=True)
        call("plan_build", {"all": True, "preset": "Missing"}, error=True)
        call("plan_build", {"filter": "[", "regex": True}, error=True)
        plan = call("plan_build", {"groups": ["Smoke Group"], "tri": True,
                                   "zaps": {"Smoke Physics": {"HideCorner": True}}})
        assert not plan["ready"] and len(plan["conflicts"]) == 3, plan
        assert not list((root / "Output").rglob("*.nif"))
        call("build_batch", {"planId": plan["planId"]}, error=True)
        call("resolve_conflicts", {"planId": plan["planId"], "choices": {
            "meshes/smoke/body_0.nif": "Smoke", "meshes/smoke/body_1.nif": "Smoke Physics"}}, error=True)
        resolved = call("resolve_conflicts", {"planId": plan["planId"], "prefer": ["Physics"]})
        assert resolved["ready"] and resolved["selected"] == ["Smoke Physics"], resolved
        report = call("build_batch", {"planId": resolved["planId"]}, with_progress=True)
        assert report["status"] == "succeeded" and report["succeeded"] == 1, report
        assert len(report["outfits"][0]["files"]) == 3, report
        assert all(Path(p).stat().st_size > 0 for p in report["outfits"][0]["files"])
        subprocess.run([str(args.tests.resolve()), "[.bodyslide-verify]"], env=env, check=True)
        assert progress, "Expected streamed MCP build progress"
        assert call("get_build_report", {"planId": resolved["planId"]}) == report
        call("build_batch", {"planId": resolved["planId"]}, error=True)
        assert (root / "BuildSelection.xml").read_bytes() == initial_selection

        # A malformed NIF exists and passes the existence preflight, then fails
        # independently of the healthy outfit during the actual engine run.
        plan = call("plan_build", {"outfits": ["Smoke Physics", "Smoke Broken"]})
        assert plan["ready"], plan
        report = call("build_batch", {"planId": plan["planId"]}, error=True)
        assert report["status"] == "partial" and report["succeeded"] == report["failed"] == 1, report
        (root / "smoke-report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

        plan = call("plan_build", {"outfits": ["Smoke"]})
        preset = root / "SliderPresets/Smoke.xml"
        preset.write_text(preset.read_text() + "\n", encoding="utf-8")
        call("build_batch", {"planId": plan["planId"]}, error=True)
        broken.unlink()
        plan = call("plan_build", {"outfits": ["Smoke Broken"]})
        assert not plan["ready"] and plan["errors"], plan
        print(f"PASS: live BodySlide MCP catalog, conflicts, build, morphs, progress, partial failure, stale plans. Artifacts: {root}")
    finally:
        proc.terminate()
        proc.wait(timeout=10)


if __name__ == "__main__":
    main()
