#include <ctype.h>

#include "liquid.h"
#include "block_body.h"
#include "tokenizer.h"

VALUE cLiquidVariable, cLiquidTemplate, cLiquidSyntaxError;
ID intern_aref,
   intern_is_blank,
   intern_line_numbers,
   intern_locale,
   intern_new,
   intern_parse,
   intern_set_line_number,
   intern_t,
   intern_tag_end,
   intern_tags,
   intern_token;
VALUE missing_tag_terminator_translation, missing_variable_terminator_translation;
VALUE variable_end, tag_end;

typedef struct block_body {
    VALUE nodelist;
    bool blank;
} block_body_t;


static void block_body_mark(void *ptr)
{
    block_body_t *block_body = ptr;
    rb_gc_mark(block_body->nodelist);
}

static void block_body_free(void *ptr)
{
    xfree(ptr);
}

static size_t block_body_memsize(const void *ptr)
{
    return ptr ? sizeof(block_body_t) : 0;
}

const rb_data_type_t block_body_data_type = {
    "liquid_block_body",
    { block_body_mark, block_body_free, block_body_memsize, },
#if defined(RUBY_TYPED_FREE_IMMEDIATELY)
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY
#endif
};

static VALUE block_body_allocate(VALUE klass)
{
    VALUE obj;
    block_body_t *block_body;

    obj = TypedData_Make_Struct(klass, block_body_t, &block_body_data_type, block_body);
    block_body->nodelist = Qnil;
    block_body->blank = true;
    return obj;
}

static VALUE block_body_initialize_method(VALUE self)
{
    block_body_t *block_body;
    TypedData_Get_Struct(self, block_body_t, &block_body_data_type, block_body);

    block_body->nodelist = rb_ary_new();
    rb_iv_set(self, "@nodelist", block_body->nodelist);
    return Qnil;
}



struct liquid_tag
{
    const char *name, *markup;
    long name_length, markup_length;
};

static bool parse_tag(struct liquid_tag *tag, const char *token, long token_length)
{
    // Strip {{ and }} braces
    token += 2;
    token_length -= 4;

    const char *end = token + token_length;
    while (token < end && isspace(*token))
        token++;
    tag->name = token;

    char c = *token;
    while (token < end && (isalnum(c) || c == '_'))
        c = *(++token);
    tag->name_length = token - tag->name;
    if (!tag->name_length) {
        memset(tag, 0, sizeof(*tag));
        return false;
    }

    while (token < end && isspace(*token))
        token++;
    tag->markup = token;

    const char *last = end - 1;
    while (token < last && isspace(*last))
        last--;
    end = last + 1;
    tag->markup_length = end - token;
    return true;
}


static int calculate_line_number(const tokenizer_t *tokenizer, const token_t *token)
{
    const char *start = RSTRING_PTR(tokenizer->source);
    const char *end = token->str;
    int count = 0;
    while (start < end) {
        start = memchr(start, '\n', (size_t)end - (size_t)start);
        if (start == NULL)
            break;
        count++;
        start++;
    }
    return count;
}

static void raise_missing_terminator(VALUE translation_name, const tokenizer_t *tokenizer, const token_t *token, VALUE tag_end, VALUE options)
{
    VALUE locale = rb_hash_aref(options, ID2SYM(intern_locale));
    VALUE translation_vars = rb_hash_new();
    rb_hash_aset(translation_vars, ID2SYM(intern_token), rb_enc_str_new(token->str, token->length, utf8_encoding));
    rb_hash_aset(translation_vars, ID2SYM(intern_tag_end), tag_end);
    VALUE message = rb_funcall(locale, intern_t, 2, translation_name, translation_vars);
    VALUE exception = rb_exc_new_str(cLiquidSyntaxError, message);

    if (RTEST(rb_hash_aref(options, ID2SYM(intern_line_numbers)))) {
        rb_funcall(exception, intern_set_line_number, 1, INT2FIX(calculate_line_number(tokenizer, token)));
    }

    rb_exc_raise(exception);
}

static void raise_missing_tag_terminator(const tokenizer_t *tokenizer, const token_t *token, VALUE options)
{
    raise_missing_terminator(missing_tag_terminator_translation, tokenizer, token, tag_end, options);
}

static void raise_missing_variable_terminator(const tokenizer_t *tokenizer, const token_t *token, VALUE options)
{
    raise_missing_terminator(missing_variable_terminator_translation, tokenizer, token, variable_end, options);
}


static VALUE block_body_parse(VALUE self, VALUE tokenizerObj, VALUE options)
{
    block_body_t *block_body;
    tokenizer_t *tokenizer;
    TypedData_Get_Struct(self, block_body_t, &block_body_data_type, block_body);
    TypedData_Get_Struct(tokenizerObj, tokenizer_t, &tokenizer_data_type, tokenizer);

    token_t token;
    VALUE tags = Qnil;
    while (true) {
        tokenizer_next(tokenizer, &token);
        switch (token.type) {
            case TOKEN_NONE:
            {
                return rb_yield_values(2, Qnil, Qnil);
            }
            case TOKEN_INVALID:
            {
                if (token.str[1] == '%') {
                    raise_missing_tag_terminator(tokenizer, &token, options);
                } else {
                    raise_missing_variable_terminator(tokenizer, &token, options);
                }
                break;
            }
            case TOKEN_TAG:
            {
                struct liquid_tag tag;
                if (!parse_tag(&tag, token.str, token.length)) {
                    raise_missing_tag_terminator(tokenizer, &token, options);
                } else {
                    if (tags == Qnil) {
                        tags = rb_funcall(cLiquidTemplate, intern_tags, 0);
                    }
                    VALUE tag_name = rb_enc_str_new(tag.name, tag.name_length, utf8_encoding);
                    VALUE tag_class = rb_funcall(tags, intern_aref, 1, tag_name);
                    VALUE markup = rb_enc_str_new(tag.markup, tag.markup_length, utf8_encoding);
                    if (tag_class != Qnil) {
                        VALUE new_tag = rb_funcall(tag_class, intern_parse, 4,
                                                   tag_name, markup, tokenizerObj, options);
                        if (block_body->blank && !RTEST(rb_funcall(new_tag, intern_is_blank, 0))) {
                            block_body->blank = false;
                        }
                        rb_ary_push(block_body->nodelist, new_tag);
                    } else {
                        return rb_yield_values(2, tag_name, markup);
                    }
                }
                break;
            }
            case TOKEN_VARIABLE:
            {
                VALUE markup = rb_enc_str_new(token.str + 2, token.length - 4, utf8_encoding);
                VALUE new_var = rb_funcall(cLiquidVariable, intern_new, 2, markup, options);
                rb_ary_push(block_body->nodelist, new_var);
                block_body->blank = false;
                break;
            }
            case TOKEN_STRING:
            {
                VALUE node = rb_enc_str_new(token.str, token.length, utf8_encoding);
                rb_ary_push(block_body->nodelist, node);
                if (block_body->blank) {
                    int i;
                    for (i = 0; i < token.length; i++) {
                        if (!isspace(token.str[i])) {
                            block_body->blank = false;
                            break;
                        }
                    }
                }
                break;
            }
        }
    }
    return Qnil;
}

static VALUE is_blank(VALUE self)
{
    const block_body_t *block_body;
    TypedData_Get_Struct(self, block_body_t, &block_body_data_type, block_body);

    return block_body->blank ? Qtrue : Qfalse;
}

void init_liquid_block_body()
{
    cLiquidVariable = rb_const_get(mLiquid, rb_intern("Variable"));
    cLiquidTemplate = rb_const_get(mLiquid, rb_intern("Template"));
    cLiquidSyntaxError = rb_const_get(mLiquid, rb_intern("SyntaxError"));

    intern_aref = rb_intern("[]");
    intern_is_blank = rb_intern("blank?");
    intern_line_numbers = rb_intern("line_numbers");
    intern_locale = rb_intern("locale");
    intern_new = rb_intern("new");
    intern_parse = rb_intern("parse");
    intern_set_line_number = rb_intern("line_number=");
    intern_t = rb_intern("t");
    intern_tag_end = rb_intern("tag_end");
    intern_tags = rb_intern("tags");
    intern_token = rb_intern("token");

    missing_tag_terminator_translation = rb_str_new_cstr("errors.syntax.tag_termination");
    missing_variable_terminator_translation = rb_str_new_cstr("errors.syntax.variable_termination");
    tag_end = rb_str_new_cstr("\"%}\"");
    variable_end = rb_str_new_cstr("\"}}\"");

    VALUE cLiquidBlockBody = rb_define_class_under(mLiquid, "BlockBody", rb_cObject);
    rb_define_alloc_func(cLiquidBlockBody, block_body_allocate);
    rb_define_method(cLiquidBlockBody, "initialize", block_body_initialize_method, 0);
    rb_define_method(cLiquidBlockBody, "parse", block_body_parse, 2);

    rb_define_method(cLiquidBlockBody, "blank?", is_blank, 0);
}
