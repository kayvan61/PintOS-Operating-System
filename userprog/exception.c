#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/interrupt.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "filesys/file.h"
#include "vm/frame.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

static int install_page (void *upage, void *kpage, SupPageEntry* SPTE, int writable);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void)
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void)
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f)
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */

  // let the parent know that we got killed
  struct list_elem* curChild;
  struct child_return_info *chld;
  for (curChild = list_begin (&thread_current()->parent->children); curChild != list_end (&thread_current()->parent->children);
           curChild = list_next (curChild))
    {
      chld = list_entry (curChild, struct child_return_info, elem);
      if(chld->tid == thread_current()->tid)
	{
	  break;
	}
    }
  
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      chld->hasBeenChecked = true;
      chld->returnCode = -1;
      intr_dump_frame (f);
      exit(-1);

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel");

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      chld->hasBeenChecked = true;
      chld->returnCode = -1;
      exit(-1);      
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f)
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */

  struct thread* t = thread_current();

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* if the falut addr is in kernel memory then we should just set eax to 0xffffffff
     and move its old value into eip)*/

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  SupPageEntry* SPTE = thread_get_SPTE(fault_addr);
  if(SPTE != NULL) {
    if(SPTE->location == SWAP) {
      // swap it back into memory;
      printf("swapping should occur\n");
    }
    // if it's anywhere else then it will be handled later.
  }else {  
    /* kernel page fault. hopefully caused by a buffer validation check */
    if(!user) {
      f->eip = (void (*)(void))f->eax;
      f->eax = 0xffffffff;
      return;
    }
  }

  /* 
     user page fault. check if it's valid.
     if its valid then swap in the page using the suplimental page table
   */
  // not in kernel section
  uint32_t stack_delta = f->esp - fault_addr;
  if((uint32_t)fault_addr >= 0xc0000000) {
    exit(-1);
  }
  
  /* get free frame. (Moved from load_segment in process.c) */
  void* new_free_frame = get_free_frame();
  int writable = 1;
  
  if(SPTE == NULL) {
    // There is no SPTE for this page yet
    // so lets check if its a stack access
    if((uint32_t)fault_addr < (uint32_t)f->esp) {
      if(((stack_delta != 4) && (stack_delta != 32) )) {
	exit(-1);
      }
    }

    // its a stack addr lets add a SPTE for the new grown stack page
    // its not linked to any file so it has no location on disk. no zero bytes. is writable
    writable = 1;
    SPTE = createSupPageEntry((void*)((uint32_t)fault_addr & 0xFFFFF000), 0, 4096, thread_current()->tid, NULL, 0, 1);
  } else {

    writable = SPTE->isWritable;
    // the SPTE exists so this is probably a segment of code or something thats in swap
    // if its on disk then load it into memory
    /* Load this page. from disk if its on disk. (Moved from load_segment in process.c) */
    if(SPTE->location == DISK) {
      file_seek (SPTE->locationOnDisk, SPTE->offsetInDisk);
      if (file_read (SPTE->locationOnDisk, new_free_frame, SPTE->readBytes) != (int)SPTE->readBytes) {
	free_user_frame (new_free_frame);
	exit(-1);
      }
      memset (new_free_frame + SPTE->readBytes, 0, SPTE->zeroBytes);
    }

    // if its in swap then load it into memory from swap
    else if(SPTE->location == SWAP) {
      // TODO: Move from swap into mem
    }
    
    // if both of the above fail then there is something very wrong with our
    // updating of SPTEs and we need to look into that
    else {
      exit(-1);
    }
  }
  
  /* Add the page to the process's address space. (Moved from load_segment in process.c)*/
  if(!install_page ((void*)((uint32_t)fault_addr & 0xFFFFF000), new_free_frame, SPTE, writable)) {
    free_user_frame(new_free_frame);
    exit(-1);
  }
  
  return;
}

static int install_page (void *upage, void *kpage, SupPageEntry* SPTE, int writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  int res = pagedir_get_page (t->pagedir, upage) == NULL
         && pagedir_set_page (t->pagedir, upage, kpage, writable);

  if(res) {
    frame_table_update(t->tid, kpage, upage);
    SPTE->currentFrame = kpage;
    SPTE->location = MEM;
  }
  
  return res;
}
