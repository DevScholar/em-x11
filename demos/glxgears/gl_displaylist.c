/*
 * Display-list emulation + small GL 1.x compat shims for emscripten
 * LEGACY_GL_EMULATION, which:
 *   - implements glBegin / glEnd / matrix stack / lighting / textures
 *   - leaves out display lists (no glGenLists / glNewList / glCallList)
 *   - aborts on glMaterialfv with GL_AMBIENT_AND_DIFFUSE
 *   - assumes glNormal3f is only ever called between glBegin / glEnd
 *     (writes into a vertex stream that's null outside that block)
 *
 * Strategy: the demo links with --wrap=<sym> for every GL function
 * we patch around. While "recording" (between glNewList / glEndList)
 * the wrappers append a tagged Cmd into a per-list buffer; otherwise
 * they go through exec_*() helpers that do the actual GL work --
 * either calling __real_* directly or transforming the call (split
 * AMBIENT_AND_DIFFUSE, defer normal-then-emit-per-vertex).
 *
 * glCallList replays the buffer through the same exec_*() helpers,
 * so list bodies and direct calls run identical paths -- including
 * the workarounds.
 *
 * Scope: tuned for glxgears's gear() body. Other LEGACY_GL clients
 * with richer list bodies will need additional wraps -- add them
 * here and to the link line, no other changes required.
 */

#include <GL/gl.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void __real_glBegin(GLenum mode);
extern void __real_glEnd(void);
extern void __real_glVertex3f(GLfloat x, GLfloat y, GLfloat z);
extern void __real_glNormal3f(GLfloat x, GLfloat y, GLfloat z);
extern void __real_glShadeModel(GLenum mode);
extern void __real_glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);

/* --- recorded command buffer -------------------------------------------- */

enum CmdTag {
    CMD_BEGIN = 1,
    CMD_END,
    CMD_VERTEX3F,
    CMD_NORMAL3F,
    CMD_SHADEMODEL,
    CMD_MATERIALFV,    /* face = e1, pname = e2, params = v[0..3] */
};

typedef struct {
    uint8_t tag;
    GLenum  e1;
    GLenum  e2;
    GLfloat v[4];
} Cmd;

typedef struct {
    Cmd  *cmds;
    size_t count;
    size_t capacity;
    int   in_use;
} List;

static List   *g_lists       = NULL;
static GLuint  g_lists_size  = 0;     /* allocated slots, including [0]. */
static GLuint  g_lists_alloc = 0;     /* highest id handed out by glGenLists. */

static int     g_recording      = 0;
static GLuint  g_recording_id   = 0;

/* --- exec state (per-thread; we only have one) -------------------------- */

/* Current sticky normal -- emitted before every __real_glVertex3f so the
 * "set normal once before glBegin, get applied to every vertex" semantics
 * of real GL hold despite emscripten's per-vertex-only glNormal3f. */
static GLfloat g_cur_normal[3] = { 0.0f, 0.0f, 1.0f };
static int     g_inside_begin  = 0;

/* GL_QUADS -> GL_TRIANGLES translation. emscripten's immediate-mode
 * emulation only supports the WebGL-native primitive types and aborts
 * on GL_QUADS (7) / GL_QUAD_STRIP (8). GL_QUAD_STRIP we just rewrite
 * to GL_TRIANGLE_STRIP at glBegin (vertex order is identical). GL_QUADS
 * needs vertex buffering: each 4-vertex quad emits as two triangles
 * (v0,v1,v2) + (v0,v2,v3), with each output vertex carrying its own
 * normal captured at the time the input vertex was issued. */
static int     g_translate_quads = 0;
static int     g_quad_count      = 0;
static GLfloat g_quad_v[4][3];
static GLfloat g_quad_n[4][3];

static int ensure_lists_capacity(GLuint id) {
    if (id < g_lists_size) return 1;
    GLuint want = g_lists_size ? g_lists_size : 16;
    while (want <= id) want *= 2;
    List *grown = (List *)realloc(g_lists, sizeof(List) * want);
    if (!grown) return 0;
    memset(grown + g_lists_size, 0, sizeof(List) * (want - g_lists_size));
    g_lists = grown;
    g_lists_size = want;
    return 1;
}

static Cmd *next_cmd(void) {
    if (!g_recording || g_recording_id == 0) return NULL;
    if (g_recording_id >= g_lists_size) return NULL;
    List *L = &g_lists[g_recording_id];
    if (L->count == L->capacity) {
        size_t cap = L->capacity ? L->capacity * 2 : 64;
        Cmd *grown = (Cmd *)realloc(L->cmds, sizeof(Cmd) * cap);
        if (!grown) return NULL;
        L->cmds = grown;
        L->capacity = cap;
    }
    Cmd *c = &L->cmds[L->count++];
    memset(c, 0, sizeof(*c));
    return c;
}

/* --- exec_*: the actual GL work, called by wrap (direct) and replay --- */

static void emit_one_vertex(const GLfloat *n, const GLfloat *v) {
    __real_glNormal3f(n[0], n[1], n[2]);
    __real_glVertex3f(v[0], v[1], v[2]);
}

static void exec_begin(GLenum mode) {
    g_quad_count = 0;
    g_translate_quads = 0;
    GLenum real_mode = mode;
    if (mode == GL_QUAD_STRIP) {
        /* QUAD_STRIP and TRIANGLE_STRIP consume vertices in the same
         * order; only the primitive interpretation differs. The
         * resulting tessellation matches pixel-for-pixel for the
         * convex-quad case glxgears uses. */
        real_mode = GL_TRIANGLE_STRIP;
    } else if (mode == GL_QUADS) {
        real_mode = GL_TRIANGLES;
        g_translate_quads = 1;
    }
    __real_glBegin(real_mode);
    g_inside_begin = 1;
}

static void exec_end(void) {
    /* Drop any in-flight partial quad (incomplete = invalid in real
     * GL too). */
    g_quad_count = 0;
    g_translate_quads = 0;
    __real_glEnd();
    g_inside_begin = 0;
}

static void exec_normal3f(GLfloat x, GLfloat y, GLfloat z) {
    /* Don't forward to __real_glNormal3f here -- emscripten's libGL
     * appends to a vertex-stream buffer that's only valid inside
     * glBegin/glEnd. We re-emit the current normal right before each
     * __real_glVertex3f instead, which matches real GL "current
     * normal" sticky semantics. */
    g_cur_normal[0] = x; g_cur_normal[1] = y; g_cur_normal[2] = z;
}

static void exec_vertex3f(GLfloat x, GLfloat y, GLfloat z) {
    if (!g_inside_begin) return;
    if (g_translate_quads) {
        /* Buffer the input quad; emit two triangles when full. */
        g_quad_v[g_quad_count][0] = x;
        g_quad_v[g_quad_count][1] = y;
        g_quad_v[g_quad_count][2] = z;
        g_quad_n[g_quad_count][0] = g_cur_normal[0];
        g_quad_n[g_quad_count][1] = g_cur_normal[1];
        g_quad_n[g_quad_count][2] = g_cur_normal[2];
        g_quad_count++;
        if (g_quad_count == 4) {
            emit_one_vertex(g_quad_n[0], g_quad_v[0]);
            emit_one_vertex(g_quad_n[1], g_quad_v[1]);
            emit_one_vertex(g_quad_n[2], g_quad_v[2]);
            emit_one_vertex(g_quad_n[0], g_quad_v[0]);
            emit_one_vertex(g_quad_n[2], g_quad_v[2]);
            emit_one_vertex(g_quad_n[3], g_quad_v[3]);
            g_quad_count = 0;
        }
        return;
    }
    emit_one_vertex(g_cur_normal, (GLfloat[]){ x, y, z });
}

static void exec_shade_model(GLenum mode) {
    /* emscripten warns "TODO: glShadeModel" but doesn't crash; forward
     * for completeness. */
    __real_glShadeModel(mode);
}

static void exec_material(GLenum face, GLenum pname, const GLfloat *params) {
    /* emscripten libGL aborts on GL_AMBIENT_AND_DIFFUSE (pname 5634);
     * split it. */
    if (pname == GL_AMBIENT_AND_DIFFUSE) {
        __real_glMaterialfv(face, GL_AMBIENT, params);
        __real_glMaterialfv(face, GL_DIFFUSE, params);
    } else {
        __real_glMaterialfv(face, pname, params);
    }
}

/* --- list lifecycle ------------------------------------------------------ */

GLuint glGenLists(GLsizei range) {
    if (range <= 0) return 0;
    GLuint first = g_lists_alloc + 1;
    if (!ensure_lists_capacity(first + range - 1)) return 0;
    for (GLsizei i = 0; i < range; i++) {
        List *L = &g_lists[first + i];
        L->cmds = NULL;
        L->count = L->capacity = 0;
        L->in_use = 1;
    }
    g_lists_alloc = first + range - 1;
    return first;
}

void glNewList(GLuint list, GLenum mode) {
    (void)mode;                /* GL_COMPILE only. */
    if (list == 0 || list >= g_lists_size || !g_lists[list].in_use) return;
    free(g_lists[list].cmds);
    g_lists[list].cmds = NULL;
    g_lists[list].count = g_lists[list].capacity = 0;
    g_recording = 1;
    g_recording_id = list;
}

void glEndList(void) {
    g_recording = 0;
    g_recording_id = 0;
}

void glDeleteLists(GLuint list, GLsizei range) {
    if (list == 0 || range <= 0) return;
    GLuint last = list + (GLuint)range - 1;
    if (last >= g_lists_size) last = g_lists_size - 1;
    for (GLuint i = list; i <= last; i++) {
        free(g_lists[i].cmds);
        g_lists[i].cmds = NULL;
        g_lists[i].count = g_lists[i].capacity = 0;
        g_lists[i].in_use = 0;
    }
}

void glCallList(GLuint list) {
    if (list == 0 || list >= g_lists_size || !g_lists[list].in_use) return;
    const List *L = &g_lists[list];
    for (size_t i = 0; i < L->count; i++) {
        const Cmd *c = &L->cmds[i];
        switch (c->tag) {
            case CMD_BEGIN:      exec_begin(c->e1);                     break;
            case CMD_END:        exec_end();                            break;
            case CMD_VERTEX3F:   exec_vertex3f(c->v[0], c->v[1], c->v[2]); break;
            case CMD_NORMAL3F:   exec_normal3f(c->v[0], c->v[1], c->v[2]); break;
            case CMD_SHADEMODEL: exec_shade_model(c->e1);               break;
            case CMD_MATERIALFV: exec_material(c->e1, c->e2, c->v);     break;
            default: break;
        }
    }
}

/* --- wrapped GL primitives ---------------------------------------------- */

void __wrap_glBegin(GLenum mode) {
    if (g_recording) {
        Cmd *c = next_cmd(); if (!c) return;
        c->tag = CMD_BEGIN; c->e1 = mode;
    } else exec_begin(mode);
}

void __wrap_glEnd(void) {
    if (g_recording) {
        Cmd *c = next_cmd(); if (!c) return;
        c->tag = CMD_END;
    } else exec_end();
}

void __wrap_glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    if (g_recording) {
        Cmd *c = next_cmd(); if (!c) return;
        c->tag = CMD_VERTEX3F; c->v[0] = x; c->v[1] = y; c->v[2] = z;
    } else exec_vertex3f(x, y, z);
}

void __wrap_glNormal3f(GLfloat x, GLfloat y, GLfloat z) {
    if (g_recording) {
        Cmd *c = next_cmd(); if (!c) return;
        c->tag = CMD_NORMAL3F; c->v[0] = x; c->v[1] = y; c->v[2] = z;
    } else exec_normal3f(x, y, z);
}

void __wrap_glShadeModel(GLenum mode) {
    if (g_recording) {
        Cmd *c = next_cmd(); if (!c) return;
        c->tag = CMD_SHADEMODEL; c->e1 = mode;
    } else exec_shade_model(mode);
}

void __wrap_glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
    if (g_recording) {
        Cmd *c = next_cmd(); if (!c) return;
        c->tag = CMD_MATERIALFV;
        c->e1 = face; c->e2 = pname;
        c->v[0] = params[0]; c->v[1] = params[1];
        c->v[2] = params[2]; c->v[3] = params[3];
    } else exec_material(face, pname, params);
}
