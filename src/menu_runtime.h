/* SPDX-License-Identifier: GPL-3.0-only */

#pragma once

#include <stddef.h>

#include "hud_tree.h"

typedef struct
{
    char text[16];
    uint64_t size, capacity;
} GameString;

static const uint32_t menu_visible_flag = UINT32_C(1) << 30;

enum
{
    MENU_IMAGE_VA = 0x055a8620u,
    MENU_INDICATOR_VA = 0x05583540u
};

typedef void (*MenuAppend)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static inline void hud_fill(void *d, unsigned char v, unsigned n)
{
    unsigned char *a = d;
    for (unsigned i = 0; i < n; i++)
        a[i] = v;
}

static inline void hud_copy(void *d, const void *s, unsigned n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    for (unsigned i = 0; i < n; i++)
        a[i] = b[i];
}

static inline int hud_same(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        if (*a != *b)
            return 0;
        if (!*a)
            return 1;
    }
}

static inline int hud_arena_span(uintptr_t address, uintptr_t size)
{
    return address >= LIVE_ARENA_LOW && address < LIVE_ARENA_HIGH && size <= LIVE_ARENA_HIGH - address;
}

static inline int hud_object(uintptr_t address, uintptr_t size)
{
    return !(address & (sizeof(uintptr_t) - 1)) && hud_arena_span(address, size);
}

static inline int hud_vector(uintptr_t begin, uintptr_t end, unsigned limit)
{
    return end > begin && !(end & (sizeof(uintptr_t) - 1)) && end - begin <= sizeof(uintptr_t) * limit &&
           hud_object(begin, end - begin);
}

static inline int hud_update_context(uintptr_t base, uintptr_t context)
{
    return hud_ptr(base) && hud_ptr(context) && !(context & (sizeof(uintptr_t) - 1)) &&
           *(const uintptr_t *)context == base + HUD_UPDATE_VT_RVA;
}

static inline int hud_name(uintptr_t node, char *out)
{
    uintptr_t p = *(uintptr_t *)(node + HUD_OFF_NAME);
    if (!hud_object(p, HUD_OFF_NAME_CAP + sizeof(uint64_t)))
        return 0;
    const unsigned char *raw = (const unsigned char *)p;
    uint64_t len = *(const uint64_t *)(p + HUD_OFF_NAME_LEN);
    uint64_t cap = *(const uint64_t *)(p + HUD_OFF_NAME_CAP);
    if (!len || len >= LIVE_HUD_NAME || cap < len)
        return 0;
    if (len > HUD_NAME_INLINE)
    {
        uintptr_t text = *(const uintptr_t *)p;
        if (!hud_arena_span(text, len))
            return 0;
        raw = (const unsigned char *)text;
    }
    unsigned i = 0;
    for (; i < len && raw[i]; i++)
        out[i] = (char)raw[i];
    out[i] = 0;
    if (!hud_name_ok(out, i))
        return 0;
    return len > HUD_NAME_INLINE ? 2 : 1;
}

static inline int menu_pointer(uintptr_t pointer)
{
    return pointer >= LIVE_ARENA_LOW && pointer < LIVE_ARENA_HIGH - 0x620u && !(pointer & 7u);
}

static inline void menu_string(GameString *text, const char *value, unsigned length)
{
    hud_fill(text, 0, sizeof(*text));
    text->size = length;
    text->capacity = 15;
    if (length <= 15)
        hud_copy(text->text, value, length + 1);
    else
    {
        /* These callers pass a borrowed, NUL-terminated buffer. GT7's
         * setters copy the string to their own storage. */
        hud_copy(text->text, &value, sizeof(value));
        text->capacity = length;
    }
}

static inline int menu_set_text(uintptr_t row, const volatile char *label)
{
    uintptr_t labels[3];
    uintptr_t child = *(const uintptr_t *)(row + 0x2c8);
    for (unsigned i = 0; i < 4; i++)
    {
        if (!menu_pointer(child) || *(const uintptr_t *)(child + 0x30) != row)
            return 0;
        if (i)
        {
            char name[LIVE_HUD_NAME];
            if (*(const uintptr_t *)child != 0x055910c0u || !hud_name(child, name))
                return 0;
            if ((i == 1 && !hud_same(name, "label")) || (i == 2 && !hud_same(name, "label_i")) ||
                (i == 3 && !hud_same(name, "label_f")))
                return 0;
            labels[i - 1] = child;
        }
        child = *(const uintptr_t *)(child + 0xa8);
    }

    char rendered[LIVE_MENU_LABEL];
    unsigned length = 0;
    while (length < sizeof(rendered) - 1 && label[length])
    {
        rendered[length] = label[length];
        length++;
    }
    rendered[length] = 0;
    GameString text;
    menu_string(&text, rendered, length);
    typedef void (*SetText)(uintptr_t, const GameString *);
    for (unsigned i = 0; i < 3; i++)
        ((SetText)(uintptr_t)0x0072cf50u)(labels[i], &text);
    return 1;
}

static inline void menu_show_widget(uintptr_t widget, int visible)
{
    uint32_t *flags = (uint32_t *)(widget + 0x214);
    *flags = (*flags & ~menu_visible_flag) | (visible ? menu_visible_flag : 0);
}

static inline int menu_copy_row(uintptr_t *copy, uintptr_t source)
{
    typedef void (*CopyWidget)(uintptr_t *, uintptr_t, unsigned, const uintptr_t *);
    ((CopyWidget)(uintptr_t)LIVE_MENU_COPY_VA)(copy, source, 0, NULL);
    return *copy != source && menu_pointer(*copy) && *(const uintptr_t *)*copy == LIVE_MENU_ITEM_VA &&
           *(const uintptr_t *)(*copy + 0x68) == source && *(const uint32_t *)(*copy + 0x2ec) == 7;
}
