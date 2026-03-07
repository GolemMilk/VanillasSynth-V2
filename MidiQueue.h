#pragma once
#include <Arduino.h>

// ===================== MIDI EVENT QUEUE (Header to beat Arduino prototypes) =====================
struct MidiEvt {
  uint8_t type;   // 1=NOTE_ON, 2=NOTE_OFF
  uint8_t note;
  uint8_t vel;
};

static const uint8_t EVT_NOTE_ON  = 1;
static const uint8_t EVT_NOTE_OFF = 2;

static const uint32_t EVT_QMASK = 63;     // 64 ring

static volatile uint32_t evtW = 0;
static volatile uint32_t evtR = 0;
static MidiEvt evtQ[64];

static inline void evt_push(uint8_t type, uint8_t note, uint8_t vel) {
  uint32_t w = evtW;
  uint32_t n = (w + 1) & EVT_QMASK;
  if (n == evtR) return; // full -> drop
  evtQ[w].type = type;
  evtQ[w].note = note;
  evtQ[w].vel  = vel;
  evtW = n;
}

static inline bool evt_pop(MidiEvt &out) {
  uint32_t r = evtR;
  if (r == evtW) return false;
  out = evtQ[r];
  evtR = (r + 1) & EVT_QMASK;
  return true;
}
