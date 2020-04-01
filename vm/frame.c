#include "vm/frame.h"
#include "vm/swap.h"
#include "vm/page.h"
#include <bitmap.h>
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "threads/interrupt.h"

#define IS_DIRTY(x)    (x & 1)
#define WAS_ACC(x)     (x & 2)

#define SET_DIRTY(x)    x = (x | 1)
#define SET_ACC(x)      x = (x | 2)

#define CLEAR_DIRTY(x)    x = (x & ~1)
#define CLEAR_ACC(x)      x = (x & ~2)

static struct bitmap* frame_used_vector;

static struct hash all_frame_list;
static struct hash all_frame_index;

static int clockHand;

int TOTAL_NUM_FRAMES = 0;

unsigned frameHash(const struct hash_elem *p_, void* aux) {
  const UserFrameTableEntry *p = hash_entry(p_, UserFrameTableEntry, hashElem);
  return hash_bytes(&p->frame_ptr, sizeof(p->frame_ptr));
}

bool frameLess(const struct hash_elem *a_, const struct hash_elem *b_, void* aux) {
  const UserFrameTableEntry *a = hash_entry(a_, UserFrameTableEntry, hashElem);
  const UserFrameTableEntry *b = hash_entry(b_, UserFrameTableEntry, hashElem);

  return a->frame_ptr < b->frame_ptr;
}

unsigned indHash(const struct hash_elem *p_, void* aux) {
  const UserFrameTableEntry *p = hash_entry(p_, UserFrameTableEntry, indexElem);
  return hash_bytes(&p->index, sizeof(p->index));
}

bool indLess(const struct hash_elem *a_, const struct hash_elem *b_, void* aux) {
  const UserFrameTableEntry *a = hash_entry(a_, UserFrameTableEntry, indexElem);
  const UserFrameTableEntry *b = hash_entry(b_, UserFrameTableEntry, indexElem);

  return a->index < b->index;
}

void frame_alloc_init() {
  hash_init (&all_frame_list, frameHash, frameLess, NULL);
  hash_init (&all_frame_index, indHash, indLess, NULL);

  UserFrameTableEntry* cur_frame;
  void* frame_ptr;
  
  /* alloc all the user frames and keep them to ourselves */
  /* this is greedy allocator scheme we can switch to lazy later if we want */
  while((frame_ptr = palloc_get_page(PAL_USER)) != NULL) {
    cur_frame = malloc(sizeof(UserFrameTableEntry));

    /* 
       set the pointers to the proper thing 
       initially it isnt linked to any page
     */
    cur_frame->frame_ptr = frame_ptr;
    cur_frame->page_ptr = NULL;
    cur_frame->owner_pd = NULL;
    cur_frame->owner_tid = -1;

    cur_frame->index = TOTAL_NUM_FRAMES;
    
    cur_frame->extra_flags = 0; /* initial its not accessed and not dirty */
    
    hash_insert(&all_frame_list, &cur_frame->hashElem);
    hash_insert(&all_frame_index, &cur_frame->indexElem);
    
    TOTAL_NUM_FRAMES++;
  }
  
  frame_used_vector = bitmap_create(TOTAL_NUM_FRAMES);
  bitmap_set_all(frame_used_vector, 0);

  clockHand = 0;
}

void* get_free_frame() {
  // all frames are 1 (used)
  if(bitmap_all(frame_used_vector, 0, TOTAL_NUM_FRAMES)) {
    return evict_frame();
  }  
  
  UserFrameTableEntry scratch;
  
  /* traverse frame list to find that free frame from the bit vector */
  size_t frame_index = bitmap_scan(frame_used_vector, 0, 1, 0);
  scratch.index = frame_index;
  struct hash_elem *p = hash_find(&all_frame_index, &scratch.indexElem);
  if(p == NULL) {
    PANIC("free frame vector stated that there is a free frame but there is not.");
  }
  UserFrameTableEntry* FTE = hash_entry(p, UserFrameTableEntry, indexElem);
  
  ASSERT(FTE->page_ptr == NULL);
  ASSERT(FTE->owner_pd == NULL);
  ASSERT(FTE->owner_tid == -1);

  bitmap_set(frame_used_vector, frame_index, 1);
  
  return FTE->frame_ptr;
}

void frame_table_update(int tid, void* frame_ptr, void* user_v_addr, void* o_pd) {

  ASSERT(o_pd != NULL);
  
  frame_ptr   = (void*)((unsigned int)frame_ptr & 0xFFFFF000);
  user_v_addr = (void*)((unsigned int)user_v_addr & 0xFFFFF000);

  UserFrameTableEntry scratch;
  scratch.frame_ptr = frame_ptr;
  struct hash_elem *p = hash_find(&all_frame_list, &scratch.hashElem);
  if(p == NULL) {
    PANIC("tried to update a frame that doesnt exist");
  }
  UserFrameTableEntry* FTE = hash_entry(p, UserFrameTableEntry, hashElem);
  
  FTE->page_ptr = user_v_addr;
  FTE->owner_pd = o_pd;
  FTE->owner_tid = tid; 
}

void free_user_frame(void* kFrame) {
  
  kFrame   = (void*)((unsigned int)kFrame & 0xFFFFF000);

  UserFrameTableEntry scratch;
  scratch.frame_ptr = kFrame;
  struct hash_elem *p = hash_find(&all_frame_list, &scratch.hashElem);
  if(p == NULL) {
    PANIC("tried to free a frame that doesnt exist");
  }
  UserFrameTableEntry* FTE = hash_entry(p, UserFrameTableEntry, hashElem);

  ASSERT(FTE->owner_tid == thread_current()->tid);
  
  FTE->page_ptr = NULL;
  FTE->owner_pd = NULL;
  FTE->owner_tid = -1;

  bitmap_set(frame_used_vector, FTE->index, 0);
}

UserFrameTableEntry* frame_find_userframe_entry(void* framePtr) {
  UserFrameTableEntry scratch;
  scratch.frame_ptr = (void*)(((uint32_t)framePtr) & 0xFFFFF000);
  struct hash_elem *p = hash_find(&all_frame_list, &scratch.hashElem);
  if(p == NULL) {
    PANIC("tried to free a frame that doesnt exist");
  }
  return hash_entry(p, UserFrameTableEntry, hashElem);
}

void advanceClockHand(void) {
  clockHand = (clockHand +1) % TOTAL_NUM_FRAMES; 
}

void updateFrameForSwap(UserFrameTableEntry *pfe) {    
  pagedir_clear_page (pfe->owner_pd, pfe->page_ptr);
  // update the frame info
  pfe->page_ptr = NULL;
  pfe->owner_tid = -1;
  pfe->extra_flags = 0;
  pfe->owner_pd = NULL;
}

void updatePageForSwap(SupPageEntry* spte, int toDisk) {
  toDisk = 0;
  if(toDisk) {
    // update the SPTE to note that its no longer present (now on disk)
    if(spte->locationOnDisk == NULL) {
      spte->location = ZERO;
    }
    else {
      spte->location = DISK;
    }
  } else {
    spte->locationInSwap = putPageIntoSwap(spte->currentFrame);
    pagedir_set_dirty(frame_find_userframe_entry(spte->currentFrame)->owner_pd, spte->pageStart, 0);
    // update the SPTE to note that its no longer present (now in swap)
    spte->location = SWAP;
  }
  spte->currentFrame = NULL;
}

void* evict_frame() {
  // clock replacement algo

  //iterateAllIndex();
  
  ASSERT(bitmap_all(frame_used_vector, 0, TOTAL_NUM_FRAMES));
  
  int first_elem = clockHand;
  int cur_elem   = first_elem;
  
  /* Look for a frame thats unused and not dirty (one that we dont have to put in swap) */
  do {
    // wrap the clock
    ASSERT(cur_elem <= TOTAL_NUM_FRAMES && cur_elem >= 0);
    if(cur_elem == TOTAL_NUM_FRAMES) {
      cur_elem = 0;
    }

    UserFrameTableEntry scratch;
    scratch.index = cur_elem;
    struct hash_elem *p = hash_find(&all_frame_index, &scratch.indexElem);
    if(p == NULL) {
      PANIC("clock hand points to a frame that doesnt exist .");
    }
    
    UserFrameTableEntry *pfe = hash_entry (p, UserFrameTableEntry, indexElem);
    if(!pagedir_is_accessed(pfe->owner_pd, pfe->page_ptr)) {
      if(!pagedir_is_dirty(pfe->owner_pd, pfe->page_ptr)) {
	// found a page that is both unaccessed and not dirty
	// prime candidate for swaping out	
	updatePageForSwap(thread_get_SPTE(pfe->page_ptr), 1);
	updateFrameForSwap(pfe);
	// advance the clock hand for next eviction
	advanceClockHand();
	// make sure to return the frame pointer here too	
	return pfe->frame_ptr;
      }
    }
    else {
      pagedir_set_accessed(pfe->owner_pd, pfe->page_ptr, 0);
      SET_ACC(pfe->extra_flags);
    }

    cur_elem++;
    
  } while(cur_elem != first_elem);

  /* we didnt find a frame that wasnt both not accessed and not dirty so we run algo again to find one that wasn't accessed */
  do {
    // wrap the clock
    if(cur_elem == TOTAL_NUM_FRAMES) {
      cur_elem = 0;
    }

    UserFrameTableEntry scratch;
    scratch.index = cur_elem;
    struct hash_elem *p = hash_find(&all_frame_index, &scratch.indexElem);
    if(p == NULL) {
      PANIC("clock hand points to a frame that doesnt exist .");
    }

    UserFrameTableEntry *pfe = hash_entry (p, UserFrameTableEntry, indexElem);
    if(!WAS_ACC(pfe->extra_flags)) {
      // found a page that was unaccessed but dirty
      // it was dirty tho so put it into swap
      
      updatePageForSwap(thread_get_SPTE(pfe->page_ptr), 0);          
      // update the frame info
      updateFrameForSwap(pfe);

      // advance the clock hand for next eviction
      advanceClockHand();
      
      // make sure to return the frame pointer here too
      return pfe->frame_ptr;
    }

    cur_elem++;
  } while(cur_elem != first_elem);

  // no good candidate to evict so just evict the clock hand
  // it was dirty tho so put it into swap
  // make sure to return the frame pointer here too
  // update the SPTE to note that its no longer present (now in swap)
  UserFrameTableEntry scratch;
  scratch.index = clockHand;
  struct hash_elem *p = hash_find(&all_frame_index, &scratch.indexElem);
  if(p == NULL) {
    PANIC("clock hand points to a frame that doesnt exist .");
  }
  UserFrameTableEntry *pfe = hash_entry (p, UserFrameTableEntry, indexElem); 
  updatePageForSwap(thread_get_SPTE(pfe->page_ptr), 0);
  // update the frame info
  updateFrameForSwap(pfe);
  advanceClockHand();
  return pfe->frame_ptr;
}
