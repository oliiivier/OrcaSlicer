#!/usr/bin/env python3
"""Test bench of the slicer's MCP server (fork feature, see src/slic3r/AIAssistant/PLAN.md).

Starts a Debug build compiled with -DSNAPMAKER_AI_TEST_HOOKS=ON on a disposable copy of the
user's configuration (dedicated port, external MCP access on, printer access off, notes and
experiences in the copy), plays the JSON scenarios of scripts/fork/mcp-tests/, stops the
instance and writes a report. The test mode answers every confirmation by itself
(SNAPMAKER_AI_TEST_CONFIRM): one instance is started per answer the scenarios need.

    PATH=$HOME/.venvs/orca-build/bin:$PATH cmake --build build --config Debug --target Snapmaker_Orca
    python3 -I scripts/fork/mcp-test.py                 # every scenario without tag
    python3 -I scripts/fork/mcp-test.py objects plates  # some scenarios, by file name
    python3 -I scripts/fork/mcp-test.py --tags printer  # also the scenarios needing a real printer
    python3 -I scripts/fork/mcp-test.py 90-printer --tags printer  # scenario names before --tags

Scenario file: { "description", "confirm": "yes" | "no", "tags": [...], "steps": [step...] }.
A step: { "call": tool, "args": {...}, "expect": {...}, "error": bool, "error_contains": text,
"image": bool, "capture": {"NAME": "dotted.path.0"}, "note": text }. `expect` is a subset of the
JSON result: objects match key by key, an expected list matches when each of its items matches
some item of the result, "$any" matches any value. `capture` keeps values of the result (keys and
list indices) for the next steps as ${NAME}; ${TMP} and ${MODELS} are the run's temporary folder
and its copy of resources/handy_models.
"""

import argparse
import json
import os
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCENARIOS = REPO / "scripts" / "fork" / "mcp-tests"
RETRY_TEXT = "The slicer is still updating"


def gpu_environment():
    """The environment of the packaged launcher: Zink for NVIDIA on Wayland."""
    env = {}
    if os.environ.get("XDG_SESSION_TYPE") == "wayland" and shutil.which("nvidia-smi"):
        env.update({
            "__GLX_VENDOR_LIBRARY_NAME": "mesa",
            "__EGL_VENDOR_LIBRARY_FILENAMES": "/usr/share/glvnd/egl_vendor.d/50_mesa.json",
            "MESA_LOADER_DRIVER_OVERRIDE": "zink",
            "GALLIUM_DRIVER": "zink",
            "WEBKIT_DISABLE_DMABUF_RENDERER": "1",
        })
    return env


def make_data_dir(source, target, port, token, printer):
    shutil.copytree(source, target, ignore=shutil.ignore_patterns("log", "cache", "user_backup-*"))
    conf_path = target / "Snapmaker_Orca.conf"
    conf = json.loads(conf_path.read_text(encoding="utf-8"))
    conf.setdefault("app", {})["single_instance"] = False
    conf["ai_assistant"] = dict(conf.get("ai_assistant", {}), **{
        "mcp_enabled": True,
        "mcp_port": str(port),
        "mcp_external_access": True,
        "mcp_printer_access": printer,
        "mcp_printer_control": False,
        "auto_connect_printer": printer,
        "suggest_adaptation": False,
        "notes_dir": str(target / "test_notes"),
        "notes_writable": True,
    })
    conf_path.write_text(json.dumps(conf, indent=4), encoding="utf-8")
    (target / "test_notes").mkdir(exist_ok=True)
    token_file = target / "ai_assistant" / "mcp_token"
    token_file.parent.mkdir(exist_ok=True)
    token_file.write_text(token)
    token_file.chmod(0o600)


class Instance:
    def __init__(self, binary, data_dir, port, token, confirm, log):
        self.url = f"http://127.0.0.1:{port}/mcp"
        self.token = token
        env = dict(os.environ, **gpu_environment(), SNAPMAKER_AI_TEST_CONFIRM=confirm, LC_ALL="C")
        self.process = subprocess.Popen([str(binary), "--datadir", str(data_dir)], env=env, stdout=log,
                                        stderr=subprocess.STDOUT, start_new_session=True)
        self.next_id = 0

    def rpc(self, method, params=None, timeout=60):
        self.next_id += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}}).encode()
        request = urllib.request.Request(self.url, data=body, method="POST", headers={
            "Authorization": f"Bearer {self.token}",
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        })
        with urllib.request.urlopen(request, timeout=timeout) as response:
            reply = json.loads(response.read().decode())
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply["result"]

    def wait_ready(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"the slicer exited with code {self.process.returncode}")
            try:
                self.rpc("ping", timeout=5)
                return
            except (urllib.error.URLError, ConnectionError, OSError):
                time.sleep(2)
        raise RuntimeError(f"the MCP server did not answer within {timeout} s")

    def stop(self):
        if self.process.poll() is not None:
            return
        os.killpg(self.process.pid, signal.SIGTERM)
        try:
            self.process.wait(20)
        except subprocess.TimeoutExpired:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait()


def substitute(value, variables):
    if isinstance(value, str):
        for name, raw in variables.items():
            if value == "${" + name + "}":
                return raw  # keeps the type of a captured number
            value = value.replace("${" + name + "}", str(raw))
        return value
    if isinstance(value, list):
        return [substitute(v, variables) for v in value]
    if isinstance(value, dict):
        return {k: substitute(v, variables) for k, v in value.items()}
    return value


def mismatch(expected, actual, path="$"):
    """Where `actual` does not contain `expected`, or None."""
    if expected == "$any":
        return None
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return f"{path}: expected an object, got {json.dumps(actual)[:200]}"
        for key, value in expected.items():
            if key not in actual:
                return f"{path}.{key}: missing"
            where = mismatch(value, actual[key], f"{path}.{key}")
            if where:
                return where
        return None
    if isinstance(expected, list):
        if not isinstance(actual, list):
            return f"{path}: expected a list, got {json.dumps(actual)[:200]}"
        for i, item in enumerate(expected):
            if all(mismatch(item, a) for a in actual):
                return f"{path}[{i}]: no item matches {json.dumps(item)}"
        return None
    if expected != actual:
        return f"{path}: expected {json.dumps(expected)}, got {json.dumps(actual)[:200]}"
    return None


def lookup(data, path):
    for key in path.split("."):
        data = data[int(key)] if isinstance(data, list) else data[key]
    return data


def call_tool(instance, name, args):
    for _ in range(20):
        result = instance.rpc("tools/call", {"name": name, "arguments": args}, timeout=35 * 60)
        texts = [c.get("text", "") for c in result.get("content", []) if c.get("type") == "text"]
        if result.get("isError") and texts and texts[0].startswith(RETRY_TEXT):
            time.sleep(1)
            continue
        return result, texts
    return result, texts


def run_step(instance, step, variables):
    args = substitute(step.get("args", {}), variables)
    started = time.time()
    result, texts = call_tool(instance, step["call"], args)
    seconds = time.time() - started
    is_error = bool(result.get("isError"))
    text = "\n".join(texts)
    problems = []
    if is_error != bool(step.get("error", False)):
        problems.append(("unexpected error: " if is_error else "expected an error, got: ") + text[:400])
    if "error_contains" in step and step["error_contains"] not in text:
        problems.append(f"the error does not contain '{step['error_contains']}': {text[:400]}")
    if step.get("image") and not any(c.get("type") == "image" for c in result.get("content", [])):
        problems.append("no image in the result")
    if "expect" in step and not is_error:
        try:
            data = json.loads(texts[0]) if texts else None
        except json.JSONDecodeError:
            data = text
        where = mismatch(substitute(step["expect"], variables), data)
        if where:
            problems.append(where + " in " + text[:600].replace("\n", " "))
    for name, path in step.get("capture", {}).items():
        try:
            variables[name] = lookup(json.loads(texts[0]), path)
        except (LookupError, ValueError, TypeError, IndexError):
            problems.append(f"nothing to capture at {path}")
    return problems, seconds, text


def measure(instance):
    tools = instance.rpc("tools/list")["tools"]
    init = instance.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "mcp-test", "version": "1"}})
    return {"tools": len(tools), "tools_list_bytes": len(json.dumps(tools)),
            "instructions_bytes": len(init.get("instructions", "")), "names": [t["name"] for t in tools]}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenarios", nargs="*", help="scenario file names without .json (default: all)")
    parser.add_argument("--binary", default=str(REPO / "build" / "src" / "Debug" / "snapmaker-orca"))
    parser.add_argument("--config", default=str(Path.home() / ".config" / "Snapmaker_Orca"),
                        help="configuration copied for the run")
    parser.add_argument("--port", type=int, default=13799)
    parser.add_argument("--tags", nargs="*", default=[], help="also run the scenarios with these tags (printer, network)")
    parser.add_argument("--startup-timeout", type=int, default=300)
    parser.add_argument("--report", default=str(REPO / "build" / "mcp-test-report.md"))
    parser.add_argument("--keep", action="store_true", help="keep the temporary folder")
    options = parser.parse_args()

    binary = Path(options.binary)
    if not binary.exists():
        sys.exit(f"{binary} not found: build the Debug configuration with -DSNAPMAKER_AI_TEST_HOOKS=ON")
    files = sorted(SCENARIOS.glob("*.json"))
    if options.scenarios:
        files = [SCENARIOS / f"{name}.json" for name in options.scenarios]
    scenarios = []
    for path in files:
        scenario = json.loads(path.read_text(encoding="utf-8"))
        scenario["name"] = path.stem
        if set(scenario.get("tags", [])) - set(options.tags):
            print(f"skipped {path.stem} (tags {scenario['tags']})")
            continue
        scenarios.append(scenario)

    temp = Path(tempfile.mkdtemp(prefix="mcp-test-"))
    shutil.copytree(REPO / "resources" / "handy_models", temp / "models")
    variables = {"TMP": str(temp), "MODELS": str(temp / "models")}
    lines = ["# MCP test bench", "", f"Binary: `{binary}`", f"Temporary folder: `{temp}`", ""]
    failures = 0
    measured = None

    for confirm in ("yes", "no"):
        group = [s for s in scenarios if s.get("confirm", "yes") == confirm]
        if not group:
            continue
        data_dir = temp / f"config-{confirm}"
        token = secrets.token_hex(32)
        make_data_dir(Path(options.config), data_dir, options.port, token, "printer" in options.tags)
        log_path = temp / f"slicer-{confirm}.log"
        print(f"starting the slicer (confirmations answered '{confirm}'), log: {log_path}")
        with open(log_path, "wb") as log:
            instance = Instance(binary, data_dir, options.port, token, confirm, log)
            try:
                instance.wait_ready(options.startup_timeout)
                if measured is None:
                    measured = measure(instance)
                for scenario in group:
                    lines += [f"## {scenario['name']} (confirm: {confirm})", "", scenario.get("description", ""), ""]
                    print(f"== {scenario['name']}")
                    for i, step in enumerate(scenario["steps"], 1):
                        label = f"{i}. `{step['call']}`" + (f" ({step['note']})" if "note" in step else "")
                        try:
                            problems, seconds, text = run_step(instance, step, variables)
                        except Exception as e:  # the instance died or did not answer
                            problems, seconds, text = [f"{type(e).__name__}: {e}"], 0, ""
                            try:
                                instance.process.wait(5)
                            except subprocess.TimeoutExpired:
                                pass
                        status = "FAIL" if problems else "ok"
                        failures += bool(problems)
                        print(f"  {status:4} {label} {seconds:.1f} s")
                        for p in problems:
                            print(f"       {p}")
                        lines.append(f"- {status} {label}, {seconds:.1f} s" + "".join(f"\n  - {p}" for p in problems))
                        if instance.process.poll() is not None:
                            raise RuntimeError(f"the slicer exited with code {instance.process.returncode}, "
                                               f"see the end of {log_path}")
                    lines.append("")
            except Exception as e:
                failures += 1
                print(f"ABORTED: {e}")
                lines += [f"**Aborted: {e}** (log: `{log_path}`)", ""]
            finally:
                instance.stop()

    if measured:
        lines += ["## Size", "", f"- tools: {measured['tools']}", f"- tools/list: {measured['tools_list_bytes']} bytes",
                  f"- instructions: {measured['instructions_bytes']} bytes", f"- names: {', '.join(measured['names'])}", ""]
    lines.append(f"**{'FAILED: ' + str(failures) + ' problem(s)' if failures else 'All steps passed'}**")
    Path(options.report).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"report: {options.report}")
    if measured:
        print(f"tools: {measured['tools']}, tools/list: {measured['tools_list_bytes']} bytes, "
              f"instructions: {measured['instructions_bytes']} bytes")
    if not options.keep and not failures:
        shutil.rmtree(temp, ignore_errors=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
