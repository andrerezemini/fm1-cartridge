#!/usr/bin/env python3
"""Verify produced PCM/report, not just the simulator's process exit code."""
import array
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import wave

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="fm1-cartridge-") as temp:
    root = Path(temp)
    for faults, tone in ((False, False), (True, False), (False, True)):
        wav = root / f"{faults}-{tone}.wav"
        report = root / f"{faults}-{tone}.json"
        cmd = [str(exe), "--seconds", "8", "--wav", str(wav), "--report", str(report)]
        subprocess.run(cmd + (["--faults"] if faults else []) + (["--tone"] if tone else []), check=True)
        data = json.loads(report.read_text())
        assert data["simulation_only"] is True
        assert data["source"] == ("tone" if tone else "synth")
        assert data["synth"] and len(data["upstream_revision"]) == 40
        assert data["midi_events"] > 300
        assert data["overruns"] == 0
        assert data["received_frames"] > 300_000
        assert data["wav_frames"] == data["silent_frames"] + data["rendered_frames"]
        with wave.open(str(wav), "rb") as audio:
            assert (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) == (2, 2, 44100)
            assert audio.getnframes() == data["wav_frames"]
            pcm = array.array("h", audio.readframes(audio.getnframes()))
            if sys.byteorder != "little":
                pcm.byteswap()
            assert max(abs(x) for x in pcm) > 100
            # A disconnected peer must not retain stale PCM/effect tails.
            if faults:
                lo = int(3.02 * 44100 * 1.0004) * 2
                hi = int(3.25 * 44100 * 1.0004) * 2
                assert all(x == 0 for x in pcm[lo:hi])
                assert max(abs(x) for x in pcm[-44100:]) > 100
        if faults:
            assert data["injected_packet_losses"] >= 100
            assert data["underruns"] >= 1
            assert data["malformed_packets"] == 1
            assert data["reconnects"] == 1
        else:
            assert data["underruns"] == data["malformed_packets"] == data["reconnects"] == 0
    for args in (["--ppm", "nan"], ["--seconds", "-1"], ["--engine", "1.5"], ["--unknown"]):
        assert subprocess.run([str(exe), *args], capture_output=True).returncode == 2
print("simulation WAV/report/reconnection: ok")
