#!/usr/bin/env python3
"""
protocol.py - the single source for the is1gl wire protocol.

Emits both halves of it, so the guest and the host cannot drift apart:

    is1gl_ops.h          opcode numbers and names        (both sides)
    is1gl_gen_guest.h    marshallers, #included by is1gl.c in the guest
    is1gl_replay.h       the replay switch, #included by hw/misc/is1gl.c

Signatures are not written out here. They are parsed from the *guest's own*
GL/gl.h and GL/glx.h by tools/glshim/gen_glshim.py's parser, which is what
makes the generated wrappers ABI-identical to what renderd already links
against. All this file adds is, for each entry point, how to marshal it.

    python3 protocol.py <gl.h.guest> <glx.h.guest> <outdir> <qemu-hw-misc-dir>

See notes/gl-acceleration-plan.md and notes/is1gl-phase2.md.
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "glshim"))
from gen_glshim import parse_decls, GL_PAT, GLX_PAT      # noqa: E402

# --------------------------------------------------------------- wire types
#
# Every scalar goes on the wire at a fixed width, read and written with
# memcpy on both sides so neither has to care about alignment inside a
# record. GLdouble is the only 8-byte one and the app does use it -
# glClipPlane takes four, and the projection matrix arrives as glMultMatrixd.

SCALARS = {
    "GLenum":     ("u32", 4), "GLbitfield": ("u32", 4), "GLuint":  ("u32", 4),
    "GLint":      ("i32", 4), "GLsizei":    ("i32", 4), "GLshort": ("i32", 4),
    "GLushort":   ("u32", 4), "GLbyte":     ("i32", 4), "GLubyte": ("u32", 4),
    "GLboolean":  ("u32", 4), "GLclampf":   ("f32", 4), "GLfloat": ("f32", 4),
    "GLdouble":   ("f64", 8), "GLclampd":   ("f64", 8), "GLintptr": ("i32", 4),
}

C_OF_WIRE = {"u32": "uint32_t", "i32": "int32_t",
             "f32": "float", "f64": "double"}


def base_type(decl, name):
    """'const GLdouble *equation' -> ('GLdouble', 1). Array suffix counts."""
    d = decl
    if d.endswith("]"):
        d = d[:d.rindex("[")]
    d = re.sub(r"\b%s\b" % re.escape(name), " ", d)
    stars = d.count("*")
    d = d.replace("*", " ").replace("const", " ")
    toks = d.split()
    return (toks[0] if toks else "GLvoid"), stars


# ------------------------------------------------------------- the table
#
# Anything not named here is a stub: counted, reported once, and dropped.
# That is deliberate. The application uses 54 of 566 entry points
# (notes/is1gl-phase0.md), so an entry appearing in the unknown-call table at
# the end of a run is a bug report, not something to paper over at runtime.
#
#   {}                  every argument is a scalar; marshal them in order
#   "arr": (param, count_param|int, type)   pointer to a counted array
#   "image": param      bulk pixel data, packed tight by the guest
#   "readback": param   destination is guest memory; send its physical address
#   "local": True       answered in the guest, never reaches the host
#   "hand": True        the public wrapper is hand-written in is1gl.c;
#                       generate only emit_<name>()
#   "manual": True      generate nothing at all - both sides are hand-written,
#                       because the call maps to a control opcode rather than
#                       to itself (glReadPixels -> READPIXELS, glFinish ->
#                       FENCE)
#   "hosthook": True    the host case calls is1gl_host_<name>() instead of the
#                       GL function, because something has to happen first -
#                       the video layer arrives as 4:2:2 YCbCr, which the host
#                       driver does not accept and we convert
#   "hoststate": True   pass the Is1glState pointer as the hook's first
#                       argument when the adaptation is device-instance state

OPS = {
    # ---- clears, raster state
    "glClearColor":       {},
    "glClear":            {},
    "glBlendFunc":        {},
    "glLineWidth":        {},
    "glScissor":          {},
    "glViewport":         {"hand": True, "hosthook": True,
                           "hoststate": True},  # per guest GLX context
    "glClipPlane":        {"arr": ("equation", 4, "GLdouble")},
    "glEnable":           {"hand": True},   # tracked: glGet(GL_*) and attrib stack
    "glDisable":          {"hand": True},
    "glPushAttrib":       {"hand": True},
    "glPopAttrib":        {"hand": True},
    "glPushClientAttrib": {"hand": True},
    "glPopClientAttrib":  {"hand": True},

    # ---- matrix stack
    "glMatrixMode":       {},
    "glPushMatrix":       {},
    "glPopMatrix":        {},
    "glLoadIdentity":     {},
    "glMultMatrixd":      {"arr": ("m", 16, "GLdouble")},
    "glMultMatrixf":      {"arr": ("m", 16, "GLfloat")},
    "glRotated":          {},
    "glRotatef":          {},
    "glScalef":           {},
    "glTranslated":       {},
    "glTranslatef":       {},

    # ---- display lists. Names are allocated in the guest and used verbatim
    # on the host: GL lets glNewList/glBindTexture take any name, so there is
    # no mapping table and no round trip to allocate one.
    "glNewList":          {"hand": True},   # tracked: compiling or not
    "glEndList":          {"hand": True},
    "glCallList":         {"hand": True},   # tracked: may change bound texture
    "glDeleteLists":      {"hand": True},

    # ---- immediate mode: ~70% of all calls
    "glBegin":            {"hand": True},   # tracked: glGetError is illegal inside
    "glEnd":              {"hand": True},
    "glVertex2f":         {},
    "glVertex2d":         {},
    "glTexCoord2f":       {},
    "glColor4f":          {"hand": True},   # tracked: GL_CURRENT_COLOR
    "glLineWidth":        {},

    # ---- texture
    "glGenTextures":      {"local": True},  # names allocated guest-side
    "glDeleteTextures":   {"arr": ("textures", "n", "GLuint"), "hand": True},
    "glBindTexture":      {"hand": True},   # tracked: GL_TEXTURE_BINDING_2D
    "glTexParameterf":    {},
    "glTexParameteri":    {},
    "glTexEnvf":          {},
    "glTexEnvi":          {},
    "glTexImage2D":       {"image": "pixels", "hand": True, "hosthook": True},
    "glTexSubImage2D":    {"image": "pixels", "hand": True, "hosthook": True},
    "glCopyTexSubImage2D": {},

    # ---- pixel path
    "glPixelStorei":      {"hand": True},   # tracked, and two pnames are ours
    "glPixelStoref":      {"hand": True},
    "glReadPixels":       {"manual": True},   # -> IS1GL_OP_READPIXELS
    "glDrawPixels":       {"image": "pixels", "hand": True, "hosthook": True},

    # ---- sync
    "glFinish":           {"manual": True},   # -> IS1GL_OP_FENCE
    "glFlush":            {"manual": True},

    # ---- answered in the guest, never marshalled
    "glGetIntegerv":      {"local": True},
    "glGetFloatv":        {"local": True},
    "glGetError":         {"local": True},
    "glGenLists":         {"local": True},
    "glGetString":        {"local": True},
}

GLX_OPS = {
    "glXChooseVisual":       {"local": True},
    "glXCreateContext":      {"local": True},
    "glXDestroyContext":     {"local": True},
    "glXMakeCurrent":        {"local": True},
    "glXSwapBuffers":        {"local": True},
    "glXGetCurrentContext":  {"local": True},
    "glXGetCurrentDrawable": {"local": True},
    "glXQueryExtension":     {"local": True},
    "glXQueryVersion":       {"local": True},
    "glXWaitGL":             {"local": True},
    "glXWaitX":              {"local": True},
    "glXIsDirect":           {"local": True},
    "glXGetConfig":          {"local": True},
    "glXFreeMemoryNV":       {"local": True},
    "glXGetAGPOffsetMESA":   {"local": True},
}

# Opcodes the core needs that are not a single GL call.
CONTROL_OPS = ["NOP", "WRAP", "FENCE", "MAKE_CURRENT", "SWAP", "READPIXELS"]


def marshalled(name):
    """Has a generated opcode and a generated case on both sides."""
    if name not in OPS:
        return False
    spec = OPS[name]
    return not spec.get("local") and not spec.get("manual")


def wire_args(fn, spec):
    """The scalar arguments that go on the wire, in order."""
    out = []
    skip = set()
    if "arr" in spec:
        skip.add(spec["arr"][0])
    if "image" in spec:
        skip.add(spec["image"])
    if "readback" in spec:
        skip.add(spec["readback"])
    for decl, pname in fn["params"]:
        if pname in skip:
            continue
        bt, stars = base_type(decl, pname)
        if stars:
            continue                     # any other pointer is handled by hand
        if bt not in SCALARS:
            continue
        out.append((pname, SCALARS[bt][0], SCALARS[bt][1]))
    return out


def gen_ops_h(funcs):
    o = ["/* GENERATED by tools/is1gl/protocol.py -- do not edit. */\n",
         "#ifndef IS1GL_OPS_H\n#define IS1GL_OPS_H\n\n"]
    n = 0
    names = []
    for c in CONTROL_OPS:
        o.append("#define IS1GL_OP_%-24s %d\n" % (c, n))
        names.append(c)
        n += 1
    o.append("\n")
    for f in funcs:
        if marshalled(f["name"]):
            o.append("#define IS1GL_OP_%-24s %d\n" % (f["name"], n))
            names.append(f["name"])
            n += 1
    o.append("\n#define IS1GL_OP_COUNT %d\n\n" % n)
    # The FreeBSD guest's old preprocessor cannot splice a continued macro
    # after a Windows checkout changes this generated header to CRLF.
    # Use short, single-line macros so long physical lines are avoided too.
    chunks = [names[i:i + 8] for i in range(0, len(names), 8)]
    for i, chunk in enumerate(chunks):
        o.append('#define IS1GL_OP_NAMES_%d %s\n' %
                 (i, ", ".join('"%s"' % nm for nm in chunk)))
    o.append("#define IS1GL_OP_NAMES { %s }\n" %
             ", ".join("IS1GL_OP_NAMES_%d" % i for i in range(len(chunks))))
    o.append("\n#endif\n")
    return "".join(o)


def gen_guest(funcs):
    o = ["/* GENERATED by tools/is1gl/protocol.py -- do not edit.\n"
         " * #included by is1gl.c, which supplies the ring writer and the\n"
         " * hand-written wrappers for the entry points that track state. */\n\n"]
    for f in funcs:
        name = f["name"]
        # `or` would be wrong here: an all-scalars entry is {}, which is
        # falsy, and every one of them would be silently dropped from the
        # generated protocol - about twenty of the forty-seven.
        spec = OPS[name] if name in OPS else GLX_OPS.get(name)
        if spec is None or spec.get("local") or spec.get("manual"):
            continue
        args = wire_args(f, spec)
        decl = ", ".join(d for d, _ in f["params"]) or "void"
        size = sum(w for _, _, w in args)

        # --- emit_<name>: append one record. No syscall, no allocation.
        o.append("static void emit_%s(%s)\n{\n" % (name, decl))
        extra = ""
        if "arr" in spec:
            pname, count, ctype = spec["arr"]
            cw, cs = SCALARS[ctype]
            cnt = str(count) if isinstance(count, int) else count
            extra = " + (%s) * %d" % (cnt, cs)
        if "image" in spec:
            o.append("    long img = is1gl_image_bytes(width, height, "
                     "format, type);\n")
            extra = " + ((img + 7) & ~7L)"
        o.append("    unsigned char *r = ring_record(IS1GL_OP_%s, %d%s);\n"
                 % (name, size, extra))
        o.append("    if (!r) return;\n")
        off = 0
        for pname, wt, w in args:
            o.append("    put_%s(r + %d, %s);\n" % (wt, off, pname))
            off += w
        if "arr" in spec:
            pname, count, ctype = spec["arr"]
            cw, cs = SCALARS[ctype]
            cnt = str(count) if isinstance(count, int) else count
            o.append("    {\n        int i_;\n")
            o.append("        for (i_ = 0; i_ < (int)(%s); i_++)\n" % cnt)
            o.append("            put_%s(r + %d + i_ * %d, %s[i_]);\n"
                     % (cw, off, cs, pname))
            o.append("    }\n")
        if "image" in spec:
            o.append("    is1gl_pack_image(r + %d, %s, width, height, "
                     "format, type);\n" % (off, spec["image"]))
        o.append("}\n\n")

        # --- the public wrapper, unless it is hand-written in is1gl.c.
        # gl_lock makes the call indivisible: renderd calls GL from two
        # threads, and libc_r can switch between them at any instruction.
        if not spec.get("hand"):
            call = ", ".join(n for _, n in f["params"])
            o.append("%s %s(%s)\n{\n    gl_enter();\n    emit_%s(%s);\n"
                     "    gl_leave();\n}\n\n"
                     % (f["ret"], name, decl, name, call))

    o.append("\n/* ---- stubs: every entry point the application does not use.\n"
             " * This library replaces libGL rather than wrapping it, so an\n"
             " * undefined symbol here would bind to the vendor library and\n"
             " * try to reach an X server that has no context on it. Each stub\n"
             " * counts itself; a non-zero count in the report is a bug\n"
             " * report, not something to paper over at runtime. */\n")
    stubs = []
    for f in funcs:
        name = f["name"]
        if name in OPS or name in GLX_OPS:
            continue
        stubs.append(name)
    o.append("#define IS1GL_STUB_COUNT %d\n" % len(stubs))
    o.append("static unsigned long is1gl_stub_hits[IS1GL_STUB_COUNT];\n")
    o.append("static const char *const is1gl_stub_names[IS1GL_STUB_COUNT] = {\n")
    for nm in stubs:
        o.append("    \"%s\",\n" % nm)
    o.append("};\n\n")
    for i, f in enumerate([f for f in funcs if f["name"] in stubs]):
        name, ret = f["name"], f["ret"]
        decl = ", ".join(d for d, _ in f["params"]) or "void"
        body = "    is1gl_stub(%d);\n" % i
        if ret != "void":
            body += "    return (%s)0;\n" % ret
        o.append("%s %s(%s)\n{\n%s}\n\n" % (ret, name, decl, body))
    return "".join(o)


def gen_host(funcs):
    o = ["/* GENERATED by tools/is1gl/protocol.py -- do not edit.\n"
         " * #included by hw/misc/is1gl.c. Regenerate with:\n"
         " *   python3 tools/is1gl/protocol.py tools/glshim/gl.h.guest \\\n"
         " *       tools/glshim/glx.h.guest src/is1gl qemu/hw/misc\n"
         " * and commit both sides together - they are one protocol. */\n\n"
         "static bool is1gl_replay_one(Is1glState *is1_, uint32_t opcode,\n"
         "                             const uint8_t *rec_, uint32_t reclen_)\n"
         "{\n"
         "    switch (opcode) {\n"]
    for f in funcs:
        name = f["name"]
        # `or` would be wrong here: an all-scalars entry is {}, which is
        # falsy, and every one of them would be silently dropped from the
        # generated protocol - about twenty of the forty-seven.
        spec = OPS[name] if name in OPS else GLX_OPS.get(name)
        if spec is None or spec.get("local") or spec.get("manual"):
            continue
        args = wire_args(f, spec)
        size = sum(w for _, _, w in args)
        o.append("    case IS1GL_OP_%s: {\n" % name)
        # `reclen_ < 0` is always false for an unsigned, and -Werror says so.
        if size:
            o.append("        if (reclen_ < %d) return false;\n" % size)
        off = 0
        callargs = []
        for pname, wt, w in args:
            o.append("        %s %s = get_%s(rec_ + %d);\n"
                     % (C_OF_WIRE[wt], pname, wt, off))
            callargs.append(pname)
            off += w
        if "arr" in spec:
            pname, count, ctype = spec["arr"]
            cw, cs = SCALARS[ctype]
            cnt = str(count) if isinstance(count, int) else count
            o.append("        %s %s[%s];\n" % (ctype, pname,
                                               cnt if isinstance(count, int) else "64"))
            o.append("        int i_;\n")
            if not isinstance(count, int):
                o.append("        if (%s > 64 || %s < 0) return false;\n" % (cnt, cnt))
            o.append("        if (reclen_ < %d + (uint32_t)(%s) * %d) "
                     "return false;\n" % (off, cnt, cs))
            o.append("        for (i_ = 0; i_ < (int)(%s); i_++)\n" % cnt)
            o.append("            %s[i_] = get_%s(rec_ + %d + i_ * %d);\n"
                     % (pname, cw, off, cs))
            # insert the array argument in its declared position
            callargs = []
            for d, pn in f["params"]:
                callargs.append(pn)
        if "image" in spec:
            o.append("        const void *%s = rec_ + %d;\n" % (spec["image"], off))
            o.append("        long need = is1gl_host_image_bytes(width, height,"
                     " format, type);\n")
            o.append("        if (need < 0 || reclen_ < %d + (uint32_t)need)"
                     " return false;\n" % off)
            callargs = [pn for _, pn in f["params"]]
        if "arr" not in spec and "image" not in spec:
            callargs = [pn for _, pn in f["params"]]
        target = ("is1gl_host_%s" % name) if spec.get("hosthook") else name
        if spec.get("hoststate"):
            callargs.insert(0, "is1_")
        o.append("        %s(%s);\n" % (target, ", ".join(callargs)))
        o.append("        return true;\n    }\n")
    o.append("    default:\n        return false;\n    }\n}\n")
    # is1_ is unused today; keep the parameter for the hooks that will need it.
    o = ["".join(o).replace("{\n    switch (opcode) {",
                            "{\n    (void)is1_;\n    switch (opcode) {")]
    return "".join(o)


def main():
    gl_src = open(sys.argv[1]).read()
    glx_src = open(sys.argv[2]).read()
    outdir, qemudir = sys.argv[3], sys.argv[4]
    funcs = parse_decls(gl_src, [GL_PAT]) + parse_decls(glx_src, [GLX_PAT])

    known = set(OPS) | set(GLX_OPS)
    parsed = set(f["name"] for f in funcs)
    missing = sorted(known - parsed)
    if missing:
        sys.stderr.write("NOT DECLARED in the guest headers: %s\n"
                         % ", ".join(missing))

    n_m = sum(1 for f in funcs if marshalled(f["name"]))
    def spec_of(nm):
        return OPS[nm] if nm in OPS else (GLX_OPS[nm] if nm in GLX_OPS else None)
    n_l = sum(1 for f in funcs if (spec_of(f["name"]) or {}).get("local"))
    sys.stderr.write("%d entry points parsed: %d marshalled, %d local, %d stubs\n"
                     % (len(funcs), n_m, n_l, len(funcs) - n_m - n_l))

    # Keep backslash-continued macros usable by the guest's old preprocessor,
    # even when this generator runs on Windows.
    open(os.path.join(outdir, "is1gl_ops.h"), "w", newline="\n").write(gen_ops_h(funcs))
    open(os.path.join(outdir, "is1gl_gen_guest.h"), "w", newline="\n").write(gen_guest(funcs))
    open(os.path.join(qemudir, "is1gl_replay.h"), "w", newline="\n").write(gen_host(funcs))
    open(os.path.join(qemudir, "is1gl_ops.h"), "w", newline="\n").write(gen_ops_h(funcs))


if __name__ == "__main__":
    main()
