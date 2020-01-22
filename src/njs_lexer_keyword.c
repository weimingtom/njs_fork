
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */


#include <njs_main.h>


static const njs_keyword_t  njs_keywords[] = {

    /* Values. */

    { njs_str("null"),          NJS_TOKEN_NULL, 0 },
    { njs_str("false"),         NJS_TOKEN_BOOLEAN, 0 },
    { njs_str("true"),          NJS_TOKEN_BOOLEAN, 1 },

    /* Operators. */

    { njs_str("in"),            NJS_TOKEN_IN, 0 },
    { njs_str("typeof"),        NJS_TOKEN_TYPEOF, 0 },
    { njs_str("instanceof"),    NJS_TOKEN_INSTANCEOF, 0 },
    { njs_str("void"),          NJS_TOKEN_VOID, 0 },
    { njs_str("new"),           NJS_TOKEN_NEW, 0 },
    { njs_str("delete"),        NJS_TOKEN_DELETE, 0 },
    { njs_str("yield"),         NJS_TOKEN_YIELD, 0 },

    /* Statements. */

    { njs_str("var"),           NJS_TOKEN_VAR, 0 },
    { njs_str("if"),            NJS_TOKEN_IF, 0 },
    { njs_str("else"),          NJS_TOKEN_ELSE, 0 },
    { njs_str("while"),         NJS_TOKEN_WHILE, 0 },
    { njs_str("do"),            NJS_TOKEN_DO, 0 },
    { njs_str("for"),           NJS_TOKEN_FOR, 0 },
    { njs_str("break"),         NJS_TOKEN_BREAK, 0 },
    { njs_str("continue"),      NJS_TOKEN_CONTINUE, 0 },
    { njs_str("switch"),        NJS_TOKEN_SWITCH, 0 },
    { njs_str("case"),          NJS_TOKEN_CASE, 0 },
    { njs_str("default"),       NJS_TOKEN_DEFAULT, 0 },
    { njs_str("function"),      NJS_TOKEN_FUNCTION, 0 },
    { njs_str("return"),        NJS_TOKEN_RETURN, 0 },
    { njs_str("with"),          NJS_TOKEN_WITH, 0 },
    { njs_str("try"),           NJS_TOKEN_TRY, 0 },
    { njs_str("catch"),         NJS_TOKEN_CATCH, 0 },
    { njs_str("finally"),       NJS_TOKEN_FINALLY, 0 },
    { njs_str("throw"),         NJS_TOKEN_THROW, 0 },

    /* Module. */

    { njs_str("import"),        NJS_TOKEN_IMPORT, 0 },
    { njs_str("export"),        NJS_TOKEN_EXPORT, 0 },

    /* Reserved words. */

    { njs_str("this"),          NJS_TOKEN_THIS, 0 },
    { njs_str("arguments"),     NJS_TOKEN_ARGUMENTS, 0 },
    { njs_str("eval"),          NJS_TOKEN_EVAL, 0 },

    { njs_str("await"),         NJS_TOKEN_RESERVED, 0 },
    { njs_str("class"),         NJS_TOKEN_RESERVED, 0 },
    { njs_str("const"),         NJS_TOKEN_RESERVED, 0 },
    { njs_str("debugger"),      NJS_TOKEN_RESERVED, 0 },
    { njs_str("enum"),          NJS_TOKEN_RESERVED, 0 },
    { njs_str("extends"),       NJS_TOKEN_RESERVED, 0 },
    { njs_str("implements"),    NJS_TOKEN_RESERVED, 0 },
    { njs_str("interface"),     NJS_TOKEN_RESERVED, 0 },
    { njs_str("let"),           NJS_TOKEN_RESERVED, 0 },
    { njs_str("package"),       NJS_TOKEN_RESERVED, 0 },
    { njs_str("private"),       NJS_TOKEN_RESERVED, 0 },
    { njs_str("protected"),     NJS_TOKEN_RESERVED, 0 },
    { njs_str("public"),        NJS_TOKEN_RESERVED, 0 },
    { njs_str("static"),        NJS_TOKEN_RESERVED, 0 },
    { njs_str("super"),         NJS_TOKEN_RESERVED, 0 },
};


static njs_int_t
njs_keyword_hash_test(njs_lvlhsh_query_t *lhq, void *data)
{
    njs_keyword_t  *keyword;

    keyword = data;

    if (njs_strstr_eq(&lhq->key, &keyword->name)) {
        return NJS_OK;
    }

    return NJS_DECLINED;
}


#if defined _MSC_VER
__declspec(align(64))
#endif
const njs_lvlhsh_proto_t  njs_keyword_hash_proto
    njs_aligned(64) =
{
    NJS_LVLHSH_DEFAULT,
    njs_keyword_hash_test,
    njs_lvlhsh_alloc,
    njs_lvlhsh_free,
};

njs_int_t
njs_lexer_keywords_init(njs_mp_t *mp, njs_lvlhsh_t *hash)
{
    njs_uint_t           n;
    njs_lvlhsh_query_t   lhq;
    const njs_keyword_t  *keyword;

    keyword = njs_keywords;
    n = njs_nitems(njs_keywords);

    lhq.replace = 0;
    lhq.proto = &njs_keyword_hash_proto;
    lhq.pool = mp;

    do {
        lhq.key_hash = njs_djb_hash(keyword->name.start, keyword->name.length);
        lhq.key = keyword->name;
        lhq.value = (void *) keyword;

        if (njs_slow_path(njs_lvlhsh_insert(hash, &lhq) != NJS_OK)) {
            return NJS_ERROR;
        }

        keyword++;
        n--;

    } while (n != 0);

    return NJS_OK;
}


void
njs_lexer_keyword(njs_lexer_t *lexer, njs_lexer_token_t *lt)
{
    njs_keyword_t       *keyword;
    njs_lvlhsh_query_t  lhq;

    lhq.key_hash = lt->key_hash;
    lhq.key = lt->text;
    lhq.proto = &njs_keyword_hash_proto;

    lexer->keyword = 0;

    if (njs_lvlhsh_find(&lexer->keywords_hash, &lhq) == NJS_OK) {
        keyword = lhq.value;
        lt->token = keyword->token;
        lt->number = keyword->number;
        lexer->keyword = 1;
    }
}
