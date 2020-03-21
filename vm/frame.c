#include "vm/frame.h"
#include <bitmap.h>
#include "threads/palloc.h"
#include "threads/malloc.h"

#define TOTAL_NUM_FRAMES 1 << 20

#define IS_ACCESSED(x) x & 1
#define IS_DIRTY(x)    x & 2

#define SET_ACCESSED(x) x | 1
#define SET_DIRTY(x)    x | 2

#define CLEAR_ACCESSED(x) x & ~1
#define CLEAR_DIRTY(x)    x & ~2


struct bitmap* frame_used_vector;

static struct list all_frame_list;

void frame_alloc_init() {
  list_init (&all_frame_list);
  frame_used_vector = bitmap_create(TOTAL_NUM_FRAMES);

  UserFrameTableEntry* cur_frame;
  void* frame_ptr;

  bitmap_set_all(frame_used_vector, 0);
  
  /* alloc all the user frames and keep them to ourselves */
  /* this is greedy allocator scheme we can switch to lazy later if we want */
  while((frame_ptr = palloc_get_page(PAL_USER)) != NULL) {
    cur_frame = malloc(sizeof(UserFrameTableEntry));

    /* 
       set the pointers to the proper thing 
       initially it isnt linked to any frame
     */
    cur_frame->frame_ptr = frame_ptr;
    cur_frame->page_ptr = NULL;
    cur_frame->owner_tid = -1;
    
    cur_frame->extra_flags = 0; /* initial its not accessed and not dirty */
    list_push_back(&all_frame_list, &cur_frame->allelem);
  }  
}

void* get_free_frame() {  
  size_t index_of_free_frame = bitmap_scan (frame_used_vector, 0, 1, 0);  
  if(index_of_free_frame == BITMAP_ERROR) {
    /* this will trigger the replacement policy in the future */
    return NULL;
  }
  struct list_elem *e;

  /* traverse frame list to find that free frame from the bit vector */
  size_t trav_index = 0;
  for (e = list_begin (&all_frame_list); e != list_end (&all_frame_list);
       e = list_next (e), trav_index++)
    {      
      if(trav_index == index_of_free_frame) {
	UserFrameTableEntry *pfe = list_entry (e, UserFrameTableEntry, allelem);
	bitmap_set(frame_used_vector, index_of_free_frame, 1);
	return pfe->frame_ptr;
      }
    }
  return NULL;
}

void frame_table_update(int tid, void* frame_ptr, void* user_v_addr) {
  struct list_elem *e;

  /* traverse frame list to find frame to free in the bit vector */
  for (e = list_begin (&all_frame_list); e != list_end (&all_frame_list);
       e = list_next (e))
    {
      UserFrameTableEntry *pfe = list_entry (e, UserFrameTableEntry, allelem);
      
      if(pfe->frame_ptr == frame_ptr) {	
	pfe->page_ptr = user_v_addr;
      }      
    } 
}

void free_user_frame(void* kFrame) {
  size_t index_of_frame_to_free = 0;

  struct list_elem *e;

  /* traverse frame list to find frame to free in the bit vector */
  for (e = list_begin (&all_frame_list); e != list_end (&all_frame_list);
       e = list_next (e), index_of_frame_to_free++)
    {
      UserFrameTableEntry *pfe = list_entry (e, UserFrameTableEntry, allelem);
      
      if(pfe->frame_ptr == kFrame) {
	bitmap_set(frame_used_vector, index_of_frame_to_free, 0);
	pfe->page_ptr = NULL;
      }      
    } 
}
