# hw-test.yml on the ADRV9009/ZC706, 2026-11-23

This is the first green `kuiper` leg of `.github/workflows/hw-test.yml` on `main`:
[run 45097585277](https://github.com/tfcollins/libdatax/actions/runs/45097585277),
dispatched with `place=zc706` at libdatax `c9b81c2`. The lab runner
(`hw-nemo`) used `scripts/hw/kuiper-run.sh` to ask labgrid for part `adrv9009`
on carrier `zc706`. The coordinator's board catalog picked the Kuiper image
(2023_R2_P1), and the place's own boot strategy booted it. The runner then ran
`tests/hardware` against the exported URI. The files here are that run's uploaded
artifacts, redacted on the runner by `scripts/hw/redact.py` before upload.

| Files | Result | What it covers |
|---|---|---|
| `evidence-zc706.json` / `junit-zc706.xml` | 10 passed, 15 skipped (the P3a tests) | P2 streaming on a single ADRV9009 (2 RX / 2 TX channels): RX on each channel, receive timeout recovery, one-shot and cyclic TX, start/stop cycles, RX throughput, digital loopback bit-exact on ch0 and ch1 (I and Q exchanged, as on tron) |

The board reports HDL `hdl_2023_r2` (`hdl_system_id` in the evidence JSON) and
a Kuiper 2023_R2 kernel. The libiio pin is
`6534f14215f4738cec224bf50f13f475fe5264a6`.

The P3a timed-control tests skip: this board has no Tick in its PL. tick's own
timed-GPIO work on this board is tracked separately (tick `support.json`,
platform `adrv9009-zc706`).
