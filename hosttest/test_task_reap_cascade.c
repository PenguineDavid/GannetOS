/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Host-testable check of task.c's task_reap() cascading into a task's
   own spawned children (and their children) before freeing anything of
   the task's own - the fix for the ordering hazard where a parent could
   be reaped while a still-running child shared its private code/data.

   This models the TRAVERSAL/RECURSION/bookkeeping logic (parent_id
   matching, cascading through grandchildren, only ever touching a
   task's OWN descendants, slot reclamation) against a plain host
   struct array, rather than the real task.c (which needs the real
   scheduler - task_yield()/schedule() - to actually exercise the
   "wait until a child becomes a zombie" half of this; that half isn't
   host-testable without a real scheduler, so every child here is
   already a zombie by the time reap_task() is called, exactly the
   state the real blocking loop eventually reaches before it proceeds).
   What's under test is: does the cascade find the right children (and
   only the right children), does it reach grandchildren, and does
   everything end up reclaimed. */
#include <stdio.h>
#include <string.h>

#define UNUSED 0
#define READY 1
#define ZOMBIE 2
#define MAX_TASKS 8

typedef struct
{
    int state;
    int id;
    int parent_id; /* -1 = no parent, same sentinel choice as task.c and for the same reason (0 is a valid task id) */
} fake_task_t;

static fake_task_t tasks[MAX_TASKS];
static int reap_call_log[MAX_TASKS]; /* order tasks actually got reclaimed in */
static int reap_call_count;

static void reset_all(void)
{
    memset(tasks, 0, sizeof(tasks));
    for (int i = 0; i < MAX_TASKS; i++)
    {
        tasks[i].parent_id = -1;
    }
    reap_call_count = 0;
}

static int find_slot_by_id(int id)
{
    for (int i = 0; i < MAX_TASKS; i++)
    {
        if (tasks[i].id == id)
        {
            return i;
        }
    }
    return -1;
}

/* --- mirrors task_reap()'s cascade: for a ZOMBIE task, recursively
   reap every child (tasks[j].parent_id == id, state != UNUSED) first,
   then reclaim this task's own slot. Every child here is assumed
   already ZOMBIE - see the file's own top comment on why the "wait"
   half isn't part of what this simulates. --- */
static void reap_task(int id)
{
    int i = find_slot_by_id(id);
    if (i < 0 || tasks[i].state != ZOMBIE)
    {
        return;
    }

    for (int j = 0; j < MAX_TASKS; j++)
    {
        if (tasks[j].parent_id == id && tasks[j].state != UNUSED)
        {
            reap_task(tasks[j].id);
        }
    }

    reap_call_log[reap_call_count++] = id;
    tasks[i].state = UNUSED;
    tasks[i].id = -1;
    tasks[i].parent_id = -1;
}

static int reaped_before(int a, int b)
{
    /* did task id `a` get reclaimed strictly before task id `b` in the log? */
    int ia = -1, ib = -1;
    for (int k = 0; k < reap_call_count; k++)
    {
        if (reap_call_log[k] == a)
        {
            ia = k;
        }
        if (reap_call_log[k] == b)
        {
            ib = k;
        }
    }
    return ia >= 0 && ib >= 0 && ia < ib;
}

static int failures = 0;
static void check(const char *name, int cond)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond)
    {
        failures++;
    }
}

int main(void)
{
    /* A parent with one zombie child: reaping the parent also reclaims
       the child - the previously-separate "nothing reaps a spawned
       child" leak this closes as a side effect. */
    reset_all();
    tasks[0] = (fake_task_t){ .state = ZOMBIE, .id = 10, .parent_id = -1 };
    tasks[1] = (fake_task_t){ .state = ZOMBIE, .id = 11, .parent_id = 10 };
    reap_task(10);
    check("parent's zombie child gets reclaimed too", tasks[1].state == UNUSED);
    check("parent itself gets reclaimed", tasks[0].state == UNUSED);
    check("the child is reclaimed BEFORE the parent (child must go first, "
          "or paging_free_private_pages(parent) could free frames the "
          "child's own reap still needs to see as live)",
          reaped_before(11, 10));

    /* Grandchildren: a child that itself spawned a child must cascade
       all the way down, not just one level. */
    reset_all();
    tasks[0] = (fake_task_t){ .state = ZOMBIE, .id = 1, .parent_id = -1 };
    tasks[1] = (fake_task_t){ .state = ZOMBIE, .id = 2, .parent_id = 1 };
    tasks[2] = (fake_task_t){ .state = ZOMBIE, .id = 3, .parent_id = 2 };
    reap_task(1);
    check("grandchild gets reclaimed via the cascade", tasks[2].state == UNUSED);
    check("child gets reclaimed via the cascade", tasks[1].state == UNUSED);
    check("root parent gets reclaimed", tasks[0].state == UNUSED);
    check("grandchild reclaimed before child", reaped_before(3, 2));
    check("child reclaimed before root parent", reaped_before(2, 1));

    /* Sibling isolation: reaping one parent must never touch an
       unrelated parent's own children. */
    reset_all();
    tasks[0] = (fake_task_t){ .state = ZOMBIE, .id = 100, .parent_id = -1 };
    tasks[1] = (fake_task_t){ .state = ZOMBIE, .id = 101, .parent_id = 100 };
    tasks[2] = (fake_task_t){ .state = ZOMBIE, .id = 200, .parent_id = -1 };
    tasks[3] = (fake_task_t){ .state = ZOMBIE, .id = 201, .parent_id = 200 };
    reap_task(100);
    check("reaping one parent reclaims only its own child", tasks[1].state == UNUSED);
    check("an unrelated parent is untouched", tasks[2].state == ZOMBIE);
    check("an unrelated parent's own child is untouched", tasks[3].state == ZOMBIE);

    /* A task with no children (the common case - every plain app,
       every task_create/task_create_ring3 task) reaps exactly itself,
       nothing more. */
    reset_all();
    tasks[0] = (fake_task_t){ .state = ZOMBIE, .id = 42, .parent_id = -1 };
    reap_task(42);
    check("a childless task reaps just itself", tasks[0].state == UNUSED);
    check("exactly one reclamation happened", reap_call_count == 1);

    /* Multiple children of the same parent all get reclaimed, not just
       the first one found. */
    reset_all();
    tasks[0] = (fake_task_t){ .state = ZOMBIE, .id = 5, .parent_id = -1 };
    tasks[1] = (fake_task_t){ .state = ZOMBIE, .id = 6, .parent_id = 5 };
    tasks[2] = (fake_task_t){ .state = ZOMBIE, .id = 7, .parent_id = 5 };
    reap_task(5);
    check("first sibling reclaimed", tasks[1].state == UNUSED);
    check("second sibling reclaimed", tasks[2].state == UNUSED);
    check("parent reclaimed after both children", reaped_before(6, 5) && reaped_before(7, 5));

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}