# v2 Rescue (factory-slot break-glass image)

Loaded when working in this tree. Never `pio run -t upload` this project (root Hard rules) — install via Master's `POST /firmware/rescue` or `esptool write_flash 0x830000`.

- **Factory slot (#193):** 2 MB `factory` app partition; bootloader factory reset (GPIO 4 low 5 s through reset) erases **otadata only** — never nvs (WiFi credentials must survive).
- **Rescue app (#195):** standalone project sharing nothing compiled with Master except the partition CSV — `Rescue*.h` pure headers are trimmed, natively tested copies. Boot: NVS read-only → 30 s STA join else `<name>-rescue` AP (captive) → slot inventory + upload-to-app0 (same `?md5=` contract) + `/rescue/exit`. Enter via GPIO 4 or `POST /firmware/rescue-boot` (409 while an install is in flight or the factory image is invalid). Master installs it via raw `esp_partition` writes (`FactorySlot.cpp` — flash sector 0 held back until the MD5 verdict, pure `FactoryChunkPlan.h`).

Custom bootloader + Master-side install path: `firmware/v2/Master/CLAUDE.md`.
