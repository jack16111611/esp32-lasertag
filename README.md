I am building a two-player, low-latency laser tag system using ESP32 microcontrollers and high-power modulated 38kHz infrared optics. The system consists of two custom blasters and two modular sensor harnesses.

Each blaster features an integrated 0.96" I2C OLED HUD displaying real-time ammo, health, and combat statistics, along with a transistor-boosted IR emitter and mechanical trigger/reload switches. Player vests and headbands house multiple VS1838B IR receivers connected to the blaster via a quick-disconnect 4-wire TRRS (3.5mm Aux) cable, routing independent signals for standard body damage and critical headshot multipliers. All real-time hit confirmations, player stats, and match coordination are synced peer-to-peer over ESP-NOW for sub-millisecond, router-free latency.

The electronics are unified on a custom 2-layer carrier PCB powered untethered by 18650 Li-ion cells with integrated USB-C charging.

Also this is my first time working with PCBs so cut me some slack.
