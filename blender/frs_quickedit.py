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


def _export(obj_path):
    # Object mode flushes any edit-mode changes into the mesh the exporter
    # reads. A save already does this, but "Send" from the menu does not.
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')

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
    _setup(obj_path)


main()
