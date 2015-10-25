/*
 * The collector
 *
 * Copyright (c) 2014, 2015 Gregor Richards
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "ggggc/gc.h"
#include "ggggc-internals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct StackLL 
{
    void *data;
    struct StackLL *next;
};

struct StackLL * markStack;

void StackLL_Init()
{
    markStack = (struct StackLL *)malloc(sizeof(struct StackLL));
    markStack->data = NULL;
    markStack->next = NULL;
}

void StackLL_Push(void *x)
{
    struct StackLL *new = (struct StackLL *)malloc(sizeof(struct StackLL));
    new->data = x;
    new->next = markStack;
    markStack = new;
}

void * StackLL_Pop()
{
    void * x = markStack->data;
    struct StackLL *old = markStack;
    if (x) {
        markStack = markStack->next;
        free(old);
    }
    return x;
}

void StackLL_Clean()
{
    while(markStack->next) {
        StackLL_Pop();
    }
    free(markStack);
}


/* run a collection */
void ggggc_collect()
{
    // Initialize our work stack.
    StackLL_Init();
    struct GGGGC_PointerStack *stack_iter = ggggc_pointerStack;
    // Set the curpool to the toList so we can allocate to the curpool and update it
    // should we have more than one pool worth of live objects.
    ggggc_curPool = ggggc_toList;
    ggggc_toList = ggggc_fromList;
    ggggc_fromList = ggggc_curPool;
    while(ggggc_curPool) {
        ggggc_curPool->free = ggggc_curPool->start;
        ggggc_curPool = ggggc_curPool->next;
    }
    ggggc_curPool = ggggc_fromList;
    while (stack_iter) {
        ggc_size_t *** ptrptr = (ggc_size_t ***) stack_iter->pointers;
        ggc_size_t ptrIter = 0;
        while (ptrIter < stack_iter->size) {
            ggggc_process((void *) ptrptr[ptrIter]);        
            ptrIter++;               
        }
        stack_iter = stack_iter->next;
    }
    void * workIter = StackLL_Pop();
    while (workIter) {
        scan(workIter);
        workIter = StackLL_Pop();
    }
    StackLL_Clean();
    // Now updated references.
    // Deprecated since implementing a cleaner cheney's algorithm.
    //ggggc_updateRefs();
}

void * forward(void * from)
{
    if (alreadyMoved(from)) {
        return cleanForwardAddress(from);
    }
    struct GGGGC_Header * toRef = NULL;
    struct GGGGC_Header * fromRef = (struct GGGGC_Header *) from;
    struct GGGGC_Descriptor * descriptor = fromRef->descriptor__ptr;
    if (ggggc_curPool->free + descriptor->size < ggggc_curPool->end) {
        toRef = (struct GGGGC_Header *) ggggc_curPool->free;
        ggggc_curPool->free = ggggc_curPool->free + descriptor->size;
    } else {
        // Next pool should always be empty because it must be since we're in to space.
        // Which means assuming we're not allocating any objects larger than pool we can just do it
        ggggc_curPool = ggggc_curPool->next;
        toRef = (struct GGGGC_Header *) (ggggc_curPool->free);
        ggggc_curPool->free = ggggc_curPool->free + descriptor->size;

    }
    memcpy(toRef,fromRef,sizeof(ggc_size_t)*descriptor->size);
    fromRef->descriptor__ptr = (struct GGGGC_Descriptor *) ( ((ggc_size_t) toRef) | 1L);
    StackLL_Push(toRef);
    return(toRef);  
}

long unsigned int alreadyMoved(void * x) {
    // Check if the lowest order bit of the "descriptor ptr" is set. If it is
    // then this object has been moved (and that's not a descriptor ptr but a forward address)
    //printf("trying to check if object at %lx has already moved its desc ptr is %lx\r\n", (long unsigned int) x, (long unsigned int) ((struct GGGGC_Header *) x)->descriptor__ptr);
    //printf("%ld is the bitwise and\r\n", (long unsigned int) ((struct GGGGC_Header *) x)->descriptor__ptr & 1L);
    //printf("Trying to check if %lx has already moved \r\n", (long unsigned int) x);
    return (long unsigned int) ((struct GGGGC_Header *) x)->descriptor__ptr & 1L;
}

void * cleanForwardAddress(void * x) {
    struct GGGGC_Header * header = (struct GGGGC_Header *) x;
    return (void *) ((long unsigned int) header->descriptor__ptr & 0xFFFFFFFFFFFE );
}

void ggggc_process(void * x) {
    struct GGGGC_Header * obj = *((struct GGGGC_Header **) x);
    if (obj) {
        *((struct GGGGC_Header **) x) = (struct GGGGC_Header *) forward((void *) obj);
    }
}

void scan(void *x) {
    struct GGGGC_Header * ref = (struct GGGGC_Header *) x;
    if (!x) {
        return;
    }
    //printf("x is %lx\r\n", lui x);
    if (alreadyMoved(x)) {
        ref = cleanForwardAddress(x);
    }
    struct GGGGC_Descriptor * desc = ref->descriptor__ptr;
    //printf("x desc is %lx\r\n", lui desc);
    if (desc->pointers[0]&1) {
        long unsigned int bitIter = 1;
        int z = 0;
        while (z < desc->size) {
            if (desc->pointers[0] & bitIter) {
                void * loc = (void *) ( ((ggc_size_t *) ref) + z );
                ggggc_process(loc);
            }
            z++;
            bitIter = bitIter<<1;
        }
    } else {
        ggggc_process((void *) ref);
    }
}

/* explicitly yield to the collector */
int ggggc_yield()
{
    //printf("Going to yield\r\n");
    if (ggggc_forceCollect) {
        //printf("going to collect\r\n");
        ggggc_collect();
        ggggc_forceCollect = 0;    
       //printf("Done collecting\r\n");
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
