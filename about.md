# Fast Frame Windows

Shows how many frames you had for every click, fast.

Red is how many frames earlier a click could have been. Blue is how many frames later. Together they make the window.

## Why it is fast

Instead of replaying the whole level again and again, the mod saves the game state a few frames before each click while you play. To measure a click it jumps to that saved state, moves the click, and runs only a short stretch of the level. It finds each side of the window by halving the range rather than trying every shift.

## Use

**Your own run**

1. Play a level (normal or practice mode).
2. Pause and press the red **FW** button, then **Analyze**.

**A macro**

1. Open the level, pause, press **FW**, then **Import macro** and choose a `.gdr2` file.
2. The macro is replayed in the level, then measured. The summary shows where the macro died, or that it finished the level.

**After analyzing**

The level plays back on its own with a coloured ring, the window number and a bell on every click, plus an overview of window sizes in the top left. Press **Playback** to watch it again. Every list row has a red and blue bar for the frames earlier and later.

## Settings

Ring size and colours, number size, position and time, overview size, which window sizes are shown or hidden, sound volume and range, the part of the run to analyze, and whether the level stays visible while analyzing.

## Good to know

- Imported macros must be for the same level and version. Their timing is matched to the game automatically.
- Measured with 240 physics steps per second. Leave TPS at 240 and turn off other bots or physics mods while analyzing.
- Clicks that cannot be reproduced from the saved state are marked and left out of the averages.
- A plus sign means the window was limited by the range tested or by the next click, so it is at least that large.
- Windows are measured by running the level, so they depend on the level behaving the same way each time.

## Credits

The idea comes from Razy's Frame Windows by tristi. This mod is an independent, from-scratch implementation and shares no code or assets with it.
