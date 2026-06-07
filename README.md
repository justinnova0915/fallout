# TALOS-01 // Desktop Helper

The TALOS-01 is a desktop device designed to make navigating workspace-heavy, tiling operating systems (like Arch Linux Hyprland or macOS) more straightforward by making it physical. No more memorizing convoluted keyboard shortcuts just to move a window. This is my Fallout project for Hack Club, so I'm super excited to share it!

![Zine](https://github.com/justinnova0915/fallout/raw/master/Assets/Zine.png)

## Why TALOS-01?

Tiling window managers like Hyprland are great, but relying entirely on abstract keybinds can get annoying and lacks physical intuition. The goal of TALOS-01 is to turn window and workspace management into a tactile experience.

## Custom Features

- The device splits into a Main Console and a Macro Pad that snap together using strong magnet pogo pins.
- The device uses an MGN7H miniature linear rail with a button on it to let you grab and slide windows across active workspaces.
- Dedicated DMG19480T088_01WN screen displays useful widgets (time, weather, Discord call status) to free up your main monitor real estate entirely for games or code.
- A custom 3x3 macro pad module for standard macro inputs.
- A physical 5V analog needle voltmeter to display real-time metrics like CPU usage or core temperatures.
- Powerful ESP32 Controller for the brain of the device. Communicates with your PC over Bluetooth.
- Supports wireless power using two internal 18650 batteries, or wired over USB-C.

## Device Design

The TALOS-01 design is heavily inspired by the design language of *Arknights: Endfield*. The entire body uses a matching color palette of black, grey, white, and yellow, detailed with topographic lines and techy text accents.
![3D Model](https://github.com/justinnova0915/fallout/raw/master/Assets/Fallout_Render.png)

> Disclaimer: This project is a non-commercial, fan-made creation featuring visual and UI assets inspired by *Arknights: Endfield*, which remain the intellectual property of **Hypergryph**.

* **PCB and circuitry:**
It's built around a custom parent PCB that hosts every single chip. The board has exposed headers to connect to the moving slider handle and the macro pad (excuse any messy routing) ;).  
To power it, you can plug in USB-C to feed the TP4056 charging circuit, or let it run completely wireless off the 18650 batteries. Power leaving the circuit is stepped up using two independent MT3608 boost converters (a 12V line for the smart display and a 5V line for the ESP32 and voltmeter), before hitting an AMS1117-3.3 linear regulator for the shared sensor bus.
![PCB](https://github.com/justinnova0915/fallout/raw/master/Assets/PCB.png)

* **The Slider & Encoder:** The custom 3D-printed handle rides on the MGN7H rail and holds a DRV2605 driver with a motor to deliver physical haptic feedback clicks as you pass items. Distance tracking is calculated using two SS49E hall effect sensors positioned exactly a quarter turn from each other on the bottom of the block. All sensors and drivers tucked inside the moving handle connect back to the parent PCB via a Flat Flexible Cable (FFC).

* **Macro Pad:** To avoid a massive, fragile bundle of wires across the split-enclosure gap, I put a PCF8574 IO expander to condense the 3x3 matrix, gauge, and power button down into a simple 4-line I2C bus, meaning the whole wing connection only needs 7 physical pogo pins to bridge the gap! The analog needle gauge runs on a 5V scale, but the ESP32 can only natively supply a 3.3V logic signal. I put together a simple 2N3094 transistor circuit to shift the downstream signal up cleanly to the required 5V.

## Firmware and Software
The screen graphics are built and flashed to the hardware independently using DWIN's native configuration environment and software utility tools. This allows the smart display to handle its own graphics processing over an 8-pin connection to the ESP32. This stops the ESP32 from lagging out under heavy rendering, so it can focus purely on passing lightweight UI control commands over UART!

On the desktop side, a custom-written daemon in GOLANG runs continuously to translate signals coming from the ESP32 over Bluetooth and send information to the device.

## How to Build and Use

#### 1. Hardware Assembly
1. **PCB:** Get the board fabricated using the gerbers files inside `Fallout_PCB`.
2. **Components:** Grab all components listed in the [BOM](https://github.com/justinnova0915/fallout/blob/master/BOM.csv).
3. **3D Printing & Assembly:** Print the housing from `Fallout_CAD`, and assemble according to the 3d model.
4. **Wiring Diagram**  
![Wiring Diagram](https://github.com/justinnova0915/fallout/blob/master/Assets/Wiring.png)  
Wiring diagram for the main board  
![Macro Pad Wiring Diagram](https://github.com/justinnova0915/fallout/blob/master/Assets/Wiring_left.png)  
Macro Pad Wiring Diagram
#### 2. Flashing Firmware
- **Smart Display:** The screen graphics are built and flashed to the hardware independently using DWIN's native configuration environment and software utility tools call DGUS. Simply follow their Bilibili tutorial.
- **ESP32 Core:** Open the `Fallout_Software/Firmware` project in your environment, then build and flash using vscode's esp-idf tool.

#### 3. Desktop Software Setup
The desktop side uses a custom daemon written in `GO` that runs continuously to catch Bluetooth strings from the ESP32 and talk to your window manager.
1. Move into the daemon directory:
    ```bash
    cd Fallout_Software/Daemon
    ```
2. Build the binary:
    ```bash
    go build main.go
    ```
3. Boot it on startup by adding it to your `hyprland.conf`:
    ```ini
    exec-once = /path/to/talos-daemon
    ```

## Resources

#### BOM
I got most of my components in person in China, but I have found equivalents for around the same price on Taobao, so that is what my BOM is going to be based off of.

You can get the BOM [here](https://github.com/justinnova0915/fallout/blob/master/BOM.xlsx).

#### CAD
The CAD files are under [`Fallout_CAD`](https://github.com/justinnova0915/fallout/tree/master/Fallout_CAD). I designed it in Fusion so you can find the f3d and stp files there.

#### PCB
> Since I was using the breakout or module versions of most ICs (it's my first PCB!), I had to make custom symbol and footprints for them.

The KiCAD source files are under [`Fallout_PCB`](https://github.com/justinnova0915/fallout/tree/master/Fallout_PCB)

#### Code
The ESP32 code is written in `C++` using `ESP-IDF`, and the Hyprland Daemon is written in `GO`.

The Code is under [`Fallout_Software`](https://github.com/justinnova0915/fallout/tree/master/Fallout_Software)

## Credits

Thanks so much to the folks on Discord and Reddit for reviewing and helping me refine the electronics! Thanks also to Hack Club for running the Fallout event which sponsored this hardware build!

## License

This project is licensed under `GNU GPLv3`.