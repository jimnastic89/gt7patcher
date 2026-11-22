/* SPDX-License-Identifier: GPL-3.0-only
 * Installed once into GT7. All state and native UI callbacks remain in the
 * game process until it exits, independently of GT7Patcher and Live Settings.
 */
#include "wheel_patch.h"
#include "menu_runtime.h"
static volatile WheelRuntime wheel_runtime = {.magic = WHEEL_RUNTIME_MAGIC};
static int wheel_fit_column(uintptr_t node, float delta, unsigned depth)
{
    if (depth > 16 || !menu_pointer(node))
        return 0;
    /* Authored allocation constraints are separate from the arranged rect.
     * SetRect alone is overwritten by packing against these original widths. */
    uintptr_t layout = *(const uintptr_t *)(node + 0x230);
    if (layout && !menu_pointer(layout))
        return 0;
    float width = layout ? *(const float *)layout : 0.0f;
    if (width >= 280.0f && width <= 500.0f)
    {
        width -= delta;
        *(float *)layout = width;
        __atomic_or_fetch((uint32_t *)(node + 0x210), 7u, __ATOMIC_RELAXED);
    }
    uintptr_t type = *(const uintptr_t *)node;
    if (type != LIVE_MENU_ITEM_VA && type != LIVE_MENU_OPTION_VA && type != MENU_INDICATOR_VA &&
        type != 0x055b1b80u && type != 0x055a51a8u)
        return 1;
    uintptr_t child = *(const uintptr_t *)(node + 0x2c8);
    for (unsigned i = 0; child && i < 128; i++)
    {
        if (!menu_pointer(child) || *(const uintptr_t *)(child + 0x30) != node ||
            !wheel_fit_column(child, delta, depth + 1))
            return 0;
        child = *(const uintptr_t *)(child + 0xa8);
    }
    return !child;
}

/* Script member maps use interned GameString keys and native tree nodes.
 * Read them in place; never clone the event's controller reference graph. */
static int wheel_key_same(uintptr_t key, const char *expected)
{
    if (!menu_pointer(key))
        return 0;
    const GameString *text = (const GameString *)key;
    unsigned length = 0;
    while (expected[length])
        length++;
    if (text->size != length || text->capacity < length)
        return 0;
    const char *actual = text->text;
    if (text->capacity > 15)
    {
        uintptr_t buffer = *(const uintptr_t *)key;
        if (!menu_pointer(buffer))
            return 0;
        actual = (const char *)buffer;
    }
    for (unsigned i = 0; i < length; i++)
        if (actual[i] != expected[i])
            return 0;
    return 1;
}

static uintptr_t wheel_map_value(uintptr_t sentinel, const char *name)
{
    if (!menu_pointer(sentinel))
        return 0;
    uintptr_t node = *(const uintptr_t *)sentinel;
    for (unsigned i = 0; node != sentinel && i < 256; i++)
    {
        if (!menu_pointer(node))
            return 0;
        if (wheel_key_same(*(const uintptr_t *)(node + 0x20), name))
            return *(const uintptr_t *)(node + 0x28);
        uintptr_t right = *(const uintptr_t *)(node + 0x10);
        if (!menu_pointer(right))
            return 0;
        if (right != sentinel)
        {
            node = right;
            for (unsigned depth = 0; depth < 64; depth++)
            {
                uintptr_t left = *(const uintptr_t *)node;
                if (left == sentinel)
                    break;
                if (!menu_pointer(left) || depth == 63)
                    return 0;
                node = left;
            }
        }
        else
        {
            uintptr_t parent = *(const uintptr_t *)(node + 8);
            for (unsigned depth = 0; parent != sentinel; depth++)
            {
                if (!menu_pointer(parent) || depth == 64)
                    return 0;
                if (node != *(const uintptr_t *)(parent + 0x10))
                    break;
                node = parent;
                parent = *(const uintptr_t *)(parent + 8);
            }
            node = parent;
        }
    }
    return 0;
}

static uintptr_t wheel_event_method(uintptr_t event, const char *name)
{
    uintptr_t type = *(const uintptr_t *)(event + 0x10);
    for (unsigned i = 0; type && i < 8; i++)
    {
        if (!menu_pointer(type) || *(const uintptr_t *)type != 0x059118e0u)
            return 0;
        uintptr_t method = wheel_map_value(*(const uintptr_t *)(type + 0x38), name);
        if (method)
            return menu_pointer(method) && *(const uintptr_t *)method == 0x0591aa50u ? method : 0;
        uintptr_t sentinel = *(const uintptr_t *)(type + 0x48);
        if (!menu_pointer(sentinel))
            return 0;
        uintptr_t node = *(const uintptr_t *)sentinel;
        for (unsigned j = 0; node != sentinel && j < 32; j++)
        {
            if (!menu_pointer(node))
                return 0;
            uintptr_t protocol = *(const uintptr_t *)(node + 0x10);
            if (!menu_pointer(protocol) || *(const uintptr_t *)protocol != 0x059118e0u)
                return 0;
            method = wheel_map_value(*(const uintptr_t *)(protocol + 0x38), name);
            if (method)
                return menu_pointer(method) && *(const uintptr_t *)method == 0x0591aa50u ? method : 0;
            node = *(const uintptr_t *)node;
        }
        if (node != sentinel)
            return 0;
        type = *(const uintptr_t *)(type + 0x60);
    }
    return 0;
}

static int wheel_invoke(uintptr_t event, const char *name, unsigned argc, const uintptr_t *args,
                        uintptr_t *out)
{
    uintptr_t method = wheel_event_method(event, name);
    if (!method || *(const uintptr_t *)(0x0591aa50u + 0xd8) != 0x0041d6e0u)
        return 0;
    typedef void (*Invoke)(uintptr_t, uintptr_t *, const uintptr_t *, unsigned, const uintptr_t *);
    ((Invoke)(uintptr_t)0x0041d6e0u)(method, out, &event, argc, args);
    return 1;
}

static uintptr_t wheel_attribute(uintptr_t object, const char *name)
{
    if (!menu_pointer(object))
        return 0;
    unsigned length = 0;
    while (name[length])
        length++;
    GameString text;
    menu_string(&text, name, length);
    uintptr_t symbol[4] = {0}, value = 0;
    ((void (*)(uintptr_t *, const GameString *))(uintptr_t)0x00479da0u)(symbol, &text);
    uintptr_t table = *(const uintptr_t *)object;
    if (table >= 0x05000000u && table < 0x06000000u)
    {
        typedef void (*GetAttribute)(uintptr_t, uintptr_t *, const uintptr_t *);
        ((GetAttribute) * (const uintptr_t *)(table + 0x40))(object, &value, symbol);
    }
    ((void (*)(uintptr_t *))(uintptr_t)0x0040ca30u)(symbol);
    return value;
}

static void wheel_release(uintptr_t *value)
{
    if (*value)
        ((void (*)(uintptr_t *))(uintptr_t)LIVE_MENU_RELEASE_VA)(value);
}

static uintptr_t wheel_event_attribute(uintptr_t event, const char *name)
{
    if (!menu_pointer(event) || *(const uintptr_t *)event != 0x05917f08u)
        return 0;
    uintptr_t attributes = *(const uintptr_t *)(event + 0x18);
    return menu_pointer(attributes) ? wheel_map_value(*(const uintptr_t *)attributes, name) : 0;
}

static uintptr_t wheel_event_rows(uintptr_t event)
{
    uintptr_t list =
        wheel_event_attribute(event, "__toplevel__::main::gt7::CommonEvents::OptionMenuEventProtocol::list");
    if (!menu_pointer(list) || *(const uintptr_t *)list != 0x0590ec08u)
        return 0;
    uintptr_t implementation = *(const uintptr_t *)(list + 0x20);
    if (!menu_pointer(implementation) || *(const uintptr_t *)implementation != 0x0592a270u)
        return 0;
    uintptr_t vector = *(const uintptr_t *)(implementation + 0x30);
    if (!menu_pointer(vector))
        return 0;
    uintptr_t begin = *(const uintptr_t *)vector;
    uintptr_t end = *(const uintptr_t *)(vector + 8);
    uintptr_t capacity = *(const uintptr_t *)(vector + 16);
    return menu_pointer(begin) && end == begin + 12u * 40u && capacity >= end && capacity <= begin + 4096u &&
                   capacity < LIVE_ARENA_HIGH
               ? begin
               : 0;
}

static int wheel_compose_rows(uintptr_t front, uintptr_t rear, unsigned front_index, unsigned rear_index)
{
    if (!front || !rear || front == rear || front_index >= 12 || rear_index >= 12)
        return 0;
    int16_t front_value = *(const int16_t *)(front + front_index * 40u + 32);
    int16_t rear_value = *(const int16_t *)(rear + rear_index * 40u + 34);
    for (unsigned i = 0; i < 12; i++)
    {
        *(int16_t *)(front + i * 40u + 34) = rear_value;
        *(int16_t *)(rear + i * 40u + 32) = front_value;
    }
    return 1;
}

static unsigned wheel_saved_rear_index(uintptr_t car, uintptr_t rows, unsigned fallback)
{
    if (!menu_pointer(car) || *(const uintptr_t *)car != 0x0590ec08u)
        return fallback;
    uintptr_t wrapper = *(const uintptr_t *)(car + 0x20);
    if (!menu_pointer(wrapper) || *(const uintptr_t *)wrapper != 0x05923868u)
        return fallback;
    uintptr_t parameter = *(const uintptr_t *)(wrapper + 0x30);
    uintptr_t implementation = menu_pointer(parameter) ? *(const uintptr_t *)parameter : 0;
    uintptr_t style = menu_pointer(implementation) ? *(const uintptr_t *)(implementation + 0x18) : 0;
    if (!menu_pointer(style))
        return fallback;
    int16_t selected = *(const int16_t *)(style + 0x22);
    for (unsigned i = 0; i < 12; i++)
        if (*(const int16_t *)(rows + i * 40u + 34) == selected)
            return i;
    return fallback;
}

static uintptr_t wheel_find_option(uintptr_t container)
{
    if (!menu_pointer(container) || *(const uintptr_t *)container != LIVE_MENU_ITEM_VA)
        return 0;
    uintptr_t child = *(const uintptr_t *)(container + 0x2c8);
    for (unsigned i = 0; child && i < 16; i++)
    {
        if (!menu_pointer(child) || *(const uintptr_t *)(child + 0x30) != container)
            return 0;
        if (*(const uintptr_t *)child == LIVE_MENU_OPTION_VA)
            return child;
        child = *(const uintptr_t *)(child + 0xa8);
    }
    return 0;
}

static uintptr_t wheel_front_event(uintptr_t option)
{
    uintptr_t callbacks = *(const uintptr_t *)(option + 0x3e0);
    if (!menu_pointer(callbacks) || *(const uintptr_t *)callbacks != 0x05911d08u)
        return 0;
    uintptr_t begin = *(const uintptr_t *)(callbacks + 0x28);
    if (!menu_pointer(begin) || *(const uintptr_t *)(callbacks + 0x30) != begin + 8)
        return 0;
    uintptr_t bound = *(const uintptr_t *)begin;
    if (!menu_pointer(bound) || *(const uintptr_t *)bound != 0x05916878u)
        return 0;
    uintptr_t event = *(const uintptr_t *)(bound + 0x20);
    return menu_pointer(event) && *(const uintptr_t *)event == 0x05917f08u ? event : 0;
}

static void wheel_axis_text(char text[32], unsigned index, int value)
{
    if (!index)
    {
        hud_copy(text, "Standard", 9);
        return;
    }
    unsigned length = 0;
    if (value < 0)
    {
        text[length++] = '-';
        value = -value;
    }
    char digits[6];
    unsigned count = 0;
    do
    {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count)
        text[length++] = digits[--count];
    text[length] = 0;
}

static int wheel_option_text(uintptr_t option, uintptr_t rows, unsigned index, int rear)
{
    if (index >= 12)
        return 0;
    char label[32];
    wheel_axis_text(label, index, *(const int16_t *)(rows + index * 40u + (rear ? 34u : 32u)));
    unsigned length = 0;
    while (label[length])
        length++;
    GameString text;
    menu_string(&text, label, length);
    uintptr_t child = *(const uintptr_t *)(option + 0x2c8);
    unsigned updated = 0;
    for (unsigned i = 0; child && i < 16; i++)
    {
        if (!menu_pointer(child) || *(const uintptr_t *)(child + 0x30) != option)
            return 0;
        char name[LIVE_HUD_NAME];
        if (*(const uintptr_t *)child == 0x055910c0u && hud_name(child, name) &&
            (hud_same(name, "label") || hud_same(name, "label_i") || hud_same(name, "label_f")))
        {
            ((void (*)(uintptr_t, const GameString *))(uintptr_t)0x0072cf50u)(child, &text);
            updated++;
        }
        child = *(const uintptr_t *)(child + 0xa8);
    }
    return updated != 0;
}

static int wheel_axis_labels(uintptr_t option, uintptr_t rows, int rear)
{
    uintptr_t box = *(const uintptr_t *)(option + 0x3c8);
    if (!menu_pointer(box) || *(const uint32_t *)(box + 0x2ec) != 12)
        return 0;
    uintptr_t row = *(const uintptr_t *)(box + 0x2c8);
    for (unsigned i = 0; i < 12; i++)
    {
        if (!menu_pointer(row) || *(const uintptr_t *)row != LIVE_MENU_ITEM_VA ||
            *(const uintptr_t *)(row + 0x30) != box)
            return 0;
        char text[32];
        wheel_axis_text(text, i, *(const int16_t *)(rows + i * 40u + (rear ? 34u : 32u)));
        if (!menu_set_text(row, text))
            return 0;
        row = *(const uintptr_t *)(row + 0xa8);
    }
    return !row;
}

static int wheel_populate_rear(uintptr_t front_option, uintptr_t rear_option, uintptr_t rows,
                               uintptr_t context)
{
    uintptr_t front_box = *(const uintptr_t *)(front_option + 0x3c8);
    uintptr_t rear_box = *(const uintptr_t *)(rear_option + 0x3c8);
    if (!menu_pointer(front_box) || !menu_pointer(rear_box) || *(const uint32_t *)(front_box + 0x2ec) != 12 ||
        *(const uint32_t *)(rear_box + 0x2ec))
    {
        wheel_runtime.wheel_clone_reserved = 101;
        return 0;
    }
    uintptr_t first = *(const uintptr_t *)(front_box + 0x2c8);
    uintptr_t source = menu_pointer(first) ? *(const uintptr_t *)(first + 0x68) : 0;
    if (!menu_pointer(source) || *(const uintptr_t *)source != LIVE_MENU_ITEM_VA ||
        *(const uint32_t *)(source + 0x2ec) != 7)
    {
        wheel_runtime.wheel_clone_reserved = 102;
        return 0;
    }
    uintptr_t copies[12] = {0};
    unsigned prepared = 0;
    int attached = 0;
    float target = *(const float *)(front_option + 0xd8);
    for (; prepared < 12; prepared++)
    {
        const GameString *label = (const GameString *)(rows + prepared * 40u);
        const char *text = label->capacity > 15 ? *(const char *const *)label : label->text;
        if (!label->size || label->size >= LIVE_MENU_LABEL || !menu_copy_row(&copies[prepared], source))
        {
            wheel_runtime.wheel_clone_reserved = 110 + prepared;
            break;
        }
        if (!menu_set_text(copies[prepared], text) ||
            (target < 400.0f && target >= 200.0f && !wheel_fit_column(copies[prepared], 400.0f - target, 0)))
        {
            wheel_runtime.wheel_clone_reserved = 120 + prepared;
            break;
        }
        menu_show_widget(copies[prepared], 1);
    }
    if (prepared == 12)
    {
        for (unsigned i = 0; i < 12; i++)
            ((MenuAppend)(uintptr_t)0x00946b20u)(rear_box + 0x2b8, context, copies[i], 0);
        ((void (*)(uintptr_t, uintptr_t))(uintptr_t)0x01d66b20u)(rear_option, context);
        attached =
            *(const uint32_t *)(rear_box + 0x2ec) == 12 && *(const uint32_t *)(rear_option + 0x390) == 12;
        if (!attached)
            wheel_runtime.wheel_clone_reserved = 130;
    }
    for (unsigned i = 0; i < 12; i++)
        wheel_release(&copies[i]);
    return attached;
}

static struct
{
    uintptr_t root, layer, front_option, rear_option;
    uintptr_t front_rows, rear_rows, front_first, rear_first;
    unsigned enabled, front_index, rear_index, rear_open, was_visible;
} wheel_split;

static uintptr_t wheel_visible_source(void);

static int wheel_split_visible(void)
{
    volatile WheelRuntime *m = &wheel_runtime;
    if (!wheel_split.enabled || !m->enabled || !hud_update_context(m->base, m->context))
        return 0;
    /* Native screen teardown can free a layer while its root remains mounted. */
    uintptr_t source = wheel_visible_source();
    if (!source || source != m->wheel_clone_source)
        return 0;
    uintptr_t parent = *(const uintptr_t *)(source + 0x30);
    uintptr_t layer = *(const uintptr_t *)(parent + 0x30);
    if (layer != wheel_split.layer || *(const uintptr_t *)(layer + 0x30) != wheel_split.root ||
        wheel_find_option(source) != wheel_split.front_option)
        return 0;
    uintptr_t rear = *(const uintptr_t *)(source + 0xa8);
    return rear && rear == m->wheel_clone_result && menu_pointer(rear) &&
           *(const uintptr_t *)rear == LIVE_MENU_ITEM_VA &&
           *(const uintptr_t *)(rear + 0x30) == parent &&
           wheel_find_option(rear) == wheel_split.rear_option;
}

static void wheel_split_tick(int after_frame)
{
    if (!wheel_split_visible())
    {
        wheel_split.was_visible = 0;
        return;
    }
    uintptr_t front_option = wheel_split.front_option;
    uintptr_t rear_option = wheel_split.rear_option;
    if (*(const uintptr_t *)front_option != LIVE_MENU_OPTION_VA ||
        *(const uintptr_t *)rear_option != LIVE_MENU_OPTION_VA)
        return;
    unsigned front_index = *(const uint32_t *)(front_option + 0x388);
    unsigned rear_index = *(const uint32_t *)(rear_option + 0x388);
    unsigned rear_open = *(const uint32_t *)(rear_option + 0x400);
    uintptr_t front_event = wheel_front_event(front_option);
    uintptr_t front = wheel_event_rows(front_event);
    uintptr_t rear = wheel_event_rows(wheel_runtime.wheel_event_result);
    if (!wheel_compose_rows(front, rear, front_index, rear_index))
        return;
    if (!after_frame)
        return;
    uintptr_t front_box = *(const uintptr_t *)(front_option + 0x3c8);
    uintptr_t rear_box = *(const uintptr_t *)(rear_option + 0x3c8);
    if (!menu_pointer(front_box) || !menu_pointer(rear_box))
        return;
    uintptr_t front_first = *(const uintptr_t *)(front_box + 0x2c8);
    uintptr_t rear_first = *(const uintptr_t *)(rear_box + 0x2c8);
    int refreshed = !wheel_split.was_visible || front != wheel_split.front_rows ||
                    rear != wheel_split.rear_rows || front_first != wheel_split.front_first ||
                    rear_first != wheel_split.rear_first;
    if (refreshed)
    {
        (void)wheel_axis_labels(front_option, front, 0);
        (void)wheel_axis_labels(rear_option, rear, 1);
    }
    /* Rear selection uses the native OptionMenu's index and Cancel restore.
     * Invoke the original wheel event's preview method after native UI locks
     * are released, including on Cancel and popup closure. */
    int rear_changed = rear_index != wheel_split.rear_index;
    int rear_closed = wheel_split.rear_open && !rear_open;
    if (rear_changed || rear_closed)
    {
        uintptr_t index = 0, value = 0;
        ((void (*)(uintptr_t *, int))(uintptr_t)0x0040eef0u)(&index, (int)rear_index);
        uintptr_t args[2] = {*(const uintptr_t *)(rear_option + 0x70), index};
        if (wheel_invoke(wheel_runtime.wheel_event_result, "ApplyPreview", 2, args, &value))
        {
            wheel_split.rear_index = rear_index;
            wheel_split.rear_open = rear_open;
        }
        wheel_release(&value);
        wheel_release(&index);
    }
    else
        wheel_split.rear_open = rear_open;
    if (refreshed || front_index != wheel_split.front_index || rear_changed || rear_closed)
    {
        (void)wheel_option_text(front_option, front, front_index, 0);
        (void)wheel_option_text(rear_option, rear, rear_index, 1);
    }
    wheel_split.front_index = front_index;
    wheel_split.front_rows = front;
    wheel_split.rear_rows = rear;
    wheel_split.front_first = front_first;
    wheel_split.rear_first = rear_first;
    wheel_split.was_visible = 1;
}

static uintptr_t wheel_visible_source(void)
{
    volatile WheelRuntime *m = &wheel_runtime;
    for (unsigned i = 0; i < 6; i++)
    {
        uintptr_t render = *(const uintptr_t *)(m->context + 0x698 + 8u * i);
        if (!hud_object(render, 0x1f8) || *(const uintptr_t *)render != m->base + HUD_RENDER_VT_RVA)
            continue;
        uintptr_t begin = *(const uintptr_t *)(render + 0x1e8);
        uintptr_t end = *(const uintptr_t *)(render + 0x1f0);
        if (!hud_vector(begin, end, 64))
            continue;
        for (uintptr_t root_slot = begin; root_slot < end; root_slot += 8)
        {
            uintptr_t root = *(const uintptr_t *)root_slot;
            char name[LIVE_HUD_NAME];
            if (!menu_pointer(root) || *(const uintptr_t *)root != m->base + HUD_ROOT_VT_RVA ||
                !hud_name(root, name) || !hud_same(name, "CarCustomizeRoot"))
                continue;
            uintptr_t children = *(const uintptr_t *)(root + 0x160);
            if (!menu_pointer(children) || *(const uintptr_t *)children != m->base + HUD_NLW_RVA)
                continue;
            uintptr_t first = *(const uintptr_t *)(children + 0x38);
            uintptr_t last = *(const uintptr_t *)(children + 0x40);
            if (!hud_vector(first, last, 256))
                continue;
            for (uintptr_t slot = first; slot < last; slot += 8)
            {
                uintptr_t layer = *(const uintptr_t *)slot;
                if (!menu_pointer(layer) || *(const uintptr_t *)layer != LIVE_MENU_ITEM_VA ||
                    !hud_name(layer, name) || !hud_same(name, "WheelEditLayer") ||
                    !(*(const uint32_t *)(layer + 0x214) & menu_visible_flag))
                    continue;
                uintptr_t row = *(const uintptr_t *)(layer + 0x2c8);
                for (unsigned j = 0; row && j < 32; j++)
                {
                    if (!menu_pointer(row) || *(const uintptr_t *)(row + 0x30) != layer)
                        break;
                    if (*(const uintptr_t *)row == 0x055a51a8u)
                    {
                        uintptr_t column = *(const uintptr_t *)(row + 0x2c8);
                        for (unsigned k = 0; column && k < 8; k++)
                        {
                            if (!menu_pointer(column) || *(const uintptr_t *)(column + 0x30) != row)
                                break;
                            if (hud_name(column, name) && hud_same(name, "Offset"))
                                return column;
                            column = *(const uintptr_t *)(column + 0xa8);
                        }
                    }
                    row = *(const uintptr_t *)(row + 0xa8);
                }
            }
        }
    }
    return 0;
}

static void wheel_auto_setup(void)
{
    volatile WheelRuntime *m = &wheel_runtime;
    if (!m->enabled || m->base != LIVE_IMAGE_BASE || !hud_update_context(m->base, m->context) ||
        *(const uint64_t *)(uintptr_t)0x03896010u != UINT64_C(0x41544153e5894855))
        return;
    uint32_t state = __atomic_load_n(&m->wheel_clone_state, __ATOMIC_ACQUIRE);
    if (state == MENU_CALL_QUEUED || state == MENU_CALL_RUNNING)
        return;
    uintptr_t source = wheel_visible_source();
    uintptr_t option = source ? wheel_find_option(source) : 0;
    if (!option || *(const uint32_t *)(option + 0x400) || *(const uint32_t *)(option + 0x390) != 12 ||
        !wheel_front_event(option))
        return;
    if (source != m->wheel_clone_source)
    {
        wheel_split.enabled = 0;
        uintptr_t event = m->wheel_event_result, value = 0;
        m->wheel_event_result = 0;
        if (menu_pointer(event))
        {
            (void)wheel_invoke(event, "Finalize", 0, 0, &value);
            wheel_release(&value);
            wheel_release(&event);
        }
        uintptr_t copy = m->wheel_clone_result;
        m->wheel_clone_result = 0;
        wheel_release(&copy);
        m->wheel_clone_source = source;
        m->wheel_clone_reserved = 0;
        state = MENU_CALL_IDLE;
    }
    if (state != MENU_CALL_IDLE && state != MENU_CALL_DONE)
        return;
    uint32_t marker = m->wheel_clone_reserved;
    uint32_t operation = 0;
    if (m->wheel_clone_result)
    {
        if (marker == 2)
            operation = 11;
        else if (marker == 4)
            operation = 12;
        else if (marker == 6)
            operation = 13;
        else if (marker == 8)
            operation = 14;
        else if (marker == 10)
            operation = 15;
        else if (marker == 12)
            operation = 16;
        else if (marker == 14)
            operation = 17;
        else if (marker == 16)
        {
            uintptr_t rear_option = wheel_find_option(m->wheel_clone_result);
            operation = rear_option && *(const uint32_t *)(rear_option + 0x390) == 12 ? 18 : 19;
        }
        else
            return;
    }
    m->wheel_clone_reserved = operation;
    __atomic_store_n(&m->wheel_clone_state, MENU_CALL_QUEUED, __ATOMIC_RELEASE);
}

static void wheel_clone(void)
{
    volatile WheelRuntime *m = &wheel_runtime;
    uint32_t queued = MENU_CALL_QUEUED;
    if (!__atomic_compare_exchange_n(&m->wheel_clone_state, &queued, MENU_CALL_RUNNING, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_RELAXED))
        return;
    uint32_t result = MENU_CALL_REJECTED;
    uint32_t operation = m->wheel_clone_reserved;
    uintptr_t source = m->wheel_clone_source;
    char name[LIVE_HUD_NAME];
    m->wheel_clone_reserved = 0;
    if (!m->enabled || m->base != LIVE_IMAGE_BASE || !menu_pointer(source) ||
        *(const uintptr_t *)source != LIVE_MENU_ITEM_VA || !hud_name(source, name) ||
        !hud_same(name, "Offset"))
        goto finish;
    uintptr_t parent = *(const uintptr_t *)(source + 0x30);
    if (!menu_pointer(parent) || *(const uintptr_t *)parent != 0x055a51a8u)
        goto finish;
    uintptr_t layer = *(const uintptr_t *)(parent + 0x30);
    if (!menu_pointer(layer) || !hud_name(layer, name) || !hud_same(name, "WheelEditLayer"))
        goto finish;
    uintptr_t child = *(const uintptr_t *)(source + 0x2c8);
    uintptr_t front_option = 0;
    unsigned options = 0;
    for (unsigned i = 0; child && i < 16; i++)
    {
        if (!menu_pointer(child) || *(const uintptr_t *)(child + 0x30) != source)
            goto finish;
        if (*(const uintptr_t *)child == LIVE_MENU_OPTION_VA)
        {
            if (*(const uint32_t *)(child + 0x400))
                goto finish;
            options++;
            front_option = child;
        }
        child = *(const uintptr_t *)(child + 0xa8);
    }
    if (child || options != 1)
        goto finish;
    if (operation >= 17 && operation <= 20)
    {
        uintptr_t event = m->wheel_event_result;
        uintptr_t original_event = wheel_front_event(front_option);
        uintptr_t rear_option = wheel_find_option(m->wheel_clone_result);
        uintptr_t context = *(const uintptr_t *)(front_option + 0x70);
        if (!menu_pointer(event) || !original_event || !rear_option ||
            *(const uint32_t *)(rear_option + 0x400) || !menu_pointer(context) ||
            *(const uintptr_t *)context != m->base + HUD_RENDER_VT_RVA)
            goto finish;
        result = MENU_CALL_FAILED;
        if (operation == 19)
        {
            uintptr_t front_rows = wheel_event_rows(original_event);
            uintptr_t rear_rows = wheel_event_rows(event);
            unsigned selected = *(const uint32_t *)(front_option + 0x388);
            uintptr_t index = 0, value = 0;
            m->wheel_clone_reserved = 15;
            if (front_rows && rear_rows && front_rows != rear_rows && selected < 12 &&
                wheel_populate_rear(front_option, rear_option, rear_rows, context))
            {
                ((void (*)(uintptr_t *, int))(uintptr_t)0x0040eef0u)(&index, (int)selected);
                uintptr_t args[2] = {context, index};
                if (wheel_invoke(event, "SetIndex", 2, args, &value))
                {
                    m->wheel_clone_reserved = 16;
                    result = MENU_CALL_DONE;
                }
            }
            wheel_release(&value);
            wheel_release(&index);
            goto finish;
        }
        if (operation == 17)
        {
            if (*(const uint32_t *)(rear_option + 0x390))
                goto finish;
            uintptr_t controller =
                wheel_event_attribute(original_event, "__toplevel__::main::gt7::Framework::EventBase::ctrl_");
            uintptr_t ctx = 0, shop = 0, car = 0, value = 0, index = 0;
            m->wheel_clone_reserved = 15;
            if (menu_pointer(controller) && wheel_invoke(controller, "Ctx", 0, 0, &ctx))
                shop = wheel_attribute(ctx, "wheelShopCtx_");
            if (shop)
                car = wheel_attribute(shop, "previewCar_");
            if (car)
            {
                uintptr_t args[3] = {context, car, 0};
                if (wheel_invoke(event, "Setup", 3, args, &value))
                {
                    wheel_release(&value);
                    unsigned selected = *(const uint32_t *)(front_option + 0x388);
                    if (selected < 12)
                    {
                        uintptr_t front_rows = wheel_event_rows(original_event);
                        uintptr_t rear_rows = wheel_event_rows(event);
                        if (rear_rows)
                            selected = wheel_saved_rear_index(car, rear_rows, selected);
                        ((void (*)(uintptr_t *, int))(uintptr_t)0x0040eef0u)(&index, (int)selected);
                        uintptr_t select_args[2] = {context, index};
                        if (front_rows && rear_rows && front_rows != rear_rows &&
                            (*(const uint32_t *)(rear_option + 0x390) == 12 ||
                             wheel_populate_rear(front_option, rear_option, rear_rows, context)) &&
                            wheel_invoke(event, "SetIndex", 2, select_args, &value))
                        {
                            m->wheel_clone_reserved = 16;
                            result = MENU_CALL_DONE;
                        }
                    }
                }
            }
            wheel_release(&value);
            wheel_release(&index);
            wheel_release(&car);
            wheel_release(&shop);
            wheel_release(&ctx);
        }
        else
        {
            uintptr_t front_rows = wheel_event_rows(original_event);
            uintptr_t rear_rows = wheel_event_rows(event);
            unsigned front_index = *(const uint32_t *)(front_option + 0x388);
            unsigned rear_index = *(const uint32_t *)(rear_option + 0x388);
            if (!front_rows || !rear_rows || front_rows == rear_rows || front_index >= 12 ||
                rear_index >= 12 || wheel_split.enabled)
                goto finish;
            m->wheel_clone_reserved = 17;
            if (!wheel_axis_labels(front_option, front_rows, 0) ||
                !wheel_axis_labels(rear_option, rear_rows, 1))
                goto finish;
            int refreshed = wheel_option_text(front_option, front_rows, front_index, 0) &&
                            wheel_option_text(rear_option, rear_rows, rear_index, 1);
            uintptr_t label = *(const uintptr_t *)(source + 0x2c8);
            if (!refreshed || !menu_pointer(label) || *(const uintptr_t *)label != 0x055910c0u)
                goto finish;
            GameString text;
            menu_string(&text, "Front Offset", 12);
            ((void (*)(uintptr_t, const GameString *))(uintptr_t)0x0072cf50u)(label, &text);
            wheel_split.root = *(const uintptr_t *)(layer + 0x30);
            wheel_split.layer = layer;
            wheel_split.front_option = front_option;
            wheel_split.rear_option = rear_option;
            wheel_split.rear_index = rear_index;
            wheel_split.rear_open = 0;
            if (menu_pointer(wheel_split.root) &&
                *(const uintptr_t *)wheel_split.root == m->base + HUD_ROOT_VT_RVA &&
                wheel_compose_rows(front_rows, rear_rows, front_index, rear_index))
            {
                wheel_split.enabled = 1;
                m->wheel_clone_reserved = 18;
                result = MENU_CALL_DONE;
            }
        }
        goto finish;
    }
    if (operation == 15 || operation == 16)
    {
        uintptr_t event = m->wheel_event_result;
        if (!menu_pointer(event) || *(const uintptr_t *)event != 0x05917f08u)
            goto finish;
        uintptr_t value = 0;
        if (operation == 15)
        {
            if (*(const uintptr_t *)(event + 0x18))
                goto finish;
            uintptr_t callbacks = *(const uintptr_t *)(front_option + 0x3e0);
            if (!menu_pointer(callbacks) || *(const uintptr_t *)callbacks != 0x05911d08u)
                goto finish;
            uintptr_t begin = *(const uintptr_t *)(callbacks + 0x28);
            if (!menu_pointer(begin) || *(const uintptr_t *)(callbacks + 0x30) != begin + 8)
                goto finish;
            uintptr_t bound = *(const uintptr_t *)begin;
            if (!menu_pointer(bound) || *(const uintptr_t *)bound != 0x05916878u)
                goto finish;
            uintptr_t original_event = *(const uintptr_t *)(bound + 0x20);
            if (!menu_pointer(original_event) || *(const uintptr_t *)original_event != 0x05917f08u ||
                *(const uintptr_t *)(original_event + 0x10) != *(const uintptr_t *)(event + 0x10))
                goto finish;
            uintptr_t attributes = *(const uintptr_t *)(original_event + 0x18);
            if (!menu_pointer(attributes))
                goto finish;
            uintptr_t controller = wheel_map_value(*(const uintptr_t *)attributes,
                                                   "__toplevel__::main::gt7::Framework::EventBase::ctrl_");
            if (!menu_pointer(controller) || *(const uintptr_t *)controller != 0x05917f08u)
                goto finish;
            m->wheel_clone_reserved = 11;
            result = MENU_CALL_FAILED;
            if (wheel_invoke(event, "Initialize", 1, &controller, &value) &&
                menu_pointer(*(const uintptr_t *)(event + 0x18)))
            {
                m->wheel_clone_reserved = 12;
                result = MENU_CALL_DONE;
            }
        }
        else
        {
            uintptr_t copy = m->wheel_clone_result;
            if (!menu_pointer(copy) || *(const uintptr_t *)(copy + 0x30) != parent)
                goto finish;
            uintptr_t option = *(const uintptr_t *)(copy + 0x2c8);
            for (unsigned i = 0; option && i < 8; i++)
            {
                if (!menu_pointer(option) || *(const uintptr_t *)(option + 0x30) != copy)
                    goto finish;
                if (*(const uintptr_t *)option == LIVE_MENU_OPTION_VA)
                    break;
                option = *(const uintptr_t *)(option + 0xa8);
            }
            if (!menu_pointer(option) || *(const uintptr_t *)option != LIVE_MENU_OPTION_VA ||
                *(const uint32_t *)(option + 0x400))
                goto finish;
            m->wheel_clone_reserved = 13;
            result = MENU_CALL_FAILED;
            if (wheel_invoke(event, "SetWidget", 1, &option, &value))
            {
                if (value)
                    ((void (*)(uintptr_t *))(uintptr_t)LIVE_MENU_RELEASE_VA)(&value);
                if (wheel_invoke(event, "Widget", 0, 0, &value) && value == option)
                {
                    m->wheel_clone_reserved = 14;
                    result = MENU_CALL_DONE;
                }
            }
        }
        if (value)
            ((void (*)(uintptr_t *))(uintptr_t)LIVE_MENU_RELEASE_VA)(&value);
        goto finish;
    }
    if (operation == 14)
    {
        /* Construct an empty instance of the original wheel-event class.
         * Do not deep-copy the event: its ctrl_ attribute leads back into the
         * controller graph. Do not initialize or bind the new instance yet. */
        uintptr_t callbacks = *(const uintptr_t *)(front_option + 0x3e0);
        if (m->wheel_event_result || !menu_pointer(callbacks) || *(const uintptr_t *)callbacks != 0x05911d08u)
            goto finish;
        uintptr_t begin = *(const uintptr_t *)(callbacks + 0x28);
        uintptr_t end = *(const uintptr_t *)(callbacks + 0x30);
        if (!menu_pointer(begin) || end != begin + sizeof(uintptr_t))
            goto finish;
        uintptr_t bound = *(const uintptr_t *)begin;
        if (!menu_pointer(bound) || *(const uintptr_t *)bound != 0x05916878u)
            goto finish;
        uintptr_t receiver = *(const uintptr_t *)(bound + 0x20);
        if (!menu_pointer(receiver) || *(const uintptr_t *)receiver != 0x05917f08u)
            goto finish;
        uintptr_t type = *(const uintptr_t *)(receiver + 0x10);
        if (!menu_pointer(type) || *(const uintptr_t *)type != 0x059118e0u || !hud_name(type, name) ||
            !hud_same(name, "WheelOffsetOptionMenuEvent"))
            goto finish;
        uintptr_t event = 0;
        m->wheel_clone_reserved = 9;
        ((void (*)(uintptr_t *, uintptr_t))(uintptr_t)0x00428c00u)(&event, type);
        m->wheel_clone_reserved = 10;
        result = MENU_CALL_FAILED;
        if (event != receiver && menu_pointer(event) && *(const uintptr_t *)event == 0x05917f08u &&
            *(const uintptr_t *)(event + 0x10) == type)
        {
            m->wheel_event_result = event;
            event = 0;
            result = MENU_CALL_DONE;
        }
        if (event)
            ((void (*)(uintptr_t *))(uintptr_t)LIVE_MENU_RELEASE_VA)(&event);
        goto finish;
    }
    if (operation == 13)
    {
        uintptr_t copy = m->wheel_clone_result;
        uintptr_t root = *(const uintptr_t *)(layer + 0x30);
        if (!menu_pointer(copy) || *(const uintptr_t *)(copy + 0x30) != parent || !menu_pointer(root) ||
            *(const uintptr_t *)root != LIVE_IMAGE_BASE + HUD_ROOT_VT_RVA ||
            *(const uint32_t *)(parent + 0x2ec) != 5)
            goto finish;
        float viewport = *(const float *)(root + HUD_OFF_RECT);
        int spacing = *(const int32_t *)(parent + 0x390);
        float target = (viewport - 170.0f - 300.0f - 6.0f * spacing) / 4.0f;
        if (viewport < 1600.0f || viewport > 4000.0f || spacing != 50 || target < 200.0f || target > 400.0f)
            goto finish;
        static const char names[4][12] = {"Diameter", "Width", "Offset", "RearOffset"};
        uintptr_t columns[4];
        uintptr_t node = *(const uintptr_t *)(parent + 0x2c8);
        for (unsigned i = 0; i < 4; i++)
        {
            uintptr_t allocation = menu_pointer(node) ? *(const uintptr_t *)(node + 0x230) : 0;
            if (!menu_pointer(node) || *(const uintptr_t *)(node + 0x30) != parent || !hud_name(node, name) ||
                !hud_same(name, names[i]) || !menu_pointer(allocation) ||
                *(const float *)allocation != 400.0f)
                goto finish;
            columns[i] = node;
            node = *(const uintptr_t *)(node + 0xa8);
        }
        if (!menu_pointer(node) || !hud_name(node, name) || !hud_same(name, "PurchaseButton") ||
            *(const float *)(node + 0xd8) != 300.0f)
            goto finish;
        m->wheel_clone_reserved = 7;
        result = MENU_CALL_FAILED;
        for (unsigned i = 0; i < 4; i++)
            if (!wheel_fit_column(columns[i], 400.0f - target, 0))
                goto finish;
        __atomic_or_fetch((uint32_t *)(parent + 0x210), 7u, __ATOMIC_RELAXED);
        __atomic_or_fetch((uint32_t *)(layer + 0x210), 7u, __ATOMIC_RELAXED);
        m->wheel_clone_reserved = 8;
        result = MENU_CALL_DONE;
        goto finish;
    }
    if (operation == 12)
    {
        uintptr_t copy = m->wheel_clone_result;
        uintptr_t context = *(const uintptr_t *)(source + 0x70);
        uintptr_t before = *(const uintptr_t *)(source + 0xa8);
        if (!menu_pointer(copy) || *(const uintptr_t *)copy != LIVE_MENU_ITEM_VA ||
            *(const uintptr_t *)(copy + 0x30) || !hud_name(copy, name) || !hud_same(name, "PulldownItemC") ||
            !menu_pointer(context) || *(const uintptr_t *)context != LIVE_IMAGE_BASE + HUD_RENDER_VT_RVA ||
            !menu_pointer(before) || *(const uintptr_t *)(before + 0x30) != parent ||
            !hud_name(before, name) || !hud_same(name, "PurchaseButton") ||
            *(const uint32_t *)(parent + 0x2ec) != 4)
            goto finish;
        uintptr_t label = *(const uintptr_t *)(copy + 0x2c8);
        if (!menu_pointer(label) || *(const uintptr_t *)label != 0x055910c0u || !hud_name(label, name) ||
            !hud_same(name, "Label"))
            goto finish;
        GameString text;
        menu_string(&text, "RearOffset", 10);
        typedef void (*SetName)(uintptr_t, const GameString *);
        uintptr_t rename = *(const uintptr_t *)(LIVE_MENU_ITEM_VA + 0x270);
        ((SetName)rename)(copy, &text);
        if (!hud_name(copy, name) || !hud_same(name, "RearOffset"))
            goto finish;
        menu_string(&text, "Rear Offset", 11);
        ((void (*)(uintptr_t, const GameString *))(uintptr_t)0x0072cf50u)(label, &text);
        menu_show_widget(copy, 1);
        m->wheel_clone_reserved = 5;
        /* Preserve native HBox packing: no manual half-width rectangles.
         * Insert immediately before Install using the native child operation. */
        ((MenuAppend)(uintptr_t)0x00946b20u)(parent + 0x2b8, context, copy, before);
        if (*(const uintptr_t *)(copy + 0x30) == parent && *(const uintptr_t *)(source + 0xa8) == copy &&
            *(const uintptr_t *)(copy + 0xa8) == before)
        {
            m->wheel_clone_reserved = 6;
            result = MENU_CALL_DONE;
        }
        else
            result = MENU_CALL_FAILED;
        goto finish;
    }
    if (operation == 11)
    {
        uintptr_t copy = m->wheel_clone_result;
        if (!menu_pointer(copy) || *(const uintptr_t *)copy != LIVE_MENU_ITEM_VA ||
            *(const uintptr_t *)(copy + 0x30) || *(const uintptr_t *)(copy + 0x70) || !hud_name(copy, name) ||
            !hud_same(name, "PulldownItemC") ||
            *(const uintptr_t *)(LIVE_MENU_ITEM_VA + 0x340) != 0x004774e0u)
            goto finish;
        /* Run native cache/resource realization separately from __init__.
         * Acquire the same initialization context used by native doCopy. */
        uintptr_t context = 0;
        ((void (*)(uintptr_t *))(uintptr_t)0x0047abf0u)(&context);
        if (menu_pointer(context))
        {
            m->wheel_clone_reserved = 3;
            ((void (*)(uintptr_t, uintptr_t))(uintptr_t)0x004774e0u)(copy, context);
            m->wheel_clone_reserved = 4;
            result = MENU_CALL_DONE;
        }
        if (context)
            ((void (*)(uintptr_t *))(uintptr_t)LIVE_MENU_RELEASE_VA)(&context);
        goto finish;
    }
    if (m->wheel_clone_result)
        goto finish;
    uintptr_t blueprint = *(const uintptr_t *)(source + 0x68);
    if (!menu_pointer(blueprint) || *(const uintptr_t *)blueprint != LIVE_MENU_ITEM_VA ||
        *(const uintptr_t *)(blueprint + 0x70) || !hud_name(blueprint, name) ||
        !hud_same(name, "PulldownItemC"))
        goto finish;
    /* Only the structural copy phase. Do not call doCopy (0xc96f50): it
     * additionally executes recursive __init__ and onRealize callbacks.
     * Virtual +0x58 is the native copy factory used by doCopy itself. */
    uintptr_t factory = *(const uintptr_t *)(LIVE_MENU_ITEM_VA + 0x58);
    if (factory != 0x00428a60u)
        goto finish;
    m->wheel_clone_reserved = 1;
    uintptr_t copy = 0;
    ((void (*)(uintptr_t *, uintptr_t))factory)(&copy, blueprint);
    m->wheel_clone_reserved = 2;
    result = MENU_CALL_FAILED;
    if (copy != blueprint && menu_pointer(copy) && *(const uintptr_t *)copy == LIVE_MENU_ITEM_VA &&
        !*(const uintptr_t *)(copy + 0x30))
    {
        m->wheel_clone_result = copy;
        copy = 0;
        result = MENU_CALL_DONE;
    }
    if (copy)
        ((void (*)(uintptr_t *))(uintptr_t)LIVE_MENU_RELEASE_VA)(&copy);
finish:
    __atomic_store_n(&m->wheel_clone_state, result, __ATOMIC_RELEASE);
}

__attribute__((visibility("hidden"))) void wheel_frame(void *context, float delta_time)
{
    wheel_runtime.heartbeat++;
    wheel_runtime.context = (uintptr_t)context;
    wheel_auto_setup();
    wheel_clone();
    wheel_split_tick(0);
    ((void (*)(void *, float))(uintptr_t)wheel_runtime.original)(context, delta_time);
    wheel_split_tick(1);
}
