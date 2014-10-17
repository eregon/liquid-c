#include "liquid.h"
#include "tokenizer.h"
#include "block_body.h"

VALUE mLiquid;
rb_encoding *utf8_encoding;

void Init_liquid_c(void)
{
    utf8_encoding = rb_utf8_encoding();
    mLiquid = rb_define_module("Liquid");
    init_liquid_tokenizer();
    init_liquid_block_body();
}
