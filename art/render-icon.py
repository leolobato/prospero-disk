# Prospero Disk - Render the selected Deep Colour icon at production resolution.
# Copyright (C) 2026 Leonardo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later

"""Run: blender --background --python art/render-icon.py"""

from pathlib import Path
import sys
import bpy
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Vector

art = Path(__file__).resolve().parent
bpy.ops.wm.open_mainfile(filepath=str(art / 'options/09-deep-color.blend'))
scene = bpy.context.scene
scene.cycles.device = 'CPU'
if '--metal' in sys.argv:
    preferences = bpy.context.preferences.addons['cycles'].preferences
    preferences.compute_device_type = 'METAL'
    preferences.get_devices()
    assert any(device.type == 'METAL' for device in preferences.devices)
    for device in preferences.devices:
        device.use = device.type == 'METAL'
    scene.cycles.device = 'GPU'
scene.cycles.samples = 1024
scene.cycles.use_adaptive_sampling = True
scene.cycles.adaptive_threshold = .005
scene.cycles.adaptive_min_samples = 64
scene.cycles.use_denoising = True
scene.render.resolution_x = 4096
scene.render.resolution_y = 4096
scene.render.resolution_percentage = 100
scene.render.image_settings.color_mode = 'RGB'
scene.render.image_settings.color_depth = '16'
scene.render.film_transparent = False
scene.render.filepath = '//icon-master.png'

points = [world_to_camera_view(scene, scene.camera, obj.matrix_world @ Vector(corner))
          for obj in scene.objects if obj.name.startswith(('Spine', 'Ice connector', 'Ring'))
          for corner in obj.bound_box]
bounds = (min(p.x for p in points), min(p.y for p in points),
          max(p.x for p in points), max(p.y for p in points))
assert bounds[0] >= .1 and bounds[1] >= .1 and bounds[2] <= .9 and bounds[3] <= .9
print('Verified central 80% framing:', bounds, flush=True)
bpy.ops.wm.save_as_mainfile(filepath=str(art / 'icon.blend'))
scene.render.filepath = str(art / 'icon-master.png')
bpy.ops.render.render(write_still=True)
