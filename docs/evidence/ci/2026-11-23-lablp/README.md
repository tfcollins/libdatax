# hw-test.yml on the ADRV9361-Z7035 (lablp), 2026-11-23

This is the first green `kuiper` leg for lablp of `.github/workflows/hw-test.yml`
on `main`: [run 45099566322](https://github.com/tfcollins/libdatax/actions/runs/45099566322),
dispatched with `place=lablp` at libdatax `e303884`. The lab runner (`hw-nemo`)
used `scripts/hw/kuiper-run.sh` to ask labgrid for part `adrv9361z7035` on
carrier `adrv1crr-fmc`. The coordinator's board catalog picked the Kuiper image
(2023_R2_P1), and the place's own boot strategy (TFTP/SD autoboot) booted it.
The runner then ran `tests/hardware` against the exported URI. The files here
are that run's uploaded artifacts, redacted on the runner by
`scripts/hw/redact.py` before upload.

| Files | Result | What it covers |
|---|---|---|
| `evidence-lablp.json` / `junit-lablp.xml` | 10 passed, 15 skipped (the P3a tests) | P2 streaming on the AD9361 (2 RX / 2 TX channels): RX on each channel, receive timeout recovery, one-shot and cyclic TX, start/stop cycles, RX throughput, digital loopback bit-exact on ch0 and ch1 (`loopback`=1, TX>>4) |

The board reports HDL `hdl_2023_r2` (`adrv9361z7035_ccfmc`, LVDS) and a Kuiper
2023_R2 kernel. The libiio pin is `6534f14215f4738cec224bf50f13f475fe5264a6`.

Getting this leg green took three lab fixes, all on 2026-11-23:
- the coordinator catalog entry for the part (labgrid-plugins #196, deployed);
- nemo's SSH key on lablp (labgrid's exporter-side agent);
- labgrid-plugins #193/#197 at libdatax's pin. The console reconnect is for the
  carrier's board-powered CP2103, which enumerates only after power-on.

These results match the controller-run P2 sessions in [p2](../../p2/README.md).
