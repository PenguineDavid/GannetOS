/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef BSP_H
#define BSP_H

/* Binary space partitioning tree for a bspwm-style tiling layout.
   This file is deliberately hardware/OS-agnostic: no fb.h, no terminal.h,
   no GannetOS headers at all. It only knows about a tree of splits and
   rectangles. The kernel-side compositor is a separate, thin layer that
   calls bsp_layout() and draws whatever rectangle it gets back for each
   window - that split keeps this file testable on the host with plain
   gcc, no cross toolchain or QEMU required. */

/* No heap in GannetOS (yet), so nodes come from a fixed static pool, same
   convention as task.c's task table. Each inserted window costs one split
   node + one leaf node beyond the initial root leaf, so this supports up
   to (BSP_MAX_NODES - 1) / 2 windows. */
#define BSP_MAX_NODES 64

/* BSP_SPLIT_VERTICAL:   container divided by a VERTICAL line into
                         LEFT (child 0) and RIGHT (child 1) - side by side.
   BSP_SPLIT_HORIZONTAL: container divided by a HORIZONTAL line into
                         TOP (child 0) and BOTTOM (child 1) - stacked.
   This matches bspwm's own naming convention. */
typedef enum
{
    BSP_SPLIT_VERTICAL = 0,
    BSP_SPLIT_HORIZONTAL = 1
} bsp_split_dir_t;

typedef struct
{
    int x, y, w, h;
} bsp_rect_t;

typedef struct
{
    int in_use;              /* 0 = free pool slot */
    int is_leaf;              /* 1 = window slot, 0 = split (internal node) */
    int parent;               /* index into the pool, -1 for the root */

    /* -- split node fields (is_leaf == 0) -- */
    bsp_split_dir_t split_dir;
    float ratio;               /* fraction of space given to child[0], (0,1) */
    int child[2];

    /* -- leaf node fields (is_leaf == 1) -- */
    int window_id;             /* caller-defined id */
} bsp_node_t;

typedef struct
{
    bsp_node_t nodes[BSP_MAX_NODES];
    int root;                  /* -1 when the tree holds no windows */
} bsp_tree_t;

#define BSP_MIN_RATIO 0.1f
#define BSP_MAX_RATIO 0.9f

void bsp_tree_init(bsp_tree_t *t);

/* Adds a window to the tree.
   - If the tree is currently empty, pass target_leaf = -1: the new window
     becomes the sole root leaf and dir/new_on_second_child are ignored.
   - Otherwise target_leaf must be an existing leaf; it is split in two
     along `dir`. The leaf's own current window keeps one child and
     new_window_id takes the other - new_on_second_child picks which.
   Returns the new leaf's node index, or -1 on failure (pool full, or
   target_leaf invalid).

   CALLER WARNING: target_leaf's own index is reused as the new split
   node's index (so anything already pointing at it - its parent's
   child[], or the tree's root - keeps working without being patched up).
   That means a leaf index you cached earlier stops being a leaf the
   moment something else splits it further. Don't hold onto a leaf index
   across other insert calls and assume it's still a leaf - look it back
   up with bsp_find(window_id) instead. */
int bsp_insert(bsp_tree_t *t, int target_leaf, bsp_split_dir_t dir,
               int new_on_second_child, int new_window_id);

/* Same as bsp_insert, but chooses the split direction automatically from
   target_leaf's current on-screen rectangle: VERTICAL (side by side) if
   it's wider than tall, HORIZONTAL (stacked) if taller than wide. This is
   bspwm's default automatic insertion scheme. The new window always
   becomes the second child. target_rect must be target_leaf's rectangle
   as most recently produced by bsp_layout. */
int bsp_insert_auto(bsp_tree_t *t, int target_leaf, const bsp_rect_t *target_rect,
                     int new_window_id);

/* Removes `leaf` from the tree. Its sibling's whole subtree is promoted
   to take the place of their shared parent. Removing the last remaining
   window empties the tree (root becomes -1). Returns 0 on success, -1 if
   `leaf` is not a valid leaf. */
int bsp_remove(bsp_tree_t *t, int leaf);

/* Swaps a split node's two children in place (mirrors the layout).
   No-op if `node` is a leaf or invalid. */
void bsp_flip(bsp_tree_t *t, int node);

/* Swaps a split node's direction (VERTICAL<->HORIZONTAL) and swaps its two
   children - a single-node 90-degree rotate. No-op if `node` is a leaf or
   invalid. (Recursive whole-subtree rotation, as bspwm's `-R` does, can be
   built on top by calling this on every split node in the subtree.) */
void bsp_rotate(bsp_tree_t *t, int node);

/* Adjusts a split node's ratio by `delta`, clamped to
   [BSP_MIN_RATIO, BSP_MAX_RATIO] so neither child ever collapses to
   nothing. No-op if `node` is a leaf or invalid. */
void bsp_resize(bsp_tree_t *t, int node, float delta);

/* Recursively computes the screen rectangle for every leaf given the
   rectangle available to the whole tree, calling visit() once per leaf.
   Traversal order is child[0] before child[1] at every split, so windows
   are visited left-to-right / top-to-bottom. This is the only place any
   rectangle math happens - insert/remove/flip/rotate/resize only ever
   touch tree shape and ratios, never coordinates. */
typedef void (*bsp_visit_fn)(void *user_data, int leaf, int window_id, const bsp_rect_t *rect);
void bsp_layout(bsp_tree_t *t, const bsp_rect_t *root_rect, bsp_visit_fn visit, void *user_data);

/* Linear search for the leaf holding `window_id`. -1 if not found. */
int bsp_find(bsp_tree_t *t, int window_id);

#endif