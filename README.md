# PomoBit
## A Pomodoro Clock With THe BitDogLab Development Board~
PomoBit is a pomodoro clock. A pomodoro clock is a tool used by students to organize their studies using the pomodoro technique. The pomodoro technique consists of dividing your studies into study cycles (25min-50min) and rest (5min-10min).

PomoBit is a project that was born out of a difficulty faced by students and educators who tried to apply the Pomodoro technique, but noticed that when they used Pomodoro timer apps or websites they ended up being distracted and/or alluded to social networks such as “Instagram”, “TikTok”, “YouTube” etc. Thus, there is a need for an “offline” tool that allows students to get away from electronic devices that are constantly trying to “steal” the user’s attention.

## Features
- Time indicator
- Set study and rest intervals
- Alert when a study cycle is over

## Technologies and Components
### Hardware:
- Raspberry Pi Pico: A microcontroller board based on the RP2040 chip.
- GPIO (General Purpose Input/Output) pins: Used for connecting buttons, LEDs, and sensors.
- NeoPixel (WS2812B) LED Strip: Individually addressable RGB LEDs controlled via PIO.
- Joystick Module: Includes an analog stick (ADC input) and button (GPIO input).
- Push Buttons: Used for changing modes and pausing the timer.
### Software & Programming Technologies:
- C Programming Language: The code is written in C, optimized for embedded systems.
- Pico SDK (Software Development Kit): Provides essential libraries for handling GPIO, ADC, and timers.
- PIO (Programmable Input/Output) on RP2040: Used to control the WS2812B LED strip efficiently.
- Multitasking using Timers & Absolute Time: Uses absolute_time_t and sleep_ms() for timing operations.
### Embedded System Features:
- State Machine Design: Implements different states (STATE_STUDY, STATE_REST, STATE_PAUSED, STATE_CONFIG).
- Debouncing for Buttons: Ensures reliable button presses using state tracking.
- Analog-to-Digital Conversion (ADC): Reads joystick values to configure timer durations.
- LED Matrix Visualization: Represents time using LEDs, providing visual feedback
### Algorithms & Concepts:
- Timers & State Transitions: Manages study/rest durations.
- PWM & GPIO Output: Controls LED status indicators.
- User Input Handling: Processes button and joystick actions.
- Visual Feedback System: Uses LED matrix to show time remaining.
