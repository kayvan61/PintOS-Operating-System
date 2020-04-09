/*
  Frame allocator files
  the init should first grab all the user pages and then
  hand them out using this instead of going through palloc_get_page
 */
#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdint.h>
#include <hash.h>

typedef struct {
  void* page_ptr;
  void* frame_ptr;
  int owner_tid;
  uint8_t extra_flags;
  uint32_t *owner_pd;
  int index;
  
  struct hash_elem hashElem;           /* List element for all frame list. */
  struct hash_elem indexElem;
  
} UserFrameTableEntry;

void frame_alloc_init(void);

// Get a free user frame
// If there are no free frames then we run the eviction polcy
void* get_free_frame(void);

// Update the frame_table entry with information about the page
// Thats in the frame pointed to by frame_ptr
void frame_table_update(int tid, void* frame_ptr, void* user_v_addr, void* o_pd);

// Frees frame without adding it back to the unmanaged pool
// ONLY USE THIS FOR USER FRAMES
void free_user_frame(void* kFrame);

UserFrameTableEntry* frame_find_userframe_entry(void* framePtr);

void* evict_frame(void);

void pin_page(void* upage, int);
void unpin_page(void* upage, int);

#endif
