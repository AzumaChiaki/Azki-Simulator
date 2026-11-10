# GPU display support and validation

The N66, N67, O66, P67 and Q66 catalogue families share `best1503-gpu.h`.
P65 and Q65 use separate renderers with texel coordinates. These are bounded
software implementations of the VGLite command stream, not complete Vivante
hardware models. Device registration and a successful build alone are not
rendering acceptance.

## Changes

All three backends now decode IMAGE_CONFIG tile bits independently from pixel
format and filtering. COLOR, PAD, REPEAT and REFLECT address each interpolation
tap correctly, including negative coordinates, repeated periods and mirror
seams. Floating coordinates are reduced before integer conversion. Each backend
retains its existing pixel-centre convention; the captured watch LINEAR/PAD
ramp and A8 LINEAR row rules remain intact.

vg_lite_blit_rect moves c_step back by half a texel on the axes it filters
(LINEAR on u, BI_LINEAR on u and v), so the sampler subtracts the half texel
only on the remaining axes.

The shared band renderer now keeps LINEAR's vertical selection identical to
POINT, while interpolating horizontally. Previously a fractional vertical
coordinate mixed two rows, effectively applying bilinear filtering. The
[NXP filter definitions](https://mcuxpresso.nxp.com/mcuxsdk/25.06.00/html/middleware/vglite/topics/blit_enumerations.html)
distinguish these modes. An independent constant-row fixture constructs SDK
inverse-translation coefficients and checks 104 vectors across all four tile
modes, including negative coordinates and edges. It reproduces 24 incorrect
LINEAR pixels before the fix and passes all 624 checks afterward. These are
host decoder checks; they do not establish physical half-texel tie behavior.

Validated layouts include BGRA8888, RGB565, packed BGRA5658 and BGR888, plus
watch BGRA5551 and Q65's captured legacy encoding. Existing alpha blending,
path coverage and clipping remain in use. Unsupported Gaussian states are
rejected atomically instead of being treated as bilinear filtering. Static
feature tables in the inspected Q65 .080 and P65 .031 original APs advertise
Gaussian blur and repeat/reflect as disabled; the new border contracts do not
claim that those firmwares exercised these modes.

GPU submission, completion, mask, read-to-ack and reset contracts are tested
against the production MMIO functions. `BES_GPU_TRACE_SYNC_WORDS` optionally
logs synchronization state and up to eight aligned hexadecimal RAM addresses;
it never changes guest state. Successful Q65 sensor CMU writes are now logged
only when `BES_SENSOR_TRACE_CMU` is enabled. Unsupported accesses remain errors.
The latter avoids millions of normal clock-write messages during panel sleep;
it does not establish full-system low-power behavior.

## Original-firmware observations

The [per-profile evidence matrix](gpu-device-validation.json) records all 39
catalogue entries independently. Thirty-eight configurations using original AP bytes were
run on the candidate core, produced nonzero GPU pixels and displayed native
firmware UI, with no GPU rejection or reported Cortex/QMP fault in those runs.
N187 was omitted under the restriction recorded for that earlier experiment.
A later audit scoped the restriction to an old SDK task-reaping probe;
ordinary display-only boot checks are separate. This GPU matrix retains its
original execution records and does not claim an N187 GPU run.
Some entries share AP bytes, but their observations are recorded separately.

P67 needs a longer observation window: all nine configurations were still at the
boot logo at 30 seconds and displayed the watchface during the 90-second runs.
The .043 clock changed from 08:00 to 08:01. Short-window captures must not be
reported as a boot failure or as completed UI acceptance.

The AP files stayed unchanged. These runs used synthetic reset vectors and
explicit local setup/storage fixtures on partial peripheral models. They are
not secure-boot, account-binding or real-radio verification.

Additional original-firmware checks:

- Q155: 90-second interaction run, 734 GPU submissions/completions, zero
  rejections, native watchface/list navigation and a captured list transition.
  Ten sampled frames are not automatically ten different or consecutive frames.
- O66 .28: an earlier core produced one BUSY rejection followed by reboot.
  A submission that arrives during a draw now retires that draw in order and
  starts, as the hardware queue does, so it no longer becomes a bus error.
  New-core runs completed 80 seconds with 831 submissions (diagnostic trace)
  and 345 submissions (normal logging), zero rejections and no reboot. The
  old race was not reproduced; its cause is not established or claimed fixed.
- Q65 .080: native watchface and app grid rendered; a captured transition has
  ten consecutive writer frames with ten different RGB hashes. A separate run
  confirmed a saved five-second timeout, DCS sleep-in, crown wake, DCS sleep-out
  and subsequent GPU/LCD activity. On the final core, all 24 scripted steps
  completed: idle, wake, app-list entry and scrolling; 816 commands produced
  pixels with zero rejection or AP/sensor fault. Normal CMU trace suppression
  reduced this 180-second run log from 282,221,718 to 1,444,018 bytes. This is
  a log-volume result, not a CPU low-power or memory-stability measurement.

P65 .031 storage fixes were also tested on the final core. The original AP
now reaches its language-selection screen (90 seconds, 21 GPU completions,
zero rejections), after correcting raw BES audio storage to NOR `0xd00000`
within 3 MiB and the resource extent to 220 MiB. The prior Q65 ROMFS audio
assumption prevented startup. The real factory extent is `0xcc0000`/256 KiB;
unverified payloads and old mismatched layouts are rejected. Upstream temporary
writable XIP overlays were removed, preserving the normal NOR controller path.

A separate P65 setup run rendered the watchface and app grid with 206 GPU
completions and zero rejections. It used an explicitly marked AP boot copy with
embedded configuration changed; it is not original-AP desktop acceptance.
Original AP bytes were retained and verified. Audio CP execution, P65 factory
payload semantics, and the other archived P65 firmware revisions remain unverified.

## Reproducible contracts

Run from the repository root with an existing C compiler and Python environment:

```sh
python3 -m unittest discover -s tests -p test_gpu_tiling.py -v
python3 -m unittest discover -s tests -p test_gpu_lifecycle.py -v
python3 -m unittest discover -s tests -p test_gpu_copy.py -v
python3 -m unittest discover -s tests -p test_gpu_q65_sampler.py -v
```

The tile/format/invalid-state suite executed 4,580 vectors and 27,410 checks
under ASan/UBSan. The MMIO lifecycle suite executed 102 checks, including
completion-time reentry, pending-work cancellation and interrupt acknowledgement.
The ordered-copy contract checks that a transparent NONE blit replaces stale
target pixels and that a later GPU draw reads the earlier offscreen result in
the same submission. These operations support multi-pass background effects;
the effect itself is a separate application.
Host/runtime and sensor contracts and Swift frame/isolation tests are separate
from these pixel tests. On macOS use the installed Xcode toolchain for
`swift test --jobs 4`; the default SwiftPM sandbox is retained.

Firmware images and private run directories are intentionally not distributed
in this repository. Reproduce runtime observations with the exact original AP
hash, matching resource/board geometry and explicitly recorded setup fixture in
the matrix. Use a new owned session directory, retain faults and unsupported
commands, and verify the actual LCD frame. P67 observations need at least 90
seconds. Do not substitute SDK samples for original-firmware UI verification.

## Remaining limits

The catalogue matrix records core `b944a821…`; its four GPU renderer/helper
files are byte-identical to final core `8675f097…`. Final P65 and Q65 checks
used the latter core with the storage and logging fixes described above.

The profile matrix distinguishes UI observation from full-device acceptance.
It does not certify every shader/blur/texture mode, all sensor consumption,
per-profile reentry and sleep/wake, or long-duration memory stability. Peak
child-process RSS is recorded as a measurement, not leak proof. Firmware logs
still expose unimplemented NFC, magnetometer, optical-health and GNSS paths.
O62M has not completed original boot and has no GPU runtime acceptance; its
research probe is not registered as a functioning watch machine.

The local research core linked Homebrew libraries with minimum-OS requirements
newer than macOS 15. A distributable macOS 15 application was not validated or
installed by this work.
