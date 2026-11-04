# Emulator fixtures

Context XML files served by `iiod-emu` (libiio v1 build) so the tests run
without hardware.

| File | Origin | Contents |
|------|--------|----------|
| `ad936x.xml` | Copy of `test/emu/devices/adrv9361-z7035.xml` from pyadi-iio (https://github.com/analogdevicesinc/pyadi-iio), revision `8ccb1de4a00698887bd50cb8a3945262ccb2f97f`, under pyadi-iio's licence. Only change: a leading XML comment naming the source. | Full ADRV9361-Z7035 context: `ad7291`, `ad9361-phy`, `xadc`, `ad9517-3`, `cf-ad9361-dds-core-lpc`, `cf-ad9361-lpc`. |
| `adrv9009-zu11eg.xml` | Hand-written. | `adrv9009-phy` (`iio:device0`) and `adrv9009-phy-b` (`iio:device1`). Each: input `voltage0`/`voltage1` (`hardwaregain`=10, `rf_bandwidth`, `sampling_frequency`=245760000), output `voltage0`/`voltage1` (`hardwaregain`=10, `sampling_frequency`=245760000), output `altvoltage0` named `TRX_LO` (`frequency`=2400000000); device attributes `calibrate`, `profile_config`. |
| `tick.xml` | Hand-written from the attribute list of Tick's `axi_timed_command_scheduler.c`. | `axi-timed-command-scheduler` (`iio:device9`) with all 31 device attributes: `capability_bitmap`=`0x0000001f`, `time_tick_hz`=100000000, `queue_depth`=16, `command_descriptor` empty, everything else 0. |

The emulator serves static values: writes are accepted but do not trigger
driver behaviour (no queue, no clock).

## Running a command under the emulator

```sh
export DATAX_LIBIIO_BUILD=/path/to/libiio/build   # contains iiod-emu/iiod-emu
tests/emu/emu.sh tests/emu/tick.xml \
    bash -c '"$DATAX_LIBIIO_BUILD"/utils/iio_info -u "$DATAX_TEST_URI"'
```

`emu.sh <xml> <cmd...>` picks a free port, starts `iiod-emu -p <port> emu:<xml>`,
waits until it accepts connections, exports `DATAX_TEST_URI=ip:127.0.0.1:<port>`,
runs the command, always kills the emulator and returns the command's status.
(Wrap in `bash -c` when the command line itself needs `$DATAX_TEST_URI`.)

From Python, `emu.py` provides the same: `with Emulator(Path(xml)) as uri: ...`.

CMake passes `DATAX_LIBIIO_BUILD` into the ctest environment, so configure with
`-DDATAX_LIBIIO_BUILD=...` and run `ctest`.
