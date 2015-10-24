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


void ggggc_updateRefs()
{
    // Update all references in our new from list.
    struct GGGGC_Pool *poolIter = ggggc_fromList;
    // Use lastIteration to iterate until we hit current pool since
    // we don't want to just iter til we hit the last pool as it could be
    // much further than current pool actually is.
    int lastIteration = 0;
    while(!lastIteration) {
        ggc_size_t * objIter = poolIter->start;
        // If we're at cur pool stop after this iteration.
        if (poolIter == ggggc_curPool) {
            lastIteration = 1;
        }
        while (objIter < poolIter->free && objIter) {
            // In from space no descriptor pointers should be incorrect...
            struct GGGGC_Descriptor *descriptor = ((struct GGGGC_Header *) objIter)->descriptor__ptr;
            if (alreadyMoved(descriptor)) {
                ((struct GGGGC_Header *)objIter)->descriptor__ptr = (struct GGGGC_Descriptor *) cleanForwardAddress(descriptor);
                descriptor =((struct GGGGC_Header *)  objIter)->descriptor__ptr;
            }
            if (descriptor->pointers[0]&1) {
                long unsigned int bitIter = 1;
                int z = 0;
                while( z < descriptor->size) {
                    if (descriptor->pointers[0] & bitIter) {
                        /* so we found a pointer in our object so check it out */
                        void * newPtr = (void *) (objIter+z);
                        struct GGGGC_Header *newHeader = *((struct GGGGC_Header **) newPtr);
                        if (newHeader) {
                            if (alreadyMoved((void *) newHeader)){
                                objIter[z] = (ggc_size_t) cleanForwardAddress(newHeader);
                            }
                        }
                    }
                    z++;
                    bitIter = bitIter << 1;
                }
            }
            objIter = objIter + descriptor->size;
        }
        poolIter = poolIter->next;
    }
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
    while (stack_iter) {
        struct GGGGC_Header *** ptrptr = (struct GGGGC_Header ***) stack_iter->pointers;
        ggc_size_t ptrIter = 0;
        while (ptrIter < stack_iter->size) {
            printf("ptrIter is %ld and stackiter size is %ld\r\n", (long unsigned int) ptrIter, (long unsigned int) stack_iter->size);
            if (*ptrptr[ptrIter]) {
                struct GGGGC_Header *header= *ptrptr[ptrIter];
                printf(" I'm in an infinite loop lol!\r\n");
                if (!alreadyMoved((void*) header)) {
                    StackLL_Push((void *) header);
                    ggggc_process();
                }                
            }
            ptrIter++;               
        }
    }
    // Now swap our lists
    struct GGGGC_Pool *temp = ggggc_toList;
    ggggc_toList = ggggc_fromList;
    ggggc_fromList = temp;
    // Now clean up our new toList's free ptrs.
    temp = ggggc_toList;
    while(temp) {
        temp->free = temp->start;
        temp = temp->next;
    }
    StackLL_Clean();
    // Now updated references.
    ggggc_updateRefs();
}

void forward(void * from, void * to)
{
    struct GGGGC_Header * fromRef = (struct GGGGC_Header *) from;
    struct GGGGC_Header * toRef = (struct GGGGC_Header *) to;
    struct GGGGC_Descriptor * descriptor = fromRef->descriptor__ptr;
    memcpy(toRef,fromRef,descriptor->size);
    fromRef->descriptor__ptr = (struct GGGGC_Descriptor *) ( ((long unsigned int) toRef) | 1L);
    //printf("lol am I forwarding %lx to %lx?\r\n", (long unsigned int) fromRef, (long unsigned int) toRef);
    //printf("Forwarded descriptor ptr is %lx\r\n", (long unsigned int) fromRef->descriptor__ptr);
    
}

long unsigned int alreadyMoved(void * x) {
    // Check if the lowest order bit of the "descriptor ptr" is set. If it is
    // then this object has been moved (and that's not a descriptor ptr but a forward address)
    //printf("trying to check if object at %lx has already moved its desc ptr is %lx\r\n", (long unsigned int) x, (long unsigned int) ((struct GGGGC_Header *) x)->descriptor__ptr);
    //printf("%ld is the bitwise and\r\n", (long unsigned int) ((struct GGGGC_Header *) x)->descriptor__ptr & 1L);
    return (long unsigned int) ((struct GGGGC_Header *) x)->descriptor__ptr & 1L;
}

void * cleanForwardAddress(void * x) {
    struct GGGGC_Header * header = (struct GGGGC_Header *) x;
    return (void *) ((long unsigned int) header->descriptor__ptr & 0xFFFFFFFFFFFFFFFE );
}

void ggggc_process() {
    struct GGGGC_Header * objIter = (struct GGGGC_Header *) StackLL_Pop();
    while (objIter) {
        if (alreadyMoved(objIter)) {
            objIter = StackLL_Pop();
            continue;
        }
        // If we got here it isn't marked so it's descriptor ptr should be valid...
        struct GGGGC_Descriptor * descriptor = objIter->descriptor__ptr;
        ggc_size_t size = descriptor->size;
        // Now that we have the descriptor let's push any references to our work queue.
        if (descriptor->pointers[0]&1) {
            long unsigned int bitIter = 1;
            int z = 0;
            while( z < descriptor->size) {
                if (descriptor->pointers[0] & bitIter) {
                    /* so we found a pointer in our object so check it out */
                    void * newPtr = (void *) (((ggc_size_t *) objIter)+z);
                    struct GGGGC_Header **newHeader = (struct GGGGC_Header **) newPtr;
                    if (*newHeader) {
                        struct GGGGC_Header * next = *newHeader;
                        if (!z) {
                            // If z is 0 this is our descriptor ptr and we need to clean it first.
                            // This shouldn't be needed since switching to bitmap free list but
                            // I'm scared to take it out :(
                            next = (struct GGGGC_Header *) descriptor;
                        }
                        StackLL_Push((void *) next);
                    }
                }
                z++;
                bitIter = bitIter << 1;
            }
        }else {
            // It only has it's descriptor ptr in it, this is still necessary to check.
            StackLL_Push((void *) descriptor);
        }
        struct GGGGC_Header * toRef = NULL;
        while(!toRef) {
            if (ggggc_curPool->free + size >= ggggc_curPool->end) {
                // Because our to space is always as big as our from space
                // we should have no problem just iterating through pools.
                // So no need to check that next isn't null.
                ggggc_curPool = ggggc_curPool->next;
            } else {
                toRef = (struct GGGGC_Header *) ggggc_curPool->free;
                ggggc_curPool->free += size;
            }
        }
        forward(objIter,toRef);
        objIter = StackLL_Pop();
    }
}

/* explicitly yield to the collector */
int ggggc_yield()
{
    /* FILLME */
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
