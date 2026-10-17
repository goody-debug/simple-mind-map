#include <adwaita.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

typedef struct _MindNode MindNode;
struct _MindNode {
    char *id;
    char *name;
    char *image_b64;
    int image_size;
    gboolean collapsed;
    double anim_progress; // 0.0 (collapsed) to 1.0 (expanded)

    MindNode *parent;
    GPtrArray *children; // MindNode*

    // Computed layout properties
    int depth;
    double x; // Vertical position in tree
    double y; // Horizontal position in tree
    double content_width;
    double content_height;
    double children_height;
    double subtree_height;
    double text_w;
    double text_h;

    cairo_surface_t *surface_cache;
    char *texture_b64_cached;
};

typedef struct {
    char *id;
    char *title;
    MindNode *root;
} Chapter;

typedef struct {
    MindNode *node;
    double start_val;
    double target_val;
    gint64 start_time;
    gint64 duration; // in microseconds
} NodeAnimation;

typedef struct {
    AdwApplication *app;
    GtkWidget *window;
    AdwOverlaySplitView *split_view;
    GtkWidget *chapter_list;
    GtkWidget *drawing_area;

    GPtrArray *chapters; // Chapter*
    char *active_chapter_id;

    // Canvas pan & zoom
    double pan_x;
    double pan_y;
    double zoom;
    double drag_start_x;
    double drag_start_y;
    double drag_pan_start_x;
    double drag_pan_start_y;

    // Undo / Redo JSON state snapshots
    GPtrArray *history; // char*
    int history_index;
    gboolean suppress_history;

    // Active animations for smooth node folding
    GPtrArray *active_animations; // NodeAnimation*

    // Inline Popover Editor
    MindNode *editing_node;
    GtkWidget *editor_popover;
    GtkWidget *editor_textview;
    GtkWidget *btn_img_minus;
    GtkWidget *btn_img_plus;
    GtkWidget *btn_img_remove;
    GtkWidget *btn_node_delete;
} AppState;

static AppState *app_state = NULL;

static const char *NORD_COLORS[] = {
    "#b48ead", "#88c0d0", "#a3be8c", "#ebcb8b", "#d08770", "#bf616a", "#81a1c1"
};

/* Forward Declarations */
static void save_state_to_disk(void);
static void commit_history(void);
static void rebuild_sidebar(void);
static void load_chapter(const char *id, gboolean reset_view);
static void open_node_editor(MindNode *node);
static gboolean drawing_area_tick_cb(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data);
static void compute_node_text_metrics(MindNode *n, PangoContext *pctx, int depth);

/* =========================================================================
 * Node & Chapter Memory Management
 * ========================================================================= */

static char *generate_id(const char *prefix) {
    return g_strdup_printf("%s_%08x%04x", prefix, g_random_int(), g_random_int() & 0xffff);
}

static MindNode *node_new(const char *name) {
    MindNode *n = g_new0(MindNode, 1);
    n->id = generate_id("node");
    n->name = g_strdup(name ? name : "");
    n->image_size = 40;
    n->collapsed = FALSE;
    n->anim_progress = 1.0;
    n->children = g_ptr_array_new_with_free_func(NULL);
    return n;
}

static void node_free(MindNode *n) {
    if (!n) return;
    g_free(n->id);
    g_free(n->name);
    g_free(n->image_b64);
    g_free(n->texture_b64_cached);
    if (n->surface_cache) cairo_surface_destroy(n->surface_cache);
    if (n->children) {
        for (guint i = 0; i < n->children->len; i++) {
            node_free(g_ptr_array_index(n->children, i));
        }
        g_ptr_array_free(n->children, TRUE);
    }
    g_free(n);
}

static void chapter_free(Chapter *c) {
    if (!c) return;
    g_free(c->id);
    g_free(c->title);
    node_free(c->root);
    g_free(c);
}

static Chapter *get_active_chapter(void) {
    if (!app_state->active_chapter_id) return NULL;
    for (guint i = 0; i < app_state->chapters->len; i++) {
        Chapter *c = g_ptr_array_index(app_state->chapters, i);
        if (g_strcmp0(c->id, app_state->active_chapter_id) == 0) return c;
    }
    return NULL;
}

static Chapter *create_default_chapter(void) {
    Chapter *c = g_new0(Chapter, 1);
    c->id = g_strdup("breathing_exchange");
    c->title = g_strdup("Breathing & Exchange of Gases");

    MindNode *root = node_new("Breathing & Exchange of Gases");
    root->image_size = 56;

    MindNode *c1 = node_new("1. Respiratory Organs");
    c1->parent = root;
    c1->collapsed = TRUE;
    c1->anim_progress = 0.0;

    const char *items[] = {
        "Sponges, Coelenterates, Flatworms (Entire body surface / Simple diffusion)",
        "Earthworms (Moist cuticle)",
        "Insects (Network of Tracheal tubes)"
    };
    for (int i = 0; i < 3; i++) {
        MindNode *sub = node_new(items[i]);
        sub->parent = c1;
        g_ptr_array_add(c1->children, sub);
    }

    g_ptr_array_add(root->children, c1);
    c->root = root;
    return c;
}

static void node_collapse_subtree_except_root(MindNode *n) {
    if (!n) return;
    if (n->parent != NULL) {
        n->collapsed = TRUE;
        n->anim_progress = 0.0;
    }
    if (n->children) {
        for (guint i = 0; i < n->children->len; i++) {
            node_collapse_subtree_except_root(g_ptr_array_index(n->children, i));
        }
    }
}

/* =========================================================================
 * Image Surface Caching (Base64 -> cairo_surface_t)
 * ========================================================================= */

static cairo_surface_t *node_get_surface(MindNode *n) {
    if (!n->image_b64 || !*n->image_b64) return NULL;
    if (n->surface_cache && g_strcmp0(n->image_b64, n->texture_b64_cached) == 0) {
        return n->surface_cache;
    }
    if (n->surface_cache) {
        cairo_surface_destroy(n->surface_cache);
        n->surface_cache = NULL;
    }
    g_free(n->texture_b64_cached);
    n->texture_b64_cached = g_strdup(n->image_b64);

    const char *comma = strchr(n->image_b64, ',');
    const char *b64_start = comma ? (comma + 1) : n->image_b64;

    gsize len = 0;
    guchar *data = g_base64_decode(b64_start, &len);
    if (!data || len == 0) {
        g_free(data);
        return NULL;
    }

    GBytes *bytes = g_bytes_new_take(data, len);
    GdkTexture *tex = gdk_texture_new_from_bytes(bytes, NULL);
    g_bytes_unref(bytes);
    if (!tex) return NULL;

    int w = gdk_texture_get_width(tex);
    int h = gdk_texture_get_height(tex);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    gdk_texture_download(tex,
                         cairo_image_surface_get_data(surf),
                         cairo_image_surface_get_stride(surf));
    cairo_surface_mark_dirty(surf);
    g_object_unref(tex);

    n->surface_cache = surf;
    return n->surface_cache;
}

/* =========================================================================
 * JSON Serialization & Persistence
 * ========================================================================= */

static JsonNode *serialize_mind_node(MindNode *n) {
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "id", n->id);
    json_object_set_string_member(obj, "name", n->name ? n->name : "");
    json_object_set_boolean_member(obj, "collapsed", n->collapsed);
    json_object_set_int_member(obj, "imageSize", n->image_size);
    if (n->image_b64) {
        json_object_set_string_member(obj, "image", n->image_b64);
    }
    if (n->children && n->children->len > 0) {
        JsonArray *arr = json_array_new();
        for (guint i = 0; i < n->children->len; i++) {
            json_array_add_element(arr, serialize_mind_node(g_ptr_array_index(n->children, i)));
        }
        json_object_set_array_member(obj, "children", arr);
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    return node;
}

static MindNode *deserialize_mind_node(JsonObject *obj, MindNode *parent, int depth) {
    const char *name = json_object_has_member(obj, "name") ? json_object_get_string_member(obj, "name") : "";
    MindNode *n = node_new(name);
    n->parent = parent;
    n->depth = depth;

    if (json_object_has_member(obj, "id")) {
        g_free(n->id);
        n->id = g_strdup(json_object_get_string_member(obj, "id"));
    }
    if (json_object_has_member(obj, "collapsed")) {
        n->collapsed = json_object_get_boolean_member(obj, "collapsed");
    }
    n->anim_progress = n->collapsed ? 0.0 : 1.0;

    if (json_object_has_member(obj, "imageSize")) {
        n->image_size = json_object_get_int_member(obj, "imageSize");
    } else {
        n->image_size = (depth == 0) ? 56 : 40;
    }
    if (json_object_has_member(obj, "image") && !json_object_get_null_member(obj, "image")) {
        n->image_b64 = g_strdup(json_object_get_string_member(obj, "image"));
    }

    const char *child_keys[] = { "children", "_children" };
    for (int k = 0; k < 2; k++) {
        if (json_object_has_member(obj, child_keys[k])) {
            JsonArray *arr = json_object_get_array_member(obj, child_keys[k]);
            if (arr) {
                if (k == 1) {
                    n->collapsed = TRUE;
                    n->anim_progress = 0.0;
                }
                guint len = json_array_get_length(arr);
                for (guint i = 0; i < len; i++) {
                    JsonObject *child_obj = json_array_get_object_element(arr, i);
                    if (child_obj) {
                        MindNode *child = deserialize_mind_node(child_obj, n, depth + 1);
                        g_ptr_array_add(n->children, child);
                    }
                }
            }
        }
    }
    return n;
}

static char *serialize_all_to_string(gboolean pretty) {
    JsonObject *root_obj = json_object_new();
    json_object_set_string_member(root_obj, "activeChapterId",
                                  app_state->active_chapter_id ? app_state->active_chapter_id : "");
    gboolean sidebar_open = adw_overlay_split_view_get_show_sidebar(app_state->split_view);
    json_object_set_boolean_member(root_obj, "sidebarOpen", sidebar_open);

    JsonArray *ch_arr = json_array_new();
    for (guint i = 0; i < app_state->chapters->len; i++) {
        Chapter *c = g_ptr_array_index(app_state->chapters, i);
        JsonObject *c_obj = json_object_new();
        json_object_set_string_member(c_obj, "id", c->id);
        json_object_set_string_member(c_obj, "title", c->title);
        json_object_set_member(c_obj, "dataset", serialize_mind_node(c->root));
        json_array_add_object_element(ch_arr, c_obj);
    }
    json_object_set_array_member(root_obj, "chapters", ch_arr);

    JsonNode *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, root_obj);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, pretty);
    json_generator_set_root(gen, root_node);
    char *str = json_generator_to_data(gen, NULL);

    g_object_unref(gen);
    json_node_free(root_node);
    return str;
}

static void restore_from_json_string(const char *json_str, gboolean restore_ui_state) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_str, -1, NULL)) return;

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return;
    JsonObject *root_obj = json_node_get_object(root);

    for (guint i = 0; i < app_state->chapters->len; i++) {
        chapter_free(g_ptr_array_index(app_state->chapters, i));
    }
    g_ptr_array_set_size(app_state->chapters, 0);

    if (json_object_has_member(root_obj, "chapters")) {
        JsonArray *arr = json_object_get_array_member(root_obj, "chapters");
        guint len = json_array_get_length(arr);
        for (guint i = 0; i < len; i++) {
            JsonObject *c_obj = json_array_get_object_element(arr, i);
            Chapter *c = g_new0(Chapter, 1);
            c->id = g_strdup(json_object_get_string_member(c_obj, "id"));
            c->title = g_strdup(json_object_get_string_member(c_obj, "title"));
            JsonObject *ds = json_object_get_object_member(c_obj, "dataset");
            c->root = deserialize_mind_node(ds, NULL, 0);
            g_ptr_array_add(app_state->chapters, c);
        }
    }

    if (restore_ui_state && json_object_has_member(root_obj, "sidebarOpen")) {
        adw_overlay_split_view_set_show_sidebar(
            app_state->split_view, json_object_get_boolean_member(root_obj, "sidebarOpen"));
    }

    if (json_object_has_member(root_obj, "activeChapterId")) {
        const char *saved_id = json_object_get_string_member(root_obj, "activeChapterId");
        if (saved_id && *saved_id) {
            g_free(app_state->active_chapter_id);
            app_state->active_chapter_id = g_strdup(saved_id);
        }
    }

    if (app_state->drawing_area && gtk_widget_get_realized(app_state->drawing_area)) {
        PangoContext *pctx = gtk_widget_get_pango_context(app_state->drawing_area);
        for (guint i = 0; i < app_state->chapters->len; i++) {
            Chapter *c = g_ptr_array_index(app_state->chapters, i);
            if (c->root) {
                compute_node_text_metrics(c->root, pctx, 0);
            }
        }
    }
}

static char *get_state_file_path(void) {
    char *dir = g_build_filename(g_get_user_data_dir(), "com.github.mindmap", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *file = g_build_filename(dir, "state.json", NULL);
    g_free(dir);
    return file;
}

static void save_state_to_disk(void) {
    char *path = get_state_file_path();
    char *data = serialize_all_to_string(TRUE);
    g_file_set_contents(path, data, -1, NULL);
    g_free(data);
    g_free(path);
}

static void load_state_from_disk(void) {
    char *path = get_state_file_path();
    char *contents = NULL;
    
    gboolean loaded_successfully = FALSE;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        restore_from_json_string(contents, TRUE);
        g_free(contents);
        loaded_successfully = (app_state->chapters->len > 0);
    }
    g_free(path);

    // If no chapters exist, or deserialization resulted in a missing root, create default
    if (!loaded_successfully || app_state->chapters->len == 0) {
        for (guint i = 0; i < app_state->chapters->len; i++) {
            chapter_free(g_ptr_array_index(app_state->chapters, i));
        }
        g_ptr_array_set_size(app_state->chapters, 0);
        g_ptr_array_add(app_state->chapters, create_default_chapter());
    }

    Chapter *active = get_active_chapter();
    if (!active || !active->root) {
        Chapter *first = g_ptr_array_index(app_state->chapters, 0);
        g_free(app_state->active_chapter_id);
        app_state->active_chapter_id = g_strdup(first->id);
    }
    
    // Final safety check: if active chapter still has no root, assign a default one
    active = get_active_chapter();
    if (active && !active->root) {
        active->root = node_new(active->title ? active->title : "Mind Map");
    }
}
/* =========================================================================
 * Optimized Text Metrics & Animation Engine
 * ========================================================================= */

static PangoLayout *create_node_pango_layout(PangoContext *pctx, MindNode *n) {
    PangoLayout *layout = pango_layout_new(pctx);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, "Cartograph CF, Cantarell, Sans");
    pango_font_description_set_weight(desc, n->depth == 0 ? PANGO_WEIGHT_BOLD :
                                            (n->depth == 1 ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_MEDIUM));
    pango_font_description_set_absolute_size(desc, 13.5 * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(layout, (n->name && *n->name) ? n->name : "", -1);

    int natural_w = 0, natural_h = 0;
    pango_layout_get_pixel_size(layout, &natural_w, &natural_h);
    if (natural_w > 290) {
        pango_layout_set_width(layout, 290 * PANGO_SCALE);
    }
    pango_layout_set_alignment(layout, (n->depth == 0) ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    return layout;
}

static void compute_node_text_metrics(MindNode *n, PangoContext *pctx, int depth) {
    n->depth = depth;

    PangoLayout *layout = create_node_pango_layout(pctx, n);
    PangoRectangle ink_rect, logical_rect;
    pango_layout_get_pixel_extents(layout, &ink_rect, &logical_rect);
    n->text_w = (n->name && *n->name) ? logical_rect.width : 0;
    n->text_h = (n->name && *n->name) ? logical_rect.height : 0;
    g_object_unref(layout);

    gboolean has_img = (n->image_b64 && *n->image_b64);
    int img_s = has_img ? (n->image_size > 0 ? n->image_size : (depth == 0 ? 56 : 40)) : 0;

    n->content_height = fmax(img_s, n->text_h) + 52.0;

    if (depth == 0) {
        n->content_width = 160.0;
    } else {
        double text_start_x = has_img ? (15.0 + img_s + 10.0) : 20.0;
        n->content_width = fmax(180.0, text_start_x + n->text_w + 75.0);
    }

    if (n->children) {
        for (guint i = 0; i < n->children->len; i++) {
            compute_node_text_metrics(g_ptr_array_index(n->children, i), pctx, depth + 1);
        }
    }
}

static void update_subtree_animation_metrics(MindNode *n, int depth) {
    n->depth = depth;
    if (n->children && n->children->len > 0) {
        double sum = 0;
        for (guint i = 0; i < n->children->len; i++) {
            MindNode *c = g_ptr_array_index(n->children, i);
            update_subtree_animation_metrics(c, depth + 1);
            sum += c->subtree_height;
        }
        n->children_height = sum;
        n->subtree_height = fmax(n->content_height, n->children_height * n->anim_progress);
    } else {
        n->children_height = 0;
        n->subtree_height = n->content_height;
    }
}

static void assign_coordinates(MindNode *n) {
    if (!n->parent) {
        n->x = 0;
        n->y = 0;
    }
    if (n->anim_progress > 0.0 && n->children && n->children->len > 0) {
        double top = n->x - ((n->children_height * n->anim_progress) / 2.0);
        for (guint i = 0; i < n->children->len; i++) {
            MindNode *c = g_ptr_array_index(n->children, i);
            c->x = top + (c->subtree_height / 2.0);
            c->y = n->y + n->content_width;
            top += c->subtree_height;
            assign_coordinates(c);
        }
    }
}

static void start_node_animation(MindNode *node, gboolean expand) {
    node->collapsed = !expand;
    double target = expand ? 1.0 : 0.0;
    if (fabs(node->anim_progress - target) < 0.001) return;

    NodeAnimation *anim = NULL;
    for (guint i = 0; i < app_state->active_animations->len; i++) {
        NodeAnimation *a = g_ptr_array_index(app_state->active_animations, i);
        if (a->node == node) {
            anim = a;
            break;
        }
    }

    GdkFrameClock *frame_clock = gtk_widget_get_frame_clock(app_state->drawing_area);
    gint64 now = frame_clock ? gdk_frame_clock_get_frame_time(frame_clock) : g_get_monotonic_time();

    if (!anim) {
        anim = g_new0(NodeAnimation, 1);
        anim->node = node;
        g_ptr_array_add(app_state->active_animations, anim);
    }
    anim->start_val = node->anim_progress;
    anim->target_val = target;
    anim->start_time = now;
    anim->duration = 300000; // 300 ms buttery smooth duration

    if (app_state->active_animations->len == 1 && anim == g_ptr_array_index(app_state->active_animations, 0)) {
        gtk_widget_add_tick_callback(app_state->drawing_area, drawing_area_tick_cb, NULL, NULL);
    }
}

static gboolean drawing_area_tick_cb(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
    if (!app_state || app_state->active_animations->len == 0) {
        return G_SOURCE_REMOVE;
    }

    gint64 now = gdk_frame_clock_get_frame_time(frame_clock);

    for (guint i = 0; i < app_state->active_animations->len; i++) {
        NodeAnimation *anim = g_ptr_array_index(app_state->active_animations, i);
        double elapsed = (double)(now - anim->start_time);
        double progress = elapsed / (double)anim->duration;
        if (progress >= 1.0) {
            anim->node->anim_progress = anim->target_val;
        } else {
            double eased = 1.0 - pow(1.0 - progress, 4.0);
            anim->node->anim_progress = anim->start_val + (anim->target_val - anim->start_val) * eased;
        }
    }

    GPtrArray *surviving_animations = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < app_state->active_animations->len; i++) {
        NodeAnimation *anim = g_ptr_array_index(app_state->active_animations, i);
        if (fabs(anim->node->anim_progress - anim->target_val) < 0.0001) {
            anim->node->anim_progress = anim->target_val;
            g_free(anim);
        } else {
            g_ptr_array_add(surviving_animations, anim);
        }
    }

    g_ptr_array_free(app_state->active_animations, FALSE);
    app_state->active_animations = surviving_animations;

    gtk_widget_queue_draw(widget);

    return (app_state->active_animations->len > 0) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* =========================================================================
 * Undo / Redo History
 * ========================================================================= */

static void commit_history(void) {
    if (app_state->suppress_history) return;
    char *snap = serialize_all_to_string(FALSE);

    if (app_state->history_index >= 0 &&
        g_strcmp0(snap, g_ptr_array_index(app_state->history, app_state->history_index)) == 0) {
        g_free(snap);
        save_state_to_disk();
        return;
    }

    while ((int)app_state->history->len > app_state->history_index + 1) {
        g_ptr_array_remove_index(app_state->history, app_state->history->len - 1);
    }

    g_ptr_array_add(app_state->history, snap);
    if (app_state->history->len > 40) {
        g_ptr_array_remove_index(app_state->history, 0);
        app_state->history_index--;
    } else {
        app_state->history_index++;
    }
    save_state_to_disk();
}

static void perform_undo(void) {
    if (app_state->history_index > 0) {
        gtk_popover_popdown(GTK_POPOVER(app_state->editor_popover));
        app_state->editing_node = NULL;
        app_state->history_index--;
        const char *snap = g_ptr_array_index(app_state->history, app_state->history_index);
        app_state->suppress_history = TRUE;
        restore_from_json_string(snap, FALSE);
        rebuild_sidebar();
        gtk_widget_queue_draw(app_state->drawing_area);
        save_state_to_disk();
        app_state->suppress_history = FALSE;
    }
}

static void perform_redo(void) {
    if (app_state->history_index < (int)app_state->history->len - 1) {
        gtk_popover_popdown(GTK_POPOVER(app_state->editor_popover));
        app_state->editing_node = NULL;
        app_state->history_index++;
        const char *snap = g_ptr_array_index(app_state->history, app_state->history_index);
        app_state->suppress_history = TRUE;
        restore_from_json_string(snap, FALSE);
        rebuild_sidebar();
        gtk_widget_queue_draw(app_state->drawing_area);
        save_state_to_disk();
        app_state->suppress_history = FALSE;
    }
}

/* =========================================================================
 * Cairo Rendering Engine
 * ========================================================================= */

static void draw_links_recursive(cairo_t *cr, MindNode *n) {
    if (n->anim_progress <= 0.0 || !n->children || n->children->len == 0) return;

    int color_idx = MIN(n->depth, 6);
    GdkRGBA col;
    gdk_rgba_parse(&col, NORD_COLORS[color_idx]);
    double lw = fmax(1.5, 4.0 - n->depth);

    cairo_set_line_width(cr, lw);
    cairo_set_source_rgba(cr, col.red, col.green, col.blue, 0.65 * n->anim_progress);

    for (guint i = 0; i < n->children->len; i++) {
        MindNode *c = g_ptr_array_index(n->children, i);
        double mid_y = (n->y + c->y) / 2.0;
        cairo_new_path(cr);
        cairo_move_to(cr, n->y, n->x);
        cairo_curve_to(cr, mid_y, n->x, mid_y, c->x, c->y, c->x);
        cairo_stroke(cr);
    }

    for (guint i = 0; i < n->children->len; i++) {
        MindNode *c = g_ptr_array_index(n->children, i);
        draw_links_recursive(cr, c);
    }
}

static void draw_nodes_recursive(cairo_t *cr, PangoContext *pctx, MindNode *n) {
    int color_idx = MIN(n->depth, 6);
    GdkRGBA nord_col, bg_col, text_col, accent_col;
    gdk_rgba_parse(&nord_col, NORD_COLORS[color_idx]);
    gdk_rgba_parse(&bg_col, "#2e3440");
    gdk_rgba_parse(&text_col, "#eceff4");
    gdk_rgba_parse(&accent_col, "#88c0d0");

    double radius = (n->depth == 0) ? 12.0 : 7.0;

    cairo_new_path(cr);
    cairo_arc(cr, n->y, n->x, radius, 0, 2 * G_PI);
    if (n->collapsed && n->children && n->children->len > 0) {
        cairo_set_source_rgba(cr, nord_col.red, nord_col.green, nord_col.blue, 1.0);
    } else {
        cairo_set_source_rgba(cr, bg_col.red, bg_col.green, bg_col.blue, 1.0);
    }
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 2.5);
    cairo_set_source_rgba(cr, nord_col.red, nord_col.green, nord_col.blue, 1.0);
    cairo_stroke(cr);

    gboolean is_root = (n->depth == 0);
    cairo_surface_t *surf = node_get_surface(n);
    gboolean has_img = (surf != NULL);
    int img_s = has_img ? (n->image_size > 0 ? n->image_size : (is_root ? 56 : 40)) : 0;

    if (has_img) {
        double img_x = is_root ? (n->y - 15.0 - img_s) : (n->y + 15.0);
        double img_y = n->x - (img_s / 2.0);

        int tw = cairo_image_surface_get_width(surf);
        int th = cairo_image_surface_get_height(surf);
        if (tw > 0 && th > 0) {
            cairo_save(cr);
            double r = 8.0;
            cairo_new_path(cr);
            cairo_arc(cr, img_x + img_s - r, img_y + r, r, -G_PI / 2, 0);
            cairo_arc(cr, img_x + img_s - r, img_y + img_s - r, r, 0, G_PI / 2);
            cairo_arc(cr, img_x + r, img_y + img_s - r, r, G_PI / 2, G_PI);
            cairo_arc(cr, img_x + r, img_y + r, r, G_PI, 3 * G_PI / 2);
            cairo_close_path(cr);
            cairo_clip(cr);

            double scale = fmax((double)img_s / tw, (double)img_s / th);
            cairo_translate(cr, img_x + img_s / 2.0, img_y + img_s / 2.0);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, surf, -tw / 2.0, -th / 2.0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }

    if (n->name && *n->name) {
        PangoLayout *layout = create_node_pango_layout(pctx, n);
        PangoRectangle ink_rect, logical_rect;
        pango_layout_get_pixel_extents(layout, &ink_rect, &logical_rect);

        double text_x;
        if (is_root) {
            double gap = has_img ? (15.0 + img_s + 10.0) : 20.0;
            text_x = n->y - gap - logical_rect.width - logical_rect.x;
        } else {
            double gap = has_img ? (15.0 + img_s + 10.0) : 20.0;
            text_x = n->y + gap - logical_rect.x;
        }
        double text_y = n->x - (logical_rect.height / 2.0) - logical_rect.y;

        if (is_root) {
            cairo_set_source_rgba(cr, accent_col.red, accent_col.green, accent_col.blue, 1.0);
        } else {
            cairo_set_source_rgba(cr, text_col.red, text_col.green, text_col.blue, 1.0);
        }

        cairo_new_path(cr);
        cairo_move_to(cr, text_x, text_y);
        pango_cairo_show_layout(cr, layout);
        cairo_new_path(cr);

        g_object_unref(layout);
    }

    if (n->anim_progress > 0.0 && n->children) {
        for (guint i = 0; i < n->children->len; i++) {
            draw_nodes_recursive(cr, pctx, g_ptr_array_index(n->children, i));
        }
    }
}

static void on_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    cairo_set_source_rgb(cr, 0x2e / 255.0, 0x34 / 255.0, 0x40 / 255.0);
    cairo_paint(cr);

    Chapter *ch = get_active_chapter();
    if (!ch || !ch->root) return;

    update_subtree_animation_metrics(ch->root, 0);
    assign_coordinates(ch->root);

    cairo_save(cr);
    cairo_translate(cr, app_state->pan_x, app_state->pan_y);
    cairo_scale(cr, app_state->zoom, app_state->zoom);

    PangoContext *pctx = gtk_widget_get_pango_context(GTK_WIDGET(area));
    draw_links_recursive(cr, ch->root);
    draw_nodes_recursive(cr, pctx, ch->root);

    cairo_restore(cr);
}

/* =========================================================================
 * Hit Testing & Interactive Canvas Gestures
 * ========================================================================= */

static MindNode *hit_test_node(MindNode *n, double world_x, double world_y) {
    if (!n) return NULL;

    gboolean is_root = (n->depth == 0);
    gboolean has_img = (n->image_b64 && *n->image_b64);
    int img_s = has_img ? (n->image_size > 0 ? n->image_size : (is_root ? 56 : 40)) : 0;

    double min_x = n->y - 18.0;
    double max_x = n->y + 18.0;
    if (is_root) {
        min_x = n->y - (has_img ? (15.0 + img_s + 10.0) : 20.0) - n->text_w - 12.0;
    } else {
        max_x = n->y + (has_img ? (15.0 + img_s + 10.0) : 20.0) + n->text_w + 12.0;
    }
    double half_h = fmax(20.0, fmax(img_s, n->text_h) / 2.0 + 10.0);

    if (world_x >= min_x && world_x <= max_x &&
        world_y >= (n->x - half_h) && world_y <= (n->x + half_h)) {
        return n;
    }

    if (n->anim_progress > 0.0 && n->children) {
        for (guint i = 0; i < n->children->len; i++) {
            MindNode *hit = hit_test_node(g_ptr_array_index(n->children, i), world_x, world_y);
            if (hit) return hit;
        }
    }
    return NULL;
}

static void on_left_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    if (hypot(x - app_state->drag_start_x, y - app_state->drag_start_y) > 5.0) return;

    Chapter *ch = get_active_chapter();
    if (!ch || !ch->root) return;

    double wx = (x - app_state->pan_x) / app_state->zoom;
    double wy = (y - app_state->pan_y) / app_state->zoom;

    MindNode *hit = hit_test_node(ch->root, wx, wy);
    if (hit && hit->children && hit->children->len > 0) {
        gboolean expand = hit->collapsed;
        start_node_animation(hit, expand);
        commit_history();
    }
}

static void on_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    Chapter *ch = get_active_chapter();
    if (!ch || !ch->root) return;

    double wx = (x - app_state->pan_x) / app_state->zoom;
    double wy = (y - app_state->pan_y) / app_state->zoom;

    MindNode *hit = hit_test_node(ch->root, wx, wy);
    if (hit) {
        open_node_editor(hit);
    }
}

static void on_drag_begin(GtkGestureDrag *gesture, double start_x, double start_y, gpointer user_data) {
    app_state->drag_start_x = start_x;
    app_state->drag_start_y = start_y;
    app_state->drag_pan_start_x = app_state->pan_x;
    app_state->drag_pan_start_y = app_state->pan_y;
}

static void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
    app_state->pan_x = app_state->drag_pan_start_x + offset_x;
    app_state->pan_y = app_state->drag_pan_start_y + offset_y;
    gtk_widget_queue_draw(app_state->drawing_area);
}

static void zoom_at_point(double factor, double center_x, double center_y) {
    double new_zoom = CLAMP(app_state->zoom * factor, 0.15, 3.5);
    double actual_factor = new_zoom / app_state->zoom;
    app_state->pan_x = center_x - (center_x - app_state->pan_x) * actual_factor;
    app_state->pan_y = center_y - (center_y - app_state->pan_y) * actual_factor;
    app_state->zoom = new_zoom;
    gtk_widget_queue_draw(app_state->drawing_area);
}

static gboolean on_scroll(GtkEventControllerScroll *ctrl, double dx, double dy, gpointer user_data) {
    double w = gtk_widget_get_width(app_state->drawing_area) / 2.0;
    double h = gtk_widget_get_height(app_state->drawing_area) / 2.0;
    double factor = (dy < 0) ? 1.15 : (1.0 / 1.15);
    zoom_at_point(factor, w, h);
    return TRUE;
}

/* =========================================================================
 * Inline Node Popover Editor & Clipboard Paste
 * ========================================================================= */

static void sync_editor_image_buttons(MindNode *n) {
    gboolean has_img = (n && n->image_b64 && *n->image_b64);
    gtk_widget_set_visible(app_state->btn_img_minus, has_img);
    gtk_widget_set_visible(app_state->btn_img_plus, has_img);
    gtk_widget_set_visible(app_state->btn_img_remove, has_img);
    gtk_widget_set_visible(app_state->btn_node_delete, n && n->depth > 0);
}

static void on_editor_text_changed(GtkTextBuffer *buffer, gpointer user_data) {
    if (!app_state->editing_node) return;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    g_free(app_state->editing_node->name);
    app_state->editing_node->name = text;

    PangoContext *pctx = gtk_widget_get_pango_context(app_state->drawing_area);
    compute_node_text_metrics(app_state->editing_node, pctx, app_state->editing_node->depth);
    gtk_widget_queue_draw(app_state->drawing_area);
}

static void on_editor_closed(GtkPopover *popover, gpointer user_data) {
    if (app_state->editing_node) {
        app_state->editing_node = NULL;
        commit_history();
    }
}

static void open_node_editor(MindNode *node) {
    app_state->editing_node = node;

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app_state->editor_textview));
    g_signal_handlers_block_by_func(buf, on_editor_text_changed, NULL);
    gtk_text_buffer_set_text(buf, node->name ? node->name : "", -1);
    g_signal_handlers_unblock_by_func(buf, on_editor_text_changed, NULL);

    sync_editor_image_buttons(node);

    double sx = node->y * app_state->zoom + app_state->pan_x;
    double sy = node->x * app_state->zoom + app_state->pan_y;
    GdkRectangle rect = { (int)sx, (int)sy, 24, 24 };
    gtk_popover_set_pointing_to(GTK_POPOVER(app_state->editor_popover), &rect);
    gtk_popover_popup(GTK_POPOVER(app_state->editor_popover));
    gtk_widget_grab_focus(app_state->editor_textview);
}

static void on_add_child_clicked(GtkButton *btn, gpointer user_data) {
    MindNode *parent = app_state->editing_node;
    if (!parent) return;

    start_node_animation(parent, TRUE);
    MindNode *child = node_new("New Branch");
    child->parent = parent;
    child->depth = parent->depth + 1;
    g_ptr_array_add(parent->children, child);

    gtk_popover_popdown(GTK_POPOVER(app_state->editor_popover));
    commit_history();

    PangoContext *pctx = gtk_widget_get_pango_context(app_state->drawing_area);
    Chapter *ch = get_active_chapter();
    if (ch && ch->root) {
        compute_node_text_metrics(ch->root, pctx, 0);
        assign_coordinates(ch->root);
    }
    gtk_widget_queue_draw(app_state->drawing_area);
    open_node_editor(child);
}

static void on_delete_node_clicked(GtkButton *btn, gpointer user_data) {
    MindNode *target = app_state->editing_node;
    if (!target || !target->parent) return;

    MindNode *parent = target->parent;
    app_state->editing_node = NULL;
    gtk_popover_popdown(GTK_POPOVER(app_state->editor_popover));

    g_ptr_array_remove(parent->children, target);
    node_free(target);

    commit_history();
    gtk_widget_queue_draw(app_state->drawing_area);
}

static void set_node_image_from_bytes(MindNode *n, GBytes *bytes) {
    gsize len = 0;
    const guchar *raw = g_bytes_get_data(bytes, &len);
    if (!raw || len == 0) return;

    char *b64 = g_base64_encode(raw, len);
    g_free(n->image_b64);
    n->image_b64 = g_strdup_printf("data:image/png;base64,%s", b64);
    g_free(b64);

    if (n->image_size <= 0) n->image_size = (n->depth == 0) ? 56 : 40;

    PangoContext *pctx = gtk_widget_get_pango_context(app_state->drawing_area);
    Chapter *ch = get_active_chapter();
    if (ch && ch->root && pctx) {
        compute_node_text_metrics(ch->root, pctx, 0);
    }

    sync_editor_image_buttons(n);
    commit_history();
    gtk_widget_queue_draw(app_state->drawing_area);
}

static void on_image_file_selected(GObject *source, GAsyncResult *res, gpointer user_data) {
    MindNode *target_node = user_data;
    GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dlg, res, &error);
    
    if (error) {
        g_error_free(error);
        if (file) g_object_unref(file);
        return;
    }
    if (!file || !target_node) {
        if (file) g_object_unref(file);
        return;
    }

    g_autoptr(GFileInputStream) stream = g_file_read(file, NULL, &error);
    g_object_unref(file);

    if (error) {
        g_error_free(error);
        return;
    }

    GError *pix_err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(G_INPUT_STREAM(stream), NULL, &pix_err);
    if (pix_err) {
        g_error_free(pix_err);
        return;
    }

    if (pixbuf) {
        char *buffer = NULL;
        gsize buffer_len = 0;
        if (gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &buffer_len, "png", NULL, NULL)) {
            GBytes *png_bytes = g_bytes_new_take(buffer, buffer_len);
            set_node_image_from_bytes(target_node, png_bytes);
            g_bytes_unref(png_bytes);
        }
        g_object_unref(pixbuf);
    }
}

static void on_upload_image_clicked(GtkButton *btn, gpointer user_data) {
    if (!app_state->editing_node) return;
    MindNode *target_node = app_state->editing_node;

    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select Image");
    
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images (PNG, JPEG, WebP)");
    gtk_file_filter_add_mime_type(filter, "image/png");
    gtk_file_filter_add_mime_type(filter, "image/jpeg");
    gtk_file_filter_add_mime_type(filter, "image/webp");
    
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_object_unref(filter);
    
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(filters);

    gtk_file_dialog_open(dlg, GTK_WINDOW(app_state->window), NULL, on_image_file_selected, target_node);
    g_object_unref(dlg);
}

static void on_resize_image_minus(GtkButton *btn, gpointer user_data) {
    if (!app_state->editing_node) return;
    app_state->editing_node->image_size = CLAMP(app_state->editing_node->image_size - 20, 20, 300);

    PangoContext *pctx = gtk_widget_get_pango_context(app_state->drawing_area);
    Chapter *ch = get_active_chapter();
    if (ch && ch->root && pctx) {
        compute_node_text_metrics(ch->root, pctx, 0);
    }

    commit_history();
    gtk_widget_queue_draw(app_state->drawing_area);
}

static void on_resize_image_plus(GtkButton *btn, gpointer user_data) {
    if (!app_state->editing_node) return;
    app_state->editing_node->image_size = CLAMP(app_state->editing_node->image_size + 20, 20, 300);

    PangoContext *pctx = gtk_widget_get_pango_context(app_state->drawing_area);
    Chapter *ch = get_active_chapter();
    if (ch && ch->root && pctx) {
        compute_node_text_metrics(ch->root, pctx, 0);
    }

    commit_history();
    gtk_widget_queue_draw(app_state->drawing_area);
}

static void on_remove_image_clicked(GtkButton *btn, gpointer user_data) {
    if (!app_state->editing_node) return;
    g_free(app_state->editing_node->image_b64);
    app_state->editing_node->image_b64 = NULL;

    PangoContext *pctx = gtk_widget_get_pango_context(app_state->drawing_area);
    Chapter *ch = get_active_chapter();
    if (ch && ch->root && pctx) {
        compute_node_text_metrics(ch->root, pctx, 0);
    }

    sync_editor_image_buttons(app_state->editing_node);
    commit_history();
    gtk_widget_queue_draw(app_state->drawing_area);
}

static void on_clipboard_texture_read(GObject *source, GAsyncResult *res, gpointer user_data) {
    GdkClipboard *cb = GDK_CLIPBOARD(source);
    g_autoptr(GdkTexture) tex = gdk_clipboard_read_texture_finish(cb, res, NULL);
    if (tex && app_state->editing_node) {
        GBytes *png_bytes = gdk_texture_save_to_png_bytes(tex);
        if (png_bytes) {
            set_node_image_from_bytes(app_state->editing_node, png_bytes);
            g_bytes_unref(png_bytes);
        }
    }
}

static gboolean on_editor_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                                      guint keycode, GdkModifierType state, gpointer user_data) {
    if (keyval == GDK_KEY_Return && !(state & GDK_SHIFT_MASK)) {
        gtk_popover_popdown(GTK_POPOVER(app_state->editor_popover));
        return TRUE;
    }
    if (keyval == GDK_KEY_Tab) {
        on_add_child_clicked(NULL, NULL);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_v || keyval == GDK_KEY_V)) {
        GdkClipboard *cb = gtk_widget_get_clipboard(app_state->window);
        if (gdk_content_formats_contain_gtype(gdk_clipboard_get_formats(cb), GDK_TYPE_TEXTURE)) {
            gdk_clipboard_read_texture_async(cb, NULL, on_clipboard_texture_read, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

/* =========================================================================
 * Sidebar & Chapter Management
 * ========================================================================= */

static void on_delete_chapter_clicked(GtkButton *btn, gpointer user_data) {
    const char *id = g_object_get_data(G_OBJECT(btn), "chapter-id");
    if (!id) return;

    for (guint i = 0; i < app_state->chapters->len; i++) {
        Chapter *c = g_ptr_array_index(app_state->chapters, i);
        if (g_strcmp0(c->id, id) == 0) {
            chapter_free(c);
            g_ptr_array_remove_index(app_state->chapters, i);
            break;
        }
    }

    if (g_strcmp0(app_state->active_chapter_id, id) == 0) {
        g_free(app_state->active_chapter_id);
        app_state->active_chapter_id = NULL;
        if (app_state->chapters->len > 0) {
            Chapter *first = g_ptr_array_index(app_state->chapters, 0);
            load_chapter(first->id, TRUE);
        } else {
            rebuild_sidebar();
            gtk_widget_queue_draw(app_state->drawing_area);
        }
    } else {
        rebuild_sidebar();
    }
    commit_history();
}

static void on_chapter_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    if (!row) return;
    const char *id = g_object_get_data(G_OBJECT(row), "chapter-id");
    if (id && g_strcmp0(id, app_state->active_chapter_id) != 0) {
        load_chapter(id, TRUE);
    }
}

static void rebuild_sidebar(void) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app_state->chapter_list)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(app_state->chapter_list), child);
    }

    for (guint i = 0; i < app_state->chapters->len; i++) {
        Chapter *c = g_ptr_array_index(app_state->chapters, i);

        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data_full(G_OBJECT(row), "chapter-id", g_strdup(c->id), g_free);
        gtk_widget_add_css_class(row, "chapter-row");
        if (g_strcmp0(c->id, app_state->active_chapter_id) == 0) {
            gtk_widget_add_css_class(row, "active-chapter");
        }

        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(hbox, 6);
        gtk_widget_set_margin_bottom(hbox, 6);
        gtk_widget_set_margin_start(hbox, 10);
        gtk_widget_set_margin_end(hbox, 6);

        GtkWidget *lbl = gtk_label_new(c->title);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(lbl, TRUE);

        GtkWidget *del_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(del_btn, "flat");
        gtk_widget_add_css_class(del_btn, "circular");
        g_object_set_data_full(G_OBJECT(del_btn), "chapter-id", g_strdup(c->id), g_free);
        g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_chapter_clicked), NULL);

        gtk_box_append(GTK_BOX(hbox), lbl);
        gtk_box_append(GTK_BOX(hbox), del_btn);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
        gtk_list_box_append(GTK_LIST_BOX(app_state->chapter_list), row);
    }
}

static void load_chapter(const char *id, gboolean reset_view) {
    if (!id) return;
    
    // Safely duplicate the ID first so we don't hit use-after-free issues if 'id' 
    // points to app_state->active_chapter_id
    char *new_id = g_strdup(id);
    g_free(app_state->active_chapter_id);
    app_state->active_chapter_id = new_id;

    Chapter *active_ch = get_active_chapter();
    if (active_ch && active_ch->root) {
        PangoContext *pctx = gtk_widget_get_pango_context(app_state->drawing_area);
        compute_node_text_metrics(active_ch->root, pctx, 0);
    }

    if (reset_view) {
        int w = gtk_widget_get_width(app_state->drawing_area);
        int h = gtk_widget_get_height(app_state->drawing_area);
        if (w <= 100) w = 1200;
        if (h <= 100) h = 800;

        gboolean sidebar_shown = adw_overlay_split_view_get_show_sidebar(app_state->split_view);
        app_state->pan_x = sidebar_shown ? 340.0 : (w / 2.0 - 150.0);
        app_state->pan_y = h / 2.0;
        app_state->zoom = 0.85;
    }

    rebuild_sidebar();
    save_state_to_disk();
    gtk_widget_queue_draw(app_state->drawing_area);
}

/* =========================================================================
 * Add Custom Map Dialog & Export / Import JSON
 * ========================================================================= */

static void on_new_map_response(AdwAlertDialog * dialog, gchar *response, gpointer user_data) {
    if (g_strcmp0(response, "create") == 0) {
        GtkEditable *entry = GTK_EDITABLE(user_data);
        const char *title = gtk_editable_get_text(entry);
        if (title && *title) {
            Chapter *c = g_new0(Chapter, 1);
            c->id = generate_id("custom");
            c->title = g_strdup(title);
            c->root = node_new(title);
            c->root->image_size = 56;
            c->root->collapsed = false;
            c->root->anim_progress = 1.0;
            node_collapse_subtree_except_root(c->root);
            g_ptr_array_add(app_state->chapters, c);
            load_chapter(c->id, TRUE);
            commit_history();
        }
    }
}

static void on_add_map_clicked(GtkButton *btn, gpointer user_data) {
    AdwDialog *dlg = adw_alert_dialog_new("New Mind Map", NULL);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Map Title...");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dlg), entry);

    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg),
                                   "cancel", "Cancel",
                                   "create", "Create",
                                   NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "create", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "create");
    g_signal_connect(dlg, "response", G_CALLBACK(on_new_map_response), entry);
    adw_dialog_present(dlg, app_state->window);
}

static void on_export_file_saved(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(dlg, res, NULL);
    Chapter *c = get_active_chapter();
    if (!file || !c) return;

    JsonObject *c_obj = json_object_new();
    json_object_set_string_member(c_obj, "id", c->id);
    json_object_set_string_member(c_obj, "title", c->title);
    json_object_set_member(c_obj, "dataset", serialize_mind_node(c->root));

    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, c_obj);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, root);
    char *json_str = json_generator_to_data(gen, NULL);

    char *path = g_file_get_path(file);
    if (path) {
        g_file_set_contents(path, json_str, -1, NULL);
        g_free(path);
    }

    g_free(json_str);
    g_object_unref(gen);
    json_node_free(root);
}

static void on_export_clicked(GtkButton *btn, gpointer user_data) {
    Chapter *c = get_active_chapter();
    if (!c) return;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Export Mind Map");
    char *default_name = g_strdup_printf("%s.json", c->title);
    gtk_file_dialog_set_initial_name(dlg, default_name);
    g_free(default_name);
    gtk_file_dialog_save(dlg, GTK_WINDOW(app_state->window), NULL, on_export_file_saved, NULL);
}

static void on_import_file_opened(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(dlg, res, NULL);
    if (!file) return;

    char *path = g_file_get_path(file);
    char *contents = NULL;
    if (path && g_file_get_contents(path, &contents, NULL, NULL)) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_data(parser, contents, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *obj = json_node_get_object(root);
                if (json_object_has_member(obj, "title") && json_object_has_member(obj, "dataset")) {
                    Chapter *c = g_new0(Chapter, 1);
                    c->id = generate_id("imported");
                    c->title = g_strdup(json_object_get_string_member(obj, "title"));
                    c->root = deserialize_mind_node(json_object_get_object_member(obj, "dataset"), NULL, 0);
                    node_collapse_subtree_except_root(c->root);
                    g_ptr_array_add(app_state->chapters, c);
                    load_chapter(c->id, TRUE);
                    commit_history();
                }
            }
        }
        g_free(contents);
    }
    g_free(path);
}

static void on_import_clicked(GtkButton *btn, gpointer user_data) {
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Import Mind Map (.json)");
    gtk_file_dialog_open(dlg, GTK_WINDOW(app_state->window), NULL, on_import_file_opened, NULL);
}

/* =========================================================================
 * Window & UI Assembly
 * ========================================================================= */

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data) {
    save_state_to_disk();
    return FALSE; // Return FALSE to allow the window to close normally
}

static void on_toggle_sidebar(GtkButton *btn, gpointer user_data) {
    gboolean cur = adw_overlay_split_view_get_show_sidebar(app_state->split_view);
    adw_overlay_split_view_set_show_sidebar(app_state->split_view, !cur);
    save_state_to_disk();
}

static void on_zoom_in_btn(GtkButton *btn, gpointer user_data) {
    double w = gtk_widget_get_width(app_state->drawing_area) / 2.0;
    double h = gtk_widget_get_height(app_state->drawing_area) / 2.0;
    zoom_at_point(1.25, w, h);
}

static void on_zoom_out_btn(GtkButton *btn, gpointer user_data) {
    double w = gtk_widget_get_width(app_state->drawing_area) / 2.0;
    double h = gtk_widget_get_height(app_state->drawing_area) / 2.0;
    zoom_at_point(1.0 / 1.25, w, h);
}

static void on_undo_btn(GtkButton *btn, gpointer user_data) { perform_undo(); }
static void on_redo_btn(GtkButton *btn, gpointer user_data) { perform_redo(); }

static gboolean on_window_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                                      guint keycode, GdkModifierType state, gpointer user_data) {
    if (state & GDK_CONTROL_MASK) {
        if (keyval == GDK_KEY_z || keyval == GDK_KEY_Z) {
            if (state & GDK_SHIFT_MASK) perform_redo();
            else perform_undo();
            return TRUE;
        }
        if (keyval == GDK_KEY_y || keyval == GDK_KEY_Y) {
            perform_redo();
            return TRUE;
        }
        if (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal) {
            on_zoom_in_btn(NULL, NULL);
            return TRUE;
        }
        if (keyval == GDK_KEY_minus) {
            on_zoom_out_btn(NULL, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

static void build_inline_popover(GtkWidget *parent_canvas) {
    app_state->editor_popover = gtk_popover_new();
    gtk_widget_set_parent(app_state->editor_popover, parent_canvas);
    gtk_popover_set_has_arrow(GTK_POPOVER(app_state->editor_popover), TRUE);
    g_signal_connect(app_state->editor_popover, "closed", G_CALLBACK(on_editor_closed), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(vbox, 6);
    gtk_widget_set_margin_bottom(vbox, 6);
    gtk_widget_set_margin_start(vbox, 6);
    gtk_widget_set_margin_end(vbox, 6);

    app_state->editor_textview = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app_state->editor_textview), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(app_state->editor_textview, 240, 56);
    gtk_widget_add_css_class(app_state->editor_textview, "inline-editor-text");

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app_state->editor_textview));
    g_signal_connect(buf, "changed", G_CALLBACK(on_editor_text_changed), NULL);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_editor_key_pressed), NULL);
    gtk_widget_add_controller(app_state->editor_textview, key_ctrl);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    GtkWidget *btn_img = gtk_button_new_from_icon_name("insert-image-symbolic");
    gtk_widget_set_tooltip_text(btn_img, "Add / Change Image (or Ctrl+V)");
    g_signal_connect(btn_img, "clicked", G_CALLBACK(on_upload_image_clicked), NULL);

    app_state->btn_img_minus = gtk_button_new_from_icon_name("zoom-out-symbolic");
    gtk_widget_set_tooltip_text(app_state->btn_img_minus, "Smaller Image");
    g_signal_connect(app_state->btn_img_minus, "clicked", G_CALLBACK(on_resize_image_minus), NULL);

    app_state->btn_img_plus = gtk_button_new_from_icon_name("zoom-in-symbolic");
    gtk_widget_set_tooltip_text(app_state->btn_img_plus, "Larger Image");
    g_signal_connect(app_state->btn_img_plus, "clicked", G_CALLBACK(on_resize_image_plus), NULL);

    app_state->btn_img_remove = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(app_state->btn_img_remove, "Remove Image");
    g_signal_connect(app_state->btn_img_remove, "clicked", G_CALLBACK(on_remove_image_clicked), NULL);

    GtkWidget *btn_branch = gtk_button_new_with_label("+ Branch");
    gtk_widget_add_css_class(btn_branch, "suggested-action");
    gtk_widget_set_tooltip_text(btn_branch, "Add Child Branch (Tab)");
    g_signal_connect(btn_branch, "clicked", G_CALLBACK(on_add_child_clicked), NULL);

    app_state->btn_node_delete = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(app_state->btn_node_delete, "destructive-action");
    gtk_widget_set_tooltip_text(app_state->btn_node_delete, "Delete Node");
    g_signal_connect(app_state->btn_node_delete, "clicked", G_CALLBACK(on_delete_node_clicked), NULL);

    gtk_box_append(GTK_BOX(toolbar), btn_img);
    gtk_box_append(GTK_BOX(toolbar), app_state->btn_img_minus);
    gtk_box_append(GTK_BOX(toolbar), app_state->btn_img_plus);
    gtk_box_append(GTK_BOX(toolbar), app_state->btn_img_remove);
    gtk_box_append(GTK_BOX(toolbar), btn_branch);
    gtk_box_append(GTK_BOX(toolbar), app_state->btn_node_delete);

    gtk_box_append(GTK_BOX(vbox), app_state->editor_textview);
    gtk_box_append(GTK_BOX(vbox), toolbar);
    gtk_popover_set_child(GTK_POPOVER(app_state->editor_popover), vbox);
}

static void apply_nord_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        "window { background-color: #2e3440; color: #eceff4; }"
        ".sidebar-box { background-color: #3b4252; border-right: 1px solid #4c566a; }"
        ".sidebar-header { background-color: #434c5e; padding: 14px; border-bottom: 1px solid #4c566a; }"
        ".sidebar-title { font-size: 18px; font-weight: bold; color: #eceff4; }"
        ".chapter-list { background-color: transparent; }"
        ".chapter-row { border-radius: 6px; margin: 2px 6px; color: #d8dee9; }"
        ".chapter-row:hover { background-color: rgba(255,255,255,0.06); color: #eceff4; }"
        ".active-chapter { background-color: rgba(136,192,208,0.18); color: #eceff4; font-weight: 600; }"
        ".floating-btn { background-color: rgba(67,76,94,0.9); color: #eceff4; border-radius: 8px; padding: 6px; }"
        ".inline-editor-text { background-color: #3b4252; color: #eceff4; border: 2px solid #88c0d0; border-radius: 6px; padding: 6px; }";
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static gboolean initialize_app_state_cb(gpointer user_data) {
    load_state_from_disk();
    
    adw_overlay_split_view_set_show_sidebar(app_state->split_view, FALSE);

    Chapter *active = get_active_chapter();
    if (!active && app_state->chapters->len > 0) {
        Chapter *first = g_ptr_array_index(app_state->chapters, 0);
        g_free(app_state->active_chapter_id);
        app_state->active_chapter_id = g_strdup(first->id);
    }

    if (app_state->active_chapter_id) {
        load_chapter(app_state->active_chapter_id, TRUE);
    }
    commit_history();
    return G_SOURCE_REMOVE;
}

static void on_drawing_area_realize(GtkWidget *widget, gpointer user_data) {
    Chapter *ch = get_active_chapter();
    if (ch && ch->root) {
        PangoContext *pctx = gtk_widget_get_pango_context(widget);
        compute_node_text_metrics(ch->root, pctx, 0);
        gtk_widget_queue_draw(widget);
    }
}

static void activate(GtkApplication *app, gpointer user_data) {
    AdwStyleManager *style_mgr = adw_style_manager_get_default();
    adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_FORCE_DARK);
    apply_nord_css();

    app_state->window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(app_state->window), "Simple Mind Map");
    gtk_window_set_default_size(GTK_WINDOW(app_state->window), 1200, 800);

    app_state->split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_show_sidebar(app_state->split_view, FALSE);
    adw_overlay_split_view_set_min_sidebar_width(app_state->split_view, 288);
    adw_overlay_split_view_set_max_sidebar_width(app_state->split_view, 288);

    /* Sidebar Construction */
    GtkWidget *sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar_box, "sidebar-box");

    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(header_box, "sidebar-header");
    GtkWidget *title_lbl = gtk_label_new("Simple Mind Map");
    gtk_widget_add_css_class(title_lbl, "sidebar-title");
    gtk_widget_set_hexpand(title_lbl, TRUE);
    gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.5);
    gtk_box_append(GTK_BOX(header_box), title_lbl);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    app_state->chapter_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app_state->chapter_list), GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(app_state->chapter_list), TRUE);
    gtk_widget_add_css_class(app_state->chapter_list, "chapter-list");
    g_signal_connect(app_state->chapter_list, "row-activated", G_CALLBACK(on_chapter_row_activated), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app_state->chapter_list);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(footer, 12);
    gtk_widget_set_margin_end(footer, 12);
    gtk_widget_set_margin_top(footer, 8);
    gtk_widget_set_margin_bottom(footer, 12);

    GtkWidget *btn_add = gtk_button_new_with_label("+ Add Custom Map");
    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_add_map_clicked), NULL);

    GtkWidget *io_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(io_box), TRUE);
    GtkWidget *btn_export = gtk_button_new_with_label("Export");
    GtkWidget *btn_import = gtk_button_new_with_label("Import");
    g_signal_connect(btn_export, "clicked", G_CALLBACK(on_export_clicked), NULL);
    g_signal_connect(btn_import, "clicked", G_CALLBACK(on_import_clicked), NULL);
    gtk_box_append(GTK_BOX(io_box), btn_export);
    gtk_box_append(GTK_BOX(io_box), btn_import);

    gtk_box_append(GTK_BOX(footer), btn_add);
    gtk_box_append(GTK_BOX(footer), io_box);

    gtk_box_append(GTK_BOX(sidebar_box), header_box);
    gtk_box_append(GTK_BOX(sidebar_box), scroll);
    gtk_box_append(GTK_BOX(sidebar_box), footer);

    adw_overlay_split_view_set_sidebar(app_state->split_view, sidebar_box);

    /* Main Content Overlay & Canvas */
    GtkWidget *overlay = gtk_overlay_new();
    app_state->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(app_state->drawing_area, TRUE);
    gtk_widget_set_vexpand(app_state->drawing_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app_state->drawing_area), on_draw, NULL, NULL);
    g_signal_connect(app_state->drawing_area, "realize", G_CALLBACK(on_drawing_area_realize), NULL);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), app_state->drawing_area);

    build_inline_popover(app_state->drawing_area);

    GtkWidget *toggle_btn = gtk_button_new_from_icon_name("sidebar-show-symbolic");
    gtk_widget_add_css_class(toggle_btn, "floating-btn");
    gtk_widget_set_halign(toggle_btn, GTK_ALIGN_START);
    gtk_widget_set_valign(toggle_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_top(toggle_btn, 14);
    gtk_widget_set_margin_start(toggle_btn, 14);
    g_signal_connect(toggle_btn, "clicked", G_CALLBACK(on_toggle_sidebar), NULL);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), toggle_btn);

    GtkWidget *right_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(right_bar, GTK_ALIGN_END);
    gtk_widget_set_valign(right_bar, GTK_ALIGN_START);
    gtk_widget_set_margin_top(right_bar, 14);
    gtk_widget_set_margin_end(right_bar, 14);

    GtkWidget *b_zin = gtk_button_new_from_icon_name("zoom-in-symbolic");
    GtkWidget *b_zout = gtk_button_new_from_icon_name("zoom-out-symbolic");
    GtkWidget *b_undo = gtk_button_new_from_icon_name("edit-undo-symbolic");
    GtkWidget *b_redo = gtk_button_new_from_icon_name("edit-redo-symbolic");
    gtk_widget_add_css_class(b_zin, "floating-btn");
    gtk_widget_add_css_class(b_zout, "floating-btn");
    gtk_widget_add_css_class(b_undo, "floating-btn");
    gtk_widget_add_css_class(b_redo, "floating-btn");

    g_signal_connect(b_zin, "clicked", G_CALLBACK(on_zoom_in_btn), NULL);
    g_signal_connect(b_zout, "clicked", G_CALLBACK(on_zoom_out_btn), NULL);
    g_signal_connect(b_undo, "clicked", G_CALLBACK(on_undo_btn), NULL);
    g_signal_connect(b_redo, "clicked", G_CALLBACK(on_redo_btn), NULL);

    gtk_box_append(GTK_BOX(right_bar), b_zin);
    gtk_box_append(GTK_BOX(right_bar), b_zout);
    gtk_box_append(GTK_BOX(right_bar), b_undo);
    gtk_box_append(GTK_BOX(right_bar), b_redo);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), right_bar);

    adw_overlay_split_view_set_content(app_state->split_view, overlay);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(app_state->window),
                                       GTK_WIDGET(app_state->split_view));

    /* Canvas Gestures */
    GtkGesture *left_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(left_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(left_click, "released", G_CALLBACK(on_left_click), NULL);
    gtk_widget_add_controller(app_state->drawing_area, GTK_EVENT_CONTROLLER(left_click));

    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_right_click), NULL);
    gtk_widget_add_controller(app_state->drawing_area, GTK_EVENT_CONTROLLER(right_click));

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), NULL);
    gtk_widget_add_controller(app_state->drawing_area, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(on_scroll), NULL);
    gtk_widget_add_controller(app_state->drawing_area, scroll_ctrl);

    GtkEventController *win_keys = gtk_event_controller_key_new();
    g_signal_connect(win_keys, "key-pressed", G_CALLBACK(on_window_key_pressed), NULL);
    gtk_widget_add_controller(app_state->window, win_keys);

    g_signal_connect(app_state->window, "close-request", G_CALLBACK(on_window_close_request), NULL);
    
    gtk_window_present(GTK_WINDOW(app_state->window));
    g_idle_add(initialize_app_state_cb, NULL);
}

int main(int argc, char **argv) {
    app_state = g_new0(AppState, 1);
    app_state->chapters = g_ptr_array_new();
    app_state->history = g_ptr_array_new_with_free_func(g_free);
    app_state->active_animations = g_ptr_array_new_with_free_func(g_free);
    app_state->history_index = -1;
    app_state->pan_x = 340.0;
    app_state->pan_y = 400.0;
    app_state->zoom = 0.85;

    app_state->app = adw_application_new("com.github.mindmap", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app_state->app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app_state->app), argc, argv);

    g_object_unref(app_state->app);
    return status;
}