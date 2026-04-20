# Accelerometer statistics — progressive implementation plan

## 0. Project context

Diagnostic-only firmware for a **Seeed XIAO ESP32-S3** + **ADXL362** worn by
the cat. Goal: record 24 h+ of accelerometer statistics to pick the thresholds
that reliably detect a **"major motion" = cat displacement > 3 m**. No Wi-Fi,
no LoRa, no Bluetooth. USB-serial only, while on the bench.

Wearing this firmware, the cat will be untethered for a full day, the device
will be brought back, plugged into USB, and its statistics read out.

## 1. What "major motion" means and what we need to measure

Accelerometers do **not** measure displacement directly — double-integrating
acceleration with gravity present and no absolute reference is unreliable.
So "3 m displacement" has to be inferred from proxies.

Observations about cat behavior:

- Grooming, breathing, purring, scratching: **high-magnitude but short-lived
  and non-directional** — bursts of <1 s, oscillatory, cat doesn't go anywhere.
- Walking to food bowl, crossing a room (≥ 3 m): **moderate magnitude, sustained
  several seconds, dominated by a rhythmic forward gait**.
- Running, jumping, being carried, in a vehicle: **high magnitude and
  sustained**.

The discriminator between "grooming in place" and "walked 3 m" is
**sustained activity over time**, not peak magnitude. So the statistics
must characterize the joint distribution of **magnitude × duration** of
activity bouts, not just samples.

## 2. Statistics gathered, and why

### 2.1. Per-sample histograms

At a fixed ODR (start at **25 Hz**), each sample produces a magnitude
`m = |a| - 1g` (gravity subtracted; alternative: use the ADXL362's built-in
high-pass filter via `FILTER_CTL.HALF_BW`). Bin `m` into a fixed log-ish
scale:

| Bin | Range (mg) | Rationale |
|---|---|---|
| 0 | 0–25 | Noise floor / perfectly still |
| 1 | 25–50 | Breathing, heartbeat |
| 2 | 50–100 | Grooming, small shifts |
| 3 | 100–200 | Walking candidate floor |
| 4 | 200–400 | Walking / trotting |
| 5 | 400–800 | Running / jumping |
| 6 | 800–1600 | Impact / hard landing |
| 7 | 1600+ | Shock |

**Why**: tells us where to set the lower threshold that separates
"essentially still" from "doing something". The cumulative-below-X value
for each threshold X gives us the false-positive rate we'd see if we set
`THRESH_ACT` = X on the ADXL362.

### 2.2. Activity "bout" statistics

A **bout** = consecutive samples with `m` above a **low threshold**
(start at **50 mg**) for **N samples in a row** (start at **3** ≈ 120 ms).

Per bout, record:

- **Duration** (sample count, histogrammed: 1–2, 3–6, 7–12, 13–25, 26–50,
  51–100, 101–250, 251–625, 626+ samples at 25 Hz → up to 25+ s).
- **Peak magnitude** (histogrammed with the same bins as §2.1).
- **Integrated magnitude** ("energy" proxy, `Σ m` over the bout, histogrammed
  on a log scale from 100 mg·samples to 100 000).
- **Direction-change count** on the dominant-axis — proxy for rhythmicity.
  Zero-crossings / sign reversals of the high-pass-filtered signal on the
  axis with max variance in the bout.

**Why**:

- Bouts are what the ADXL362's `THRESH_ACT` + `TIME_ACT` detector fires on.
  Distribution of bout durations directly informs `TIME_ACT`.
- Peak × duration separates *grooming* (short, high peak, many direction
  changes) from *walking* (medium duration, moderate peak, few direction
  changes).
- Integrated magnitude is the single best scalar proxy for "how much did the
  cat accelerate, total" — and its relationship to displacement is more linear
  than peak alone.
- Direction changes help identify locomotion vs. oscillatory grooming: a
  walking cat has a ~2 Hz forward rhythm (few reversals per second per axis),
  grooming has many.

### 2.3. Stillness statistics

Histogram the **gap duration** between bouts (time below low threshold).

**Why**: informs `TIME_INACT` — how long does a typical cat stay still, and
how long a still period is long enough that re-arming wake is safe?

### 2.4. Threshold-crossing counters (cheap sanity)

For each of **5 candidate activity thresholds** (50, 100, 200, 400, 800 mg),
count:

- Number of samples above threshold in the hour
- Number of distinct bouts (upward crossings preceded by ≥ 1 s below)

**Why**: gives us an immediate answer to "if I set THRESH_ACT = X on the
ADXL362, how many wake events per hour would I get?" — without needing to
re-process raw data.

### 2.5. Gross counters

- Uptime / samples collected (sanity — did we actually sample for the whole
  hour, or did something stall?)
- Max single-sample magnitude
- Max bout duration, max bout integrated magnitude

### 2.6. Per-hour size budget

Per hour, one fixed-size record:

- 8 magnitude-histogram bins × 2 B = 16 B
- 9 duration bins × 2 B = 18 B
- 8 peak-in-bout bins × 2 B = 16 B
- 8 bout-energy bins × 2 B = 16 B
- 8 direction-change bins × 2 B = 16 B
- 9 stillness-gap bins × 2 B = 18 B
- 5 threshold counters (samples + bouts) × 2 × 4 B = 40 B
- Gross counters = ~24 B
- Total ≈ **~165 B per hour**, round up to **256 B** with room for metadata
  and version field.

24 h × 256 B = **6 kB** — fits comfortably in NVS.

### 2.7. What we ask of the ground truth

The stats alone can tell us distribution shapes. To **calibrate** them into
"this is 3 m displacement or more", we need a few hand-annotated events:

- User marks via serial command `event <label>` at moments of known cat
  movement (e.g., "cat just walked from bedroom to kitchen, ~4 m").
- Also mark known non-events ("cat is grooming on sofa").
- Each annotation is timestamped and saved alongside the hour's stats so
  we can back-correlate bout signatures with labels.

## 3. Hardware

| Role | Part | Notes |
|---|---|---|
| MCU | Seeed XIAO ESP32-S3 | Same board as the Cat firmware. |
| Sensor | ADXL362 | SPI, 3-axis, ultra-low-power. |
| Bus | SPI mode 0, 5 MHz | Dedicated — no other SPI devices in this project. |

Wiring (XIAO ESP32-S3 default SPI pins):

| ADXL362 | XIAO pin |
|---|---|
| VS, VDDIO | 3V3 |
| GND | GND |
| SCLK | GPIO7 |
| MOSI | GPIO9 |
| MISO | GPIO8 |
| CS | GPIO2 |
| INT1 | GPIO3 (optional — not strictly required here, useful for real-time bout detection without polling) |
| INT2 | n/c |

## 4. Progressive implementation plan

Each step ends with an **explicit pass criterion**. Do not proceed past a
step until the criterion is met and logged.

### Step 1 — Project scaffold

- Create `platformio.ini` (board `seeed_xiao_esp32s3`, monitor_speed 115200,
  `upload_port` / `monitor_port` = COM4 or the ADXL dev unit's port).
- Create `src_dir = ./`, a top-level `.ino` with `Serial.begin(115200)` and
  a 1 Hz `Serial.println("alive")` heartbeat.
- Git-track.

**Pass**: `pio run -t upload` succeeds, `pio device monitor` shows the
heartbeat within 5 s of reset.

### Step 2 — ADXL362 bring-up

- Add `lib/ADXL362/` with a minimal driver: SPI register read/write,
  `begin()`, `readId()`.
- In `setup()`, read `DEVID_AD` (0x00), `DEVID_MST` (0x01), `PARTID` (0x02).
- Print to serial.

**Pass**: serial shows `AD=0xAD MST=0x1D PART=0xF2` (the three ADXL362
fixed IDs). If any differ, wiring is wrong — stop.

### Step 3 — Live XYZ streaming

- Configure ADXL362 measurement mode at 25 Hz, ±2 g range.
- Read `XDATA_L/H`, `YDATA_L/H`, `ZDATA_L/H` every 40 ms.
- Print CSV: `ms,x_mg,y_mg,z_mg` (so it can be plotted with the Arduino
  serial plotter or a Python helper).

**Pass criteria (sanity)**:

- Flat on desk → X ≈ 0, Y ≈ 0, Z ≈ +1000 mg.
- Rotate 180° → Z ≈ −1000.
- Tap once → single-sample spike > 2000 mg on the tapped axis, rest return
  to gravity within 2 samples.

### Step 4 — Magnitude and gravity removal

- Two options (implement both, pick at Step 6):
  - **Software**: `m = sqrt(x² + y² + z²) − 1000 mg` (clamp to ≥ 0).
  - **Hardware**: enable ADXL362 `FILTER_CTL.HALF_BW` high-pass filter and
    report per-axis absolute values summed.
- Print `ms,m_mg` on serial at 25 Hz.

**Pass**: still → `m` < 25 mg for > 95 % of samples over 60 s. Pick-up-and-
shake → sustained `m` > 200 mg. If still-state `m` floor > 50 mg, the
gravity-removal is wrong; diagnose before proceeding.

### Step 5 — Serial command interface

Command parser on a line-buffered `Serial.readStringUntil('\n')`. Commands:

| Command | Effect |
|---|---|
| `help` / `?` | List commands. |
| `status` | Uptime, current hour index, samples this hour, free heap, free NVS. |
| `live [N]` | Stream next N samples of `m` (default 100). |
| `raw [N]` | Stream next N XYZ triples. |
| `dump` | Dump all hour records as CSV to serial. |
| `dump <h>` | Dump hour `<h>` only. |
| `reset` | Clear all stored stats, reset hour counter. |
| `event <label>` | Timestamped ground-truth annotation; stored with the current hour. |
| `set odr <Hz>` | 12.5 / 25 / 50 / 100. |
| `set low <mg>` | Low threshold for bout detection (default 50). |
| `set mincount <N>` | Min consecutive samples for a bout (default 3). |

**Pass**: each command documented, parses cleanly, rejects unknown input
with a helpful error. `help` output matches actual commands.

### Step 6 — In-RAM statistics engine (no persistence yet)

- Implement the data structure of §2.6 as a single `HourStats` struct.
- Update it on every ADXL362 sample: magnitude histogram, bout tracker
  (state machine: Idle → Rising → Active → Falling → Idle), per-bout stats
  on bout close, threshold-crossing counters.
- Update `event` annotations to append into the current `HourStats`'s
  bounded label ring (e.g., 8 labels × 16 chars).
- `dump` returns a single hour (the current one).

**Pass** — each of these tests must produce the **expected stats** with
manual / scripted stimulus:

| Stimulus | Expected |
|---|---|
| 60 s still on desk | bins 0–1 filled, all others ≈ 0, 0 bouts. |
| 10 slow taps with 2 s gaps | ~10 bouts, short durations, high peak bin, gap histogram centered on 2 s. |
| 10 s continuous shake | 1–3 long bouts, high energy bin, high duration bin. |
| 30 s of walking the device across the desk in a straight line | 1 long bout, moderate peak, **low** direction-change count. |
| 30 s of "grooming" (rapid random multi-axis wiggle) | Many short bouts OR 1 long bout with **high** direction-change count. |

### Step 7 — Hourly rollover + NVS persistence

- Use ESP-IDF `nvs_flash` (or Arduino `Preferences` wrapper) to store up to
  **30 hourly records** keyed by hour index (circular).
- Write the current `HourStats` at the end of each hour, then reset RAM copy
  and increment hour index.
- `dump` reads from NVS and prints all records as CSV.

**Pass 7a** — short-interval test: temporarily set hour length to **60 s**,
run 5 minutes, `dump` shows 5 records, each with plausible distinct stats.

**Pass 7b** — reboot test: `dump` after reset still shows the 5 records.

**Pass 7c** — wear check: monitor `esp_timer_get_time` around `nvs_commit` —
target < 100 ms per hourly write.

### Step 8 — Self-test during startup

- At `setup()`, log: firmware version, ADXL362 ID, current hour index in
  NVS, number of stored records, time since last rollover (if any
  persisted), free heap, reset reason.

**Pass**: after a clean flash, `reset` reason is "power-on", all other
values are sensible. After a pulled-power test, `reset` reason is
"brownout/reset" and the prior hour's record is intact in NVS.

### Step 9 — Power-sipping mode during deployment

- Optional light-sleep between samples: at 25 Hz, we have ~35 ms of
  idle per sample. Light-sleep-between-samples should cut average current
  by ~4–5× with negligible impact on stats.
- Guard behind `#define POWER_SAVE 1`; default off for the bench sessions,
  on for deployment.

**Pass** (bench-measured with the `current_measurement/` jig, same port
as the Lora_cat power runs): average current ≤ 1.5 mA over 60 s with
POWER_SAVE=1, and the resulting hour record is statistically
indistinguishable from a 60 s run at POWER_SAVE=0 (same totals ± 2 %,
same histogram shapes).

### Step 10 — Real deployment

- Attach to cat with a light harness.
- Press `reset` over serial; confirm `status` shows "hour 0, N=0 samples".
- Disconnect USB. Cat wears for 24 h + some slack.
- Reconnect, `status` → confirm hour ≥ 24. `dump` → capture all records.

**Pass**: 24 records, no hour has `samples < 0.9 × expected_samples_per_hour`.
No NVS write failures. Battery didn't die.

### Step 11 — Offline analysis

Off-device (Python / notebook):

- Per-hour: plot magnitude histogram (diurnal pattern).
- Aggregate: joint histogram of (bout duration, bout peak) with user
  annotations overlaid. Manual annotations of "3 m+ displacement" events
  must cluster in one region of this plane.
- Propose `(THRESH_ACT, TIME_ACT)` pairs that produce an expected event rate
  matching the annotated truth, and compute:
  - **Recall**: fraction of annotated 3 m+ events that the proposed
    settings would catch.
  - **Precision**: fraction of detector firings that correspond to real
    3 m+ events (using the non-event annotations and the distribution of
    "everyday activity" bouts as the denominator).

**Pass**: produce a shortlist of 3–5 candidate `(THRESH_ACT, TIME_ACT)`
pairs with predicted recall ≥ 0.95 and precision ≥ 0.5. Document them in
the cat firmware's `TODO_ACCELEROMETER_ADXL362.md` as the starting
thresholds for that integration.

## 5. Risks / open questions

- **Battery life for 24 h.** Size the Li-Po accordingly, or restrict the
  first deployment to 12 h and extrapolate. Step 9 exists to make 24 h
  feasible.
- **Mounting.** Cat fur and harness motion may dominate the signal; the
  accel should ride as rigidly as possible against the harness shell.
- **ODR choice.** 25 Hz captures cat gait (1–3 Hz) comfortably (Nyquist
  × 8). 50 Hz doubles storage for no clear benefit; 12.5 Hz risks aliasing
  fast grooming twitches. Revisit if grooming distribution looks smeared.
- **Gravity leak.** If the cat changes posture slowly, the software
  gravity-removal (subtract 1 g from vector magnitude) is exact; the
  hardware HPF is approximate. Prefer software unless it costs too much
  CPU time at higher ODR.
- **NVS wear.** 24 writes/day × 1 year = ~9 k writes on a ~100 k-cycle
  flash with wear leveling. Fine for this diagnostic, not a concern.
- **Ground-truth annotations are manual and biased.** Mitigate by also
  recording a video of the cat during a few hours and aligning it
  off-line.
