# Per-Row Harness

One harness per row. Carries 12 V + GND + RS-485 A + B from the master's
6-pin row output through 16 unit drops to a terminator at the far end.
**Single combined cable from master to harness — no separate brick at
the row.**

Four harnesses per 4-row system, one per row.

## Topology

```
   Master row port (6-pin: 12V/12V/GND/GND/A/B)
        |
        | combined cable (4 active conductors: 12V, GND, A, B)
        v
   +---------+ +-----+        +-----+              +-----+        +----+
   | Harness |-| U0  |        | U1  |    ...       | U15 |        | TR |
   | master  | +-----+        +-----+              +-----+        +----+
   | end     |    |              |                    |              |
   +---------+ +--+--+--+     +--+--+--+   ...   +--+--+--+      [120R]
                  | drop|     | drop|       ...      | drop|         |
   ============================ 4-conductor cable trunk =================== ===
                                                                           ^
                                                                    far-end term
```

- Master end of harness: **6-pin shrouded female connector** mating with
  the master's 6-pin row output. Pins 1+2 (12V) and 3+4 (GND) are
  paralleled together at the harness end onto the trunk's single 12V
  and single GND conductor.
- Trunk: 4-conductor cable carrying 12V, GND, A, B continuously through
  all 16 unit positions.
- Drops: 4-pin shrouded headers crimped onto the trunk at fixed unit
  spacing. Each unit plugs into its drop.
- Far end of harness: terminator plug (120 ohm across A/B) on the final
  drop or directly on the trunk.

## Cable specification

### Master-end combined cable (master to harness)

| Parameter | Spec |
|---|---|
| Conductor count | 4 active (12V, GND, A, B) |
| Connector ends | 6-pin shrouded female on both ends; pins 1+2 and 3+4 jumpered for current capacity |
| Conductor gauge | 22 AWG minimum for the 12V/GND legs; 24 AWG fine for A/B |
| Twisted pair | A/B should be a twisted pair for noise immunity |
| Cable type options | 4-conductor instrument cable, custom-built CAT5e (1 pair 12V parallel, 1 pair GND parallel, 1 pair A/B twisted, 1 pair spare), 4-conductor speaker wire with twisted-pair signal pair zip-tied alongside |
| Length | depends on master placement — typical 0.3-2 m. Cut and terminated at build time. |

### Harness trunk (within row, through 16 units)

| Parameter | Spec |
|---|---|
| Conductor count | 4 (12V, GND, A, B) |
| Conductor gauge | 22 AWG minimum |
| Twisted pair | A/B twisted; power runs alongside |
| Length | row span + ~0.5 m service loop |

**Voltage drop budget on 12 V leg (combined cable + harness trunk):**
- 16 units × ~250 mA peak = 4 A peak per row (rare).
- 22 AWG = ~0.053 ohm/m. For a 1 m master-to-harness cable + 1.5 m
  harness trunk, total round-trip resistance ~0.27 ohm. At 4 A peak =
  ~1.1 V drop. Acceptable.
- For longer total runs, step up to 20 AWG on the power conductors.

## Unit drop connector

- 4-pin shrouded box header, 2.54 mm pitch, 2x2, indexed (notched).
- Pinout: 1=12V, 2=GND, 3=A, 4=B.
- Mating connector: standard 4-pin housing.

## Termination plug

A small connector shell containing a 120 ohm 1% resistor between the A
and B pins. 1 plug per row.

## Stub length

- Stub = wire from trunk drop to unit board's connector.
- Keep stubs < 30 cm at 250 kbaud.
- Typical: 5-15 cm pigtail per unit.

## Parts list (per harness, all four rows take same parts)

| Item | Qty | Notes |
|---|---|---|
| 4-conductor cable for master-to-harness link | 1 | Length per master placement; 22 AWG minimum |
| 4-conductor cable for harness trunk | 1 | Length = row span + service loop; 22 AWG minimum |
| 6-pin shrouded female connector for master-side cable end (×2) | 2 | One end into master, one end into harness master-end |
| 4-pin shrouded box header for unit drops | 16 | One per unit position |
| 4-pin housing (mating to unit drop) | 16 | Trunk-side T-tap |
| 4-pin housing for unit-side cable | 16 | Cable from drop to unit (or unit-board connector if drop is plugged directly) |
| 120 ohm resistor 1% | 1 | Termination |
| Connector shell for terminator plug | 1 | Holds 120 R across A/B pins |

(Standard hobby parts; no specific MFGs required.)

## Master-end connector wiring detail

The 6-pin shrouded connector at the harness master end mates with the
master's 6-pin output. The 6 pins coming in are:

```
Pin 1 (12V)  ┐
             ├──── jumpered together at harness ──► single 12V conductor on trunk
Pin 2 (12V)  ┘

Pin 3 (GND)  ┐
             ├──── jumpered together at harness ──► single GND conductor on trunk
Pin 4 (GND)  ┘

Pin 5 (A)   ──────────────────────────────────────► A conductor on trunk
Pin 6 (B)   ──────────────────────────────────────► B conductor on trunk
```

The doubled 12V/GND pins on the connector exist for current-carrying
capacity through the connector itself (~3 A per pin × 2 pins = ~6 A
margin per leg). The harness trunk only needs 1 conductor per net
because the trunk wire is sized for the full 4 A peak.

## Why no rigid backplane

- A backplane is a 4th PCB design (and 4 builds for 4 rows).
- A harness is buildable with off-the-shelf cable + connectors and
  hobbyist tools.
- One step up from v1's per-unit cable mess: still cabled, but
  trunk + drops instead of point-to-point.

## When a backplane *would* be preferred

If during build the user discovers that:
- The case has a cleaner mounting solution that suits a long PCB.
- 16 hand-crimped IDC drops per row is more tedious than 4 PCBs.

A drop-in passive backplane PCB can be added later that mates to the
same 4-pin unit connector. The unit PCB does not need any change.

## BOM

This `HARNESS.md` is the canonical spec. There is no separate
`HARNESS_BOM.csv` because the harness is a cable assembly, not a
fab-orderable PCB.
