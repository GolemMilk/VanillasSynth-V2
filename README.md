VanillaSynth V2

Raspberry Pi Pico 2 based polyphonic digital synthesizer.

RP2040 dual-core architecture, I2S DAC output, MIDI input and realtime FX.

⸻

Overview

VanillaSynth V2 is a DIY polyphonic synthesizer built around the Raspberry Pi Pico 2.

It is designed to be simple, stable and expressive while running entirely on a microcontroller.

Features include polyphonic oscillators, analog-style drift, filters and realtime effects.

⸻

Features

• Raspberry Pi Pico 2
• 4 voice polyphonic virtual analog synthesizer
• I2S audio output (PCM5102A DAC)
• MIDI input (UART + 6N138 / 6N136)
• 74HC4051 analog multiplexer for knob expansion
• SSD1306 OLED display UI
• Dual-core architecture

⸻

Effects

• Phaser
• Ladder filter
• Tap ping-pong delay
• Octaver
• Soft clip limiter

⸻

Hardware

Main components

Raspberry Pi Pico 2 — Main MCU
PCM5102A — I2S DAC
6N138 / 6N136 — MIDI input
74HC4051 — Analog multiplexer
SSD1306 OLED — Display

⸻

Controls

8 potentiometers via 74HC4051

A — Attack
B — Tone (LPF cutoff)
C — Tail / Release
D — Volume
E — Phaser mix
F — Ladder mix
G — Delay mix
H — Octaver mix

Waveforms selectable via hardware switch.

Sine
Saw
Square
Triangle

⸻

Audio Architecture

44.1kHz audio
16bit stereo output
I2S DAC

RP2040 dual core separation

Core0
UI / MIDI / OLED

Core1
Audio engine

⸻

Firmware

Main firmware file

VanillaSynth_V2.ino

Additional module

MidiQueue.h

Developed with

Arduino IDE
RP2040 core

⸻

License

Firmware
MIT License

Hardware
CERN Open Hardware License v2

⸻

Author

GolemMilk

DIY synthesizer experiments and electronic music tools.

GitHub
https://github.com/GolemMilk

⸻

Notes

This project is experimental DIY hardware.

Feel free to modify, build and improve.

Pull requests welcome.
