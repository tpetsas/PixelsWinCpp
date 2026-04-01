# C++ Pixels Dice Library For Windows

This is the C++ Pixels library for Windows.
Windows Runtime [WinRT](https://github.com/microsoft/cppwinrt) APIs are used
to access Bluetooth.

Windows 10 version 1709 (Fall Creators Update) or more recent is required.

## Foreword

Pixels are full of LEDs, smarts and no larger than regular dice, they can be
customized to light up when and how you desire.
Check our [website](https://gamewithpixels.com/) for more information.

> **Warning**
> Before jumping into programming please make sure to read our Pixels developer's
> [guide](https://github.com/GameWithPixels/.github/blob/main/doc/DevelopersGuide.md).

Please open a [ticket](https://github.com/GameWithPixels/PixelsWinCpp/issues)
on GitHub if you're having any issue.

## Overview

This is the initial release of our Pixels dice C++ library for Windows.

Please not that all the library source code is currently included in the example app.

As such you will need to either copy the library code in your own project or modify
this project to generate a library with the configuration settings that your project
requires.

There are many ways to configure a Visual Studio C++ library project and we didn't
want to pick one that is unsuitable for our users.
So we decided to postpone that decision until we get some community feedback that will
inform us of the most popular configuration needs.

Thank you for your understanding!

## Documentation

See the library documentation [here](https://gamewithpixels.github.io/PixelsWinCpp/modules.html).

Documentation generated with [Doxygen](https://www.doxygen.nl).

## Example App Modes

The built example app supports three modes:

- `--list`: Scan and print nearby Pixels dice.
- `--setup`: Scan, choose 1 or 2 dice, and save `pixels.cfg`.
- no arguments: Load `pixels.cfg` and connect to the configured die/dice.
- `--rolls-only`: In normal mode, print only roll result lines (minimal output).

`pixels.cfg` is saved in the app working directory using this format:

```ini
version=1
dice=0xAABBCCDD,0x11223344
```

### Quickstart

1. Discover nearby dice:

   ```powershell
   Pixels.exe --list
   ```

2. Choose your active die/dice and save config:

   ```powershell
   Pixels.exe --setup
   ```

3. Run normally with the saved config:

   ```powershell
   Pixels.exe
   ```

### Entry Point Notes

- `app_main.cpp` is the config-based multi-die example app entry point used by the current build.
- `main.cpp` is kept as a simpler single-die sample/test entry point for quick reference.

## License

MIT
