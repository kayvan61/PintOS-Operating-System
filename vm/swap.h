/*
  This should contain functions that swap pages into and out of disk  
 */

#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <hash.h>

typedef struct {

  struct hash_elem hashElem;
  
  unsigned int index;
  int owner_tid;
  void* upage;
  
} SwapTableEntry;

void swapInit(void);

/*
  evit the page that currently in the frame that contains address kpage into swap 
  If swap is full then we panick the Kernel  
 */
int putPageIntoSwap(void *kpage);

/* 
   get the page that belongs to tid and contains address upage and put it 
   into a free frame
   This assumes that the frame passed in is a free frame
   This is called by the evitction policy to try and swap a page in
 */
int getPageFromSwap(int tid, void* upage, void* kpage);

#endif
