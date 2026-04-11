# Labubu Bop It

An ESP32-S3-based interactive "Bop It" game housed inside a Labubu figure. Built as a junior design project for ECE 1895 at the University of Pittsburgh.

**Team:** Noel Johnbosco, Ethan Getgen, Aaron Yang

![Labubu Concept](docs/labubu_concept.png) <!-- Replace with your actual image path -->

## Overview

The Labubu Bop It issues randomized commands that the player must respond to within a shrinking time window. Commands span four input types:

- **Tilt / Shake / Flip** — Physical gestures detected by a BNO055 9-DOF IMU with onboard sensor fusion
- **Scream at it!** — A PDM MEMS microphone monitors RMS amplitude to detect loud audio input
- **Twist the ear** — A rotary potentiometer mounted at the Labubu's ear registers twist-to-target actions
- **Screen alignment** — The SPI LCD displays directional targets; the player tilts the figure to align a cursor into the target zone

The game gets progressively harder — the command pool grows, the response window shrinks, and precision targets tighten.

## Hardware

### Schematic

<!-- Add your schematic screenshots/exports here -->
![MCU Sheet](docs/schematic_mcu.png)
![Power Sheet](docs/schematic_power.png)
![Peripherals Sheet](docs/schematic_peripherals.png)

### PCB

4-layer PCB designed in Altium Designer. Fabricated by JLCPCB.

![PCB Layout](docs/pcb_layout.png)
![PCB 3D](docs/pcb_3d.png)

### Key Components

| Component | Part | Interface |
|---|---|---|
| MCU | ESP32-S3-WROOM-1 (N16R8) | — |
| IMU | BNO055 | I2C |
| Microphone | PDM MEMS | I2S PDM |
| Display | ILI9341 SPI TFT LCD | SPI |
| Audio | DFPlayer Mini | UART |
| LEDs | WS2812B NeoPixels | Single-wire GPIO |
| Potentiometer | Rotary pot (ear twist) | ADC |

### Power

- 2S Li-ion battery input
- 3.3V buck converter
- 5V LDO for peripherals

## Firmware

Built with Arduino + PlatformIO targeting the ESP32-S3.

### Game State Machine

```
[IDLE] → [COUNTDOWN] → [ISSUE_COMMAND] → [WAIT_FOR_INPUT]
                              ↑                    |
                              |          correct / wrong / timeout
                              |                    |
                         [SCORE++]           [GAME_OVER]
```

### Gesture Detection

Threshold-based classification on BNO055 quaternion and linear acceleration data:

- Tilt: roll/pitch exceeds ±30°
- Shake: accel magnitude > 2.5g within 500ms
- Twist: yaw rate > 90°/s sustained for 200ms
- Scream: RMS amplitude exceeds loudness threshold

## Repo Structure

```
├── firmware/          # PlatformIO project (ESP32-S3)
├── hardware/          # Altium project files, Gerbers, BOM
├── docs/              # Schematics, images, reports
└── README.md
```

## License

MIT
