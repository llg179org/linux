# Fairphone 3 (sdm632) mainline port — what is still open

This file lives on `debug-int/7.1.3`, the one branch here that is **not**
upstream-bound, which is why a project TODO can sit in the kernel tree at all: it
must never appear in a `submit/7.1.3/*` series, and it is deliberately **not** on
`integration/7.1.3`. Do not carry it onto any other branch.

That split is the point of `debug-int/<base>`:

```
integration/<base>   audio + voice + camera + charger + sensor
                     the pure cherry-pick sum of the upstream-bound categories,
                     so it stays a faithful mirror of what submit/* will carry
      |
      +-> debug-int/<base>   + the debug layer (the watchdog safety net, this file)
                             <- and this is the branch the linux-fp3 package builds
```

The package builds `debug-int/<base>` on purpose. The safety net has to be on the
phone — without the watchdog running from probe, a hang before userspace opens
`/dev/watchdog` leaves a device that has to be switched off by hand, and this one
is often not within arm's reach.

It is a **kernel-side index**. The reasoning, the measurements and the register
dumps behind every item live in
[`llg179/fp3-pmaports/docs/`](https://github.com/llg179/fp3-pmaports/tree/main/docs),
and that is the authoritative copy — this file only says *what is open, on which
branch, and where to read about it*. When the two disagree, the docs win.

The branch layout itself (`wip/<base>/<category>` → `integration/<base>` →
`submit/<base>/<category>`, and the rule that a change must land on both its wip
branch and its integration) is defined in
[`fp3-pmaports/README.md`](https://github.com/llg179/fp3-pmaports#the-branch-model);
the base-bump procedure is in
[`docs/rolling-a-new-base.md`](https://github.com/llg179/fp3-pmaports/blob/main/docs/rolling-a-new-base.md).

Hashes are deliberately absent except where a commit is being *cited* rather than
*tracked* — a head written into a file is wrong by the next push. Re-derive with:

```sh
git for-each-ref --format='%(refname:short) %(objectname:short=12)' \
  'refs/remotes/fork/wip/7.1.3/*' 'refs/remotes/fork/submit/7.1.3/*' \
  'refs/remotes/fork/integration/7.1.3' 'refs/remotes/fork/debug-int/7.1.3'
# note: there is no wip/<base>/debug - see "The `debug` layer" below
```

---

## Where the work can go at all

Read this before spending effort on "upstreaming" anything. All of it is
AI-assisted, and that closes two of the three doors:

| destination | AI-assisted work | verdict |
|---|---|---|
| postmarketOS (pmaports, wiki) | banned outright | closed |
| msm8953-mainline (GitHub PR) | "we don't merge AI assisted work" — maintainer, [issue #197](https://github.com/msm8953-mainline/linux/issues/197), 2026-07-25 | closed |
| mainline Linux (LKML) | permitted **with disclosure** | the only path |

So `submit/7.1.3/*` targets the subsystem lists, never a pull request here.
Upstream-bound commits carry `Assisted-by: Claude:<model-id>` and the AI must
**never** carry a `Signed-off-by` — only a human can certify the DCO.

## Does it even apply to a maintainer tree?

Measured 2026-07-30 by cherry-picking each group onto a detached head at the real
destination, not inferred from "the files exist upstream". Bases: Mark Brown
`sound/for-next` `1523ce38eeb6`, Sebastian Reichel `linux-power-supply/for-next`
`5584ad5706e5`, `torvalds/master` `11028ab62899`. 11 of 21 commits applied clean.

| group | destination | result |
|---|---|---|
| charger driver + binding | `psy/for-next` | 6/6 clean |
| charger dts | mainline | 2/2 clean |
| charger `adc5` channel | mainline | 1/1 clean |
| sensor (`qmi_encdec`) | mainline | 1/1 clean |
| camera dts | mainline | 1/1 clean |
| camera driver | mainline | conflicts, 2 lines of `Kconfig` |
| audio driver | `sound/for-next` | conflicts on patch 1 — missing prerequisite |
| audio dts | mainline | conflicts — `&sound_card` label does not exist |
| voice | `sound/for-next` | the file does not exist upstream |

Redo this after every base bump; it is the only thing that answers the question.

---

## Before anything is submitted

Cross-cutting, mostly `dtbs_check` fallout. Detail:
[`docs/TODO.md`](https://github.com/llg179/fp3-pmaports/blob/main/docs/TODO.md).

1. **The camera needs `sony,imx363.yaml` and a MAINTAINERS entry.** A new sensor
   driver without a binding is refused on sight; `imx258` has both. In the same
   round, drop the leftover `printk(KERN_INFO)` and the commented-out register
   writes — but as a **third commit**, never folded into the byte-identical
   import, whose byte-identity is the thing that makes the delta checkable. That
   commit carries all 4 checkpatch errors and 17 warnings of the series.
2. **Six undocumented codec properties** on the audio `slim217,1a0` node:
   `qcom,micbias{1..4}-microvolt`, `qcom,dmic-sample-rate`,
   `qcom,mbhc-vthreshold`. Same class of gap the charger already had.
3. **`divclk1` and `wcd-vout-1p8` must move out from under `soc@0`** into the
   board file's root — `simple-bus` wants `ranges`.
4. **`wcd-intr-default-state` fails the `qcom,msm8953-pinctrl` schema.**
5. **The battery node's four `qcom,*` properties.** `battery.yaml` has
   `additionalProperties: false` and zero vendor properties; the one JEITA
   precedent (`qcom,jeita-extended-temp-range`) sits on the *charger* node. There
   is a layering argument against the current placement too — see
   [`docs/charger/README.md`](https://github.com/llg179/fp3-pmaports/blob/main/docs/charger/README.md#where-these-properties-belong).
6. **`-ohm` → `-ohms`.** The canonical suffix is plural; `-microamp`/`-percent`
   are already right. Same cycle as 5, same properties.
7. **The camera driver's two-line `Kconfig` conflict** — the neighbouring IMX355
   entry gained a `select V4L2_CCI_I2C`. Trivial, but manual.
8. **The audio prerequisite is named and was posted:** Adam Skladowski,
   *MSM8953/MSM8976 ASoC support* v3, 8 patches, 2024-07-31, state `new`
   ([series 875540](https://patchwork.kernel.org/project/alsa-devel/list/?series=875540),
   cover `<20240731-msm8953-msm8976-asoc-v3-0-163f23c3a28d@gmail.com>`). We need
   1/8, 5/8 and 6/8: `qcom,msm8953-qdsp6-sndcard`, `msm8953_qdsp6_add_ops` and
   `use_ibit_clk` are all out-of-tree today, and so is the `&sound_card` label the
   DTS patch appends to. Declarable with `b4 prep --edit-deps`. Worth asking on
   alsa-devel whether it is still alive before building on it.
9. **Voice is not sendable as-is.** Prior art: Joel Selvaraj's
   `5a63debde2db` (2022-10-02, `sdm670-mainline/linux`) already contains the
   SLIMbus voice routing line for line, including the
   `{ "SLIMBUS_0_RX", NULL, "SLIMBUS_0_RX Voice Mixer" }` edge whose absence we
   booked as our own discovery — and it covers SLIMBUS_0 through 6, where we cover
   0. The `q6voice` driver was never posted to a list, so there is no message-id to
   cite and no upstream file to patch. The realistic move is to offer the
   SLIMBUS_0 work to that series' authors, not to send ours.
10. **Cover-letter disclosure** per `Documentation/process/generated-content.rst`:
    which tools, which prompts, which parts, and how it was tested.

---

## `wip/7.1.3/charger` — PMI632 SMB5

Fast charge, hardware JEITA, battery ID + thermistor, cooling device. All nine
commits of `submit/7.1.3/charger` apply clean, though to three different trees —
six to `psy/for-next`, two dts and one `adc5` channel to mainline. Gaps, in
[`docs/charger/README.md`](https://github.com/llg179/fp3-pmaports/blob/main/docs/charger/README.md#known-gaps):

11. **No high-voltage negotiation on the input side** — the port settles near
    1.9 A, just under the programmed 2 A. This is the next real feature here, and
    a piece of work in its own right.
12. **2 A has never been seen flowing.** Needs a wall charger, a low state of
    charge and a USB meter. Physical.
13. **The mismatch path has never run on hardware.** A DTB-only cycle with a
    deliberately wrong `qcom,batt-id-ohm = <50000>`; expected: the refusal message
    plus `0x1061` staying at `0x14`. Two DTB deploys, no kernel build, no flash.
14. **After a mismatch the previous boot's JEITA thresholds stay in the
    comparators**, not the PMIC defaults — a warm reboot does not reset the PMIC.
    The current limit is safe; the temperature limits are stale. Needs a
    characterised safe default.
15. **The DT can only describe one of the two packs** the FP3 ships (this one is
    Fuji). The ID is checked, so a wrong pack cannot be charged on the wrong
    limits — but it falls back to ~1 A, and the OCV curve is still read from the
    battery node even when the ID did not match. What is missing is the
    *selection*: a multi-`monitored-battery` binding mainline does not have.
16. **Half of the float-voltage story is untouched** — the `*_SL_FCV` bits are at
    their PMIC default; the scaling register is undocumented in every source
    available for this generation.
17. **Hardware JEITA gives one threshold per side; the downstream profile has five
    bands.** The 40–45 °C / 1500 mA step cannot be expressed. The full table would
    mean software JEITA — driven by the approximate temperature curve, which is
    the reason not to.
18. **The trip temperatures are a choice, not a measurement.** Nobody has charged
    this phone hard enough to find out which one it reaches.
19. **No step charging and no `auto-recharge-vbat-mv`** (downstream sets both,
    4300 mV). Worth adopting after the above.

## `wip/7.1.3/audio` — WCD9335 on SLIMbus

Playback, microphone, MBHC and the call path all work on the device. Blocked
upstream on item 8. Gaps, in
[`docs/audio/bringup/README.md`](https://github.com/llg179/fp3-pmaports/blob/main/docs/audio/bringup/README.md#what-is-still-open):

20. **The intermittent first-use failure needs a new lead, not another
    workaround.** The QDSP6SS framer-poke suspicion was closed by measurement
    (A/B, 8 cold boots each side, no difference) and the pokes were reverted; see
    [`docs/audio/bringup/qdsp6ss-framer-poke.md`](https://github.com/llg179/fp3-pmaports/blob/main/docs/audio/bringup/qdsp6ss-framer-poke.md).
21. **The `21`/`22` acoustic selftest checks fail** at −12 dB and at 0 dB while the
    speaker path itself measures clean (999.76 Hz at 31.77 dB). Unexplained, and
    deliberately not filed as environmental.
22. **A stray `Quinary MI2S` backend can attach to the voice front end.**
23. **The jack is treated as 3-pole**, and no TX gain control is exposed for the
    call path.

## `wip/7.1.3/camera` — Sony IMX363

Three commits: a verbatim import, our power-path delta, the DT node. The driver
is **Joel Selvaraj's** (`sdm670-mainline/linux` MR !3, commit `5130bc702ea2`,
2024-08-15), archived byte-identically on `vendor/imx363-sdm670`; our measured
delta is +68/−21 on 1514 lines, roughly half comments, functionally four things in
the power path.

24. **Streaming does not work end to end.** The sensor probes and the CAMSS link
    enables; the remainder is CAMSS-side. The one in-driver lead: the two modes'
    link frequencies disagree with the DT `link-frequencies`, and one of them is
    commented `// NOT SURE HOW TO FIND THIS VALUE` by its own author.
25. **Parked: the PMI632 flash LED.** The node exists, but
    `leds-qcom-flash.c` subtype detection is unverified on this hardware and
    risks a probe failure until it is. Kept out of the tree for now.

## `wip/7.1.3/sensor` — SMGR over QMI/QRTR

Accelerometer, gyroscope, magnetometer, proximity, ambient light. Only one commit
has been distilled so far — `soc: qcom: qmi: read QMI_DATA_LEN at its declared
width`, which applies clean to mainline. The SMGR driver itself, 2778 lines across
30 files on the wip branch including the QRTR-bus prerequisites, has no series
yet. Gaps, in
[`docs/sensors/README.md`](https://github.com/llg179/fp3-pmaports/blob/main/docs/sensors/README.md#known-gaps):

26. **The magnetometer is uncalibrated and its scale unverified** — a full-sphere
    fit is needed; the two cannot be solved from each other.
27. **The mount matrix is probably wrong** — it is the msm8996 matrix with a
    `TODO` on it, and `iio-sensor-proxy` reports face-down at `z = −9.69`. One
    deliberate screen-up reading settles it.
28. **Registry groups 20, 2691 and 3050 are zero-filled**, not real. The actual
    offsets, or the key lists, need to go into `sns.reg`.
29. **`snsregd.py` is still a Python stand-in** for upstream's C `sns-reg`; it
    should become an aport. (Userspace, tracked here because the driver depends on
    it.)

## `wip/7.1.3/voice` — q6voice / CS-Voice over SLIMbus

One commit. Working on the device; see item 9 for why it is not sendable.

## The `debug` layer — bring-up aids, never upstream-bound

Starting the watchdog at probe, and this file. Nothing here gets a `submit/`
series, ever, and it stays off `integration/7.1.3`.

**It is the only category with no `wip` branch.** `wip/7.1.3/debug` was retired on
2026-07-30 (kept as the tag `archive/wip-7.1.3-debug-final`) once the layer became
reproducible without it: every other category needs a `wip` branch because it
carries evolving work against a moving base, while this one is a fixed, additive
change that replays anywhere. It now lives here plus in
`fp3-pmaports/docs/debug/files/`, and those payloads are half of the storage
rather than a copy — refresh them in the same commit that changes the layer.

The watchdog commit is the one place in the tree where mixing `.dts` with `.c` is
allowed, and it uses that licence: it adds an undocumented `qcom,start-at-probe`
property. That would be fatal in a `submit/` series and is fine here; the reason
is written into the commit message, along with why there is deliberately no
`ramoops` node (tried at `0x8ee00000` and at `0xd0000000`; nothing survives a
reset on this device, so it would cost 2 MB and imply a post-mortem capability
that does not exist).

### Replaying the debug layer onto any branch

The safety net is worth having on any branch you are about to boot — an
experimental offshoot is exactly where an early hang is likely, and exactly where
nobody wants to walk to the phone. One command, from the target branch:

```sh
git am ../fp3-pmaports/docs/debug/files/0001-watchdog-*.patch
```

The step-by-step — preconditions with defined failure actions, a by-hand
reconstruction for when the patch stops applying, and verification in three
places — is `fp3-pmaports/docs/debug/create_debug.md`.

It applies clean everywhere because the board-side change is a **separate**
`sdm632-fairphone-fp3-debug.dtsi` plus one `#include` among the other includes.
That is not cosmetic: every other category appends its nodes to the *end* of
`sdm632-fairphone-fp3.dts`, so the earlier form — which appended there too —
collided with whichever of them was present. Measured 2026-07-30: the appended
form conflicted on `wip/7.1.3/audio` and on `integration/7.1.3` and applied clean
on `camera` and `charger`; the split form applies clean on all five wip branches
and on integration. Verified again by rebuilding the layer from the stored
payloads onto a fresh branch off `integration/7.1.3`: same tree object as
`debug-int/7.1.3`, same blob for every file it touches.

---

## Not kernel work, kept here so it is not lost

30. **The notification LED blinks forever after a missed call** (`rgb:status`, not
    the flash). The real bug is a missing `EndFeedback` call in whatever raised it
    — phosh or the call app; secondarily, a `fairphone,fp3.json` feedbackd theme
    is missing.
31. **Untested: the interconnect path for the SCM/crypto node.** Non-blocking;
    kept in case the ADSP-boot timing question reopens.
32. **The package now pins `debug-int/7.1.3`, not `integration/7.1.3`.** Two
    rewrites have moved out from under the old pin — the camera provenance and
    then the debug split — so `_commit` is reached only through
    `archive/integration-7.1.3-pre-camera-provenance` and
    `archive/integration-7.1.3-pre-debug-split`. Anything built from that pin has
    **no watchdog**, which is the practical reason to bump rather than a
    bookkeeping one.
    ☠️ GitHub serves a source tarball only while the commit is reachable from some
    ref, which is why those archive tags exist at all — check before trusting a
    pin:

    ```sh
    curl -sI -o /dev/null -w '%{http_code}\n' \
      "https://github.com/llg179/linux/archive/<_commit>.tar.gz"   # 302, not 404
    ```

## The `vendor/*` and `archive/*` namespaces

Neither is a base and neither is ever pruned when a base is rolled.

- `vendor/imx363-sdm670`, `vendor/q6voice-sdm670` — **parentless snapshots** of
  third-party imports, made with `git commit-tree` and no `-p`, so the tree is
  byte-identical to the source without dragging in 71,541 unrelated commits.
  `git diff <snapshot> <source>` is empty, which is the check.
- `vendor/asoc-msm8953-base`, `vendor/q6voice-base` — tags, not branches: those
  commits are already in `7.1.3/main`, so they need a name, not a copy.
- `archive/*` — rewritten history kept reachable, so an old pin still resolves
  and its tarball still downloads.
