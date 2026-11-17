# Prospero Disk icon studies

Original artwork for this project. These are candidates, not packaged assets.

The first three SVGs explore the sunburst, orbital disk and Directory D.
`comparison.png` shows those flat concepts and background compositions.

The selected Directory D is reconstructed as solid beveled geometry by
`render-glass.py`. Run from the repository root with Blender 5.3:

```sh
blender --background --python art/options/render-glass.py
```

The script now writes three revised, clear colored glass scenes and PNGs:

| File stem | Treatment |
| --- | --- |
| `07-clear-color` | Thin polished colored glass |
| `08-prismatic-color` | Medium depth and stronger refraction |
| `09-deep-color` | Thick saturated glass with the strongest refraction |

These use full transmission, very low roughness, colored connectors and spine,
and narrow edge lights instead of large white reflections. A graduated studio
card behind the glass is visible to transmission rays but hidden from direct
camera rays, preserving the dark tile while lighting the transparent material.

The earlier, paler studies are retained for comparison:

| File stem | Treatment |
| --- | --- |
| `04-clear-glass` | Clear blue, violet and cyan glass; pale ice spine |
| `05-frosted-glass` | Roughened glass with softer transmission |
| `06-jewel-glass` | Thicker glass with a higher refractive index |

Each PNG is 1024 × 1024, opaque RGB, with a navy-to-violet background.
An orthographic camera keeps the symbol within the central 80%; a slight
angle reveals depth. The background has no drop shadow or rounded corners.
The scenes share geometry and lighting so the material differences are
easy to compare. Cycles uses CPU rendering by default. Append `-- --metal`
to opt into Metal acceleration (the first run may take time to compile shaders).

The meshes preserve the SVG's ring dimensions and segment colors, with
lighter glass tints to keep transmitted colors readable. No external models,
textures or HDRIs are used. Console appearance has not been verified.

After choosing a treatment, its scene can also supply the widescreen
selection and launch compositions described in `../README.md`.
