/* Redwood - GTK3 front end. */
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "redwood.h"

enum { PAGE_NETWORK, PAGE_BLOCKS, PAGE_TXS, PAGE_CHART, PAGE_EVENTS };

typedef struct {
    Sim *sim;
    GtkWidget *win, *notebook;
    /* toolbar */
    GtkWidget *coin_combo, *btn_run, *btn_step, *btn_reset, *speed, *view_combo, *lbl_status;
    /* network page */
    GtkWidget *nodes_view;
    GtkListStore *nodes_store;
    GtkWidget *ed_title, *ed_hash, *ed_bal, *ed_act, *ed_auth, *ed_victim, *ed_cheat[CHEAT_COUNT];
    GtkWidget *set_interval, *set_reward, *set_latency, *set_jitter, *set_seed, *set_rate;
    GtkWidget *btn_add, *btn_del, *lbl_hint;
    /* blocks page */
    GtkWidget *blk_view, *blk_scroll, *blk_detail, *blk_show, *blk_follow;
    GtkListStore *blk_store;
    /* transactions page */
    GtkWidget *tx_view, *tx_scroll, *tx_show;
    GtkListStore *tx_store;
    /* events, chart */
    GtkWidget *log_view, *chart;
    GtkTextBuffer *log_buf;
    GtkTextMark *log_end;
    /* state */
    int running, sel, view_node, suppress, sel_block;
    double acc, pending_scroll_blk, pending_scroll_tx;
    gint64 last_us;
    int log_shown, log_epoch;
    int last_tick_blk, last_reset_blk, last_tick_tx, last_reset_tx;
} App;

static App A;

/* ---------------------------------------------------------------- helpers */

static const char *PALETTE[MAX_NODES] = {
    "#e6194b", "#3cb44b", "#4363d8", "#f58231", "#911eb4", "#42d4f4", "#f032e6", "#bfa100",
    "#469990", "#9a6324", "#800000", "#808000", "#000075", "#e377c2", "#7f7f7f", "#17becf"};

static void add_column(GtkWidget *tv, const char *title, int col, int color_col, gboolean right)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    if (right) g_object_set(r, "xalign", 1.0, NULL);
    GtkTreeViewColumn *c = color_col >= 0
        ? gtk_tree_view_column_new_with_attributes(title, r, "text", col, "foreground", color_col, NULL)
        : gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), c);
}

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

static GtkWidget *grid_spin(GtkWidget *grid, int row, const char *label, double lo, double hi, double step,
                            int digits, const char *tip, GCallback cb)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    GtkWidget *sp = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(sp), digits);
    gtk_widget_set_tooltip_text(sp, tip);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sp, 1, row, 1, 1);
    g_signal_connect(sp, "value-changed", cb, NULL);
    return sp;
}

static GtkWidget *framed(const char *title, GtkWidget *child)
{
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static int sel_node(void)
{
    if (A.sel < 0 || A.sel >= A.sim->nnodes) return -1;
    return A.sel;
}

static void ui_refresh(int force);

/* ------------------------------------------------------- status & enabling */

static void update_status(void)
{
    Sim *s = A.sim;
    int best = sim_best_tip(s);
    Block *b = &s->blocks[best];
    int64_t supply = 0;
    for (int i = 0; i < s->nnodes; i++) supply += b->bal[i];
    int tips[MAX_NODES], nt = 0;
    for (int i = 0; i < s->nnodes; i++) {
        int seen = 0;
        for (int k = 0; k < nt; k++) seen |= tips[k] == s->nodes[i].tip;
        if (!seen) tips[nt++] = s->nodes[i].tip;
    }
    char buf[400];
    snprintf(buf, sizeof buf,
             "%s   |   tick %d   |   best height %d   |   supply %lld %s   |   distinct tips %d   |   %d blocks, %d txs stored%s",
             coin_name(s->cfg.coin), s->tick, b->height, (long long)supply, coin_ticker(s->cfg.coin), nt,
             s->nblocks, s->ntxs, s->limit_hit ? "   |   STORAGE LIMIT REACHED" : "");
    gtk_label_set_text(GTK_LABEL(A.lbl_status), buf);
}

static void update_sensitivity(void)
{
    Sim *s = A.sim;
    int fresh = s->tick == 0;
    CoinType c = s->cfg.coin;
    gtk_widget_set_sensitive(A.btn_add, fresh && s->nnodes < MAX_NODES);
    gtk_widget_set_sensitive(A.btn_del, fresh && s->nnodes > 3);
    gtk_widget_set_sensitive(A.ed_bal, fresh);
    gtk_widget_set_sensitive(A.ed_auth, fresh && c == COIN_POA);
    gtk_widget_set_sensitive(A.ed_hash, c == COIN_POW);
    gtk_widget_set_sensitive(A.set_interval, fresh);
    gtk_widget_set_sensitive(A.set_reward, fresh);
    gtk_widget_set_sensitive(A.set_seed, fresh);
    gtk_widget_set_sensitive(A.ed_cheat[6], c != COIN_POW);
    gtk_widget_set_sensitive(A.btn_step, !A.running);
    gtk_label_set_text(GTK_LABEL(A.lbl_hint),
                       fresh ? "Nodes, balances, authorities and chain\nparameters can be edited until the\nfirst tick."
                             : "Press Reset to edit the network\nlayout, balances and chain parameters.");
}

/* ------------------------------------------------------------ nodes table */

enum { NC_NAME, NC_ROLE, NC_HASH, NC_BAL, NC_TIP, NC_MADE, NC_MAIN, NC_REJ, NC_REORG, NC_BEHAV, NC_COLOR, NC_N };

static void rebuild_node_widgets(void)
{
    Sim *s = A.sim;
    gtk_list_store_clear(A.nodes_store);
    for (int i = 0; i < s->nnodes; i++) {
        GtkTreeIter it;
        gtk_list_store_append(A.nodes_store, &it);
    }
    A.suppress++;
    int keep = gtk_combo_box_get_active(GTK_COMBO_BOX(A.view_combo));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(A.view_combo));
    for (int i = 0; i < s->nnodes; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.view_combo), s->nodes[i].name);
    if (keep < 0 || keep >= s->nnodes) keep = 0;
    gtk_combo_box_set_active(GTK_COMBO_BOX(A.view_combo), keep);
    A.view_node = keep;
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(A.ed_victim), 0, s->nnodes - 1);
    A.suppress--;
    if (A.sel >= s->nnodes) A.sel = s->nnodes - 1;
    if (A.sel < 0) A.sel = 0;
    GtkTreePath *p = gtk_tree_path_new_from_indices(A.sel, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(A.nodes_view)), p);
    gtk_tree_path_free(p);
}

static void refresh_nodes(void)
{
    Sim *s = A.sim;
    int rows = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(A.nodes_store), NULL);
    if (rows != s->nnodes) rebuild_node_widgets();
    int best = sim_best_tip(s);
    int main_cnt[MAX_NODES] = {0};
    for (int c = best; c > 0; c = s->blocks[c].parent) main_cnt[s->blocks[c].proposer]++;
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(A.nodes_store), &it);
    for (int i = 0; i < s->nnodes && ok; i++, ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(A.nodes_store), &it)) {
        Node *n = &s->nodes[i];
        char role[48], hash[32], beh[240], reorg[32];
        switch (s->cfg.coin) {
        case COIN_POW: snprintf(role, sizeof role, "Miner"); snprintf(hash, sizeof hash, "%d", n->hashpower); break;
        case COIN_POA: snprintf(role, sizeof role, "%s", n->is_auth ? "Authority" : "Observer"); snprintf(hash, sizeof hash, "-"); break;
        default: snprintf(role, sizeof role, "Validator"); snprintf(hash, sizeof hash, "-"); break;
        }
        snprintf(reorg, sizeof reorg, "%d (max %d)", n->reorgs, n->max_reorg);
        node_behaviour(n, beh, sizeof beh);
        gtk_list_store_set(A.nodes_store, &it, NC_NAME, n->name, NC_ROLE, role, NC_HASH, hash,
                           NC_BAL, (gint64)s->blocks[n->tip].bal[i], NC_TIP, s->blocks[n->tip].height,
                           NC_MADE, n->mined, NC_MAIN, main_cnt[i], NC_REJ, n->rejected, NC_REORG, reorg,
                           NC_BEHAV, beh, NC_COLOR, n->cheats ? "#d35400" : NULL, -1);
    }
}

/* ----------------------------------------------------------- node editor */

static void load_editor(void)
{
    int i = sel_node();
    if (i < 0) return;
    Node *n = &A.sim->nodes[i];
    A.suppress++;
    gtk_label_set_text(GTK_LABEL(A.ed_title), n->name);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.ed_hash), n->hashpower);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.ed_bal), (double)n->init_balance);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.ed_act), n->activity * 100.0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.ed_auth), n->is_auth);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.ed_victim), n->victim);
    for (int k = 0; k < CHEAT_COUNT; k++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.ed_cheat[k]), (n->cheats >> k) & 1);
    A.suppress--;
}

static void on_editor_changed(void)
{
    if (A.suppress) return;
    int i = sel_node();
    if (i < 0) return;
    Sim *s = A.sim;
    Node *n = &s->nodes[i];
    n->hashpower = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.ed_hash));
    n->init_balance = (int64_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(A.ed_bal));
    n->activity = gtk_spin_button_get_value(GTK_SPIN_BUTTON(A.ed_act)) / 100.0;
    n->is_auth = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.ed_auth));
    n->victim = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.ed_victim));
    unsigned c = 0;
    for (int k = 0; k < CHEAT_COUNT; k++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.ed_cheat[k]))) c |= 1u << k;
    n->cheats = c;
    if (s->tick == 0) sim_reset(s);
    ui_refresh(1);
}

static void on_node_selected(GtkTreeSelection *sel, gpointer d)
{
    (void)d;
    GtkTreeModel *m;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return;
    GtkTreePath *p = gtk_tree_model_get_path(m, &it);
    A.sel = gtk_tree_path_get_indices(p)[0];
    gtk_tree_path_free(p);
    load_editor();
}

static void on_settings_changed(void)
{
    if (A.suppress) return;
    Sim *s = A.sim;
    s->cfg.interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.set_interval));
    s->cfg.reward = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.set_reward));
    s->cfg.latency = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.set_latency));
    s->cfg.jitter = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.set_jitter));
    s->cfg.seed = (uint64_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(A.set_seed));
    s->cfg.cheat_rate = gtk_spin_button_get_value(GTK_SPIN_BUTTON(A.set_rate)) / 100.0;
    if (s->tick == 0) sim_reset(s);
    ui_refresh(1);
}

static void sync_settings_widgets(void)
{
    Sim *s = A.sim;
    A.suppress++;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.set_interval), s->cfg.interval);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.set_reward), (double)s->cfg.reward);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.set_latency), s->cfg.latency);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.set_jitter), s->cfg.jitter);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.set_seed), (double)s->cfg.seed);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.set_rate), s->cfg.cheat_rate * 100.0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(A.coin_combo), (int)s->cfg.coin);
    A.suppress--;
}

/* ------------------------------------------------------------ blocks page */

enum { B_ID, B_HEIGHT, B_HASH, B_PARENT, B_PROP, B_NTX, B_TIME, B_STATUS, B_COLOR, B_N };

static const char *block_status(Sim *s, int v, int id, const uint8_t *onchain, char *buf, size_t cap)
{
    Block *b = &s->blocks[id];
    Node *vn = &s->nodes[v];
    if (vn->known_blk[id] == 2) {
        snprintf(buf, cap, "Rejected: %s", reject_name(b->reason));
        return "#c0392b";
    }
    if (!b->is_public) {
        snprintf(buf, cap, "Private (%s)", s->nodes[b->author].name);
        return "#d35400";
    }
    if (!vn->known_blk[id]) {
        snprintf(buf, cap, "Not yet received");
        return "#7f8c8d";
    }
    if (onchain[id]) {
        snprintf(buf, cap, "Main chain");
        return NULL;
    }
    snprintf(buf, cap, "Stale fork");
    return "#8e44ad";
}

static gboolean restore_scroll_blk(gpointer d)
{
    (void)d;
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(A.blk_scroll)), A.pending_scroll_blk);
    return G_SOURCE_REMOVE;
}

static void show_block_detail(int id)
{
    Sim *s = A.sim;
    if (id < 0 || id >= s->nblocks) return;
    int v = A.view_node < s->nnodes ? A.view_node : 0;
    uint8_t *onchain = calloc((size_t)s->nblocks, 1);
    for (int c = s->nodes[v].tip; c >= 0; c = s->blocks[c].parent) onchain[c] = 1;
    char st[80], hx[65];
    block_status(s, v, id, onchain, st, sizeof st);
    Block *b = &s->blocks[id];
    GString *g = g_string_new(NULL);
    g_string_append_printf(g, "Block #%d, height %d\n\n", id, b->height);
    hex_full(b->hash, hx);
    g_string_append_printf(g, "Hash        %s\n", hx);
    if (b->parent >= 0) {
        hex_full(b->prev, hx);
        g_string_append_printf(g, "Parent      %s (block #%d)\n", hx, b->parent);
    }
    g_string_append_printf(g, "Proposer    %s", s->nodes[b->proposer].name);
    if (b->author != b->proposer) g_string_append_printf(g, "  (claimed; actually created by %s)", s->nodes[b->author].name);
    g_string_append_printf(g, "\nCreated     tick %d (time field %d, slot %d)\n", b->created, b->time, b->slot);
    if (id > 0) {
        if (s->cfg.coin == COIN_POW) {
            g_string_append_printf(g, "Proof       target %016llx, nonce %llu\n", (unsigned long long)b->target,
                                   (unsigned long long)b->nonce);
        } else {
            hex_full(b->sig, hx);
            g_string_append_printf(g, "Signature   %s\n", hx);
        }
    }
    g_string_append_printf(g, "Reward      %lld %s\n", (long long)b->reward, coin_ticker(s->cfg.coin));
    if (b->validated == 1) g_string_append_printf(g, "Validity    valid (cumulative weight %.0f)\n", b->weight);
    else if (b->validated == 2) g_string_append_printf(g, "Validity    INVALID: %s\n", reject_name(b->reason));
    else g_string_append(g, "Validity    not yet checked by any node\n");
    g_string_append_printf(g, "Visibility  %s\n", b->is_public ? "broadcast" : "private, not broadcast");
    g_string_append_printf(g, "Status      %s (from %s's viewpoint)\n", st, s->nodes[v].name);
    g_string_append_printf(g, "\nTransactions (%d)\n", b->ntx);
    for (int k = 0; k < b->ntx; k++) {
        Tx *t = &s->txs[b->txs[k]];
        g_string_append_printf(g, "  #%-6d %s -> %s  %lld  (nonce %u)\n", b->txs[k], s->nodes[t->from].name,
                               s->nodes[t->to].name, (long long)t->amount, t->nonce);
    }
    if (b->validated == 1) {
        g_string_append(g, "\nBalances after this block\n");
        for (int i = 0; i < s->nnodes; i++)
            g_string_append_printf(g, "  %-8s %lld\n", s->nodes[i].name, (long long)b->bal[i]);
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(A.blk_detail)), g->str, -1);
    g_string_free(g, TRUE);
    free(onchain);
}

static void on_block_selected(GtkTreeSelection *sel, gpointer d)
{
    (void)d;
    GtkTreeModel *m;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return;
    int id;
    gtk_tree_model_get(m, &it, B_ID, &id, -1);
    A.sel_block = id;
    show_block_detail(id);
}

static void refresh_blocks(int force)
{
    Sim *s = A.sim;
    gboolean follow = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.blk_follow));
    if (!force && s->reset_count == A.last_reset_blk && !(follow && s->tick != A.last_tick_blk)) return;
    A.last_tick_blk = s->tick;
    A.last_reset_blk = s->reset_count;
    int v = A.view_node < s->nnodes ? A.view_node : 0;
    uint8_t *onchain = calloc((size_t)s->nblocks, 1);
    for (int c = s->nodes[v].tip; c >= 0; c = s->blocks[c].parent) onchain[c] = 1;
    int show = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.blk_show));
    int lo = s->nblocks - show;
    if (lo < 0) lo = 0;
    A.pending_scroll_blk = gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(A.blk_scroll)));
    gtk_tree_view_set_model(GTK_TREE_VIEW(A.blk_view), NULL);
    gtk_list_store_clear(A.blk_store);
    GtkTreeIter it, selit;
    int have_sel = 0;
    for (int id = s->nblocks - 1; id >= lo; id--) {
        Block *b = &s->blocks[id];
        char hs[9], ps[9] = "-", st[80], prop[40];
        hex_short(b->hash, hs);
        if (b->parent >= 0) hex_short(b->prev, ps);
        const char *color = block_status(s, v, id, onchain, st, sizeof st);
        if (id == 0) snprintf(prop, sizeof prop, "genesis");
        else if (b->author != b->proposer) snprintf(prop, sizeof prop, "%s (forged by %d)", s->nodes[b->proposer].name, b->author);
        else snprintf(prop, sizeof prop, "%s", s->nodes[b->proposer].name);
        gtk_list_store_append(A.blk_store, &it);
        gtk_list_store_set(A.blk_store, &it, B_ID, id, B_HEIGHT, b->height, B_HASH, hs, B_PARENT, ps, B_PROP, prop,
                           B_NTX, (int)b->ntx, B_TIME, b->time, B_STATUS, st, B_COLOR, color, -1);
        if (id == A.sel_block) { selit = it; have_sel = 1; }
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(A.blk_view), GTK_TREE_MODEL(A.blk_store));
    if (have_sel) gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(A.blk_view)), &selit);
    else gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(A.blk_detail)),
                                  "Select a block to inspect it.", -1);
    g_idle_add(restore_scroll_blk, NULL);
    free(onchain);
}

/* ------------------------------------------------------- transactions page */

enum { T_ID, T_FROM, T_TO, T_AMT, T_NONCE, T_TICK, T_STATUS, T_COLOR, T_N };

static gboolean restore_scroll_tx(gpointer d)
{
    (void)d;
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(A.tx_scroll)), A.pending_scroll_tx);
    return G_SOURCE_REMOVE;
}

static void refresh_txs(int force)
{
    Sim *s = A.sim;
    if (!force && s->reset_count == A.last_reset_tx && s->tick == A.last_tick_tx) return;
    A.last_tick_tx = s->tick;
    A.last_reset_tx = s->reset_count;
    int v = A.view_node < s->nnodes ? A.view_node : 0;
    Block *tp = &s->blocks[s->nodes[v].tip];
    int *depth = calloc((size_t)s->ntxs + 1, sizeof(int));
    for (int c = s->nodes[v].tip; c >= 0; c = s->blocks[c].parent) {
        Block *b = &s->blocks[c];
        for (int k = 0; k < b->ntx; k++) depth[b->txs[k]] = tp->height - b->height + 1;
    }
    int show = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.tx_show));
    int lo = s->ntxs - show;
    if (lo < 0) lo = 0;
    A.pending_scroll_tx = gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(A.tx_scroll)));
    gtk_tree_view_set_model(GTK_TREE_VIEW(A.tx_view), NULL);
    gtk_list_store_clear(A.tx_store);
    for (int id = s->ntxs - 1; id >= lo; id--) {
        Tx *t = &s->txs[id];
        char st[80];
        const char *color = NULL;
        if (depth[id] > 0) snprintf(st, sizeof st, "Confirmed (%d)", depth[id]);
        else if (t->nonce < tp->nonces[t->from]) { snprintf(st, sizeof st, "Replaced (nonce already used)"); color = "#c0392b"; }
        else { snprintf(st, sizeof st, "Pending"); color = "#7f8c8d"; }
        GtkTreeIter it;
        gtk_list_store_append(A.tx_store, &it);
        gtk_list_store_set(A.tx_store, &it, T_ID, id, T_FROM, s->nodes[t->from].name, T_TO, s->nodes[t->to].name,
                           T_AMT, (gint64)t->amount, T_NONCE, (int)t->nonce, T_TICK, t->tick, T_STATUS, st,
                           T_COLOR, color, -1);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(A.tx_view), GTK_TREE_MODEL(A.tx_store));
    g_idle_add(restore_scroll_tx, NULL);
    free(depth);
}

/* ------------------------------------------------------------------ chart */

static gboolean chart_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    Sim *s = A.sim;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    gtk_render_background(sc, cr, 0, 0, W, H);
    GdkRGBA fg;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &fg);
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 1);
    cairo_move_to(cr, 12, 18);
    cairo_show_text(cr, "Balances on the heaviest chain over time");
    if (s->hist_n < 2) {
        cairo_move_to(cr, 12, 44);
        cairo_show_text(cr, "Run the simulation to collect data.");
        return FALSE;
    }
    const int L = 78, R = 130, T = 30, B = 30;
    double pw = W - L - R, ph = H - T - B;
    if (pw < 50 || ph < 50) return FALSE;
    int64_t maxv = 1;
    for (int k = 0; k < s->hist_n; k++)
        for (int i = 0; i < s->nnodes; i++)
            if (s->hist[k][i] > maxv) maxv = s->hist[k][i];
    int t0 = s->histt[0], t1 = s->histt[s->hist_n - 1];
    if (t1 <= t0) t1 = t0 + 1;
    for (int g = 0; g <= 4; g++) {
        double y = T + ph - ph * g / 4.0;
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.18);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, L, y);
        cairo_line_to(cr, L + pw, y);
        cairo_stroke(cr);
        char lab[32];
        snprintf(lab, sizeof lab, "%lld", (long long)(maxv * g / 4));
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 1);
        cairo_move_to(cr, 8, y + 4);
        cairo_show_text(cr, lab);
    }
    char lab[32];
    snprintf(lab, sizeof lab, "tick %d", t0);
    cairo_move_to(cr, L, H - 10);
    cairo_show_text(cr, lab);
    snprintf(lab, sizeof lab, "tick %d", t1);
    cairo_move_to(cr, L + pw - 70, H - 10);
    cairo_show_text(cr, lab);
    for (int i = 0; i < s->nnodes; i++) {
        GdkRGBA c;
        gdk_rgba_parse(&c, PALETTE[i]);
        cairo_set_source_rgba(cr, c.red, c.green, c.blue, 1);
        cairo_set_line_width(cr, 1.6);
        for (int k = 0; k < s->hist_n; k++) {
            double x = L + pw * (s->histt[k] - t0) / (double)(t1 - t0);
            double y = T + ph - ph * (double)s->hist[k][i] / (double)maxv;
            if (k == 0) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
        cairo_rectangle(cr, L + pw + 16, T + 6 + i * 18, 10, 10);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 1);
        cairo_move_to(cr, L + pw + 32, T + 15 + i * 18);
        cairo_show_text(cr, s->nodes[i].name);
    }
    return FALSE;
}

/* ----------------------------------------------------------------- events */

static void append_log(void)
{
    Sim *s = A.sim;
    if (s->reset_count != A.log_epoch || s->log_total < A.log_shown) {
        gtk_text_buffer_set_text(A.log_buf, "", 0);
        A.log_shown = 0;
        A.log_epoch = s->reset_count;
    }
    if (A.log_shown == s->log_total) return;
    int from = A.log_shown;
    if (s->log_total - from > LOG_LINES) from = s->log_total - LOG_LINES;
    GString *g = g_string_new(NULL);
    for (int i = from; i < s->log_total; i++) {
        g_string_append(g, s->log[i % LOG_LINES]);
        g_string_append_c(g, '\n');
    }
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(A.log_buf, &end);
    gtk_text_buffer_insert(A.log_buf, &end, g->str, -1);
    g_string_free(g, TRUE);
    A.log_shown = s->log_total;
    if (gtk_text_buffer_get_line_count(A.log_buf) > 8000) {
        GtkTextIter a, b;
        gtk_text_buffer_get_start_iter(A.log_buf, &a);
        gtk_text_buffer_get_iter_at_line(A.log_buf, &b, 3000);
        gtk_text_buffer_delete(A.log_buf, &a, &b);
    }
    gtk_text_buffer_get_end_iter(A.log_buf, &end);
    gtk_text_buffer_move_mark(A.log_buf, A.log_end, &end);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(A.log_view), A.log_end, 0.0, FALSE, 0, 0);
}

/* ---------------------------------------------------------------- refresh */

static void ui_refresh(int force)
{
    update_status();
    update_sensitivity();
    refresh_nodes();
    append_log();
    switch (gtk_notebook_get_current_page(GTK_NOTEBOOK(A.notebook))) {
    case PAGE_BLOCKS: refresh_blocks(force); break;
    case PAGE_TXS: refresh_txs(force); break;
    case PAGE_CHART: gtk_widget_queue_draw(A.chart); break;
    default: break;
    }
}

static gboolean ui_timer(gpointer d)
{
    (void)d;
    ui_refresh(0);
    return G_SOURCE_CONTINUE;
}

static gboolean sim_timer(gpointer d)
{
    (void)d;
    gint64 now = g_get_monotonic_time();
    if (!A.running) { A.last_us = 0; return G_SOURCE_CONTINUE; }
    if (A.last_us == 0) A.last_us = now;
    double dt = (double)(now - A.last_us) / 1e6;
    A.last_us = now;
    double speed = gtk_range_get_value(GTK_RANGE(A.speed));
    A.acc += dt * speed;
    gint64 t0 = now;
    while (A.acc >= 1.0 && !A.sim->limit_hit) {
        sim_step(A.sim);
        A.acc -= 1.0;
        if (g_get_monotonic_time() - t0 > 12000) break;   /* keep the UI responsive */
    }
    if (A.acc > 2.0) A.acc = 2.0;
    if (A.sim->limit_hit) {
        A.running = 0;
        gtk_button_set_label(GTK_BUTTON(A.btn_run), "Run");
    }
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- actions */

static void on_run(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    A.running = !A.running;
    gtk_button_set_label(GTK_BUTTON(A.btn_run), A.running ? "Pause" : "Run");
    ui_refresh(1);
}

static void on_step(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!A.running) sim_step(A.sim);
    ui_refresh(1);
}

static void stop_running(void)
{
    A.running = 0;
    A.acc = 0;
    gtk_button_set_label(GTK_BUTTON(A.btn_run), "Run");
}

static void on_reset(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    stop_running();
    sim_reset(A.sim);
    A.sel_block = -1;
    ui_refresh(1);
}

static void on_coin_changed(GtkComboBox *c, gpointer d)
{
    (void)d;
    if (A.suppress) return;
    int idx = gtk_combo_box_get_active(c);
    if (idx < 0) return;
    stop_running();
    A.sim->cfg.coin = (CoinType)idx;
    A.sim->cfg.interval = idx == COIN_POW ? 12 : 6;
    sim_reset(A.sim);
    A.sel_block = -1;
    sync_settings_widgets();
    ui_refresh(1);
}

static void on_view_changed(GtkComboBox *c, gpointer d)
{
    (void)d;
    if (A.suppress) return;
    int i = gtk_combo_box_get_active(c);
    if (i >= 0) A.view_node = i;
    ui_refresh(1);
}

static void on_add_node(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (A.sim->tick != 0) return;
    sim_add_node(A.sim);
    sim_reset(A.sim);
    ui_refresh(1);
}

static void on_del_node(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (A.sim->tick != 0) return;
    sim_remove_last_node(A.sim);
    sim_reset(A.sim);
    ui_refresh(1);
}

static gboolean deferred_refresh(gpointer d)
{
    (void)d;
    ui_refresh(1);
    return G_SOURCE_REMOVE;
}

static void on_page_switched(GtkNotebook *nb, GtkWidget *page, guint num, gpointer d)
{
    (void)nb; (void)page; (void)num; (void)d;
    g_idle_add(deferred_refresh, NULL);
}

static void on_view_param_changed(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    ui_refresh(1);
}

/* --------------------------------------------------------------- building */

static GtkWidget *build_network_page(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 8);

    A.nodes_store = gtk_list_store_new(NC_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_INT,
                                       G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    A.nodes_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(A.nodes_store));
    add_column(A.nodes_view, "Node", NC_NAME, NC_COLOR, FALSE);
    add_column(A.nodes_view, "Role", NC_ROLE, NC_COLOR, FALSE);
    add_column(A.nodes_view, "Hashpower", NC_HASH, NC_COLOR, TRUE);
    add_column(A.nodes_view, "Balance (own view)", NC_BAL, NC_COLOR, TRUE);
    add_column(A.nodes_view, "Tip height", NC_TIP, NC_COLOR, TRUE);
    add_column(A.nodes_view, "Blocks created", NC_MADE, NC_COLOR, TRUE);
    add_column(A.nodes_view, "On main chain", NC_MAIN, NC_COLOR, TRUE);
    add_column(A.nodes_view, "Blocks rejected", NC_REJ, NC_COLOR, TRUE);
    add_column(A.nodes_view, "Reorgs", NC_REORG, NC_COLOR, TRUE);
    add_column(A.nodes_view, "Behaviour", NC_BEHAV, NC_COLOR, FALSE);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(A.nodes_view)), "changed", G_CALLBACK(on_node_selected), NULL);
    GtkWidget *sw = scrolled(A.nodes_view);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_pack_start(GTK_BOX(page), sw, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);

    /* selected node */
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 10);
    A.ed_title = gtk_label_new("");
    gtk_widget_set_halign(A.ed_title, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g), A.ed_title, 0, 0, 2, 1);
    A.ed_hash = grid_spin(g, 1, "Hashpower (hashes/tick)", 0, 1000000, 50, 0,
                          "Proof of Work only: hash attempts per tick.", G_CALLBACK(on_editor_changed));
    A.ed_bal = grid_spin(g, 2, "Initial balance / stake", 0, 100000000, 100, 0,
                         "Genesis balance. In Proof of Stake this is also the starting stake.", G_CALLBACK(on_editor_changed));
    A.ed_act = grid_spin(g, 3, "Activity (% per tick)", 0, 100, 1, 0,
                         "Chance each tick that this node sends a payment.", G_CALLBACK(on_editor_changed));
    A.ed_victim = grid_spin(g, 4, "Victim node index", 0, MAX_NODES - 1, 1, 0,
                            "Target of 'Censor victim'.", G_CALLBACK(on_editor_changed));
    A.ed_auth = gtk_check_button_new_with_label("Trusted authority (Cedar)");
    gtk_widget_set_tooltip_text(A.ed_auth, "Proof of Authority: only authorities may produce blocks, in round-robin order.");
    g_signal_connect(A.ed_auth, "toggled", G_CALLBACK(on_editor_changed), NULL);
    gtk_grid_attach(GTK_GRID(g), A.ed_auth, 0, 5, 2, 1);
    gtk_box_pack_start(GTK_BOX(row), framed("Selected node", g), FALSE, FALSE, 0);

    /* cheats */
    GtkWidget *cv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    for (int k = 0; k < CHEAT_COUNT; k++) {
        A.ed_cheat[k] = gtk_check_button_new_with_label(cheat_name(k));
        gtk_widget_set_tooltip_text(A.ed_cheat[k], cheat_tip(k));
        g_signal_connect(A.ed_cheat[k], "toggled", G_CALLBACK(on_editor_changed), NULL);
        gtk_box_pack_start(GTK_BOX(cv), A.ed_cheat[k], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(row), framed("Cheating behaviour (default: follows the rules)", cv), FALSE, FALSE, 0);

    /* settings */
    GtkWidget *sg = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(sg), 6);
    gtk_grid_set_column_spacing(GTK_GRID(sg), 10);
    A.set_interval = grid_spin(sg, 0, "Block interval / slot (ticks)", 2, 60, 1, 0,
                               "Target block time for PoW; slot length for PoA and PoS.", G_CALLBACK(on_settings_changed));
    A.set_reward = grid_spin(sg, 1, "Block reward", 1, 100000, 1, 0, "Coins minted per block.", G_CALLBACK(on_settings_changed));
    A.set_latency = grid_spin(sg, 2, "Network latency (ticks)", 1, 30, 1, 0, "Minimum message delay.", G_CALLBACK(on_settings_changed));
    A.set_jitter = grid_spin(sg, 3, "Latency jitter (ticks)", 0, 30, 1, 0, "Random extra delay, 0 to this value.", G_CALLBACK(on_settings_changed));
    A.set_seed = grid_spin(sg, 4, "Random seed", 0, 4294967295.0, 1, 0, "Same seed and settings reproduce the same run.", G_CALLBACK(on_settings_changed));
    A.set_rate = grid_spin(sg, 5, "Attack frequency (% per tick)", 0, 100, 0.5, 1,
                           "How often an idle cheater launches a double-spend or rewrite attack.", G_CALLBACK(on_settings_changed));
    gtk_box_pack_start(GTK_BOX(row), framed("Chain and network", sg), FALSE, FALSE, 0);

    GtkWidget *bv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    A.btn_add = gtk_button_new_with_label("Add node");
    A.btn_del = gtk_button_new_with_label("Remove last node");
    A.lbl_hint = gtk_label_new("");
    gtk_widget_set_halign(A.lbl_hint, GTK_ALIGN_START);
    g_signal_connect(A.btn_add, "clicked", G_CALLBACK(on_add_node), NULL);
    g_signal_connect(A.btn_del, "clicked", G_CALLBACK(on_del_node), NULL);
    gtk_box_pack_start(GTK_BOX(bv), A.btn_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bv), A.btn_del, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bv), A.lbl_hint, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(row), framed("Network", bv), FALSE, FALSE, 0);
    return page;
}

static GtkWidget *build_blocks_page(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 8);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Show last"), FALSE, FALSE, 0);
    A.blk_show = gtk_spin_button_new_with_range(20, 5000, 20);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.blk_show), 300);
    g_signal_connect(A.blk_show, "value-changed", G_CALLBACK(on_view_param_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar), A.blk_show, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("blocks, every fork and rejected block included. Status is shown from the viewpoint node."), FALSE, FALSE, 0);
    A.blk_follow = gtk_check_button_new_with_label("Follow live");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.blk_follow), TRUE);
    gtk_box_pack_end(GTK_BOX(bar), A.blk_follow, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), bar, FALSE, FALSE, 0);

    A.blk_store = gtk_list_store_new(B_N, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    A.blk_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(A.blk_store));
    add_column(A.blk_view, "Height", B_HEIGHT, B_COLOR, TRUE);
    add_column(A.blk_view, "Block", B_ID, B_COLOR, TRUE);
    add_column(A.blk_view, "Hash", B_HASH, B_COLOR, FALSE);
    add_column(A.blk_view, "Parent", B_PARENT, B_COLOR, FALSE);
    add_column(A.blk_view, "Proposer", B_PROP, B_COLOR, FALSE);
    add_column(A.blk_view, "Txs", B_NTX, B_COLOR, TRUE);
    add_column(A.blk_view, "Time", B_TIME, B_COLOR, TRUE);
    add_column(A.blk_view, "Status", B_STATUS, B_COLOR, FALSE);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(A.blk_view)), "changed", G_CALLBACK(on_block_selected), NULL);
    A.blk_scroll = scrolled(A.blk_view);

    A.blk_detail = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(A.blk_detail), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(A.blk_detail), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(A.blk_detail), 8);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(paned), A.blk_scroll, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), scrolled(A.blk_detail), FALSE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 330);
    gtk_box_pack_start(GTK_BOX(page), paned, TRUE, TRUE, 0);
    return page;
}

static GtkWidget *build_txs_page(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 8);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Show last"), FALSE, FALSE, 0);
    A.tx_show = gtk_spin_button_new_with_range(50, 20000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.tx_show), 500);
    g_signal_connect(A.tx_show, "value-changed", G_CALLBACK(on_view_param_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar), A.tx_show, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("transactions. Confirmation depth is measured on the viewpoint node's best chain."), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), bar, FALSE, FALSE, 0);
    A.tx_store = gtk_list_store_new(T_N, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_INT,
                                    G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    A.tx_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(A.tx_store));
    add_column(A.tx_view, "Tx", T_ID, T_COLOR, TRUE);
    add_column(A.tx_view, "From", T_FROM, T_COLOR, FALSE);
    add_column(A.tx_view, "To", T_TO, T_COLOR, FALSE);
    add_column(A.tx_view, "Amount", T_AMT, T_COLOR, TRUE);
    add_column(A.tx_view, "Nonce", T_NONCE, T_COLOR, TRUE);
    add_column(A.tx_view, "Created (tick)", T_TICK, T_COLOR, TRUE);
    add_column(A.tx_view, "Status", T_STATUS, T_COLOR, FALSE);
    A.tx_scroll = scrolled(A.tx_view);
    gtk_box_pack_start(GTK_BOX(page), A.tx_scroll, TRUE, TRUE, 0);
    return page;
}

static GtkWidget *build_toolbar(void)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 8);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Coin"), FALSE, FALSE, 0);
    A.coin_combo = gtk_combo_box_text_new();
    for (int c = 0; c < COIN_COUNT; c++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.coin_combo), coin_name((CoinType)c));
    g_signal_connect(A.coin_combo, "changed", G_CALLBACK(on_coin_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar), A.coin_combo, FALSE, FALSE, 0);
    A.btn_run = gtk_button_new_with_label("Run");
    A.btn_step = gtk_button_new_with_label("Step");
    A.btn_reset = gtk_button_new_with_label("Reset");
    g_signal_connect(A.btn_run, "clicked", G_CALLBACK(on_run), NULL);
    g_signal_connect(A.btn_step, "clicked", G_CALLBACK(on_step), NULL);
    g_signal_connect(A.btn_reset, "clicked", G_CALLBACK(on_reset), NULL);
    gtk_widget_set_tooltip_text(A.btn_step, "Advance one tick while paused.");
    gtk_widget_set_tooltip_text(A.btn_reset, "Discard the chain and start again with the same nodes and settings.");
    gtk_box_pack_start(GTK_BOX(bar), A.btn_run, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), A.btn_step, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), A.btn_reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Speed (ticks/s)"), FALSE, FALSE, 8);
    A.speed = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 500, 1);
    gtk_range_set_value(GTK_RANGE(A.speed), 30);
    gtk_scale_set_value_pos(GTK_SCALE(A.speed), GTK_POS_RIGHT);
    gtk_widget_set_size_request(A.speed, 220, -1);
    gtk_box_pack_start(GTK_BOX(bar), A.speed, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Viewpoint"), FALSE, FALSE, 8);
    A.view_combo = gtk_combo_box_text_new();
    gtk_widget_set_tooltip_text(A.view_combo, "Block and transaction status is shown as this node sees it.");
    g_signal_connect(A.view_combo, "changed", G_CALLBACK(on_view_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar), A.view_combo, FALSE, FALSE, 0);
    return bar;
}

static void activate(int argc, char **argv)
{
    int start_run = 0, start_page = 0, start_coin = -1;
    int cheat_node[16], cheat_mask[16], ncheat = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run")) start_run = 1;
        else if (!strcmp(argv[i], "--cheat") && i + 1 < argc && ncheat < 16) {
            if (sscanf(argv[++i], "%d:%d", &cheat_node[ncheat], &cheat_mask[ncheat]) == 2) ncheat++;
        }
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) start_page = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--coin") && i + 1 < argc) start_coin = atoi(argv[++i]);
    }
    A.sim = sim_create();
    A.sel = 0;
    A.sel_block = -1;
    A.last_tick_blk = A.last_tick_tx = -1;
    A.last_reset_blk = A.last_reset_tx = -1;
    A.log_epoch = -1;

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "treeview { font-size: 10pt; } textview { font-size: 10pt; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    A.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(A.win), "Redwood - Cryptocurrency Consensus Testbed");
    gtk_window_set_default_size(GTK_WINDOW(A.win), 1320, 820);
    g_signal_connect(A.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(A.win), root);
    gtk_box_pack_start(GTK_BOX(root), build_toolbar(), FALSE, FALSE, 0);

    A.notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(A.notebook), build_network_page(), gtk_label_new("Network"));
    gtk_notebook_append_page(GTK_NOTEBOOK(A.notebook), build_blocks_page(), gtk_label_new("Blocks"));
    gtk_notebook_append_page(GTK_NOTEBOOK(A.notebook), build_txs_page(), gtk_label_new("Transactions"));
    A.chart = gtk_drawing_area_new();
    g_signal_connect(A.chart, "draw", G_CALLBACK(chart_draw), NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(A.notebook), A.chart, gtk_label_new("Balances"));
    A.log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(A.log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(A.log_view), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(A.log_view), 8);
    A.log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(A.log_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(A.log_buf, &end);
    A.log_end = gtk_text_buffer_create_mark(A.log_buf, "end", &end, FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(A.notebook), scrolled(A.log_view), gtk_label_new("Events"));
    g_signal_connect(A.notebook, "switch-page", G_CALLBACK(on_page_switched), NULL);
    gtk_box_pack_start(GTK_BOX(root), A.notebook, TRUE, TRUE, 0);

    A.lbl_status = gtk_label_new("");
    gtk_widget_set_halign(A.lbl_status, GTK_ALIGN_START);
    gtk_widget_set_margin_start(A.lbl_status, 10);
    gtk_widget_set_margin_top(A.lbl_status, 4);
    gtk_widget_set_margin_bottom(A.lbl_status, 6);
    gtk_box_pack_start(GTK_BOX(root), A.lbl_status, FALSE, FALSE, 0);

    if (start_coin >= 0 && start_coin < COIN_COUNT) {
        A.sim->cfg.coin = (CoinType)start_coin;
        A.sim->cfg.interval = start_coin == COIN_POW ? 12 : 6;
    }
    for (int i = 0; i < ncheat; i++)
        if (cheat_node[i] >= 0 && cheat_node[i] < A.sim->nnodes) A.sim->nodes[cheat_node[i]].cheats = (unsigned)cheat_mask[i];
    sim_reset(A.sim);
    gtk_widget_show_all(A.win);
    rebuild_node_widgets();
    sync_settings_widgets();
    load_editor();
    gtk_notebook_set_current_page(GTK_NOTEBOOK(A.notebook), start_page);
    if (start_run) {
        A.running = 1;
        gtk_button_set_label(GTK_BUTTON(A.btn_run), "Pause");
    }
    ui_refresh(1);
    g_timeout_add(15, sim_timer, NULL);
    g_timeout_add(200, ui_timer, NULL);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    activate(argc, argv);
    gtk_main();
    sim_free(A.sim);
    return 0;
}
