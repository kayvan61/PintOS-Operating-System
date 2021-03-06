/*
  Contains data structures and managing functions
  for keeping track of Supplimental page table and 
  Page table
 */

#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "threads/malloc.h"
#include "filesys/file.h"

enum PageLocation {SWAP, MEM, DISK, ZERO};

typedef struct {
  struct hash_elem hashElem;
  
  void* pageStart;
  void* currentFrame;

  unsigned int locationInSwap;
  struct file* locationOnDisk;
  unsigned int offsetInDisk;
  
  unsigned int readBytes, zeroBytes;
  int isWritable;
  
  enum PageLocation location;
  int tid;
  int flags;
} SupPageEntry;

SupPageEntry* findSupPageEntry(void* upage);

SupPageEntry* createSupPageEntry(void* upage, int zBytes, int rBytes, int tid, struct file* f, int, int);

unsigned pageHash(const struct hash_elem *p_, void* aux);

bool pageLess(const struct hash_elem *a_, const struct hash_elem *b_, void* aux);

#endif
