# Test 111 validation map

This file maps the questions in `测试111.docx` to executable telemetry and
the remaining controlled experiments. Run the live audit with:

```powershell
python tools/ra8_esp32_protocol/ra8_system_audit.py --samples 20 --interval 1 `
  --output artifacts/ra8_audit.json
```

The credentials file is read locally and is never included in the report.

## Automated now

| Test area | Evidence |
| --- | --- |
| S3 and RA8 connectivity | S3 ready TX/error count, UART RX bytes, valid/status frames, RA8 status age |
| RA8 sender failure | status attempts/successes, payload length/detail, too-long and UART errors |
| Thermal integrity | state, 10248-byte frame, 5120 points, CRC equality |
| Five TMP117 channels | connected and error bit masks, per-channel values on the dashboard |
| Pressure stream | driver state and last error; full matrix remains a separate server matrix |
| Microphone capture | state, read counter progress, error count, peak and mean |
| SD initialization | ready, init step, FSP/FAT failure stage and counters |
| Serial camera | probe state, writer state, saved count and writer errors |
| Vision runtime | ready-model mask, inference progress and NPU error count |
| Timing and resources | observed vision mean/P95/max, current/minimum heap and six task stack margins |
| Version traceability | CPU0, vision, fusion-rule and both cry-model build identifiers |

## Requires a controlled experiment

1. Build a tagged replay set for normal, boundary, danger, recovery and fault
   states. Export precision, recall, F1 and confusion matrices per model.
2. Save the same samples' float, INT8 host and RA8 outputs to compare
   quantization and board consistency.
3. Run each fusion scenario repeatedly and record candidate-to-final debounce,
   event creation, speaker action, S3 ACK, server persistence, mobile delivery
   and recovery timing.
4. Inject one fault at a time: camera disconnect/corrupt JPEG, thermal/TMP117/
   pressure disconnect, microphone clock/data loss, SD removal/full card, UART
   disconnect, Wi-Fi/server outage and withheld safety ACK.
5. Run for multiple hours and retain the audit JSON, server logs, SD file list
   and hashes. Compare minimum heap, stack high-water marks, UART/CRC counters,
   reconnect counts, duplicate events and file integrity before and after.

## Acceptance rules

- A sensor is healthy only when its integrity contract passes, not merely when
  a nonzero value appears.
- Level-3 alarms must remain latched until the configured recovery/acknowledge
  rule completes; a network outage must not disable local warning behavior.
- The audit reports `unavailable` for a missing test field. It must not convert
  missing evidence into a pass.
- Precision and latency targets must be set before the formal run and recorded
  with the model and firmware version fields from the same telemetry stream.
