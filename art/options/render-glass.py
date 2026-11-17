# Prospero Disk - Reproducible Directory D glass icon studies for Blender.
# Copyright (C) 2026 Leonardo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later

"""Run: blender --background --python art/options/render-glass.py

Reconstructs the SVG's exact ring/spine dimensions as beveled solid meshes.
Writes three opaque 1024px PNGs and one reusable scene beside this script.
"""

from pathlib import Path
import math
import sys
import bpy
from mathutils import Matrix, Vector

OUT = Path(__file__).resolve().parent
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.samples = 96
scene.cycles.use_denoising = True
scene.cycles.max_bounces = 16
scene.cycles.transmission_bounces = 12
scene.render.resolution_x = 1024
scene.render.resolution_y = 1024
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'PNG'
scene.render.image_settings.color_mode = 'RGB'
scene.render.film_transparent = False
scene.view_settings.view_transform = 'AgX'
scene.world.use_nodes = True
scene.world.node_tree.nodes['Background'].inputs['Color'].default_value = (.18, .22, .35, 1)
scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = .35

prefs = bpy.context.preferences.addons['cycles'].preferences
try:
    if '--metal' in sys.argv:
        prefs.compute_device_type = 'METAL'
        prefs.get_devices()
    gpu = False
    for device in prefs.devices:
        device.use = '--metal' in sys.argv and device.type == 'METAL'
        gpu |= device.use
    if gpu:
        scene.cycles.device = 'GPU'
except Exception as error:
    print('Using CPU rendering:', error)

def material(name, color):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (*color, 1)
    mat.use_nodes = True
    node = mat.node_tree.nodes.get('Principled BSDF')
    node.inputs['Base Color'].default_value = (*color, 1)
    node.inputs['Transmission Weight'].default_value = 1.0
    node.inputs['IOR'].default_value = 1.46
    node.inputs['Coat Weight'].default_value = 0
    return mat

materials = [material('Azure spine', (.22, .65, 1)),
             material('Blue', (.12, .38, 1)),
             material('Violet', (.52, .15, 1)),
             material('Cyan', (.06, .85, .95))]
parts = []

def solid(name, points, mat):
    # SVG Y points down; center the visible silhouette, not the SVG canvas.
    points = [((x-549)/100, (512-y)/100) for x, y in points]
    n = len(points)
    vertices = [(x, y, z) for z in (-.23, .23) for x, y in points]
    # Keep caps as single polygons so bevels never affect tessellation diagonals.
    faces = [tuple(reversed(range(n))), tuple(range(n, 2*n))]
    faces += [(i, (i+1)%n, (i+1)%n+n, i+n) for i in range(n)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.object.mode_set(mode='OBJECT')
    obj.select_set(False)
    obj.data.materials.append(mat)
    bevel = obj.modifiers.new('Soft glass edges', 'BEVEL')
    bevel.width = .065
    bevel.segments = 8
    bevel.limit_method = 'ANGLE'
    for face in mesh.polygons:
        # Flat cap normals prevent triangulation from streaking glass reflections.
        face.use_smooth = False
    parts.append(obj)

def rect(name, x, y, w, h):
    solid(name, [(x,y), (x+w,y), (x+w,y+h), (x,y+h)], materials[0])

rect('Spine', 222, 148, 80, 728)
for i, y in enumerate([148, 798, 240, 706, 332, 614]):
    rect('Ice connector '+str(i), 326, y, 186, 78)
    parts[-1].data.materials[0] = materials[[1,3,3,2,2,3][i]]
for ring, (ri, ro, cuts, palette) in enumerate([
    (286,364,[-90,-35,28,90],[1,2,3]),
    (194,272,[-90,-10,48,90],[3,1,2]),
    (102,180,[-90,25,90],[2,3]),
]):
    for j in range(len(cuts)-1):
        a, b = cuts[j]+1, cuts[j+1]-1
        steps = max(8, int(b-a))
        angles = [math.radians(a+(b-a)*i/steps) for i in range(steps+1)]
        points = [(512+ro*math.cos(t),512+ro*math.sin(t)) for t in angles]
        points += [(512+ri*math.cos(t),512+ri*math.sin(t)) for t in reversed(angles)]
        solid(f'Ring {ring} segment {j}', points, materials[palette[j]])

def aim(obj, target=(0,0,0)):
    forward = (Vector(target)-obj.location).normalized()
    right = forward.cross(Vector((0,1,0))).normalized()
    up = right.cross(forward)
    obj.rotation_euler = Matrix((right, up, -forward)).transposed().to_euler()

bpy.ops.object.camera_add(location=(1.6,-1.1,16))
camera = bpy.context.object
camera.name = 'Icon camera'
camera.data.type = 'ORTHO'
camera.data.ortho_scale = 10.2
aim(camera)
scene.camera = camera

def light(name, location, energy, size, color, size_y=None):
    data = bpy.data.lights.new(name, 'AREA')
    data.energy = energy
    data.shape = 'RECTANGLE'
    data.size = size
    data.size_y = size_y or size
    data.color = color
    obj = bpy.data.objects.new(name, data)
    scene.collection.objects.link(obj)
    obj.location = location
    aim(obj)

light('Blue edge strip',(-5,3,4),700,1,(.25,.55,1),6)
light('Cyan edge strip',(5,1,3),850,.8,(.15,.85,1),7)
light('Violet lower strip',(-1,-5,3),500,4,(.6,.2,1),.7)
light('Top glint',(-1,6,3),600,3,(.75,.9,1),.5)

# An emissive navy-violet backdrop avoids a drop shadow on the console tile.
bpy.ops.mesh.primitive_plane_add(size=200, location=(0,0,-4))
back = bpy.context.object
back.name = 'Opaque navy violet background'
mat = bpy.data.materials.new('Navy violet gradient')
mat.use_nodes = True
nodes = mat.node_tree.nodes
nodes.clear()
output = nodes.new('ShaderNodeOutputMaterial')
emission = nodes.new('ShaderNodeEmission')
tex = nodes.new('ShaderNodeTexCoord')
sep = nodes.new('ShaderNodeSeparateXYZ')
ramp = nodes.new('ShaderNodeValToRGB')
ramp.color_ramp.elements[0].position = .46
ramp.color_ramp.elements[0].color = (.0034,.0044,.0103,1)
ramp.color_ramp.elements[1].position = .54
ramp.color_ramp.elements[1].color = (.012,.008,.034,1)
links = mat.node_tree.links
links.new(tex.outputs['Generated'],sep.inputs[0])
links.new(sep.outputs['X'],ramp.inputs[0])
links.new(ramp.outputs[0],emission.inputs['Color'])
links.new(emission.outputs[0],output.inputs['Surface'])
back.data.materials.append(mat)

# A studio transmission card lights the glass from behind without washing out
# the navy tile. It is hidden only from direct camera rays, like a product-shot
# compositing plate; refraction still sees its graduated light and dark bands.
bpy.ops.mesh.primitive_plane_add(size=14, location=(0,0,-2))
card = bpy.context.object
card.name = 'Transmission studio card'
card.visible_camera = False
card_mat = bpy.data.materials.new('Graduated transmitted studio light')
card_mat.use_nodes = True
nodes = card_mat.node_tree.nodes
nodes.clear()
output = nodes.new('ShaderNodeOutputMaterial')
card_emission = nodes.new('ShaderNodeEmission')
tex = nodes.new('ShaderNodeTexCoord')
mapping = nodes.new('ShaderNodeVectorMath')
mapping.operation = 'DOT_PRODUCT'
mapping.inputs[1].default_value = (.45,.8,0)
bands = nodes.new('ShaderNodeValToRGB')
stops = [(0,(.08,.12,.2,1)),(.35,(.6,.75,1,1)),
         (.52,(.1,.15,.25,1)),(.63,(.65,.8,1,1)),(1,(.2,.3,.5,1))]
for i,(position,color) in enumerate(stops):
    stop = bands.color_ramp.elements[i] if i < 2 else bands.color_ramp.elements.new(position)
    stop.position = position
    stop.color = color
bands.color_ramp.interpolation = 'EASE'
links = card_mat.node_tree.links
links.new(tex.outputs['Generated'],mapping.inputs[0])
links.new(mapping.outputs['Value'],bands.inputs[0])
links.new(bands.outputs[0],card_emission.inputs['Color'])
links.new(card_emission.outputs[0],output.inputs['Surface'])
card.data.materials.append(card_mat)

variants = [('07-clear-color', .018, 1.46, 1.0),
            ('08-prismatic-color', .035, 1.56, 1.35),
            ('09-deep-color', .025, 1.65, 1.8)]
for name, roughness, ior, depth in variants:
    for obj in parts:
        obj.scale.z = depth
    for mat in materials:
        node = mat.node_tree.nodes['Principled BSDF']
        node.inputs['Roughness'].default_value = roughness
        node.inputs['IOR'].default_value = ior
    scene.render.filepath = '//'+name+'.png'
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/(name+'.blend')))
    scene.render.filepath = str(OUT/(name+'.png'))
    bpy.ops.render.render(write_still=True)
