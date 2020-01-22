njs_int_t
njs_parser_string_create(njs_vm_t *vm, njs_value_t *value)
{
    u_char        *dst;
    ssize_t       size, length;
    uint32_t      cp;
    njs_str_t     *src;
    const u_char  *p, *end;

    src = njs_parser_text(vm->parser);

    length = njs_utf8_safe_length(src->start, src->length, &size);

-->    dst = njs_string_alloc(vm, value, size, length);
    
    
console.log("hello");
create "hello", search type = NJS_STRING;

NJS_OBJECT_STRING
-->NJS_STRING

--------------------------------

//FIXME:why???
#if !defined(_MSC_VER)
#define njs_argument(args, n)                                                 \
    (njs_value_t *) ((u_char *) args + n * 16)
#else
#define njs_argument(args, n)                                                 \
	(njs_value_t *)((u_char *)args + n * 16 + 16)
#endif

--------------------------

