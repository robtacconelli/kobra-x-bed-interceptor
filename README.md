# Anycubic Kobra X — Bed Interceptor

An Arduino-based device that eliminates LED flicker caused by the Anycubic Kobra X's heated bed when running on a battery-powered PV inverter.

## The problem

When my house was running on battery mode (Anern AN-SCI02-PRO-6200 hybrid inverter), every LED in the house flickered noticeably while the 3D printer was running. The flicker was rhythmic at roughly 2–3 Hz, perfectly correlated with the printer's heated bed thermal cycling.

The flicker disappeared when the bed was off, regardless of any other printer activity.

## Root cause

The Kobra X uses a **1000 W mains-voltage silicone heated bed** controlled by the mainboard via a triac driver. To deliver fractional power without the long thermal cycles of bang-bang control, the firmware uses **proportional pulse-width firing**: short trigger pulses every ~80 ms, with pulse widths varying from 1 ms (minimum demand) to ~10 ms (maximum demand within a slot).

This works fine on grid power. But on battery mode, every pulse causes a brief AC waveform distortion as the inverter's regulation loop responds to the load step. Each distortion event is a brief brightness blip in cheap LED drivers — repeated 12 times per second, this creates visible flicker.

![AC waveform distortion when bed turns on](images/ac-distortion-with-bed.png)
*The AC waveform becomes visibly "pointier" when the bed switches on (4th half-cycle in the trace). Each pulse from the bed causes a similar brief distortion.*

## The solution

Intercept the bed control signal and convert the fractional pulse-width firing into **slow bang-bang cycles** — much fewer transitions, much less frequent flicker. The bed gets the same average power but in larger, less frequent chunks.

An Arduino UNO R4 WiFi sits between the mainboard and the PSU board:
1. Reads the mainboard's IO control signal
2. Measures the average duty cycle over a 10-second window
3. Halves the requested ON time (damping factor to absorb thermal inertia)
4. If the resulting ON time is ≥ 3 seconds, fires the output for that duration; otherwise skips
5. Returns to measurement mode for the next window

The mainboard's PID loop self-compensates: when the bed cools below setpoint, the next window's measured demand is higher, eventually crossing the 3-second threshold and triggering a "real" heating burst.

### What you observe with the interceptor running

- **Cold start (warmup)**: bed reaches setpoint in roughly 2× the normal time. Output is mostly continuous HIGH because mainboard demand is 100%.
- **Steady state**: one heating burst every ~30–60 seconds, lasting 3–5 seconds. Bed temperature oscillates ±2–3°C around setpoint.
- **LED flicker**: completely gone in steady state. Only one barely-noticeable transition per minute or so, instead of constant strobing.

## Hardware

- Anycubic Kobra X (latest firmware early 2026 - hardware revision TDX-026)
- Arduino UNO R4 WiFi (`R7FA4M1AB3CFM` / Renesas RA4M1)
- A few wires, optional small project box

The Arduino is powered from the printer's own 5V rail (available on the same connector as the IO signal).

### Why UNO R4 specifically

I initially tried an ESP32 (works, but needs voltage divider for the 5V signal) and an Arduino Pro Mini 5V (worked briefly, then the input pin appeared to fail — probably damaged by accumulated transient spikes from the triac switching environment). The UNO R4 has more robust I/O protection and 5V-tolerant pins through onboard level shifters, making it the most reliable choice for this dirty signal environment.

If you replicate this and use a smaller/cheaper board, **add input protection**: 1 kΩ series resistor, plus Schottky clamp diodes to GND and VCC.

UNO R4 works just fine as it is without any additional component.

## Wiring

The Kobra X PSU board (TDX-026) has a 5-pin connector linking to the mainboard, labeled:

```
GND | ADC | 5V | IO | ZERO
```

- **GND**: ground reference
- **ADC**: analog feedback (bed current sense) → mainboard. Don't touch.
- **5V**: 5V rail from PSU. Tap into this for Arduino power.
- **IO**: bed heat control signal from mainboard → triac driver. **This is what we intercept.**
- **ZERO**: zero-crossing detection signal → mainboard. Don't touch.

![Kobra X PSU board with the 5-pin connector visible](images/psu-board-closeup.png)
*The PSU board (TDX-026). The white 5-pin connector at top-left carries the GND/ADC/5V/IO/ZERO wires from the mainboard.*

### Wiring diagram

```
                ORIGINAL CABLE (5 wires)
        ┌───────────────────────────────────────┐
        │ GND ──────────┬───────────────► PSU   │
        │ ADC ──────────┼───────────────► PSU   │ (untouched)
Mainboard 5V ──────────┼─┬─────────────► PSU   │
        │ IO  ──╳ CUT  │ │                     │
        │ ZERO ─────────┼─┼─────────────► PSU   │ (untouched)
        └───────────────┼─┼─────────────────────┘
                        │ │
                Arduino:│ │
                  GND ──┘ │
                  VIN ────┘
                  D2  ← mainboard side of cut IO wire
                  D3  → PSU side of cut IO wire
```

Only the **IO** wire is cut. All other wires remain physically connected end-to-end.

The Arduino reads the mainboard's intent on D2, processes it, and drives D3 with the modified bang-bang signal.

## Theory of operation

### The mainboard's native control scheme

The Kobra X's bed control signal observed on a scope:

| Phase | Bed temperature | IO signal pattern |
|-------|----------------|-------------------|
| Cold warmup | Far from setpoint (>15°C error) | Continuous HIGH (100% duty) |
| Approach | Close to setpoint (5–15°C error) | Wide pulses, ~10 ms each, every 80 ms |
| Steady state | At setpoint (<5°C error) | Narrow pulses, 1–2 ms each, every 80 ms |

![Narrow pulses during steady-state hold](images/io-signal-narrow-pulses.png)
*Steady-state hold: brief 1–2 ms triggers every ~80 ms deliver about 12% bed power. Each pulse causes a brief AC distortion event → continuous LED flicker.*

![Wide pulses during approach to setpoint](images/io-signal-wide-pulses.png)
*Approach phase: wider 8–10 ms pulses for higher power delivery. Still at the 80 ms repetition rate.*

### What the interceptor does

Every 10 seconds:
1. Sample the IO signal at 2 kHz, count HIGH samples
2. Compute average duty cycle
3. Halve it (damping factor) to estimate the bang-bang ON time
4. If under 3 seconds, do nothing (output stays LOW); the bed cools, mainboard demand grows, eventually a window measures high enough to trigger
5. If over 3 seconds, drive output HIGH for that duration. Continuously monitor the input during this — if the mainboard goes silent for 1.5+ seconds (setpoint reached), abort the burst early to avoid overshoot.

### Why halve the requested time?

Because the bed has significant **thermal inertia**. When the mainboard requests, e.g., 70% duty, applying 70% via bang-bang causes overshoot — by the time we stop heating, residual heat in the aluminum bed plate continues raising temperature. Halving the requested time absorbs this inertia: the mainboard sees the bed reaching temperature with smaller-than-expected energy input, so it requests a higher duty next window, and the system self-balances.

### Why skip short bursts instead of accumulating them?

I tried accumulation first ("save up demand until threshold reached"). It overshoots terribly: bed cycles small demands at first, accumulating lots of "thermal debt", but by the time the debt threshold is crossed the bed might already be at temperature — firing 5+ seconds of full power then sends it 10°C over setpoint.

The mainboard's PID is the right place to integrate demand. If the bed is genuinely cool, the mainboard will demand high duty, and the next window will fire. If the bed is fine, demand stays low, and we keep skipping. No need for our own integrator on top of theirs.

## Tuning

The four parameters at the top of the sketch can be adjusted to taste:

| Parameter | Default | Effect of increasing |
|-----------|---------|---------------------|
| `WINDOW_MS` | 10000 | Less frequent flicker, larger temperature ripple |
| `DAMPING_FACTOR` | 0.5 | Higher bed temperature average, more overshoot |
| `MIN_BURST_MS` | 3000 | Less frequent flicker, larger temperature ripple |
| `IDLE_TIMEOUT_MS` | 1500 | Slower overshoot prevention, smoother ON bursts |

If the bed can't reach setpoint, increase `DAMPING_FACTOR` to 0.6–0.7. If it overshoots, decrease to 0.3–0.4. The 0.5 default works well for a Kobra X reaching 60–80°C bed temperatures.

## Files

- `src/bed_interceptor/bed_interceptor.ino` — the Arduino sketch
- `images/` — scope traces and hardware photos documenting the project
- `docs/` — additional documentation

## Caveats

This modification voids the printer's warranty. The Arduino sits inline with a low-voltage signal — there's no mains exposure on the modification side — but you are altering the bed control behavior. Thermal runaway protection still works (mainboard's responsibility, unaffected), but if the Arduino crashes the bed will go cold rather than hot.

I've run this for hundreds of print hours without issue. Your mileage may vary.

Anyway you can always restore previous behavior of the printing just resolder the IO wire removing UNO R4.

You are not touching any board, just the cable running from the control board to the power supply.

## License

Apache 2.0
