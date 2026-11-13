# FRS World Editor -- quick edit in Blender.
#
# The editor starts Blender with this script and the OBJ it has just exported:
#
#     blender.exe --python frs_quickedit.py -- <selection.obj>
#
# The script opens the OBJ with the axes the editor wrote it in, saves the scene
# as a .blend beside it -- so Ctrl+S has somewhere to go without asking -- and
# from then on every save also writes the OBJ back over the one it came from,
# with the options the editor's importer expects. A small ".sent" file beside
# the OBJ is touched last; that, not the OBJ itself, is what the editor watches,
# so it never reads a file Blender is halfway through writing.
#
# Nothing here has to be installed. The editor writes the script out each time
# it starts Blender, so the version always matches the editor that reads the
# result.

import os
import sys
import time

import bpy
from bpy.app.handlers import persistent


def _args():
    argv = sys.argv
    return argv[argv.index("--") + 1:] if "--" in argv else []


def _obj_path():
    return bpy.app.driver_namespace.get("frs_quickedit_obj", "")


def _collision_mode():
    # The editor leaves "<obj>.collision" beside a collision export.
    path = _obj_path()
    return bool(path) and os.path.exists(path + ".collision")


# --- collision ----------------------------------------------------------------
#
# The game's surface table (SPEED2.EXE 0x800EE0): what byte 2 of a polygon
# names, and the colours the editor draws them in.
SURFACES = [
    ("NONE", 0xF0F040), ("ROAD", 0x8C8C96), ("ROAD_WET", 0x6E7A96), ("ROAD_DRIFT", 0x9C8CB4),
    ("ROAD_SMOKE_1", 0x9C8CB4), ("ROAD_SMOKE_2", 0x9C8CB4), ("ROAD_SMOKE_3", 0x9C8CB4),
    ("ROAD_BRIDGE", 0x7890B4), ("DIRT", 0xA0784A), ("GRAVEL", 0xB4A07A), ("ROUGH_ROAD", 0x8C7860),
    ("COBBLESTONE", 0xA08C78), ("STAIRS", 0xC8B48C), ("PUDDLE", 0x50B4DC), ("DEEP_WATER", 0x2864DC),
    ("GRASS", 0x50B446), ("SIDEWALK", 0xE6DCB4), ("WOOD", 0xC88232), ("PLASTIC", 0xB45AC8),
    ("GLASS", 0x96E6F0), ("SOLID_WALL", 0xC84646), ("SEE_THROUGH_WALL", 0xDC7878), ("PLANT", 0x3C963C),
    ("POST", 0xB4B4B4), ("PILLAR", 0xB4B4B4), ("METAL_GRATE", 0x8CAAC8), ("METAL", 0x8CAAC8),
    ("CHAINLINK", 0xA0A0C8), ("CAR", 0xF0F040),
]


def _rgb(value):
    return (((value >> 16) & 255) / 255.0, ((value >> 8) & 255) / 255.0, (value & 255) / 255.0, 1.0)


def _surface_of(material_name):
    # ground_00_34_01_00_road -> 1; the third byte is the surface.
    parts = material_name.split("_")
    if len(parts) >= 5 and parts[0] in ("ground", "barrier"):
        try:
            return int(parts[3], 16)
        except ValueError:
            return None
    return None


def _colour_materials():
    for material in bpy.data.materials:
        surface = _surface_of(material.name)
        if surface is None:
            continue
        colour = _rgb(SURFACES[surface][1] if surface < len(SURFACES) else 0xF0F040)
        if material.name.startswith("barrier"):
            colour = (0.9, 0.2, 0.2, 1.0)
        material.diffuse_color = colour
        material.use_backface_culling = False


def _material(kind, surface):
    # The editor reads the bytes: race group 0, the surface, and flags it
    # works out itself from the shape (quad, vertical) -- barriers keep 0x16.
    flags = 0x16 if kind == "barrier" else 0x00
    name = "%s_00_00_%02X_%02X_%s" % (kind, surface, flags, SURFACES[surface][0].lower())
    material = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    material.diffuse_color = (0.9, 0.2, 0.2, 1.0) if kind == "barrier" else _rgb(SURFACES[surface][1])
    return material


def _slot(obj, material):
    for index, slot in enumerate(obj.material_slots):
        if slot.material == material:
            return index
    obj.data.materials.append(material)
    return len(obj.material_slots) - 1


class FRS_OT_surface(bpy.types.Operator):
    """Give the selected faces this surface (what the car feels and hears)"""
    bl_idname = "frs.assign_surface"
    bl_label = "Assign surface"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        import bmesh
        obj = context.edit_object
        if not obj:
            self.report({'WARNING'}, "Select faces in Edit Mode first")
            return {'CANCELLED'}
        surface = int(context.scene.frs_surface)
        index = _slot(obj, _material("ground", surface))
        mesh = bmesh.from_edit_mesh(obj.data)
        count = 0
        for face in mesh.faces:
            if face.select:
                face.material_index = index
                count += 1
        bmesh.update_edit_mesh(obj.data)
        self.report({'INFO'}, "%d faces are now %s" % (count, SURFACES[surface][0]))
        return {'FINISHED'}


class FRS_OT_wall(bpy.types.Operator):
    """Raise a wall on every selected edge: a barrier (no top) or a wall with real height"""
    bl_idname = "frs.wall_from_edges"
    bl_label = "Wall from selected edges"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        import bmesh
        obj = context.edit_object
        if not obj:
            self.report({'WARNING'}, "Select edges in Edit Mode first")
            return {'CANCELLED'}
        scene = context.scene
        barrier = scene.frs_wall_kind == 'BARRIER'
        height = 3.0 if barrier else scene.frs_wall_height
        sink = 0.5 if barrier else 0.5
        material = _material("barrier" if barrier else "ground", 20 if barrier else int(scene.frs_wall_surface))
        index = _slot(obj, material)
        mesh = bmesh.from_edit_mesh(obj.data)
        edges = [edge for edge in mesh.edges if edge.select]
        made = 0
        # Up is the world's Z; the importer may have turned the object rather
        # than its vertices, so it is taken into the mesh's own space.
        from mathutils import Vector
        up = (obj.matrix_world.inverted_safe().to_3x3() @ Vector((0.0, 0.0, 1.0)))
        scale = up.length or 1.0
        up = up / scale / scale   # one world unit along world Z, in local units
        for edge in edges:
            a, b = edge.verts[0].co.copy(), edge.verts[1].co.copy()
            a0 = mesh.verts.new(a - up * sink)
            b0 = mesh.verts.new(b - up * sink)
            b1 = mesh.verts.new(b + up * height)
            a1 = mesh.verts.new(a + up * height)
            face = mesh.faces.new((a0, b0, b1, a1))
            face.material_index = index
            made += 1
        bmesh.update_edit_mesh(obj.data)
        self.report({'INFO'}, "%d wall pieces made" % made)
        return {'FINISHED'}


class FRS_PT_collision(bpy.types.Panel):
    bl_label = "FRS Collision"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "FRS"

    @classmethod
    def poll(cls, _context):
        return _collision_mode()

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        layout.label(text="Surface of selected faces")
        layout.prop(scene, "frs_surface", text="")
        layout.operator(FRS_OT_surface.bl_idname, icon='MATERIAL')
        layout.separator()
        layout.label(text="Walls on selected edges")
        layout.prop(scene, "frs_wall_kind", expand=True)
        if scene.frs_wall_kind == 'VERTICAL':
            layout.prop(scene, "frs_wall_height")
            layout.prop(scene, "frs_wall_surface", text="")
        layout.operator(FRS_OT_wall.bl_idname, icon='MOD_SOLIDIFY')
        layout.separator()
        layout.label(text="Ctrl+S sends it to the editor", icon='INFO')


def _surface_items(_self, _context):
    return [(str(i), name.replace("_", " ").title(), "") for i, (name, _colour) in enumerate(SURFACES)]


def _register_collision():
    for cls in (FRS_OT_surface, FRS_OT_wall, FRS_PT_collision):
        bpy.utils.register_class(cls)
    bpy.types.Scene.frs_surface = bpy.props.EnumProperty(name="Surface", items=_surface_items, default=1)
    bpy.types.Scene.frs_wall_surface = bpy.props.EnumProperty(name="Wall surface", items=_surface_items,
                                                               default=20)
    bpy.types.Scene.frs_wall_kind = bpy.props.EnumProperty(
        name="Kind", items=[('BARRIER', "Barrier", "No top: nothing gets over it"),
                            ('VERTICAL', "With height", "A real height: a car can jump it")])
    bpy.types.Scene.frs_wall_height = bpy.props.FloatProperty(name="Height", default=3.0, min=0.25, max=100.0)


def _setup_collision(obj_path):
    _colour_materials()
    # The drawn world around the collision, to model against: wire, locked,
    # and never written back.
    reference = os.path.join(os.path.dirname(obj_path), "reference.obj")
    if os.path.exists(reference):
        before = set(bpy.data.objects)
        bpy.ops.wm.obj_import(filepath=reference, forward_axis='NEGATIVE_Z', up_axis='Y')
        collection = bpy.data.collections.get("FRS Reference") or bpy.data.collections.new("FRS Reference")
        if collection.name not in bpy.context.scene.collection.children:
            bpy.context.scene.collection.children.link(collection)
        for obj in set(bpy.data.objects) - before:
            for owner in list(obj.users_collection):
                owner.objects.unlink(obj)
            collection.objects.link(obj)
            obj.display_type = 'WIRE'
            obj.hide_select = True
    # Faces come in welded, which is what editing wants.
    bpy.ops.object.select_all(action='DESELECT')


def _export(obj_path):
    # Object mode flushes any edit-mode changes into the mesh the exporter
    # reads. A save already does this, but "Send" from the menu does not.
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')

    if _collision_mode():
        # Only the collision goes back, and untriangulated: a barrier is a
        # quad whose bottom edge is the wall, and cut in two it would not be.
        selected = [o for o in bpy.context.selected_objects]
        bpy.ops.object.select_all(action='DESELECT')
        for obj in bpy.data.objects:
            if obj.name.startswith("collision_"):
                obj.select_set(True)
        bpy.ops.wm.obj_export(
            filepath=obj_path, check_existing=False, forward_axis='NEGATIVE_Z', up_axis='Y',
            global_scale=1.0, apply_modifiers=True, apply_transform=True, export_selected_objects=True,
            export_uv=False, export_normals=False, export_colors=False, export_materials=True,
            export_triangulated_mesh=False, export_object_groups=False, export_material_groups=False,
            export_vertex_groups=False, export_smooth_groups=False, path_mode='RELATIVE')
        bpy.ops.object.select_all(action='DESELECT')
        for obj in selected:
            obj.select_set(True)
        with open(obj_path + ".sent", "w", encoding="utf-8") as marker:
            marker.write("%f\n" % time.time())
        return

    # Each option is here because the editor's importer depends on it:
    #  - the axes undo the Y-up the editor wrote, so vertices land where they were;
    #  - colours carry the game's baked lighting, which a mesh must not lose;
    #  - triangles are what the game draws, and fixing the split here means the
    #    editor never has to guess how a quad was meant to be cut;
    #  - materials keep their tex_XXXXXXXX names, which is how a face finds its
    #    texture again.
    bpy.ops.wm.obj_export(
        filepath=obj_path,
        check_existing=False,
        forward_axis='NEGATIVE_Z',
        up_axis='Y',
        global_scale=1.0,
        apply_modifiers=True,
        apply_transform=True,
        export_selected_objects=False,
        export_uv=True,
        export_normals=True,
        export_colors=True,
        export_materials=True,
        export_triangulated_mesh=True,
        export_object_groups=False,
        export_material_groups=False,
        export_vertex_groups=False,
        export_smooth_groups=False,
        path_mode='RELATIVE',
    )

    with open(obj_path + ".sent", "w", encoding="utf-8") as marker:
        marker.write("%f\n" % time.time())


def _send(report=None):
    obj_path = _obj_path()
    if not obj_path:
        return False
    try:
        _export(obj_path)
    except Exception as error:  # the save itself must not be lost to this
        print("FRS quick edit: export failed:", error)
        if report:
            report({'ERROR'}, "FRS: export failed: %s" % error)
        return False
    print("FRS quick edit: sent", obj_path)
    if report:
        report({'INFO'}, "Sent to FRS World Editor")
    return True


@persistent
def _on_save(_dummy=None, *_rest):
    _send()


class FRS_OT_send(bpy.types.Operator):
    """Write the OBJ back and have FRS World Editor load it"""
    bl_idname = "frs.send_to_editor"
    bl_label = "Send to FRS World Editor"

    def execute(self, context):
        return {'FINISHED'} if _send(self.report) else {'CANCELLED'}


def _menu(self, _context):
    self.layout.separator()
    self.layout.operator(FRS_OT_send.bl_idname, icon='EXPORT')


def _frame_view():
    # The UI is not up while the script runs, so the view is set on the first
    # tick after it is.
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type != 'VIEW_3D':
                continue
            space = area.spaces.active
            # A street runs to hundreds of units; the default clip end of a
            # thousand cuts the far half away as soon as the view pulls back.
            space.clip_start = 0.1
            space.clip_end = 20000.0
            space.shading.type = 'SOLID'
            space.shading.color_type = 'TEXTURE'
            region = next((r for r in area.regions if r.type == 'WINDOW'), None)
            if region is None:
                continue
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.view3d.view_all(center=False)
    return None


def _setup(obj_path):
    bpy.app.driver_namespace["frs_quickedit_obj"] = obj_path

    blend_path = os.path.splitext(obj_path)[0] + ".blend"

    if os.path.exists(blend_path):
        # Opened again for the same selection: the earlier work is in the .blend.
        bpy.ops.wm.open_mainfile(filepath=blend_path)
    else:
        # A clean scene: the default cube, camera and light would otherwise be
        # written into the OBJ as objects the editor has never heard of.
        for obj in list(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)

        bpy.ops.wm.obj_import(filepath=obj_path, forward_axis='NEGATIVE_Z', up_axis='Y')

        # The game's surfaces are open on the back, and Blender draws backfaces
        # the same as front ones, which hides which side a face is on.
        for material in bpy.data.materials:
            material.use_backface_culling = True

        bpy.app.driver_namespace["frs_quickedit_obj"] = obj_path
        if _collision_mode():
            _setup_collision(obj_path)

        bpy.ops.wm.save_as_mainfile(filepath=blend_path)

    bpy.app.driver_namespace["frs_quickedit_obj"] = obj_path
    if _on_save not in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.append(_on_save)

    if not bpy.app.background:
        bpy.app.timers.register(_frame_view, first_interval=0.2)


def register():
    bpy.utils.register_class(FRS_OT_send)
    bpy.types.TOPBAR_MT_file.append(_menu)


# The OBJ importer and exporter this relies on -- the axis options, vertex
# colours both ways -- are the ones Blender has shipped since 3.6. Only 5.2
# has been tried.
MINIMUM_BLENDER = (3, 6, 0)


def _too_old():
    message = ("FRS World Editor needs Blender %d.%d or newer; this is %s. "
               "Choose another in the editor: File > Choose Blender."
               % (MINIMUM_BLENDER[0], MINIMUM_BLENDER[1], bpy.app.version_string))
    print("FRS quick edit:", message)
    if bpy.app.background:
        return

    def warn():
        def draw(self, _context):
            self.layout.label(text=message)
        bpy.context.window_manager.popup_menu(draw, title="FRS World Editor", icon='ERROR')
        return None
    bpy.app.timers.register(warn, first_interval=0.5)


def main():
    if bpy.app.version < MINIMUM_BLENDER:
        _too_old()
        return
    args = _args()
    if not args:
        print("FRS quick edit: no OBJ given")
        return
    obj_path = os.path.abspath(args[0])
    if not os.path.exists(obj_path):
        print("FRS quick edit: not found:", obj_path)
        return
    register()
    if os.path.exists(obj_path + ".collision"):
        _register_collision()
    _setup(obj_path)


main()
