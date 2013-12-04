/*
 * Xpost - a Level-2 Postscript interpreter
 * Copyright (C) 2013, Michael Joshua Ryan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the Xpost software product nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/** \file ar.c
   array functions
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>

#include "xpost_log.h"
#include "xpost_memory.h"  /* arrays live in mfile, accessed via mtab */
#include "xpost_object.h"  /* array is an object, containing objects */
#include "xpost_stack.h"  /* may count the save stack */
#include "xpost_free.h"  /* arrays are allocated from the free list */

#include "xpost_save.h"  /* arrays obey save/restore */
#include "xpost_context.h"
#include "xpost_interpreter.h"  /* banked arrays may be in global or local mfiles */
#include "xpost_error.h"  /* array functions may throw errors */
#include "xpost_array.h"  /* double-check prototypes */



/**
  Allocate an entity with xpost_memory_table_alloc,
   find the appropriate mtab,
   set the current save level in the "mark" field,
   wrap it up in an object.
*/
Xpost_Object consarr(Xpost_Memory_File *mem,
               unsigned sz)
{
    unsigned ent;
    unsigned rent;
    unsigned cnt;
    Xpost_Memory_Table *tab;
    Xpost_Object o;
    unsigned i;
    unsigned int vs;

    assert(mem->base);

    if (sz == 0) {
        ent = 0;
    } else {
        if (!xpost_memory_table_alloc(mem, (unsigned)(sz * sizeof(Xpost_Object)), arraytype, &ent ))
        {
            //error(VMerror, "consarr cannot allocate array");
            XPOST_LOG_ERR("cannot allocate array");
            return null;
        }
        tab = (void *)(mem->base);
        rent = ent;
        xpost_memory_table_find_relative(mem, &tab, &rent);
        xpost_memory_table_get_addr(mem,
                XPOST_MEMORY_TABLE_SPECIAL_SAVE_STACK, &vs);
        cnt = xpost_stack_count(mem, vs);
        tab->tab[rent].mark = ( (0 << XPOST_MEMORY_TABLE_MARK_DATA_MARK_OFFSET)
                | (0 << XPOST_MEMORY_TABLE_MARK_DATA_REFCOUNT_OFFSET)
                | (cnt << XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET)
                | (cnt << XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_OFFSET) );

        for (i = 0; i < sz; i++)
        {
            int ret;

            ret = xpost_memory_put(mem,
                    ent, i, (unsigned)sizeof(Xpost_Object), &null);
            if (!ret)
            {
                //error(rangecheck, "consarr cannot fill array value");
                XPOST_LOG_ERR("cannot fill array value");
                return null;
            }
        }
    }

    /* return (Xpost_Object){ .comp_.tag = arraytype, .comp_.sz = sz, .comp_.ent = ent, .comp_.off = 0}; */
    o.tag = arraytype
        | (XPOST_OBJECT_TAG_ACCESS_UNLIMITED
                << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    o.comp_.sz = (word)sz;
    o.comp_.ent = (word)ent;
    o.comp_.off = 0;
    return o;
} 

/** Select a memory file according to vmmode,
   call consarr,
   set BANK flag.
*/
Xpost_Object consbar(Xpost_Context *ctx,
               unsigned sz)
{
    Xpost_Object a = consarr(ctx->vmmode==GLOBAL? ctx->gl: ctx->lo, sz);
    if (xpost_object_get_type(a) != nulltype)
    {
        xpost_stack_push(ctx->lo, ctx->hold, a);
        if (ctx->vmmode==GLOBAL)
            a.tag |= XPOST_OBJECT_TAG_DATA_FLAG_BANK;
    }
    return a;
}

/** Copy if necessary,
   call put.
*/
int arrput(Xpost_Memory_File *mem,
            Xpost_Object a,
            integer i,
            Xpost_Object o)
{
    int ret;
    if (!xpost_save_ent_is_saved(mem, a.comp_.ent))
        if (!xpost_save_save_ent(mem, arraytype, a.comp_.sz, a.comp_.ent))
            return VMerror;
    if (i > a.comp_.sz)
    {
        //error(rangecheck, "arrput");
        XPOST_LOG_ERR("cannot put value in array");
        /*breakhere((Xpost_Context *)mem);*/
        return rangecheck;
    }
    ret = xpost_memory_put(mem, a.comp_.ent, (unsigned)(a.comp_.off + i), (unsigned)sizeof(Xpost_Object), &o);
    if (!ret)
        return VMerror;
    return 0;
}

/** Select Xpost_Memory_File according to BANK flag,
   call arrput.
*/
int barput(Xpost_Context *ctx,
            Xpost_Object a,
            integer i,
            Xpost_Object o)
{
    Xpost_Memory_File *mem = xpost_context_select_memory(ctx, a);
    if (!ignoreinvalidaccess) {
        if ( mem == ctx->gl
                && xpost_object_is_composite(o)
                && mem != xpost_context_select_memory(ctx, o))
            error(invalidaccess, "local value into global array");
    }

    return arrput(mem, a, i, o);
}

/* call get. */
Xpost_Object arrget(Xpost_Memory_File *mem,
              Xpost_Object a,
              integer i)
{
    Xpost_Object o;
    int ret;

    ret = xpost_memory_get(mem, a.comp_.ent, (unsigned)(a.comp_.off +i), (unsigned)(sizeof(Xpost_Object)), &o);
    if (!ret)
    {
        error(rangecheck, "arrget");
    }

    return o;
}

/* Select Xpost_Memory_File according to BANK flag,
   call arrget. */
Xpost_Object barget(Xpost_Context *ctx,
              Xpost_Object a,
              integer i)
{
    return arrget(xpost_context_select_memory(ctx, a), a, i);
}

/* adjust the offset and size fields in the object.
   NB. since this function only modifies the fields in the object
   itself, it also works for string and dict objects which use
   the same comp_ substructure. So this function is used everywhere
   for strings and dicts. It does not touch VM.
 */
Xpost_Object arrgetinterval(Xpost_Object a,
                      integer off,
                      integer sz)
{
    if (sz - off > a.comp_.sz)
        return invalid; /* should be interpreted as a rangecheck error */
    a.comp_.off += off;
    a.comp_.sz = sz;
    return a;
}

#ifdef TESTMODULE_AR
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Xpost_Context *ctx;
Xpost_Memory_File *mem;

int main(void)
{
    if (!xpost_init())
    {
        fprintf(stderr, "Fail to initialize xpost array test\n");
        return -1;
    }

    itpdata = malloc(sizeof*itpdata);
    memset(itpdata, 0, sizeof*itpdata);
    xpost_interpreter_init(itpdata);
    ctx = &itpdata->ctab[0];
    mem = ctx->lo;
    /* xpost_memory_file_init(&mem, "x.mem"); */
    /* (void)xpost_memory_table_init(&mem); */
    /* xpost_free_init(&mem); */
    /* xpost_save_init(&mem); */

    enum { SIZE = 10 };
    printf("\n^test ar.c\n");
    printf("allocating array occupying %zu bytes\n", SIZE*sizeof(Xpost_Object));
    Xpost_Object a = consarr(mem, SIZE);

    /* printf("the memory table:\n"); dumpmtab(mem, 0); */

    printf("test array by filling\n");
    int i;
    for (i=0; i < SIZE; i++) {
        printf("%d ", i+1);
        arrput(mem, a, i, xpost_cons_int( i+1 ));
    }
    puts("");

    printf("and accessing.\n");
    for (i=0; i < SIZE; i++) {
        Xpost_Object t;
        t = arrget(mem, a, i);
        printf("%d: %d\n", i, t.int_.val);
    }

    printf("the memory table:\n");
    dumpmtab(mem, 0);

    xpost_quit();

    return 0;
}

#endif

