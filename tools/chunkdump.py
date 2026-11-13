import sys, struct, os

CONTAINER = lambda i: (i & 0x80000000) != 0

NAMES = {
 0x80134000:"SOLID_LIST_PACK", 0x00134001:"SOLID_LIST_HEADER", 0x00134002:"PADDING",
 0x00134003:"SOLID_LIST_OBJ_HASHTABLE", 0x80134010:"SOLID_OBJECT",
 0x00134011:"SOLID_OBJ_HEADER", 0x00134012:"SOLID_OBJ_TEXREFS", 0x00134013:"SOLID_OBJ_LIGHTMAT",
 0x80134100:"MESH_CONTAINER", 0x00134900:"MESH_DESCRIPTOR", 0x00134b01:"MESH_MATERIALS",
 0x00134b02:"MESH_B02", 0x00134b03:"MESH_VERTICES", 0x00134b04:"MESH_INDICES",
 0x00134b05:"MESH_B05", 0x00134c02:"MESH_C02",
 0x33310001:"TPK_ROOT", 0x33310002:"TPK_DATA",
 0x00034110:"TRACK_INFO", 0x80034100:"TRACK_PACK",
}

def name(i): return NAMES.get(i, "")

def walk(buf, off, end, depth, out, maxdepth):
    while off + 8 <= end:
        cid, csz = struct.unpack_from("<II", buf, off)
        if csz < 0 or off + 8 + csz > end:
            out.append("  "*depth + "!! bad chunk id=%08X size=%d at %08X (end %08X)" % (cid, csz, off, end))
            return
        out.append("  "*depth + "%08X size=%-9d @%08X %s" % (cid, csz, off, name(cid)))
        if CONTAINER(cid) and depth < maxdepth and csz >= 8:
            walk(buf, off+8, off+8+csz, depth+1, out, maxdepth)
        off += 8 + csz

f = sys.argv[1]
start = int(sys.argv[2],0) if len(sys.argv)>2 else 0
length = int(sys.argv[3],0) if len(sys.argv)>3 else None
maxdepth = int(sys.argv[4]) if len(sys.argv)>4 else 8
buf = open(f,'rb').read()
end = len(buf) if length is None else min(len(buf), start+length)
out=[]
walk(buf, start, end, 0, out, maxdepth)
print("\n".join(out))
