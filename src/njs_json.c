
/*
 * Copyright (C) Dmitry Volyntsev
 * Copyright (C) NGINX, Inc.
 */


#include <njs_main.h>


typedef struct {
    njs_vm_t                   *vm;
    njs_mp_t                   *pool;
    njs_uint_t                 depth;
    const u_char               *start;
    const u_char               *end;
} njs_json_parse_ctx_t;


typedef struct {
    njs_value_t                value;

    uint8_t                    written;       /* 1 bit */

    enum {
       NJS_JSON_OBJECT,
       NJS_JSON_ARRAY,
    }                          type:8;

    uint32_t                   index;
    njs_array_t                *keys;
    njs_object_prop_t          *prop;
} njs_json_state_t;


typedef struct {
    njs_value_t                retval;

    njs_uint_t                 depth;
#define NJS_JSON_MAX_DEPTH     32
    njs_json_state_t           states[NJS_JSON_MAX_DEPTH];

    njs_function_t             *function;
} njs_json_parse_t;


typedef struct njs_chb_node_s njs_chb_node_t;

struct njs_chb_node_s {
    njs_chb_node_t             *next;
    u_char                     *start;
    u_char                     *pos;
    u_char                     *end;
};


typedef struct {
    njs_value_t                retval;

    njs_vm_t                   *vm;
    njs_mp_t                   *pool;
    njs_chb_node_t             *nodes;
    njs_chb_node_t             *last;

    njs_uint_t                 depth;
    njs_json_state_t           states[NJS_JSON_MAX_DEPTH];

    njs_value_t                replacer;
    njs_str_t                  space;
    u_char                     space_buf[16];
} njs_json_stringify_t;


static const u_char *njs_json_parse_value(njs_json_parse_ctx_t *ctx,
    njs_value_t *value, const u_char *p);
static const u_char *njs_json_parse_object(njs_json_parse_ctx_t *ctx,
    njs_value_t *value, const u_char *p);
static const u_char *njs_json_parse_array(njs_json_parse_ctx_t *ctx,
    njs_value_t *value, const u_char *p);
static const u_char *njs_json_parse_string(njs_json_parse_ctx_t *ctx,
    njs_value_t *value, const u_char *p);
static const u_char *njs_json_parse_number(njs_json_parse_ctx_t *ctx,
    njs_value_t *value, const u_char *p);
njs_inline uint32_t njs_json_unicode(const u_char *p);
static const u_char *njs_json_skip_space(const u_char *start,
    const u_char *end);

static njs_int_t njs_json_parse_iterator(njs_vm_t *vm, njs_json_parse_t *parse,
    njs_value_t *value);
static njs_int_t njs_json_parse_iterator_call(njs_vm_t *vm,
    njs_json_parse_t *parse, njs_json_state_t *state);
static void njs_json_parse_exception(njs_json_parse_ctx_t *ctx,
    const char *msg, const u_char *pos);

static njs_int_t njs_json_stringify_iterator(njs_vm_t *vm,
    njs_json_stringify_t *stringify, njs_value_t *value);
static njs_function_t *njs_object_to_json_function(njs_vm_t *vm,
    njs_value_t *value);
static njs_int_t njs_json_stringify_to_json(njs_json_stringify_t* stringify,
    njs_json_state_t *state, njs_value_t *key, njs_value_t *value);
static njs_int_t njs_json_stringify_replacer(njs_json_stringify_t* stringify,
    njs_json_state_t  *state, njs_value_t *key, njs_value_t *value);
static njs_int_t njs_json_stringify_array(njs_vm_t *vm,
    njs_json_stringify_t *stringify);

static njs_int_t njs_json_append_value(njs_json_stringify_t *stringify,
    const njs_value_t *value);
static njs_int_t njs_json_append_string(njs_json_stringify_t *stringify,
    const njs_value_t *value, char quote);
static njs_int_t njs_json_append_number(njs_json_stringify_t *stringify,
    const njs_value_t *value);

static njs_object_t *njs_json_wrap_value(njs_vm_t *vm, njs_value_t *wrapper,
    const njs_value_t *value);


#define NJS_JSON_BUF_MIN_SIZE       128

#define njs_json_buf_written(stringify, bytes)                              \
    (stringify)->last->pos += (bytes);

#define njs_json_buf_node_size(n) (size_t) ((n)->pos - (n)->start)
#define njs_json_buf_node_room(n) (size_t) ((n)->end - (n)->pos)

static njs_int_t njs_json_buf_append(njs_json_stringify_t *stringify,
    const char *msg, size_t len);
static u_char *njs_json_buf_reserve(njs_json_stringify_t *stringify,
    size_t size);
static njs_int_t njs_json_buf_pullup(njs_json_stringify_t *stringify,
    njs_str_t *str);


static const njs_object_prop_t  *njs_json_object_properties;


static njs_int_t
njs_json_parse(njs_vm_t *vm, njs_value_t *args, njs_uint_t nargs,
    njs_index_t unused)
{
    njs_int_t             ret;
    njs_value_t           *text, value, lvalue;
    const u_char          *p, *end;
    njs_json_parse_t      *parse, json_parse;
    const njs_value_t     *reviver;
    njs_string_prop_t     string;
    njs_json_parse_ctx_t  ctx;

    parse = &json_parse;

    text = njs_lvalue_arg(&lvalue, args, nargs, 1);

    if (njs_slow_path(!njs_is_string(text))) {
        ret = njs_value_to_string(vm, text, text);
        if (njs_slow_path(ret != NJS_OK)) {
            return ret;
        }
    }

    (void) njs_string_prop(&string, text);

    p = string.start;
    end = p + string.size;

    ctx.vm = vm;
    ctx.pool = vm->mem_pool;
    ctx.depth = NJS_JSON_MAX_DEPTH;
    ctx.start = string.start;
    ctx.end = end;

    p = njs_json_skip_space(p, end);
    if (njs_slow_path(p == end)) {
        njs_json_parse_exception(&ctx, "Unexpected end of input", p);
        return NJS_ERROR;
    }

    p = njs_json_parse_value(&ctx, &value, p);
    if (njs_slow_path(p == NULL)) {
        return NJS_ERROR;
    }

    p = njs_json_skip_space(p, end);
    if (njs_slow_path(p != end)) {
        njs_json_parse_exception(&ctx, "Unexpected token", p);
        return NJS_ERROR;
    }

    reviver = njs_arg(args, nargs, 2);

    if (njs_is_function(reviver) && njs_is_object(&value)) {
        parse->function = njs_function(reviver);
        parse->depth = 0;

        return njs_json_parse_iterator(vm, parse, &value);
    }

    vm->retval = value;

    return NJS_OK;
}


njs_int_t
njs_vm_json_parse(njs_vm_t *vm, njs_value_t *args, njs_uint_t nargs)
{
    njs_function_t  *parse;

    parse = njs_function(&njs_json_object_properties[0].value);

    return njs_vm_call(vm, parse, args, nargs);
}


static njs_int_t
njs_json_stringify(njs_vm_t *vm, njs_value_t *args, njs_uint_t nargs,
    njs_index_t unused)
{
    double                num;
    njs_int_t             i;
    njs_int_t             ret;
    njs_value_t           *replacer, *space;
    njs_json_stringify_t  *stringify, json_stringify;

    stringify = &json_stringify;

    stringify->vm = vm;
    stringify->pool = vm->mem_pool;
    stringify->depth = 0;
    stringify->nodes = NULL;
    stringify->last = NULL;

    replacer = njs_arg(args, nargs, 2);

    if (njs_is_function(replacer) || njs_is_array(replacer)) {
        stringify->replacer = *replacer;
        if (njs_is_array(replacer)) {
            ret = njs_json_stringify_array(vm, stringify);
            if (njs_slow_path(ret != NJS_OK)) {
                goto memory_error;
            }
        }

    } else {
        njs_set_undefined(&stringify->replacer);
    }

    stringify->space.length = 0;

    space = njs_arg(args, nargs, 3);

    if (njs_is_string(space) || njs_is_number(space)) {
        if (njs_is_string(space)) {
            njs_string_get(space, &stringify->space);
            stringify->space.length = njs_min(stringify->space.length, 10);

        } else {
            num = njs_number(space);

            if (!isnan(num) && !isinf(num) && num > 0) {
                num = njs_min(num, 10);

                stringify->space.length = (size_t) num;
                stringify->space.start = stringify->space_buf;

                for (i = 0; i < (int) num; i++) {
                    stringify->space.start[i] = ' ';
                }
            }
        }
    }

    return njs_json_stringify_iterator(vm, stringify, njs_arg(args, nargs, 1));

memory_error:

    njs_memory_error(vm);

    return NJS_ERROR;
}


njs_int_t
njs_vm_json_stringify(njs_vm_t *vm, njs_value_t *args, njs_uint_t nargs)
{
    njs_function_t  *stringify;

    stringify = njs_function(&njs_json_object_properties[1].value);

    return njs_vm_call(vm, stringify, args, nargs);
}


static const u_char *
njs_json_parse_value(njs_json_parse_ctx_t *ctx, njs_value_t *value,
    const u_char *p)
{
    switch (*p) {
    case '{':
        return njs_json_parse_object(ctx, value, p);

    case '[':
        return njs_json_parse_array(ctx, value, p);

    case '"':
        return njs_json_parse_string(ctx, value, p);

    case 't':
        if (njs_fast_path(ctx->end - p >= 4 && memcmp(p, "true", 4) == 0)) {
            *value = njs_value_true;

            return p + 4;
        }

        goto error;

    case 'f':
        if (njs_fast_path(ctx->end - p >= 5 && memcmp(p, "false", 5) == 0)) {
            *value = njs_value_false;

            return p + 5;
        }

        goto error;

    case 'n':
        if (njs_fast_path(ctx->end - p >= 4 && memcmp(p, "null", 4) == 0)) {
            *value = njs_value_null;

            return p + 4;
        }

        goto error;
    }

    if (njs_fast_path(*p == '-' || (*p - '0') <= 9)) {
        return njs_json_parse_number(ctx, value, p);
    }

error:

    njs_json_parse_exception(ctx, "Unexpected token", p);

    return NULL;
}


static const u_char *
njs_json_parse_object(njs_json_parse_ctx_t *ctx, njs_value_t *value,
    const u_char *p)
{
    njs_int_t           ret;
    njs_object_t        *object;
    njs_value_t         prop_name, prop_value;
    njs_object_prop_t   *prop;
    njs_lvlhsh_query_t  lhq;

    if (njs_slow_path(--ctx->depth == 0)) {
        njs_json_parse_exception(ctx, "Nested too deep", p);
        return NULL;
    }

    object = njs_object_alloc(ctx->vm);
    if (njs_slow_path(object == NULL)) {
        goto memory_error;
    }

    prop = NULL;

    for ( ;; ) {
        p = njs_json_skip_space(p + 1, ctx->end);
        if (njs_slow_path(p == ctx->end)) {
            goto error_end;
        }

        if (*p != '"') {
            if (njs_fast_path(*p == '}')) {
                if (njs_slow_path(prop != NULL)) {
                    njs_json_parse_exception(ctx, "Trailing comma", p - 1);
                    return NULL;
                }

                break;
            }

            goto error_token;
        }

        p = njs_json_parse_string(ctx, &prop_name, p);
        if (njs_slow_path(p == NULL)) {
            /* The exception is set by the called function. */
            return NULL;
        }

        p = njs_json_skip_space(p, ctx->end);
        if (njs_slow_path(p == ctx->end || *p != ':')) {
            goto error_token;
        }

        p = njs_json_skip_space(p + 1, ctx->end);
        if (njs_slow_path(p == ctx->end)) {
            goto error_end;
        }

        p = njs_json_parse_value(ctx, &prop_value, p);
        if (njs_slow_path(p == NULL)) {
            /* The exception is set by the called function. */
            return NULL;
        }

        prop = njs_object_prop_alloc(ctx->vm, &prop_name, &prop_value, 1);
        if (njs_slow_path(prop == NULL)) {
            goto memory_error;
        }

        njs_string_get(&prop_name, &lhq.key);
        lhq.key_hash = njs_djb_hash(lhq.key.start, lhq.key.length);
        lhq.value = prop;
        lhq.replace = 1;
        lhq.pool = ctx->pool;
        lhq.proto = &njs_object_hash_proto;

        ret = njs_lvlhsh_insert(&object->hash, &lhq);
        if (njs_slow_path(ret != NJS_OK)) {
            njs_internal_error(ctx->vm, "lvlhsh insert/replace failed");
            return NULL;
        }

        p = njs_json_skip_space(p, ctx->end);
        if (njs_slow_path(p == ctx->end)) {
            goto error_end;
        }

        if (*p != ',') {
            if (njs_fast_path(*p == '}')) {
                break;
            }

            goto error_token;
        }
    }

    njs_set_object(value, object);

    ctx->depth++;

    return p + 1;

error_token:

    njs_json_parse_exception(ctx, "Unexpected token", p);

    return NULL;

error_end:

    njs_json_parse_exception(ctx, "Unexpected end of input", p);

    return NULL;

memory_error:

    njs_memory_error(ctx->vm);

    return NULL;
}


static const u_char *
njs_json_parse_array(njs_json_parse_ctx_t *ctx, njs_value_t *value,
    const u_char *p)
{
    njs_int_t    ret;
    njs_bool_t   empty;
    njs_array_t  *array;
    njs_value_t  element;

    if (njs_slow_path(--ctx->depth == 0)) {
        njs_json_parse_exception(ctx, "Nested too deep", p);
        return NULL;
    }

    array = njs_array_alloc(ctx->vm, 0, 0);
    if (njs_slow_path(array == NULL)) {
        return NULL;
    }

    empty = 1;

    for ( ;; ) {
        p = njs_json_skip_space(p + 1, ctx->end);
        if (njs_slow_path(p == ctx->end)) {
            goto error_end;
        }

        if (*p == ']') {
            if (njs_slow_path(!empty)) {
                njs_json_parse_exception(ctx, "Trailing comma", p - 1);
                return NULL;
            }

            break;
        }

        p = njs_json_parse_value(ctx, &element, p);
        if (njs_slow_path(p == NULL)) {
            return NULL;
        }

        ret = njs_array_add(ctx->vm, array, &element);
        if (njs_slow_path(ret != NJS_OK)) {
            return NULL;
        }

        empty = 0;

        p = njs_json_skip_space(p, ctx->end);
        if (njs_slow_path(p == ctx->end)) {
            goto error_end;
        }

        if (*p != ',') {
            if (njs_fast_path(*p == ']')) {
                break;
            }

            goto error_token;
        }
    }

    njs_set_array(value, array);

    ctx->depth++;

    return p + 1;

error_token:

    njs_json_parse_exception(ctx, "Unexpected token", p);

    return NULL;

error_end:

    njs_json_parse_exception(ctx, "Unexpected end of input", p);

    return NULL;
}


static const u_char *
njs_json_parse_string(njs_json_parse_ctx_t *ctx, njs_value_t *value,
    const u_char *p)
{
    u_char        ch, *s, *dst;
    size_t        size, surplus;
    ssize_t       length;
    uint32_t      utf, utf_low;
    njs_int_t     ret;
    const u_char  *start, *last;

    enum {
        sw_usual = 0,
        sw_escape,
        sw_encoded1,
        sw_encoded2,
        sw_encoded3,
        sw_encoded4,
    } state;

    start = p + 1;

    dst = NULL;
    state = 0;
    surplus = 0;

    for (p = start; p < ctx->end; p++) {
        ch = *p;

        switch (state) {

        case sw_usual:

            if (ch == '"') {
                break;
            }

            if (ch == '\\') {
                state = sw_escape;
                continue;
            }

            if (njs_fast_path(ch >= ' ')) {
                continue;
            }

            njs_json_parse_exception(ctx, "Forbidden source char", p);

            return NULL;

        case sw_escape:

            switch (ch) {
            case '"':
            case '\\':
            case '/':
            case 'n':
            case 'r':
            case 't':
            case 'b':
            case 'f':
                surplus++;
                state = sw_usual;
                continue;

            case 'u':
                /*
                 * Basic unicode 6 bytes "\uXXXX" in JSON
                 * and up to 3 bytes in UTF-8.
                 *
                 * Surrogate pair: 12 bytes "\uXXXX\uXXXX" in JSON
                 * and 3 or 4 bytes in UTF-8.
                 */
                surplus += 3;
                state = sw_encoded1;
                continue;
            }

            njs_json_parse_exception(ctx, "Unknown escape char", p);

            return NULL;

        case sw_encoded1:
        case sw_encoded2:
        case sw_encoded3:
        case sw_encoded4:

            if (njs_fast_path((ch >= '0' && ch <= '9')
                              || (ch >= 'A' && ch <= 'F')
                              || (ch >= 'a' && ch <= 'f')))
            {
                state = (state == sw_encoded4) ? sw_usual : state + 1;
                continue;
            }

            njs_json_parse_exception(ctx, "Invalid Unicode escape sequence", p);

            return NULL;
        }

        break;
    }

    if (njs_slow_path(p == ctx->end)) {
        njs_json_parse_exception(ctx, "Unexpected end of input", p);
        return NULL;
    }

    /* Points to the ending quote mark. */
    last = p;

    size = last - start - surplus;

    if (surplus != 0) {
        p = start;

        dst = njs_mp_alloc(ctx->pool, size);
        if (njs_slow_path(dst == NULL)) {
            njs_memory_error(ctx->vm);;
            return NULL;
        }

        s = dst;

        do {
            ch = *p++;

            if (ch != '\\') {
                *s++ = ch;
                continue;
            }

            ch = *p++;

            switch (ch) {
            case '"':
            case '\\':
            case '/':
                *s++ = ch;
                continue;

            case 'n':
                *s++ = '\n';
                continue;

            case 'r':
                *s++ = '\r';
                continue;

            case 't':
                *s++ = '\t';
                continue;

            case 'b':
                *s++ = '\b';
                continue;

            case 'f':
                *s++ = '\f';
                continue;
            }

            /* "\uXXXX": Unicode escape sequence. */

            utf = njs_json_unicode(p);
            p += 4;

            if (utf >= 0xd800 && utf <= 0xdfff) {

                /* Surrogate pair. */

                if (utf > 0xdbff || p[0] != '\\' || p[1] != 'u') {
                    s = njs_utf8_encode(s, NJS_UTF8_REPLACEMENT);
                    continue;
                }

                p += 2;

                utf_low = njs_json_unicode(p);
                p += 4;

                if (njs_fast_path(utf_low >= 0xdc00 && utf_low <= 0xdfff)) {
                    utf = njs_string_surrogate_pair(utf, utf_low);

                } else if (utf_low >= 0xd800 && utf_low <= 0xdbff) {
                    utf = NJS_UTF8_REPLACEMENT;
                    s = njs_utf8_encode(s, NJS_UTF8_REPLACEMENT);

                } else {
                    utf = utf_low;
                    s = njs_utf8_encode(s, NJS_UTF8_REPLACEMENT);
                }
            }

            s = njs_utf8_encode(s, utf);

        } while (p != last);

        size = s - dst;
        start = dst;
    }

    length = njs_utf8_length(start, size);
    if (njs_slow_path(length < 0)) {
        length = 0;
    }

    ret = njs_string_new(ctx->vm, value, (u_char *) start, size, length);
    if (njs_slow_path(ret != NJS_OK)) {
        return NULL;
    }

    if (dst != NULL) {
        njs_mp_free(ctx->pool, dst);
    }

    return last + 1;
}


static const u_char *
njs_json_parse_number(njs_json_parse_ctx_t *ctx, njs_value_t *value,
    const u_char *p)
{
    double        num;
    njs_int_t     sign;
    const u_char  *start;

    sign = 1;

    if (*p == '-') {
        if (p + 1 == ctx->end) {
            goto error;
        }

        p++;
        sign = -1;
    }

    start = p;
    num = njs_number_dec_parse(&p, ctx->end);
    if (p != start) {
        njs_set_number(value, sign * num);
        return p;
    }

error:

    njs_json_parse_exception(ctx, "Unexpected number", p);

    return NULL;
}


njs_inline uint32_t
njs_json_unicode(const u_char *p)
{
    u_char      c;
    uint32_t    utf;
    njs_uint_t  i;

    utf = 0;

    for (i = 0; i < 4; i++) {
        utf <<= 4;
        c = p[i] | 0x20;
        c -= '0';
        if (c > 9) {
            c += '0' - 'a' + 10;
        }

        utf |= c;
    }

    return utf;
}


static const u_char *
njs_json_skip_space(const u_char *start, const u_char *end)
{
    const u_char  *p;

    for (p = start; njs_fast_path(p != end); p++) {

        switch (*p) {
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            continue;
        }

        break;
    }

    return p;
}


static njs_json_state_t *
njs_json_push_parse_state(njs_vm_t *vm, njs_json_parse_t *parse,
    njs_value_t *value)
{
    njs_json_state_t  *state;

    if (njs_slow_path(parse->depth >= NJS_JSON_MAX_DEPTH)) {
        njs_type_error(vm, "Nested too deep or a cyclic structure");
        return NULL;
    }

    state = &parse->states[parse->depth++];
    state->value = *value;
    state->index = 0;

    if (njs_is_array(value)) {
        state->type = NJS_JSON_ARRAY;

    } else {
        state->type = NJS_JSON_OBJECT;
        state->prop = NULL;
        state->keys = njs_value_own_enumerate(vm, value, NJS_ENUM_KEYS, 0);
        if (state->keys == NULL) {
            return NULL;
        }
    }

    return state;
}


njs_inline njs_json_state_t *
njs_json_pop_parse_state(njs_json_parse_t *parse)
{
    if (parse->depth > 1) {
        parse->depth--;
        return &parse->states[parse->depth - 1];
    }

    return NULL;
}


#define njs_json_is_non_empty(_value)                                         \
    ((njs_is_object(_value) && !njs_object_hash_is_empty(_value))             \
     || (njs_is_array(_value) && njs_array_len(_value) != 0))


static njs_int_t
njs_json_parse_iterator(njs_vm_t *vm, njs_json_parse_t *parse,
    njs_value_t *object)
{
    njs_int_t           ret;
    njs_value_t         *key, *value, wrapper;
    njs_object_t        *obj;
    njs_json_state_t    *state;
    njs_object_prop_t   *prop;
    njs_lvlhsh_query_t  lhq;

    obj = njs_json_wrap_value(vm, &wrapper, object);
    if (njs_slow_path(obj == NULL)) {
        goto memory_error;
    }

    state = njs_json_push_parse_state(vm, parse, &wrapper);
    if (njs_slow_path(state == NULL)) {
        goto memory_error;
    }

    lhq.proto = &njs_object_hash_proto;

    for ( ;; ) {

        switch (state->type) {
        case NJS_JSON_OBJECT:
            if (state->index < state->keys->length) {
                key = &state->keys->start[state->index];
                njs_string_get(key, &lhq.key);
                lhq.key_hash = njs_djb_hash(lhq.key.start, lhq.key.length);

                ret = njs_lvlhsh_find(njs_object_hash(&state->value), &lhq);
                if (njs_slow_path(ret == NJS_DECLINED)) {
                    state->index++;
                    break;
                }

                prop = lhq.value;

                if (prop->type == NJS_WHITEOUT) {
                    state->index++;
                    break;
                }

                state->prop = prop;

                if (njs_json_is_non_empty(&prop->value)) {
                    state = njs_json_push_parse_state(vm, parse, &prop->value);
                    if (state == NULL) {
                        goto memory_error;
                    }

                    break;
                }

            } else {
                state = njs_json_pop_parse_state(parse);
                if (state == NULL) {
                    vm->retval = parse->retval;
                    return NJS_OK;
                }
            }

            ret = njs_json_parse_iterator_call(vm, parse, state);
            if (njs_slow_path(ret != NJS_OK)) {
                return ret;
            }

            break;

        case NJS_JSON_ARRAY:
            if (state->index < njs_array_len(&state->value)) {
                value = &njs_array_start(&state->value)[state->index];

                if (njs_json_is_non_empty(value)) {
                    state = njs_json_push_parse_state(vm, parse, value);
                    if (state == NULL) {
                        goto memory_error;
                    }

                    break;
                }

            } else {
                state = njs_json_pop_parse_state(parse);
            }

            ret = njs_json_parse_iterator_call(vm, parse, state);
            if (njs_slow_path(ret != NJS_OK)) {
                return ret;
            }

            break;
        }
    }

memory_error:

    njs_memory_error(vm);

    return NJS_ERROR;
}


static njs_int_t
njs_json_parse_iterator_call(njs_vm_t *vm, njs_json_parse_t *parse,
    njs_json_state_t *state)
{
    njs_int_t    ret;
    njs_value_t  arguments[3], *value;

    arguments[0] = state->value;

    switch (state->type) {
    case NJS_JSON_OBJECT:
        arguments[1] = state->keys->start[state->index++];
        arguments[2] = state->prop->value;

        ret = njs_function_apply(vm, parse->function, arguments, 3,
                                 &parse->retval);
        if (njs_slow_path(ret != NJS_OK)) {
            return ret;
        }

        if (njs_is_undefined(&parse->retval)) {
            state->prop->type = NJS_WHITEOUT;

        } else {
            state->prop->value = parse->retval;
        }

        break;

    case NJS_JSON_ARRAY:
        njs_uint32_to_string(&arguments[1], state->index);
        value = &njs_array_start(&state->value)[state->index++];
        arguments[2] = *value;

        ret = njs_function_apply(vm, parse->function, arguments, 3,
                                 &parse->retval);
        if (njs_slow_path(ret != NJS_OK)) {
            return ret;
        }

        *value = parse->retval;

        break;
    }

    return NJS_OK;
}


static void
njs_json_parse_exception(njs_json_parse_ctx_t *ctx, const char *msg,
    const u_char *pos)
{
    ssize_t  length;

    length = njs_utf8_length(ctx->start, pos - ctx->start);
    if (njs_slow_path(length < 0)) {
        length = 0;
    }

    njs_syntax_error(ctx->vm, "%s at position %z", msg, length);
}


static njs_json_state_t *
njs_json_push_stringify_state(njs_vm_t *vm, njs_json_stringify_t *stringify,
    const njs_value_t *value)
{
    njs_json_state_t  *state;

    if (njs_slow_path(stringify->depth >= NJS_JSON_MAX_DEPTH)) {
        njs_type_error(vm, "Nested too deep or a cyclic structure");
        return NULL;
    }

    state = &stringify->states[stringify->depth++];
    state->value = *value;
    state->index = 0;
    state->written = 0;

    if (njs_is_array(value)) {
        state->type = NJS_JSON_ARRAY;

    } else {
        state->type = NJS_JSON_OBJECT;

        if (njs_is_array(&stringify->replacer)) {
            state->keys = njs_array(&stringify->replacer);

        } else {
            if (njs_is_external(value)) {
                state->keys = njs_extern_keys_array(vm, value->external.proto);

            } else {
                state->keys = njs_value_own_enumerate(vm, value, NJS_ENUM_KEYS,
                                                      0);
            }

            if (njs_slow_path(state->keys == NULL)) {
                return NULL;
            }
        }
    }

    return state;
}


njs_inline njs_json_state_t *
njs_json_pop_stringify_state(njs_json_stringify_t *stringify)
{
    njs_json_state_t  *state;

    if (stringify->depth > 1) {
        stringify->depth--;
        state = &stringify->states[stringify->depth - 1];
        state->written = 1;
        return state;
    }

    return NULL;
}


#define njs_json_is_object(value)                                             \
    (((value)->type == NJS_OBJECT)                                            \
     || ((value)->type == NJS_ARRAY)                                          \
     || ((value)->type >= NJS_REGEXP))


#define njs_json_stringify_append(str, len)                                   \
    ret = njs_json_buf_append(stringify, str, len);                           \
    if (ret != NJS_OK) {                                                      \
        goto memory_error;                                                    \
    }


#define njs_json_stringify_indent(times)                                      \
    if (stringify->space.length != 0) {                                       \
        njs_json_stringify_append("\n", 1);                                   \
        for (i = 0; i < (njs_int_t) (times) - 1; i++) {                       \
            njs_json_stringify_append((char *) stringify->space.start,        \
                                      stringify->space.length);               \
        }                                                                     \
    }

#define njs_json_stringify_append_value(value)                                \
    ret = njs_json_append_value(stringify, value);                            \
    if (njs_slow_path(ret != NJS_OK)) {                                       \
        if (ret == NJS_DECLINED) {                                            \
            return NJS_ERROR;                                                 \
        }                                                                     \
                                                                              \
        goto memory_error;                                                    \
    }


static njs_int_t
njs_json_stringify_iterator(njs_vm_t *vm, njs_json_stringify_t *stringify,
    njs_value_t *object)
{
    u_char            *start;
    size_t            size;
    ssize_t           length;
    njs_int_t         i;
    njs_int_t         ret;
    njs_str_t         str;
    njs_value_t       *key, *value, wrapper;
    njs_object_t      *obj;
    njs_json_state_t  *state;

    obj = njs_json_wrap_value(vm, &wrapper, object);
    if (njs_slow_path(obj == NULL)) {
        goto memory_error;
    }

    state = njs_json_push_stringify_state(vm, stringify, &wrapper);
    if (njs_slow_path(state == NULL)) {
        goto memory_error;
    }

    for ( ;; ) {
        switch (state->type) {
        case NJS_JSON_OBJECT:
            if (state->index == 0) {
                njs_json_stringify_append("{", 1);
                njs_json_stringify_indent(stringify->depth);
            }

            if (state->index >= state->keys->length) {
                njs_json_stringify_indent(stringify->depth - 1);
                njs_json_stringify_append("}", 1);

                state = njs_json_pop_stringify_state(stringify);
                if (state == NULL) {
                    goto done;
                }

                break;
            }

            value = &stringify->retval;
            key = &state->keys->start[state->index++];
            ret = njs_value_property(vm, &state->value, key, value);
            if (njs_slow_path(ret == NJS_ERROR)) {
                return ret;
            }

            if (njs_is_undefined(value)
                || njs_is_function(value)
                || !njs_is_valid(value))
            {
                break;
            }

            ret = njs_json_stringify_to_json(stringify, state, key, value);
            if (njs_slow_path(ret != NJS_OK)) {
                return ret;
            }

            ret = njs_json_stringify_replacer(stringify, state, key, value);
            if (njs_slow_path(ret != NJS_OK)) {
                return ret;
            }

            if (njs_is_undefined(value)) {
                break;
            }

            if (state->written) {
                njs_json_stringify_append(",", 1);
                njs_json_stringify_indent(stringify->depth);
            }

            state->written = 1;
            njs_json_append_string(stringify, key, '\"');
            njs_json_stringify_append(":", 1);
            if (stringify->space.length != 0) {
                njs_json_stringify_append(" ", 1);
            }

            if (njs_json_is_object(value)) {
                state = njs_json_push_stringify_state(vm, stringify, value);
                if (njs_slow_path(state == NULL)) {
                    return NJS_ERROR;
                }

                break;
            }

            njs_json_stringify_append_value(value);

            break;

        case NJS_JSON_ARRAY:
            if (state->index == 0) {
                njs_json_stringify_append("[", 1);
                njs_json_stringify_indent(stringify->depth);
            }

            if (state->index >= njs_array_len(&state->value)) {
                njs_json_stringify_indent(stringify->depth - 1);
                njs_json_stringify_append("]", 1);

                state = njs_json_pop_stringify_state(stringify);
                if (state == NULL) {
                    goto done;
                }

                break;
            }

            if (state->written) {
                njs_json_stringify_append(",", 1);
                njs_json_stringify_indent(stringify->depth);
            }

            stringify->retval = njs_array_start(&state->value)[state->index++];
            value = &stringify->retval;

            ret = njs_json_stringify_to_json(stringify, state, NULL, value);
            if (njs_slow_path(ret != NJS_OK)) {
                return ret;
            }

            ret = njs_json_stringify_replacer(stringify, state, NULL, value);
            if (njs_slow_path(ret != NJS_OK)) {
                return ret;
            }

            if (njs_json_is_object(value)) {
                state = njs_json_push_stringify_state(vm, stringify, value);
                if (state == NULL) {
                    return NJS_ERROR;
                }

                break;
            }

            state->written = 1;
            njs_json_stringify_append_value(value);

            break;
        }
    }

done:

    ret = njs_json_buf_pullup(stringify, &str);
    if (njs_slow_path(ret != NJS_OK)) {
        goto memory_error;
    }

    /*
     * The value to stringify is wrapped as '{"": value}'.
     * An empty object means empty result.
     */
    if (str.length <= njs_length("{\n\n}")) {
        njs_set_undefined(&vm->retval);
        goto release;
    }

    /* Stripping the wrapper's data. */

    start = str.start + njs_length("{\"\":");
    size = str.length - njs_length("{\"\":}");

    if (stringify->space.length != 0) {
        start += njs_length("\n ");
        size -= njs_length("\n \n");
    }

    length = njs_utf8_length(start, size);
    if (njs_slow_path(length < 0)) {
        length = 0;
    }

    ret = njs_string_new(vm, &vm->retval, start, size, length);
    if (njs_slow_path(ret != NJS_OK)) {
        goto memory_error;
    }

release:

    njs_mp_free(vm->mem_pool, str.start);

    return NJS_OK;

memory_error:

    njs_memory_error(vm);

    return NJS_ERROR;
}


static njs_function_t *
njs_object_to_json_function(njs_vm_t *vm, njs_value_t *value)
{
    njs_int_t           ret;
    njs_value_t         retval;
    njs_lvlhsh_query_t  lhq;

    njs_object_property_init(&lhq, "toJSON", NJS_TO_JSON_HASH);

    ret = njs_object_property(vm, value, &lhq, &retval);

    if (njs_slow_path(ret == NJS_ERROR)) {
        return NULL;
    }

    return njs_is_function(&retval) ? njs_function(&retval) : NULL;
}


static njs_int_t
njs_json_stringify_to_json(njs_json_stringify_t* stringify,
    njs_json_state_t *state, njs_value_t *key, njs_value_t *value)
{
    njs_value_t     arguments[2];
    njs_function_t  *to_json;

    if (!njs_is_object(value)) {
        return NJS_OK;
    }

    to_json = njs_object_to_json_function(stringify->vm, value);

    if (to_json == NULL) {
        return NJS_OK;
    }

    arguments[0] = *value;

    switch (state->type) {
    case NJS_JSON_OBJECT:
        if (key != NULL) {
            arguments[1] = *key;

        } else {
            njs_string_short_set(&arguments[1], 0, 0);
        }

        break;

    case NJS_JSON_ARRAY:
        njs_uint32_to_string(&arguments[1], state->index - 1);

        break;
    }

    return njs_function_apply(stringify->vm, to_json, arguments, 2,
                              &stringify->retval);
}


static njs_int_t
njs_json_stringify_replacer(njs_json_stringify_t* stringify,
    njs_json_state_t *state, njs_value_t *key, njs_value_t *value)
{
    njs_value_t  arguments[3];

    if (!njs_is_function(&stringify->replacer)) {
        return NJS_OK;
    }

    arguments[0] = state->value;

    switch (state->type) {
    case NJS_JSON_OBJECT:
        arguments[1] = *key;
        arguments[2] = *value;

        break;

    case NJS_JSON_ARRAY:
        njs_uint32_to_string(&arguments[1], state->index - 1);
        arguments[2] = *value;

        break;
    }

    return njs_function_apply(stringify->vm, njs_function(&stringify->replacer),
                              arguments, 3, &stringify->retval);
}


static njs_int_t
njs_json_stringify_array(njs_vm_t *vm, njs_json_stringify_t  *stringify)
{
    njs_int_t    ret;
    uint32_t     i, n, k, properties_length, array_length;
    njs_value_t  *value, num_value;
    njs_array_t  *properties, *array;

    properties_length = 1;
    array = njs_array(&stringify->replacer);
    array_length = array->length;

    for (i = 0; i < array_length; i++) {
        if (njs_is_valid(&array->start[i])) {
            properties_length++;
        }
    }

    properties = njs_array_alloc(vm, properties_length, NJS_ARRAY_SPARE);
    if (njs_slow_path(properties == NULL)) {
        return NJS_ERROR;
    }

    n = 0;
    properties->start[n++] = njs_string_empty;

    for (i = 0; i < array_length; i++) {
        value = &array->start[i];

        if (!njs_is_valid(&array->start[i])) {
            continue;
        }

        switch (value->type) {
        case NJS_OBJECT_NUMBER:
            value = njs_object_value(value);
            /* Fall through. */

        case NJS_NUMBER:
            ret = njs_number_to_string(vm, &num_value, value);
            if (njs_slow_path(ret != NJS_OK)) {
                return NJS_ERROR;
            }

            value = &num_value;
            break;

        case NJS_OBJECT_STRING:
            value = njs_object_value(value);
            break;

        case NJS_STRING:
            break;

        default:
            continue;
        }

        for (k = 0; k < n; k ++) {
            if (njs_values_strict_equal(value, &properties->start[k]) == 1) {
                break;
            }
        }

        if (k == n) {
            properties->start[n++] = *value;
        }
    }

    properties->length = n;
    stringify->replacer.data.u.array = properties;

    return NJS_OK;
}


static njs_int_t
njs_json_append_value(njs_json_stringify_t *stringify, const njs_value_t *value)
{
    switch (value->type) {
    case NJS_OBJECT_STRING:
        value = njs_object_value(value);
        /* Fall through. */

    case NJS_STRING:
        return njs_json_append_string(stringify, value, '\"');

    case NJS_OBJECT_NUMBER:
        value = njs_object_value(value);
        /* Fall through. */

    case NJS_NUMBER:
        return njs_json_append_number(stringify, value);

    case NJS_OBJECT_BOOLEAN:
        value = njs_object_value(value);
        /* Fall through. */

    case NJS_BOOLEAN:
        if (njs_is_true(value)) {
            return njs_json_buf_append(stringify, "true", 4);

        } else {
            return njs_json_buf_append(stringify, "false", 5);
        }

    case NJS_UNDEFINED:
    case NJS_NULL:
    case NJS_INVALID:
    case NJS_FUNCTION:
    default:
        return njs_json_buf_append(stringify, "null", 4);
    }
}


static njs_int_t
njs_json_append_string(njs_json_stringify_t *stringify,
    const njs_value_t *value, char quote)
{
    u_char             c, *dst, *dst_end;
    size_t             length;
    const u_char       *p, *end;
    njs_string_prop_t  str;

    static char   hex2char[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
                                   '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };

    (void) njs_string_prop(&str, value);

    p = str.start;
    end = p + str.size;
    length = str.length;

    dst = njs_json_buf_reserve(stringify, 64);
    if (njs_slow_path(dst == NULL)) {
        return NJS_ERROR;
    }

    dst_end = dst + 64;

    *dst++ = quote;

    while (p < end) {

        if (*p < ' '
            || *p == '\\'
            || (*p == '\"' && quote == '\"'))
        {
            c = (u_char) *p++;
            *dst++ = '\\';

            switch (c) {
            case '\\':
                *dst++ = '\\';
                break;
            case '"':
                *dst++ = '\"';
                break;
            case '\r':
                *dst++ = 'r';
                break;
            case '\n':
                *dst++ = 'n';
                break;
            case '\t':
                *dst++ = 't';
                break;
            case '\b':
                *dst++ = 'b';
                break;
            case '\f':
                *dst++ = 'f';
                break;
            default:
                *dst++ = 'u';
                *dst++ = '0';
                *dst++ = '0';
                *dst++ = hex2char[(c & 0xf0) >> 4];
                *dst++ = hex2char[c & 0x0f];
            }
        }

        /*
         * Control characters less than space are encoded using 6 bytes
         * "\uXXXX".  Checking there is at least 6 bytes of destination storage
         * space.
         */

        while (p < end && (dst_end - dst) > 6) {
            if (*p < ' ' || (*p == '\"' && quote == '\"') || *p == '\\') {
                break;
            }

            if (length != 0) {
                /* UTF-8 or ASCII string. */
                dst = njs_utf8_copy(dst, &p, end);

            } else {
                /* Byte string. */
                *dst++ = *p++;
            }
        }

        if (dst_end - dst <= 6) {
            njs_json_buf_written(stringify, dst - stringify->last->pos);

            dst = njs_json_buf_reserve(stringify, 64);
            if (njs_slow_path(dst == NULL)) {
                return NJS_ERROR;
            }

            dst_end = dst + 64;
        }
    }

    njs_json_buf_written(stringify, dst - stringify->last->pos);

    njs_json_buf_append(stringify, &quote, 1);

    return NJS_OK;
}


static njs_int_t
njs_json_append_number(njs_json_stringify_t *stringify,
    const njs_value_t *value)
{
    u_char  *p;
    size_t  size;
    double  num;

    num = njs_number(value);

    if (isnan(num) || isinf(num)) {
        return njs_json_buf_append(stringify, "null", 4);

    } else {
        p = njs_json_buf_reserve(stringify, 64);
        if (njs_slow_path(p == NULL)) {
            return NJS_ERROR;
        }

        size = njs_dtoa(num, (char *) p);

        njs_json_buf_written(stringify, size);
    }

    return NJS_OK;
}


/*
 * Wraps a value as '{"": <value>}'.
 */
static njs_object_t *
njs_json_wrap_value(njs_vm_t *vm, njs_value_t *wrapper,
    const njs_value_t *value)
{
    njs_int_t           ret;
    njs_object_prop_t   *prop;
    njs_lvlhsh_query_t  lhq;

    wrapper->data.u.object = njs_object_alloc(vm);
    if (njs_slow_path(njs_object(wrapper) == NULL)) {
        return NULL;
    }

    wrapper->type = NJS_OBJECT;
    wrapper->data.truth = 1;

    lhq.replace = 0;
    lhq.proto = &njs_object_hash_proto;
    lhq.pool = vm->mem_pool;
    lhq.key = njs_str_value("");
    lhq.key_hash = NJS_DJB_HASH_INIT;

    prop = njs_object_prop_alloc(vm, &njs_string_empty, value, 1);
    if (njs_slow_path(prop == NULL)) {
        return NULL;
    }

    lhq.value = prop;

    ret = njs_lvlhsh_insert(njs_object_hash(wrapper), &lhq);
    if (njs_slow_path(ret != NJS_OK)) {
        return NULL;
    }

    return wrapper->data.u.object;
}


static njs_int_t
njs_json_buf_append(njs_json_stringify_t *stringify, const char *msg,
    size_t len)
{
    u_char  *p;

    if (len != 0) {
        p = njs_json_buf_reserve(stringify, len);
        if (njs_slow_path(p == NULL)) {
            return NJS_ERROR;
        }

        memcpy(p, msg, len);

        njs_json_buf_written(stringify, len);
    }

    return NJS_OK;
}


static u_char *
njs_json_buf_reserve(njs_json_stringify_t *stringify, size_t size)
{
    njs_chb_node_t  *n;

    if (njs_slow_path(size == 0)) {
        return NULL;
    }

    n = stringify->last;

    if (njs_fast_path(n != NULL && njs_json_buf_node_room(n) >= size)) {
        return n->pos;
    }

    if (size < NJS_JSON_BUF_MIN_SIZE) {
        size = NJS_JSON_BUF_MIN_SIZE;
    }

    n = njs_mp_alloc(stringify->pool, sizeof(njs_chb_node_t) + size);
    if (njs_slow_path(n == NULL)) {
        return NULL;
    }

    n->next = NULL;
    n->start = (u_char *) n + sizeof(njs_chb_node_t);
    n->pos = n->start;
    n->end = n->pos + size;

    if (stringify->last != NULL) {
        stringify->last->next = n;

    } else {
        stringify->nodes = n;
    }

    stringify->last = n;

    return n->start;
}


static njs_int_t
njs_json_buf_pullup(njs_json_stringify_t *stringify, njs_str_t *str)
{
    u_char          *start;
    size_t          size;
    njs_chb_node_t  *n;

    n = stringify->nodes;

    if (n == NULL) {
        str->length = 0;
        str->start = NULL;
        return NJS_OK;
    }

    if (n->next == NULL) {
        str->length = njs_json_buf_node_size(n);
        str->start = n->start;
        return NJS_OK;
    }

    size = 0;

    while (n != NULL) {
        size += njs_json_buf_node_size(n);
        n = n->next;
    }

    start = njs_mp_alloc(stringify->pool, size);
    if (njs_slow_path(start == NULL)) {
        return NJS_ERROR;
    }

    n = stringify->nodes;
    str->length = size;
    str->start = start;

    while (n != NULL) {
        size = njs_json_buf_node_size(n);
        memcpy(start, n->start, size);
        start += size;
        n = n->next;
    }

    return NJS_OK;
}


static const njs_object_prop_t  *njs_json_object_properties;
#if !defined(_MSC_VER)
 = 
{
    /* JSON.parse(). */
    {
        .type = NJS_PROPERTY,
        .name = njs_string("parse"),
        .value = njs_native_function(njs_json_parse, 2),
        .writable = 1,
        .configurable = 1,
    },

    /* JSON.stringify(). */
    {
        .type = NJS_PROPERTY,
        .name = njs_string("stringify"),
        .value = njs_native_function(njs_json_stringify, 3),
        .writable = 1,
        .configurable = 1,
    },
};
#endif


const njs_object_init_t  njs_json_object_init;
#if !defined(_MSC_VER)
 = {
    njs_json_object_properties,
    njs_nitems(njs_json_object_properties),
};
#endif

#define njs_dump(str)                                                         \
    ret = njs_json_buf_append(stringify, str, njs_length(str));               \
    if (njs_slow_path(ret != NJS_OK)) {                                       \
        goto memory_error;                                                    \
    }


#define njs_dump_item(str)                                                    \
    if (written) {                                                            \
        njs_json_buf_append(stringify, ",", 1);                               \
    }                                                                         \
                                                                              \
    written = 1;                                                              \
    ret = njs_json_buf_append(stringify, str, njs_length(str));               \
    if (njs_slow_path(ret != NJS_OK)) {                                       \
        goto memory_error;                                                    \
    }


static njs_int_t
njs_dump_value(njs_json_stringify_t *stringify, const njs_value_t *value,
    njs_uint_t console)
{
    njs_int_t           ret;
    njs_str_t           str;
    njs_uint_t          written;
    njs_value_t         str_val;
    const njs_extern_t  *ext_proto;
    u_char              buf[32], *p;

    njs_int_t           (*to_string)(njs_vm_t *, njs_value_t *,
                                     const njs_value_t *);

    switch (value->type) {
    case NJS_OBJECT_STRING:
        value = njs_object_value(value);

        njs_string_get(value, &str);

        njs_dump("[String: ");
        njs_json_append_string(stringify, value, '\'');
        njs_dump("]")
        break;

    case NJS_STRING:
        njs_string_get(value, &str);

        if (!console || stringify->depth != 0) {
            return njs_json_append_string(stringify, value, '\'');
        }

        return njs_json_buf_append(stringify, (char *) str.start, str.length);

        break;

    case NJS_OBJECT_NUMBER:
        value = njs_object_value(value);

        if (njs_slow_path(njs_number(value) == 0.0
                          && signbit(njs_number(value))))
        {

            njs_dump("[Number: -0]");
            break;
        }

        ret = njs_number_to_string(stringify->vm, &str_val, value);
        if (njs_slow_path(ret != NJS_OK)) {
            return NJS_ERROR;
        }

        njs_string_get(&str_val, &str);

        njs_dump("[Number: ");
        njs_json_buf_append(stringify, (char *) str.start, str.length);
        njs_dump("]")
        break;

    case NJS_OBJECT_BOOLEAN:
        value = njs_object_value(value);

        if (njs_is_true(value)) {
            njs_dump("[Boolean: true]");

        } else {
            njs_dump("[Boolean: false]");
        }

        break;

    case NJS_BOOLEAN:
        if (njs_is_true(value)) {
            njs_dump("true");

        } else {
            njs_dump("false");
        }

        break;

    case NJS_UNDEFINED:
        njs_dump("undefined");
        break;

    case NJS_NULL:
        njs_dump("null");
        break;

    case NJS_INVALID:
        njs_dump("<empty>");
        break;

    case NJS_FUNCTION:
        if (njs_function(value)->native) {
            njs_dump("[Function: native]");

        } else {
            njs_dump("[Function]");
        }

        break;

    case NJS_EXTERNAL:
        ext_proto = value->external.proto;

        written = 0;
        njs_dump_item("{type:");

        switch (ext_proto->type) {
        case NJS_EXTERN_PROPERTY:
            njs_dump("\"property\"");
            break;
        case NJS_EXTERN_METHOD:
            njs_dump("\"method\"");
            break;
        case NJS_EXTERN_OBJECT:
            njs_dump("\"object\"");
            break;
        case NJS_EXTERN_CASELESS_OBJECT:
            njs_dump("\"caseless_object\"");
            break;
        }

        njs_dump_item("props:[");
        written = 0;

        if (ext_proto->get != NULL) {
            njs_dump_item("\"getter\"");
        }

        if (ext_proto->set != NULL) {
            njs_dump_item("\"setter\"");
        }

        if (ext_proto->function != NULL) {
            njs_dump_item("\"method\"");
        }

        if (ext_proto->find != NULL) {
            njs_dump_item("\"find\"");
        }

        if (ext_proto->keys != NULL) {
            njs_dump_item("\"keys\"");
        }

        return njs_json_buf_append(stringify, "]}", 2);

    case NJS_NUMBER:
        if (njs_slow_path(njs_number(value) == 0.0
                          && signbit(njs_number(value))))
        {

            njs_dump("-0");
            break;
        }

        /* Fall through. */

    case NJS_OBJECT:
    case NJS_REGEXP:
    case NJS_DATE:

        switch (value->type) {
        case NJS_NUMBER:
            to_string = njs_number_to_string;
            break;

        case NJS_REGEXP:
            to_string = njs_regexp_to_string;
            break;

        case NJS_DATE:
            to_string = njs_date_to_string;
            break;

        default:
            to_string = njs_error_to_string;
        }

        ret = to_string(stringify->vm, &str_val, value);
        if (njs_slow_path(ret != NJS_OK)) {
            return NJS_ERROR;
        }

        njs_string_get(&str_val, &str);

        return njs_json_buf_append(stringify, (char *) str.start, str.length);

    default:
        p = njs_sprintf(buf, buf + njs_length(buf), "[Unknown value type:%uD]",
                        value->type);
        return njs_json_buf_append(stringify, (char *) buf, p - buf);
    }

    return ret;

memory_error:

    njs_memory_error(stringify->vm);

    return NJS_ERROR;
}


#define njs_dump_is_object(value)                                             \
    (((value)->type == NJS_OBJECT && !njs_object(value)->error_data)          \
     || ((value)->type == NJS_ARRAY)                                          \
     || ((value)->type == NJS_OBJECT_VALUE)                                   \
     || ((value)->type == NJS_EXTERNAL                                        \
         && !njs_lvlhsh_is_empty(&(value)->external.proto->hash)))


#define njs_dump_append_value(value)                                          \
    ret = njs_dump_value(stringify, value, console);                          \
    if (njs_slow_path(ret != NJS_OK)) {                                       \
        if (ret == NJS_DECLINED) {                                            \
            goto exception;                                                   \
        }                                                                     \
                                                                              \
        goto memory_error;                                                    \
    }


njs_int_t
njs_vm_value_dump(njs_vm_t *vm, njs_str_t *retval, const njs_value_t *value,
    njs_uint_t console, njs_uint_t indent)
{
    njs_int_t             i;
    njs_int_t             ret;
    njs_str_t             str;
    njs_value_t           *key, *val, ext_val;
    njs_object_t          *object;
    njs_json_state_t      *state;
    njs_object_prop_t     *prop;
    njs_lvlhsh_query_t    lhq;
    njs_json_stringify_t  *stringify, dump_stringify;

    const njs_value_t  string_get = njs_string("[Getter]");
    const njs_value_t  string_set = njs_string("[Setter]");
    const njs_value_t  string_get_set = njs_long_string("[Getter/Setter]");

    if (njs_vm_backtrace(vm) != NULL) {
        goto exception;
    }

    stringify = &dump_stringify;

    stringify->vm = vm;
    stringify->pool = vm->mem_pool;
    stringify->depth = 0;
    stringify->nodes = NULL;
    stringify->last = NULL;
    njs_set_undefined(&stringify->replacer);

    if (!njs_dump_is_object(value)) {
        ret = njs_dump_value(stringify, value, console);
        if (njs_slow_path(ret != NJS_OK)) {
            goto memory_error;
        }

        goto done;
    }

    indent = njs_min(indent, 10);
    stringify->space.length = indent;
    stringify->space.start = stringify->space_buf;

    njs_memset(stringify->space.start, ' ', indent);

    state = njs_json_push_stringify_state(vm, stringify, value);
    if (njs_slow_path(state == NULL)) {
        goto memory_error;
    }

    for ( ;; ) {
        switch (state->type) {
        case NJS_JSON_OBJECT:
            if (state->index == 0) {
                njs_json_stringify_append("{", 1);
                njs_json_stringify_indent(stringify->depth + 1);
            }

            if (state->index >= state->keys->length) {
                njs_json_stringify_indent(stringify->depth);
                njs_json_stringify_append("}", 1);

                state = njs_json_pop_stringify_state(stringify);
                if (state == NULL) {
                    goto done;
                }

                break;
            }

            key = &state->keys->start[state->index++];
            njs_string_get(key, &lhq.key);
            lhq.key_hash = njs_djb_hash(lhq.key.start, lhq.key.length);

            if (njs_is_external(&state->value)) {
                lhq.proto = &njs_extern_hash_proto;

                ret = njs_lvlhsh_find(&state->value.external.proto->hash, &lhq);
                if (njs_slow_path(ret == NJS_DECLINED)) {
                    break;
                }

                ext_val.type = NJS_EXTERNAL;
                ext_val.data.truth = 1;
                ext_val.external.proto = lhq.value;

                val = &ext_val;

            } else {
                object = njs_object(&state->value);
                lhq.proto = &njs_object_hash_proto;

                ret = njs_lvlhsh_find(&object->hash, &lhq);
                if (ret == NJS_DECLINED) {
                    ret = njs_lvlhsh_find(&object->shared_hash, &lhq);
                    if (njs_slow_path(ret == NJS_DECLINED)) {
                        break;
                    }
                }

                prop = lhq.value;
                val = &prop->value;

                if (prop->type == NJS_WHITEOUT || !prop->enumerable) {
                    break;
                }

                if (njs_is_accessor_descriptor(prop)) {
                    if (njs_is_defined(&prop->getter)) {
                        if (njs_is_defined(&prop->setter)) {
                            val = njs_value_arg(&string_get_set);
                        } else {
                            val = njs_value_arg(&string_get);
                        }

                    } else {
                        val = njs_value_arg(&string_set);
                    }
                }
            }

            if (state->written) {
                njs_json_stringify_append(",", 1);
                njs_json_stringify_indent(stringify->depth + 1);
            }

            state->written = 1;
            njs_json_stringify_append((char *) lhq.key.start, lhq.key.length);
            njs_json_stringify_append(":", 1);
            if (stringify->space.length != 0) {
                njs_json_stringify_append(" ", 1);
            }

            if (njs_dump_is_object(val)) {
                state = njs_json_push_stringify_state(vm, stringify, val);
                if (njs_slow_path(state == NULL)) {
                    goto exception;
                }

                break;
            }

            njs_dump_append_value(val);

            break;

        case NJS_JSON_ARRAY:
            if (state->index == 0) {
                njs_json_stringify_append("[", 1);
                njs_json_stringify_indent(stringify->depth + 1);
            }

            if (state->index >= njs_array_len(&state->value)) {
                njs_json_stringify_indent(stringify->depth);
                njs_json_stringify_append("]", 1);

                state = njs_json_pop_stringify_state(stringify);
                if (state == NULL) {
                    goto done;
                }

                break;
            }

            if (state->written) {
                njs_json_stringify_append(",", 1);
                njs_json_stringify_indent(stringify->depth + 1);
            }

            val = &njs_array_start(&state->value)[state->index++];

            if (njs_dump_is_object(val)) {
                state = njs_json_push_stringify_state(vm, stringify, val);
                if (njs_slow_path(state == NULL)) {
                    goto exception;
                }

                break;
            }

            state->written = 1;
            njs_dump_append_value(val);

            break;
        }
    }

done:

    ret = njs_json_buf_pullup(stringify, &str);
    if (njs_slow_path(ret != NJS_OK)) {
        goto memory_error;
    }

    *retval = str;

    return NJS_OK;

memory_error:

    njs_memory_error(vm);

exception:

    njs_vm_value_string(vm, retval, &vm->retval);

    return NJS_OK;
}
