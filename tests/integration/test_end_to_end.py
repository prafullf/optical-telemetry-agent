"""End-to-end tests: run the real ota-agent binary and check its output files.

Set OTA_AGENT to the binary path (defaults to build/ota-agent).
"""

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
AGENT = Path(os.environ.get("OTA_AGENT", ROOT / "build" / "ota-agent"))
sys.path.insert(0, str(ROOT / "tools"))

import analyze  # noqa: E402


def run_agent(tmp_path, *extra):
    alarms = tmp_path / "alarms.jsonl"
    telemetry = tmp_path / "telemetry.jsonl"
    summary = tmp_path / "summary.json"
    cmd = [str(AGENT), "--ports", "12", "--workers", "3", "--rate", "0", "--samples", "300",
           "--seed", "7", "--alarm-log", str(alarms), "--telemetry-log", str(telemetry),
           "--summary-json", str(summary), *extra]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, proc.stderr
    return (analyze.load_jsonl(str(alarms)), analyze.load_jsonl(str(telemetry)),
            json.loads(summary.read_text()))


@pytest.fixture(scope="module")
def faulty_run(tmp_path_factory):
    return run_agent(tmp_path_factory.mktemp("faulty"),
                     "--inject", "1:los:50:40",
                     "--inject", "4:ber-degrade:100:120",
                     "--inject", "8:over-temp:60:80",
                     "--inject", "10:rx-degrade:150:60")


def test_binary_exists():
    assert AGENT.exists(), f"build the agent first ({AGENT})"


def test_no_samples_lost(faulty_run):
    _, telemetry, summary = faulty_run
    assert summary["samples_polled"] == 12 * 300
    assert summary["samples_processed"] == summary["samples_polled"]
    assert len(telemetry) == summary["samples_polled"]


def test_expected_alarms_and_no_false_positives(faulty_run):
    alarms, _, _ = faulty_run
    raised = {(a["port"], a["alarm"]) for a in alarms if a["state"] == "RAISED"}
    assert (1, "LOS") in raised
    assert (1, "RX_POWER_LOW") not in raised          # masked by LOS
    assert (4, "PRE_FEC_BER_HIGH") in raised
    assert (8, "TEMP_HIGH") in raised
    assert (10, "RX_POWER_LOW") in raised
    assert {p for p, _ in raised} == {1, 4, 8, 10}    # healthy ports stay quiet


def test_every_alarm_clears_after_fault(faulty_run):
    alarms, _, summary = faulty_run
    report = analyze.alarm_report(alarms)
    assert report["still_active"] == 0
    assert summary["active_alarms"] == 0


def test_drift_detector_warns_before_threshold_alarm(faulty_run):
    alarms, telemetry, _ = faulty_run
    warnings = analyze.early_warnings(telemetry, alarms)
    ber = [w for w in warnings if w.port == 4 and w.metric == "pre_fec_ber"]
    assert ber and ber[0].alarm == "PRE_FEC_BER_HIGH"
    assert ber[0].lead_samples > 0
    temp = [w for w in warnings if w.port == 8 and w.metric == "temp_c"]
    assert temp and temp[0].lead_samples > 0
    healthy_ports = {0, 2, 3, 5, 6, 7, 9, 11}
    assert not [w for w in warnings if w.port in healthy_ports]


def test_cli_rejects_bad_injection(tmp_path):
    proc = subprocess.run([str(AGENT), "--inject", "1:meteor:0:5"], capture_output=True, text=True)
    assert proc.returncode == 1
    assert "unknown fault" in proc.stderr
