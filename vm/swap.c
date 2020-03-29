#include "vm/swap.h"
#include "vm/frame.h"
#include <bitmap.h>
#include <debug.h>
#include "devices/block.h"
#include "threads/malloc.h"

struct bitmap* swap_used_vector;
struct block* swapBlock;
struct hash swapTable;

unsigned swapHash(const struct hash_elem *p_, void* aux);
bool swapLess(const struct hash_elem *a_, const struct hash_elem *b_, void* aux);

void swapInit() {
  swapBlock = block_get_role(BLOCK_SWAP);
  // size is in sectors which are 512 bytes each
  // a frame is 4096 bytes so divide by 8 to get the total number of swap slots
  swap_used_vector = bitmap_create(block_size(swapBlock) / 8);
  bitmap_set_all(swap_used_vector, 0);

  // table to keep track of swap table entries
  hash_init(&swapTable, swapHash, swapLess, NULL);
}


/*
  evit the page that currently in the frame that contains address kpage into swap 
  If swap is full then we panick the Kernel  
 */
int putPageIntoSwap(void *kpage) {

  SwapTableEntry* potentialSTE = malloc(sizeof(SwapTableEntry));
  
  // find index of first free table
  size_t index_of_free_swap = bitmap_scan (swap_used_vector, 0, 1, 0);  
  ASSERT(index_of_free_swap != BITMAP_ERROR);

  // check if there is already a swap slot for this.
  potentialSTE->index = index_of_free_swap;
  
  struct hash_elem *e = hash_find(&swapTable, &potentialSTE->hashElem);
  ASSERT(e == NULL);
  
  UserFrameTableEntry* frameEntry = frame_find_userframe_entry(kpage);
  potentialSTE->owner_tid = frameEntry->owner_tid;
  potentialSTE->upage = frameEntry->page_ptr;

  hash_insert(&swapTable, &potentialSTE->hashElem);

  bitmap_set(swap_used_vector, index_of_free_swap, 1);

  // each frame is 8 sectors large. write them into swap sequentially
  unsigned int kpageOff = 0;
  for(unsigned int i = index_of_free_swap*8; i < index_of_free_swap*8 + 8; i++, kpageOff += 512) {
    block_write (swapBlock, i, (char*)kpage + kpageOff);
  }
  
  return 1;
}

/* 
   get the page that belongs to tid and contains address upage and put it 
   into a free frame
   This assumes that the frame passed in is a free frame
   This assumes the frame is actually in swap
   This is called by the evitction policy to try and swap a page in
 */
int getPageFromSwap(int tid, void* upage, void* kpage) {

  // get the frame and read our frame out

  // find the frame in the swap list
  struct hash_iterator i;
  SwapTableEntry *f;
  hash_first (&i, &swapTable);
  while (hash_next (&i)){
    f = hash_entry (hash_cur (&i), SwapTableEntry, hashElem);
    if(f->owner_tid == tid && f->upage == upage) {
      break;
    }
  }

  // each frame is 8 sectors large. read them out of swap one by one  
  unsigned int kpageOff = 0;
  for(unsigned int i = f->index*8; i < f->index*8 + 8; i++, kpageOff += 512) {
    block_read (swapBlock, i, (char*)kpage + kpageOff);
  } 
    
  return 0;
}


unsigned swapHash(const struct hash_elem *p_, void* aux) {
  const SwapTableEntry *p = hash_entry(p_, SwapTableEntry, hashElem);
  return hash_bytes(&p->index, sizeof(p->index));
}

bool swapLess(const struct hash_elem *a_, const struct hash_elem *b_, void* aux) {
  const SwapTableEntry *a = hash_entry(a_, SwapTableEntry, hashElem);
  const SwapTableEntry *b = hash_entry(b_, SwapTableEntry, hashElem);
  
  return a->index < b->index;
}
