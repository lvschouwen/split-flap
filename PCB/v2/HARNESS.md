# Per-Row Harness

One harness per row. Carries 12 V + GND + RS-485 A + B from a row 12 V
brick at the master end through 16 unit drops to a terminator at the far
end. No PCB. No backplane.

Four harnesses per 4-row system, one per row.

## Topology

```
   +-----------+        +-----+        +-----+              +-----+        +----+
   | Row brick |--+     | U0  |        | U1  |    ...       | U15 |        | TR |
   | 12V/5A    |  |     +-----+        +-----+              +-----+        +----+
   +-----------+  |        |              |                    |              |
                  |     +--+--+--+     +--+--+--+   ...   +--+--+--+      [120R]
                  |     | drop|     ... | drop|       ...      | drop|       |
   ============================ 4-conductor cable trunk =================== ===
                  ^        ^              ^                    ^              ^
                  |        |              |                    |              |
              power +    drop 0        drop 1               drop 15      far-end
              signal                                                      term
              entry
              from
              master
```

- Trunk: 4-conductor cable carrying 12V, GND, A, B continuously through
  all 16 unit positions.
- Drops: 4-pin shrouded headers crimped onto the trunk at fixed unit
  spacing. Each unit plugs into its drop with a short stub cable (or
  directly).
- Master end of harness:
  - 12 V + GND from the row's 12 V brick (through a fuse holder or
    barrel-jack adapter).
  - RS-485 A + B from the master's row port (3-pin connector).
- Far end of harness:
  - Terminator plug: 120 ohm resistor across A/B in a small connector
    shell. Plugs into a final 4-pin drop or directly onto the trunk.

## Cable specification

| Parameter | Spec |
|---|---|
| Conductor count | 4 |
| Conductor function | 12V, GND, A, B |
| Conductor gauge | 22 AWG minimum (24 AWG acceptable for short rows < 1.5 m) |
| Insulation | PVC or similar, hobby-rated |
| Jacketing | Round or flat ribbon |
| Shielding | optional; bond to GND at master only if present |

**Voltage drop budget on 12 V leg:**
- 16 units × 250 mA peak = 4 A peak (rare, all motors stepping
  simultaneously).
- 22 AWG: ~0.053 ohm/m. Round-trip 2 m at 4 A = ~0.4 V drop. Acceptable.
- 24 AWG: ~0.084 ohm/m. Round-trip 2 m at 4 A = ~0.7 V drop.
- Above 2 m row length, step up to 20 AWG or feed power at both ends of
  the harness.

## Unit drop connector

- 4-pin shrouded box header, 2.54 mm pitch, 2x2, indexed (notched).
- Pinout: 1=12V, 2=GND, 3=A, 4=B.
- Mating connector: standard 4-pin housing (Dupont-style or shrouded IDC).

For a flat ribbon trunk (1.27 mm pitch): a 4-pin IDC connector
mass-terminates onto the ribbon. Suitable for short, neat rows.

For a round 4-conductor trunk (e.g., 22 AWG hookup wire): T-tap each unit
position with a 4-pin "drop" header soldered onto the trunk. Less neat
but more robust at higher current.

## Termination plug

A small connector shell containing a 120 ohm 1% resistor between the A
and B pins. Plugs into the harness's final unit drop position (or into a
dedicated terminator drop). 1 plug per row.

## Master end

The master's row output connector (3-pin: A, B, GND) connects to the
harness signal wires. The harness's 12 V + GND wires connect to the row
brick (via a barrel-jack adapter or screw-terminal block at the master
end of the harness).

This means the master's row output cable is **3-conductor** (A, B, GND)
and runs from master to the master end of each row's harness. The 12 V
to the harness comes from the row brick at the same point — not from
the master.

## Stub length

- The "stub" is the wire from the trunk drop to the unit board's
  connector.
- Keep stubs < 30 cm at 250 kbaud. Shorter is better.
- Typical: 5-15 cm pigtail per unit.

## Parts list (per harness)

| Item | Qty | Notes |
|---|---|---|
| 4-conductor cable, 22 AWG, suitable length | 1 | Length = row span + ~0.5 m service loop |
| 4-pin shrouded box header, 2.54 mm | 16 | Drops, one per unit position |
| 4-pin housing for unit-side cable | 16 | Mating to unit drop |
| 4-pin housing for trunk-side | 16 | Mating to drop header (or IDC mass-term) |
| 120 ohm resistor 1% | 1 | Termination |
| Connector shell for terminator plug | 1 | Holds 120 R across A/B pins |
| 5 A inline fuse holder + 5 A fuse | 1 | Row power input fuse |
| Barrel-jack adapter or screw terminal block | 1 | Row brick connection |
| 3-pin shrouded box header connector | 1 | Mates to master's row port |

(All standard hobby parts; no specific MFGs required.)

## Why no rigid backplane

- A backplane is a 4th PCB design (and × 4 builds for 4 rows).
- A harness is buildable with off-the-shelf cable + connectors and tooling
  available to hobbyists (crimper, soldering iron).
- No mechanical fit issue — harness conforms to whatever the case provides.
- No PCBA cost for backplanes.
- One step up from v1's per-unit-cable mess: still cabled, but trunk +
  drops instead of point-to-point.

## When a backplane *would* be preferred

If during build the user discovers that:
- The case has a cleaner mounting solution that suits a long PCB.
- 16 hand-crimped IDC drops per row is more tedious than 4 PCBs.
- The 12 V trunk gauge becomes awkward.

A drop-in passive backplane PCB can be added later that mates to the same
4-pin unit connector. The unit PCB does not need any change.

## BOM

This `HARNESS.md` is the canonical spec. There is no separate
`HARNESS_BOM.csv` because the harness has no fab-orderable PCB component
— it is a cable assembly.
