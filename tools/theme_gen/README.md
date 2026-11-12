# Soft theme generator

Generates the 13 soft-skin themes in `resources/themes`:
Soft (Light, Dark, OLED), Blossom, Grove, Ocean, Ember and Iris (each Light and Dark).

```sh
python3 -I -B tools/theme_gen/gen.py resources/themes          # regenerate
python3 -I -B tools/theme_gen/gen.py --check resources/themes  # verify (non-zero exit on any difference)
```

- `palettes.json` holds the colour tokens (OKLCH). They are adapted from the built-in
  themes of [T3 Code](https://github.com/pingdotgg/t3code), MIT licence, see
  `LICENSE-T3Code.txt`. Theme names were changed to neutral ones.
- `Soft OLED` is `Soft Dark` on pure black (#000000) with slightly brighter borders.
- Each theme's `"Soft"` block switches on SatDump's soft skin (`src-core/imgui/imgui_soft.h`).
- The themes use the Inter font (`resources/fonts/Inter-Regular.ttf`, SIL Open Font
  Licence 1.1, see `resources/fonts/Inter-LICENSE.txt`).
