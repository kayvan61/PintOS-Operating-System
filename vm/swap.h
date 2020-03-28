/*
  This should contain functions that swap pages into and out of disk  
 */


void swapInit();
/*
  evit the page that currently in the frame that contains address kpage into swap 
  If swap is full then we panick the Kernel  
 */
int putPageIntoSwap(void *kpage);
/* 
   get the page that belongs to tid and contains address upage and put it 
   into a free frame
   If there is no free frame then we will return 0 
   This is called by the evitction policy to try and swap a page in
 */
int getPageFromSwap(int tid, void* upage);
