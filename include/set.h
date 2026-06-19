#ifndef SET_H
#define SET_H

typedef struct set_item s_set_item;

struct set_item {
    void *data;
    size_t len;
    size_t hash;
    unsigned long usage;
    int long_p;
    long long_value;
    int double_p;
    double double_value;
};

s_set_item *new_set_item(size_t len, void *data, size_t hash);

typedef size_t (*f_hash)(const void *data, size_t len);

typedef struct set {
    size_t max;
    s_set_item *items;
    size_t count;
    size_t collisions;
    f_hash hash;
} s_set;

void set_init(s_set *set, size_t max);

void set_destroy(s_set *set);

s_set *new_set(size_t max);

void delete_set(s_set *set);

s_set_item *set_add(s_set *set, void *data, size_t len);

s_set_item *set_add_h(s_set *set, void *data, size_t len, size_t hash);

int set_remove(s_set *set, s_set_item *item);

s_set_item *set_get(s_set *set, const void *data, size_t len);

s_set_item *set_get_h(s_set *set, const void *data, size_t len, size_t hash);

void set_resize(s_set *set, size_t max);

typedef struct set_cursor {
    s_set *set;
    size_t i;
    s_set_item *item;
    size_t count;
} s_set_cursor;

void set_cursor_init(s_set *set, s_set_cursor *c);

s_set_item *set_cursor_next(s_set_cursor *c);

#endif
