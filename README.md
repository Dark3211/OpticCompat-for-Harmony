# OpticCompat for Harmony

## 1. Installation

1. Build the project as **Release | Win32**.
2. Copy the generated file to:

```text
Halo Custom Edition\mods\harmony.dll
```

3. Install `optic.lua` as a Chimera global script:

```text
<Chimera profile>\lua\scripts\global\optic.lua
```

4. Put `optic.json` and medal packs in:

```text
<Chimera profile>\lua\data\global\optic\
```

Example:

```text
lua\data\global\optic\
├── optic.json
├── halo_4\
└── halo_infinite\
```

---

## 2. New Medal Pack Structure

Each medal pack uses this structure:

```text
halo_infinite\
├── sprites.style
├── images\
│   ├── double_kill.png
│   ├── triple_kill.png
│   ├── headshot.png
│   ├── hitmarker.png
│   └── ...
└── sounds\
    ├── double_kill.mp3
    ├── triple_kill.mp3
    ├── headshot.mp3
    └── ...
```

`sprites.style` is JSON. A minimal file is:

```json
{
  "medalSizeFactor": 15
}
```

Medal images use **PNG** and medal sounds use **MP3**. To attach a sound to a medal, use the same base filename:

```text
images/double_kill.png
sounds/double_kill.mp3
```

Transparent PNG files are recommended.

---

## 3. New Hitmarker / HeadShot Assets

Modern packs can include these new assets:

```text
images/hitmarker.png
images/hitmarker_critical.png
images/hitmarker_shield.png
images/hitmarker_shield_broken.png
images/hitmarker_vehicle.png
images/hitmarker_kill.png

images/hitmarker_critical_icon.png
images/hitmarker_shield_icon.png
images/hitmarker_shield_broken_icon.png
images/hitmarker_vehicle_icon.png

images/headshot.png
sounds/headshot.mp3
```

Meaning:

- `hitmarker.png` — normal hit.
- `hitmarker_critical.png` — critical hit.
- `hitmarker_shield.png` — damage while the target has shield.
- `hitmarker_shield_broken.png` — impact that breaks the shield.
- `hitmarker_vehicle.png` — damage to a player in a vehicle.
- `hitmarker_kill.png` — lethal hit.
- `*_icon.png` — optional status icon shown with that hitmarker type.
- `headshot.png` / `headshot.mp3` — HeadShot medal and sound.

Normal and critical hitmarkers can be recolored by the player. Keep their artwork clean and use transparency so the color override works well.

---

## 4. Color Commands

Show the 20 available colors:

```text
ocolors
```

Show the current selections:

```text
ocolor show
```

Change colors independently:

```text
ocolor hit 10
ocolor critical 2
ocolor damage 1
ocolor damage_critical 2
```

Names can also be used:

```text
ocolor hit cyan
ocolor critical light_red
ocolor damage white
ocolor damage_critical red
```

Available colors:

```text
1  white
2  light_red
3  red
4  orange
5  gold
6  yellow
7  lime
8  green
9  mint
10 cyan
11 sky
12 blue
13 indigo
14 violet
15 purple
16 magenta
17 pink
18 silver
19 gray
20 teal
```

The selected colors are saved in `optic.json`.