#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "wyhash.h"
#include "set.h"

s_set_item *new_set_item(size_t len, void *data, size_t hash)
{
    (void)len;
    (void)data;
    (void)hash;
    // new_set_item is legacy and no longer needed for open addressing.
    return NULL;
}

static size_t set_wyhash(const void *key, size_t len)
{
    return (size_t)wyhash(key, len, 0, _wyp);
}

void set_init(s_set *set, size_t max)
{
    assert(set);
    assert(max > 0);
    set->max = max;
    set->items = calloc(max, sizeof(s_set_item));
    assert(set->items);
    set->count = 0;
    set->collisions = 0;
    set->hash = set_wyhash;
}

void set_destroy(s_set *set)
{
    assert(set);
    free(set->items);
    set->items = NULL;
    set->max = 0;
    set->count = 0;
    set->collisions = 0;
}

s_set *new_set(size_t max)
{
    s_set *set;
    assert(max > 0);
    set = malloc(sizeof(s_set));
    if (set)
        set_init(set, max);
    return set;
}

void delete_set(s_set *set)
{
    if (set) {
        set_destroy(set);
        free(set);
    }
}

s_set_item *set_add(s_set *set, void *data, size_t len)
{
    size_t hash;
    assert(set);
    assert(data);
    assert(len > 0);
    hash = set->hash(data, len);
    return set_add_h(set, data, len, hash);
}

s_set_item *set_add_h(s_set *set, void *data, size_t len, size_t hash)
{
    assert(set);
    assert(data);
    assert(len > 0);

    // Keep load factor <= 50% for optimal performance under linear probing
    if (set->count * 2 >= set->max) {
        set_resize(set, set->max * 2);
    }

    size_t i = hash % set->max;
    while (set->items[i].len > 0) {
        if (set->items[i].hash == hash && set->items[i].len == len && memcmp(set->items[i].data, data, len) == 0) {
            return &set->items[i];
        }
        i = (i + 1) % set->max;
    }

    if (i != hash % set->max) {
        set->collisions++;
    }

    set->items[i].data = data;
    set->items[i].len = len;
    set->items[i].hash = hash;
    set->items[i].usage = 0;
    set->items[i].long_p = 0;
    set->items[i].long_value = 0;
    set->items[i].double_p = 0;
    set->items[i].double_value = 0.0;
    set->count++;
    return &set->items[i];
}

int set_remove(s_set *set, s_set_item *item)
{
    assert(set);
    if (!item)
        return 0;

    if (item < set->items || (size_t)(item - set->items) >= set->max) {
        return 0;
    }

    size_t i = item - set->items;
    if (set->items[i].len == 0) {
        return 0;
    }

    if (i != set->items[i].hash % set->max) {
        set->collisions--;
    }

    set->items[i].len = 0;
    set->items[i].data = NULL;
    set->count--;

    size_t j = i;
    while (1) {
        j = (j + 1) % set->max;
        if (set->items[j].len == 0) {
            break;
        }
        size_t bucket = set->items[j].hash % set->max;
        int can_move = 0;
        if (i < j) {
            can_move = (bucket <= i || bucket > j);
        } else {
            can_move = (bucket <= i && bucket > j);
        }
        if (can_move) {
            int was_collision = (j != bucket);
            int is_collision = (i != bucket);
            if (was_collision && !is_collision) {
                set->collisions--;
            } else if (!was_collision && is_collision) {
                set->collisions++;
            }
            set->items[i] = set->items[j];
            set->items[j].len = 0;
            set->items[j].data = NULL;
            i = j;
        }
    }
    return 1;
}

s_set_item *set_get(s_set *set, const void *data, size_t len)
{
    size_t hash;
    assert(set);
    assert(data);
    assert(len > 0);
    hash = set->hash(data, len);
    return set_get_h(set, data, len, hash);
}

s_set_item *set_get_h(s_set *set, const void *data, size_t len, size_t hash)
{
    assert(set);
    assert(data);
    assert(len > 0);
    if (set->max == 0)
        return NULL;

    size_t i = hash % set->max;
    while (set->items[i].len > 0) {
        if (set->items[i].hash == hash && set->items[i].len == len && memcmp(set->items[i].data, data, len) == 0) {
            return &set->items[i];
        }
        i = (i + 1) % set->max;
    }
    return NULL;
}

void set_resize(s_set *set, size_t max)
{
    assert(set);
    if (max == set->max)
        return;
    if (max < set->count)
        return;

    s_set_item *old_items = set->items;
    size_t old_max = set->max;

    set->max = max;
    set->items = calloc(max, sizeof(s_set_item));
    assert(set->items);
    set->count = 0;
    set->collisions = 0;

    for (size_t i = 0; i < old_max; i++) {
        if (old_items[i].len > 0) {
            s_set_item *new_item = set_add_h(set, old_items[i].data, old_items[i].len, old_items[i].hash);
            new_item->usage = old_items[i].usage;
            new_item->long_p = old_items[i].long_p;
            new_item->long_value = old_items[i].long_value;
            new_item->double_p = old_items[i].double_p;
            new_item->double_value = old_items[i].double_value;
        }
    }
    free(old_items);
}

void set_cursor_init(s_set *set, s_set_cursor *c)
{
    assert(set);
    assert(c);
    c->set = set;
    c->i = 0;
    c->item = NULL;
    c->count = 0;
}

s_set_item *set_cursor_next(s_set_cursor *c)
{
    assert(c);
    if (c->count >= c->set->count)
        return NULL;
    while (c->i < c->set->max) {
        s_set_item *item = &c->set->items[c->i++];
        if (item->len > 0) {
            c->count++;
            return item;
        }
    }
    return NULL;
}
