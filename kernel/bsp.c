/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/bsp.h"

static int alloc_node(bsp_tree_t *t)
{
    for (int i = 0; i < BSP_MAX_NODES; i++)
    {
        if (!t->nodes[i].in_use)
        {
            t->nodes[i].in_use = 1;
            t->nodes[i].parent = -1;
            t->nodes[i].child[0] = -1;
            t->nodes[i].child[1] = -1;
            return i;
        }
    }
    return -1;
}

static void free_node(bsp_tree_t *t, int idx)
{
    if (idx < 0 || idx >= BSP_MAX_NODES)
    {
        return;
    }
    t->nodes[idx].in_use = 0;
}

static int valid_leaf(bsp_tree_t *t, int idx)
{
    return idx >= 0 && idx < BSP_MAX_NODES &&
           t->nodes[idx].in_use && t->nodes[idx].is_leaf;
}

static int valid_split(bsp_tree_t *t, int idx)
{
    return idx >= 0 && idx < BSP_MAX_NODES &&
           t->nodes[idx].in_use && !t->nodes[idx].is_leaf;
}

void bsp_tree_init(bsp_tree_t *t)
{
    for (int i = 0; i < BSP_MAX_NODES; i++)
    {
        t->nodes[i].in_use = 0;
        t->nodes[i].is_leaf = 1;
        t->nodes[i].parent = -1;
        t->nodes[i].child[0] = -1;
        t->nodes[i].child[1] = -1;
        t->nodes[i].ratio = 0.5f;
        t->nodes[i].window_id = -1;
    }
    t->root = -1;
}

int bsp_insert(bsp_tree_t *t, int target_leaf, bsp_split_dir_t dir,
               int new_on_second_child, int new_window_id)
{
    if (target_leaf == -1)
    {
        if (t->root != -1)
        {
            return -1; // tree isn't actually empty
        }
        int idx = alloc_node(t);
        if (idx < 0)
        {
            return -1;
        }
        t->nodes[idx].is_leaf = 1;
        t->nodes[idx].parent = -1;
        t->nodes[idx].window_id = new_window_id;
        t->root = idx;
        return idx;
    }

    if (!valid_leaf(t, target_leaf))
    {
        return -1;
    }

    int old_leaf = alloc_node(t);
    if (old_leaf < 0)
    {
        return -1;
    }
    int new_leaf = alloc_node(t);
    if (new_leaf < 0)
    {
        free_node(t, old_leaf);
        return -1;
    }

    int old_window_id = t->nodes[target_leaf].window_id;
    int parent_of_target = t->nodes[target_leaf].parent;

    t->nodes[old_leaf].is_leaf = 1;
    t->nodes[old_leaf].parent = target_leaf;
    t->nodes[old_leaf].window_id = old_window_id;

    t->nodes[new_leaf].is_leaf = 1;
    t->nodes[new_leaf].parent = target_leaf;
    t->nodes[new_leaf].window_id = new_window_id;

    /* Reuse target_leaf's own index for the new split node, so whatever
       pointed at it (its parent's child[], or t->root) still does - no
       need to walk up and fix anything else. */
    t->nodes[target_leaf].is_leaf = 0;
    t->nodes[target_leaf].split_dir = dir;
    t->nodes[target_leaf].ratio = 0.5f;
    t->nodes[target_leaf].parent = parent_of_target;
    if (new_on_second_child)
    {
        t->nodes[target_leaf].child[0] = old_leaf;
        t->nodes[target_leaf].child[1] = new_leaf;
    }
    else
    {
        t->nodes[target_leaf].child[0] = new_leaf;
        t->nodes[target_leaf].child[1] = old_leaf;
    }

    return new_leaf;
}

int bsp_insert_auto(bsp_tree_t *t, int target_leaf, const bsp_rect_t *target_rect,
                     int new_window_id)
{
    bsp_split_dir_t dir = (target_rect->w >= target_rect->h)
                              ? BSP_SPLIT_VERTICAL
                              : BSP_SPLIT_HORIZONTAL;
    return bsp_insert(t, target_leaf, dir, 1, new_window_id);
}

int bsp_remove(bsp_tree_t *t, int leaf)
{
    if (!valid_leaf(t, leaf))
    {
        return -1;
    }

    int parent = t->nodes[leaf].parent;
    if (parent == -1)
    {
        /* leaf was the only window in the tree */
        free_node(t, leaf);
        t->root = -1;
        return 0;
    }

    int sibling = (t->nodes[parent].child[0] == leaf)
                      ? t->nodes[parent].child[1]
                      : t->nodes[parent].child[0];
    int grandparent = t->nodes[parent].parent;

    t->nodes[sibling].parent = grandparent;
    if (grandparent == -1)
    {
        t->root = sibling;
    }
    else
    {
        if (t->nodes[grandparent].child[0] == parent)
        {
            t->nodes[grandparent].child[0] = sibling;
        }
        else
        {
            t->nodes[grandparent].child[1] = sibling;
        }
    }

    free_node(t, leaf);
    free_node(t, parent);
    return 0;
}

void bsp_flip(bsp_tree_t *t, int node)
{
    if (!valid_split(t, node))
    {
        return;
    }
    int tmp = t->nodes[node].child[0];
    t->nodes[node].child[0] = t->nodes[node].child[1];
    t->nodes[node].child[1] = tmp;
}

void bsp_rotate(bsp_tree_t *t, int node)
{
    if (!valid_split(t, node))
    {
        return;
    }
    t->nodes[node].split_dir = (t->nodes[node].split_dir == BSP_SPLIT_VERTICAL)
                                    ? BSP_SPLIT_HORIZONTAL
                                    : BSP_SPLIT_VERTICAL;
    bsp_flip(t, node);
}

void bsp_resize(bsp_tree_t *t, int node, float delta)
{
    if (!valid_split(t, node))
    {
        return;
    }
    float ratio = t->nodes[node].ratio + delta;
    if (ratio < BSP_MIN_RATIO)
    {
        ratio = BSP_MIN_RATIO;
    }
    if (ratio > BSP_MAX_RATIO)
    {
        ratio = BSP_MAX_RATIO;
    }
    t->nodes[node].ratio = ratio;
}

static void layout_recurse(bsp_tree_t *t, int node, const bsp_rect_t *rect,
                            bsp_visit_fn visit, void *user_data)
{
    if (node < 0 || !t->nodes[node].in_use)
    {
        return;
    }

    if (t->nodes[node].is_leaf)
    {
        visit(user_data, node, t->nodes[node].window_id, rect);
        return;
    }

    bsp_rect_t rect0 = *rect, rect1 = *rect;
    if (t->nodes[node].split_dir == BSP_SPLIT_VERTICAL)
    {
        int width0 = (int)(rect->w * t->nodes[node].ratio + 0.5f);
        rect0.w = width0;
        rect1.x = rect->x + width0;
        rect1.w = rect->w - width0;
    }
    else
    {
        int height0 = (int)(rect->h * t->nodes[node].ratio + 0.5f);
        rect0.h = height0;
        rect1.y = rect->y + height0;
        rect1.h = rect->h - height0;
    }

    layout_recurse(t, t->nodes[node].child[0], &rect0, visit, user_data);
    layout_recurse(t, t->nodes[node].child[1], &rect1, visit, user_data);
}

void bsp_layout(bsp_tree_t *t, const bsp_rect_t *root_rect, bsp_visit_fn visit, void *user_data)
{
    if (t->root == -1)
    {
        return;
    }
    layout_recurse(t, t->root, root_rect, visit, user_data);
}

int bsp_find(bsp_tree_t *t, int window_id)
{
    for (int i = 0; i < BSP_MAX_NODES; i++)
    {
        if (t->nodes[i].in_use && t->nodes[i].is_leaf && t->nodes[i].window_id == window_id)
        {
            return i;
        }
    }
    return -1;
}