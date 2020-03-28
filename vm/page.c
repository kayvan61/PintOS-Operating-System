#include "vm/page.h"
#include "threads/malloc.h"

SupPageEntry* createSupPageEntry(void* upage, int zBytes, int rBytes, int tid, struct file* f, int ofs, int wr) {
  SupPageEntry* SPTE = (SupPageEntry*)malloc(sizeof(SupPageEntry));
  
  SPTE->pageStart = (void*) ((int)upage & 0xFFFFF000);
  SPTE->currentFrame = NULL;
  SPTE->locationInSwap = NULL;
  
  SPTE->locationOnDisk = f;
  SPTE->offsetInDisk = ofs;
  
  SPTE->readBytes = rBytes;
  SPTE->zeroBytes = zBytes;
  
  SPTE->location = DISK;
  SPTE->tid = tid;

  SPTE->isWriteable = wr;
  
  return SPTE;
}

unsigned pageHash(const struct hash_elem *p_, void* aux) {
  const SupPageEntry *p = hash_entry(p_, SupPageEntry, hashElem);
  return hash_bytes(&p->pageStart, sizeof(p->pageStart));
}

bool pageLess(const struct hash_elem *a_, const struct hash_elem *b_, void* aux) {
  const SupPageEntry *a = hash_entry(a_, SupPageEntry, hashElem);
  const SupPageEntry *b = hash_entry(b_, SupPageEntry, hashElem);

  return a->pageStart < b->pageStart;
}

