/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Samuele Voltan.  See LICENSE. */
/*
 * keypad.c - the Jaguar keypad as the game's own verb keys (the port's, not ScummVM's).
 *
 * The keypad is three columns of four (1 2 3 / 4 5 6 / 7 8 9 / * 0 #).  A key press becomes
 * the key the game itself gave a verb (verbOps SO_VERB_KEY, matched by checkExecVerbs), so the
 * press runs the original keyboard shortcut and nothing else.  Which verb a key means comes
 * from the verbs the game has up, not from a table:
 *  - the grid: the active verbs whose key is a letter, the three leftmost columns of their
 *    left edges and, among the verbs in those columns, the four topmost rows of their tops
 *    (MI1 DOS: Open Close Push Pull / Walk to Pick up Talk to Give / Use Look at Turn on Turn
 *    off, three columns of four, the keypad's own shape; a v5 nine-verb panel takes the top
 *    three rows).  What lies right of the panel stays out of it: MI1's inventory arrows
 *    ('q' 'a', x 149) and its items, whose keys are digits ('1'..'6', script 22).  The letters
 *    differ per language (Italian is not 'o' 'c' ...), the positions do not;
 *  - digits: no letter-keyed verb up, but one whose key is a digit (MI1's dialogue choices
 *    '1', '2', ...: the twelve verbs are saved then): the keypad sends its own character,
 *    '*' and '#' nothing;
 *  - no active verb with a key at all (the Dial-A-Pirate wheel, before any verb exists: its
 *    input script 155 reads the digits '0'..'9' and Backspace; "Press ENTER" before it takes
 *    any input): the keypad is a keyboard's digits, '*' Backspace (8), '#' Return (13).
 * "Active" is what checkExecVerbs accepts: verbid, curmode 1, saveid 0.
 * (Session 10: with the digit rule first, the first item picked up turned the keypad into the
 * inventory's digits, seen by the user in the first play-through.)
 */
#include "scumm.h"

#define GRID_COLS 3
#define GRID_ROWS 4

static int active_key(const VerbSlot *vs)
{
    return vs->verbid && vs->curmode == 1 && vs->saveid == 0 && vs->key;
}

/* insert v into the sorted set s[0..*n), at most max entries; 0 if it does not fit */
static int add_sorted(int16_t *s, int *n, int max, int16_t v)
{
    int i, j;
    for (i = 0; i < *n && s[i] < v; i++)
        ;
    if (i < *n && s[i] == v)
        return 1;
    if (*n == max)
        return 0;
    for (j = *n; j > i; j--)
        s[j] = s[j - 1];
    s[i] = v;
    (*n)++;
    return 1;
}

static int index_of(const int16_t *s, int n, int16_t v)
{
    int i;
    for (i = 0; i < n; i++)
        if (s[i] == v)
            return i;
    return -1;
}

static int is_digit(int key)
{
    return key >= '0' && key <= '9';
}

/* key: 0-9, 10 '*', 11 '#' (KEY_B_* on the Jaguar).  Returns the ASCII to press, 0 = none. */
int scumm_keypadKey(int key)
{
    static const uint8_t col_of[12] = { 1, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 2 };
    static const uint8_t row_of[12] = { 3, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3 };
    int16_t xs[NUM_VERBS], ys[NUM_VERBS];
    int nx = 0, ny = 0, i, digits = 0;
    if (key < 0 || key > 11)
        return 0;
    for (i = 1; i < NUM_VERBS; i++) {
        const VerbSlot *vs = &g.verbs[i];
        if (!active_key(vs))
            continue;
        if (is_digit(vs->key))
            digits = 1;
        else
            add_sorted(xs, &nx, NUM_VERBS, vs->x);
    }
    if (nx > GRID_COLS)
        nx = GRID_COLS;                      /* the leftmost three columns */
    for (i = 1; i < NUM_VERBS; i++) {
        const VerbSlot *vs = &g.verbs[i];
        if (active_key(vs) && !is_digit(vs->key) && index_of(xs, nx, vs->x) >= 0)
            add_sorted(ys, &ny, NUM_VERBS, vs->y);
    }
    if (ny > GRID_ROWS)
        ny = GRID_ROWS;                      /* the topmost four rows */
    if (nx > 0) {
        for (i = 1; i < NUM_VERBS; i++) {
            const VerbSlot *vs = &g.verbs[i];
            if (active_key(vs) && !is_digit(vs->key) && index_of(xs, nx, vs->x) == col_of[key] &&
                index_of(ys, ny, vs->y) == row_of[key])
                return vs->key;
        }
        return 0;
    }
    if (digits)
        return key <= 9 ? '0' + key : 0;
    return key <= 9 ? '0' + key : key == 10 ? 8 : 13;      /* no keyed verb: a keyboard */
}
