#include "collider_debug.h"
#include "compat_bridge.h"

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unity's libmono is already in this process. 32-bit MonoArray header is
 * 16 bytes; reference elements follow. */
#define GROUP_COUNT 5

struct col_color {
    float r, g, b, a;
};

static const struct col_color group_color[GROUP_COUNT] = {
    { 1.0f, 0.15f, 0.15f, 0.55f },
    { 0.15f, 1.0f, 0.25f, 0.55f },
    { 0.2f, 0.45f, 1.0f, 0.55f },
    { 1.0f, 0.85f, 0.1f, 0.55f },
    { 1.0f, 0.2f, 0.9f, 0.45f }
};

static const char *group_title[GROUP_COUNT] = {
    "start dock 0-1",
    "bridge 2-3",
    "middle 4",
    "far 5-11",
    "box walls"
};

typedef void *(*fn_void)(void);
typedef void *(*fn_p)(void *);
typedef void *(*fn_pp)(void *, void *);
typedef void *(*fn_ppp)(void *, void *, void *);
typedef void *(*fn_psz)(void *, const char *);
typedef void *(*fn_ppsz)(void *, const char *, const char *);
typedef void *(*fn_ppszi)(void *, const char *, int);
typedef void *(*fn_invoke)(void *, void *, void **, void **);
typedef void (*fn_foreach)(void (*)(void *, void *), void *);
typedef char *(*fn_str)(void *);
typedef void (*fn_free)(void *);

static fn_void p_domain_get;
static fn_p p_thread_attach;
static fn_foreach p_assembly_foreach;
static fn_p p_assembly_get_image;
static fn_str p_image_get_name;
static fn_ppsz p_class_from_name;
static fn_p p_class_get_type;
static fn_pp p_type_get_object;
static fn_ppszi p_method;
static fn_invoke p_invoke;
static fn_psz p_string_new;
static fn_pp p_object_new;
static fn_str p_string_utf8;
static fn_free p_mono_free;
static fn_p p_unbox;

static void *unity_image;
static void *game_image;
static void *domain;
static int ready;
static int step = -1;
static int group_shown[GROUP_COUNT];
static void *group_material[GROUP_COUNT];

static void *m_find_objects;
static void *m_set_enabled;
static void *m_get_go;
static void *m_get_name;
static void *m_get_mesh;
static void *m_add_component;
static void *m_get_component;
static void *m_set_shared_mesh;
static void *m_set_material;
static void *m_shader_find;
static void *m_set_color;
static void *m_mat_ctor;
static void *m_create_prim;
static void *m_get_transform;
static void *m_get_size;
static void *m_get_center;
static void *m_transform_point;
static void *m_get_rotation;
static void *m_get_position;
static void *m_set_position;
static void *m_set_rotation;
static void *m_set_scale;

static void *klass_mesh_col;
static void *klass_box_col;
static void *klass_mesh_filter;
static void *klass_mesh_renderer;

static void on_assembly(void *assembly, void *unused)
{
    void *image;
    const char *name;
    (void)unused;
    image = p_assembly_get_image(assembly);
    name = image ? p_image_get_name(image) : NULL;
    if (name != NULL && strstr(name, "UnityEngine") != NULL &&
        strstr(name, "UI") == NULL)
        unity_image = image;
    if (name != NULL && strstr(name, "Assembly-CSharp") != NULL &&
        strstr(name, "firstpass") == NULL)
        game_image = image;
}

static void *sym(const char *name)
{
    void *guest = nfsmw_guest_symbol(name);
    if (guest != NULL)
        return guest;
    return dlsym(RTLD_DEFAULT, name);
}

static void *method_of(const char *ns, const char *cls, const char *name, int args)
{
    void *k = p_class_from_name(unity_image, ns, cls);
    void *m = k ? p_method(k, name, args) : NULL;
    if (m == NULL)
        printf("SG-COL missing %s.%s.%s/%d\n", ns, cls, name, args);
    return m;
}

static void *type_object(void *klass)
{
    return p_type_get_object(domain, p_class_get_type(klass));
}

static int load_api(void)
{
    if (ready)
        return 1;
    p_domain_get = (fn_void)sym("mono_domain_get");
    p_thread_attach = (fn_p)sym("mono_thread_attach");
    p_assembly_foreach = (fn_foreach)sym("mono_assembly_foreach");
    p_assembly_get_image = (fn_p)sym("mono_assembly_get_image");
    p_image_get_name = (fn_str)sym("mono_image_get_name");
    p_class_from_name = (fn_ppsz)sym("mono_class_from_name");
    p_class_get_type = (fn_p)sym("mono_class_get_type");
    p_type_get_object = (fn_pp)sym("mono_type_get_object");
    p_method = (fn_ppszi)sym("mono_class_get_method_from_name");
    p_invoke = (fn_invoke)sym("mono_runtime_invoke");
    p_string_new = (fn_psz)sym("mono_string_new");
    p_object_new = (fn_pp)sym("mono_object_new");
    p_string_utf8 = (fn_str)sym("mono_string_to_utf8");
    p_mono_free = (fn_free)sym("mono_free");
    p_unbox = (fn_p)sym("mono_object_unbox");
    if (!p_domain_get || !p_invoke || !p_class_from_name) {
        printf("SG-COL mono api missing\n");
        return 0;
    }
    domain = p_domain_get();
    if (domain == NULL) {
        printf("SG-COL no mono domain yet\n");
        return 0;
    }
    p_thread_attach(domain);
    p_assembly_foreach(on_assembly, NULL);
    if (unity_image == NULL) {
        printf("SG-COL UnityEngine image not loaded\n");
        return 0;
    }
    klass_mesh_col = p_class_from_name(unity_image, "UnityEngine", "MeshCollider");
    klass_box_col = p_class_from_name(unity_image, "UnityEngine", "BoxCollider");
    klass_mesh_filter = p_class_from_name(unity_image, "UnityEngine", "MeshFilter");
    klass_mesh_renderer = p_class_from_name(unity_image, "UnityEngine", "MeshRenderer");
    m_find_objects = method_of("UnityEngine", "Object", "FindObjectsOfType", 1);
    m_set_enabled = method_of("UnityEngine", "Collider", "set_enabled", 1);
    if (m_set_enabled == NULL)
        m_set_enabled = method_of("UnityEngine", "Behaviour", "set_enabled", 1);
    m_get_go = method_of("UnityEngine", "Component", "get_gameObject", 0);
    m_get_name = method_of("UnityEngine", "Object", "get_name", 0);
    m_get_mesh = method_of("UnityEngine", "MeshCollider", "get_sharedMesh", 0);
    m_add_component = method_of("UnityEngine", "GameObject", "AddComponent", 1);
    m_get_component = method_of("UnityEngine", "GameObject", "GetComponent", 1);
    m_set_shared_mesh = method_of("UnityEngine", "MeshFilter", "set_sharedMesh", 1);
    m_set_material = method_of("UnityEngine", "Renderer", "set_sharedMaterial", 1);
    if (m_set_material == NULL) {
        void *renderer = p_class_from_name(unity_image, "UnityEngine", "Renderer");
        m_set_material = renderer ? p_method(renderer, "set_material", 1) : NULL;
    }
    m_shader_find = method_of("UnityEngine", "Shader", "Find", 1);
    m_set_color = method_of("UnityEngine", "Material", "SetColor", 2);
    m_mat_ctor = method_of("UnityEngine", "Material", ".ctor", 1);
    m_create_prim = method_of("UnityEngine", "GameObject", "CreatePrimitive", 1);
    m_get_transform = method_of("UnityEngine", "Component", "get_transform", 0);
    m_get_size = method_of("UnityEngine", "BoxCollider", "get_size", 0);
    m_get_center = method_of("UnityEngine", "BoxCollider", "get_center", 0);
    m_transform_point = method_of("UnityEngine", "Transform", "TransformPoint", 1);
    m_get_rotation = method_of("UnityEngine", "Transform", "get_rotation", 0);
    m_set_position = method_of("UnityEngine", "Transform", "set_position", 1);
    m_set_rotation = method_of("UnityEngine", "Transform", "set_rotation", 1);
    m_set_scale = method_of("UnityEngine", "Transform", "set_localScale", 1);
    if (!m_find_objects || !m_set_enabled || !klass_mesh_col) {
        printf("SG-COL unity methods missing\n");
        return 0;
    }
    ready = 1;
    printf("SG-COL api ready\n");
    return 1;
}

static int group_for(const char *name, int box)
{
    if (name == NULL)
        return -1;
    if (box) {
        if (strcmp(name, "colin") == 0 || strcmp(name, "colout") == 0 ||
            strcmp(name, "col") == 0)
            return 4;
        return -1;
    }
    if (strcmp(name, "0") == 0 || strcmp(name, "1") == 0)
        return 0;
    if (strcmp(name, "2") == 0 || strcmp(name, "3") == 0 ||
        strcmp(name, "cbridge") == 0)
        return 1;
    if (strcmp(name, "4") == 0)
        return 2;
    if ((name[0] >= '5' && name[0] <= '9' && name[1] == '\0') ||
        strcmp(name, "10") == 0 || strcmp(name, "11") == 0)
        return 3;
    return -1;
}

static char *object_name(void *obj)
{
    void *exc = NULL;
    void *str = p_invoke(m_get_name, obj, NULL, &exc);
    char *utf;
    char *copy;
    if (exc != NULL || str == NULL)
        return NULL;
    utf = p_string_utf8(str);
    if (utf == NULL)
        return NULL;
    copy = strdup(utf);
    p_mono_free(utf);
    return copy;
}

static uint32_t array_count(void *arr, void ***out)
{
    uint32_t n;
    if (arr == NULL)
        return 0;
    n = *(uint32_t *)((char *)arr + 12);
    if (n > 4096U)
        n = *(uint32_t *)((char *)arr + 8);
    if (n > 4096U)
        return 0;
    *out = (void **)((char *)arr + 16);
    return n;
}

static void *find_of(void *klass)
{
    void *exc = NULL;
    void *type = type_object(klass);
    void *args[1];
    args[0] = type;
    return p_invoke(m_find_objects, NULL, args, &exc);
}

static void *ensure_material(int group)
{
    void *exc = NULL;
    void *shader_name;
    void *shader;
    void *mat_class;
    void *mat;
    void *color_name;
    void *args[2];
    if (group_material[group] != NULL)
        return group_material[group];
    shader_name = p_string_new(domain, "Diffuse");
    args[0] = shader_name;
    shader = p_invoke(m_shader_find, NULL, args, &exc);
    if (exc != NULL || shader == NULL) {
        printf("SG-COL shader Diffuse failed\n");
        return NULL;
    }
    mat_class = p_class_from_name(unity_image, "UnityEngine", "Material");
    mat = p_object_new(domain, mat_class);
    args[0] = shader;
    p_invoke(m_mat_ctor, mat, args, &exc);
    color_name = p_string_new(domain, "_Color");
    args[0] = color_name;
    args[1] = (void *)&group_color[group];
    p_invoke(m_set_color, mat, args, &exc);
    group_material[group] = mat;
    return mat;
}

static void tint_mesh(void *col)
{
    void *exc = NULL;
    void *go;
    void *mesh;
    void *filter_type;
    void *renderer_type;
    void *filter;
    void *renderer;
    void *mat;
    void *args[1];
    int g;
    char *name = object_name(col);
    g = group_for(name, 0);
    free(name);
    if (g < 0)
        return;
    go = p_invoke(m_get_go, col, NULL, &exc);
    mesh = p_invoke(m_get_mesh, col, NULL, &exc);
    if (go == NULL || mesh == NULL)
        return;
    filter_type = type_object(klass_mesh_filter);
    args[0] = filter_type;
    filter = p_invoke(m_get_component, go, args, &exc);
    if (filter == NULL)
        filter = p_invoke(m_add_component, go, args, &exc);
    if (filter == NULL)
        return;
    args[0] = mesh;
    p_invoke(m_set_shared_mesh, filter, args, &exc);
    renderer_type = type_object(klass_mesh_renderer);
    args[0] = renderer_type;
    renderer = p_invoke(m_get_component, go, args, &exc);
    if (renderer == NULL)
        renderer = p_invoke(m_add_component, go, args, &exc);
    mat = ensure_material(g);
    if (renderer != NULL && mat != NULL) {
        args[0] = mat;
        p_invoke(m_set_material, renderer, args, &exc);
    }
}

static void tint_box(void *col, int group)
{
    void *exc = NULL;
    int prim = 3;
    void *args[1];
    void *cube;
    void *tr_col;
    void *tr_cube;
    void *center;
    void *point;
    void *rot;
    void *size;
    void *go;
    void *renderer_type;
    void *renderer;
    void *mat;
    float *c;
    float *p;
    float *s;
    if (m_create_prim == NULL || p_unbox == NULL)
        return;
    args[0] = &prim;
    cube = p_invoke(m_create_prim, NULL, args, &exc);
    if (exc != NULL || cube == NULL)
        return;
    tr_col = p_invoke(m_get_transform, col, NULL, &exc);
    tr_cube = p_invoke(m_get_transform, cube, NULL, &exc);
    center = p_invoke(m_get_center, col, NULL, &exc);
    size = p_invoke(m_get_size, col, NULL, &exc);
    rot = p_invoke(m_get_rotation, tr_col, NULL, &exc);
    if (center == NULL || size == NULL || rot == NULL || tr_cube == NULL)
        return;
    c = (float *)p_unbox(center);
    args[0] = c;
    point = p_invoke(m_transform_point, tr_col, args, &exc);
    if (point == NULL)
        return;
    p = (float *)p_unbox(point);
    s = (float *)p_unbox(size);
    args[0] = p;
    p_invoke(m_set_position, tr_cube, args, &exc);
    args[0] = p_unbox(rot);
    p_invoke(m_set_rotation, tr_cube, args, &exc);
    args[0] = s;
    p_invoke(m_set_scale, tr_cube, args, &exc);
    go = cube;
    renderer_type = type_object(klass_mesh_renderer);
    args[0] = renderer_type;
    renderer = p_invoke(m_get_component, go, args, &exc);
    mat = ensure_material(group);
    if (renderer != NULL && mat != NULL) {
        args[0] = mat;
        p_invoke(m_set_material, renderer, args, &exc);
    }
}

static void disable_group(void *klass, int group, int box)
{
    void **elems = NULL;
    uint32_t n = array_count(find_of(klass), &elems);
    uint32_t i;
    unsigned char off = 0;
    void *args[1];
    for (i = 0; i < n; ++i) {
        char *name;
        int g;
        void *exc = NULL;
        if (elems[i] == NULL)
            continue;
        name = object_name(elems[i]);
        g = group_for(name, box);
        free(name);
        if (g != group)
            continue;
        args[0] = &off;
        p_invoke(m_set_enabled, elems[i], args, &exc);
    }
}

static void show_group(int group)
{
    void **elems = NULL;
    uint32_t n;
    uint32_t i;
    void *klass = group == 4 ? klass_box_col : klass_mesh_col;
    if (group_shown[group])
        return;
    n = array_count(find_of(klass), &elems);
    for (i = 0; i < n; ++i) {
        char *name;
        int g;
        if (elems[i] == NULL)
            continue;
        name = object_name(elems[i]);
        g = group_for(name, group == 4);
        free(name);
        if (g != group)
            continue;
        if (group == 4)
            tint_box(elems[i], group);
        else
            tint_mesh(elems[i]);
    }
    group_shown[group] = 1;
}

void collider_debug_next(void)
{
    int group;
    int disable;
    if (!load_api())
        return;
    p_thread_attach(domain);
    step += 1;
    if (step >= GROUP_COUNT * 2) {
        step = -1;
        printf("SG-COL off\n");
        return;
    }
    group = step / 2;
    disable = step & 1;
    if (!disable) {
        show_group(group);
        printf("SG-COL show %d %s\n", group, group_title[group]);
    } else {
        disable_group(group == 4 ? klass_box_col : klass_mesh_col, group,
                      group == 4);
        printf("SG-COL disable %d %s\n", group, group_title[group]);
    }
}

void collider_noclip_arm(void)
{
}

void collider_noclip_move(float lx, float ly)
{
    (void)lx;
    (void)ly;
}

/* The first lever is the one furthest back along the level (smallest Z).
 * Within 3 m it is switched on, and the body is carried by that lever's
 * motion. The use-order never finishes: the body cannot walk up to the
 * lever, so the game never starts the bridge itself. */
static int lever_fired;
static int lever_note;
static int ride_left;
static int ride_moved;
static void *m_lever_use;
static void *m_get_entry;
static void *klass_lever;
static void *klass_body;
static void *ride_anchor;
static void *ride_body;
static float anchor_last[3];

static int read_position(void *obj, float out[3])
{
    void *exc = NULL;
    void *tr;
    void *boxed;
    float *pos;

    if (obj == NULL || m_get_transform == NULL || m_get_position == NULL ||
        p_unbox == NULL)
        return 0;
    tr = p_invoke(m_get_transform, obj, NULL, &exc);
    if (tr == NULL || exc != NULL)
        return 0;
    boxed = p_invoke(m_get_position, tr, NULL, &exc);
    if (boxed == NULL || exc != NULL)
        return 0;
    pos = (float *)p_unbox(boxed);
    if (pos == NULL)
        return 0;
    out[0] = pos[0];
    out[1] = pos[1];
    out[2] = pos[2];
    return 1;
}

static void write_position(void *obj, const float p[3])
{
    void *exc = NULL;
    void *tr;
    void *args[1];
    float v[3];

    if (obj == NULL || m_set_position == NULL || m_get_transform == NULL)
        return;
    tr = p_invoke(m_get_transform, obj, NULL, &exc);
    if (tr == NULL || exc != NULL)
        return;
    v[0] = p[0];
    v[1] = p[1];
    v[2] = p[2];
    args[0] = v;
    p_invoke(m_set_position, tr, args, &exc);
}

/* Nearest body within 3 m on the ground. Returns the controller. */
static void *closest_body(float tx, float tz, float out[3])
{
    void **elems = NULL;
    void *arr;
    void *best_obj = NULL;
    uint32_t n;
    uint32_t i;
    float best = 3.0f * 3.0f;
    float p[3];

    if (klass_body == NULL)
        return NULL;
    arr = find_of(klass_body);
    n = array_count(arr, &elems);
    for (i = 0; i < n; ++i) {
        float dx, dz, d;
        if (!read_position(elems[i], p))
            continue;
        dx = p[0] - tx;
        dz = p[2] - tz;
        d = dx * dx + dz * dz;
        if (d < best) {
            best = d;
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            best_obj = elems[i];
        }
    }
    return best_obj;
}

static void ride_platform(void)
{
    float now[3];
    float body[3];
    float dx, dy, dz;

    if (ride_left <= 0 || ride_anchor == NULL || ride_body == NULL)
        return;
    ride_left--;
    if (!read_position(ride_anchor, now) || !read_position(ride_body, body))
        return;
    dx = now[0] - anchor_last[0];
    dy = now[1] - anchor_last[1];
    dz = now[2] - anchor_last[2];
    anchor_last[0] = now[0];
    anchor_last[1] = now[1];
    anchor_last[2] = now[2];
    if (fabsf(dx) + fabsf(dy) + fabsf(dz) < 0.0005f)
        return;
    body[0] += dx;
    body[1] += dy;
    body[2] += dz;
    write_position(ride_body, body);
    if (!ride_moved) {
        ride_moved = 1;
        printf("SG-LEVER carrying %.2f %.2f %.2f\n", dx, dy, dz);
    }
}

void collider_first_lever(void)
{
    void **elems = NULL;
    void *arr;
    void *exc = NULL;
    void *first = NULL;
    uint32_t n;
    uint32_t i;
    void *body_obj;
    float lever[3];
    float body[3];
    float best_z = 1.0e9f;
    int mode = 1;
    void *args[1];

    if (!load_api())
        return;
    if (ride_left > 0) {
        ride_platform();
        return;
    }
    if (lever_fired)
        return;
    if (game_image == NULL && p_assembly_foreach != NULL)
        p_assembly_foreach(on_assembly, NULL);
    if (game_image == NULL) {
        if (!lever_note) {
            lever_note = 1;
            printf("SG-LEVER no game image\n");
        }
        return;
    }
    if (m_get_position == NULL)
        m_get_position = method_of("UnityEngine", "Transform", "get_position", 0);
    if (klass_body == NULL)
        klass_body = p_class_from_name(unity_image, "UnityEngine",
                                       "CharacterController");
    if (klass_lever == NULL)
        klass_lever = p_class_from_name(game_image, "", "InteractionLever");
    if (m_lever_use == NULL && klass_lever != NULL)
        m_lever_use = p_method(klass_lever, "DoInteraction", 1);
    if (m_get_entry == NULL && klass_lever != NULL)
        m_get_entry = p_method(klass_lever, "GetEntryTransform", 0);
    if (klass_body == NULL || klass_lever == NULL || m_lever_use == NULL ||
        m_get_entry == NULL || m_get_position == NULL || m_set_position == NULL) {
        if (!lever_note) {
            lever_note = 1;
            printf("SG-LEVER lookup failed body=%p lever=%p use=%p entry=%p\n",
                   klass_body, klass_lever, m_lever_use, m_get_entry);
        }
        return;
    }

    arr = find_of(klass_lever);
    n = array_count(arr, &elems);
    for (i = 0; i < n; ++i) {
        float p[3];
        if (!read_position(elems[i], p))
            continue;
        if (p[2] < best_z) {
            best_z = p[2];
            lever[0] = p[0];
            lever[1] = p[1];
            lever[2] = p[2];
            first = elems[i];
        }
    }
    if (first == NULL)
        return;
    body_obj = closest_body(lever[0], lever[2], body);
    if (body_obj == NULL)
        return;
    args[0] = &mode;
    p_invoke(m_lever_use, first, args, &exc);
    lever_fired = 1;
    if (exc != NULL) {
        printf("SG-LEVER bridge failed\n");
        return;
    }
    printf("SG-LEVER bridge at %.1f %.1f, lever %.1f %.1f %.1f\n",
           body[0], body[2], lever[0], lever[1], lever[2]);
    ride_anchor = p_invoke(m_get_entry, first, NULL, &exc);
    if (ride_anchor != NULL && exc == NULL &&
        read_position(ride_anchor, anchor_last)) {
        ride_body = body_obj;
        ride_left = 600;
    } else {
        printf("SG-LEVER no platform transform\n");
    }
}
