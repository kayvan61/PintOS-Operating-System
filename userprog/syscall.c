#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/synch.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/init.h"
#include "lib/syscall-nr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "vm/frame.h"

static void syscall_handler (struct intr_frame *);
static bool validate_ptr(const void* ptr, int spanSize);

static struct lock file_system_lock; 

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&file_system_lock);
}

static void set_page_to_dirty(void* upage) {
  upage = (void*)((unsigned int)upage & 0xFFFFF000);
  pagedir_set_dirty(thread_current()->pagedir, upage, 1);
}

static void set_page_to_accessed(void* upage) {
  upage = (void*)((unsigned int)upage & 0xFFFFF000);
  pagedir_set_accessed(thread_current()->pagedir, upage, 1);
}

static void
syscall_handler (struct intr_frame *f)
{

  thread_current()->t_esp = f->esp;
  
  if(!validate_ptr(f->esp, 1)) {
    exit(-1);
  }
  int32_t statusCode = *(int32_t *)(f->esp);
  if(statusCode > SYS_INUMBER) {
    exit(-1);
  }
  int32_t* argv = f->esp;
  argv += 1;
  if(!validate_ptr(argv, 1)) {
    exit(-1);
  }
  int return_val = -1;
  
  switch(statusCode) {
  case SYS_HALT:
    halt();
    break;
  case SYS_EXIT:
    exit(*argv);
    break;
  case SYS_EXEC:
    f->eax = exec(*(char**)argv);
    break;
  case SYS_WAIT:
    return_val = wait(*(argv));
    f->eax = return_val;
    break;
  case SYS_CREATE:
    if(!validate_ptr(*(void**)argv, 1)) {
      exit(-1);
    }
    return_val = create(*(char**)argv, *(argv+1));
    f->eax = return_val;
    break;
  case SYS_REMOVE:
    if(!validate_ptr(*(void**)argv, 1)) {
      exit(-1);
    }
    return_val = remove(*(char**)argv);
    f->eax = return_val;
    break;
  case SYS_OPEN:
    if(!validate_ptr(*(void**)argv, 1)) {
      exit(-1);
    }
    return_val = open(*(char**)argv);
    f->eax = return_val;
    break;
  case SYS_FILESIZE:
    f->eax = filesize(*(argv));
    break;
  case SYS_READ:
    if(!validate_ptr((void *)*(argv+1), *(argv+2))) {
      exit(-1);
    }
    return_val = read(*argv, (void *)*(argv+1), *(argv+2));
    f->eax = return_val;
    break;
  case SYS_WRITE:    
    if(!validate_ptr((void *)*(argv+1), *(argv+2))) {
      exit(-1);
    }
    return_val = write(*argv, (void *)*(argv+1), *(argv+2));
    f->eax = return_val;
    break;
  case SYS_SEEK:
    seek(*(argv), *(argv+1));
    break;
  case SYS_TELL:
    tell(*(argv));
    break;
  case SYS_CLOSE:
    close(*(argv));
    break;
  case SYS_CHDIR:
    f->eax = chdir(*(argv));
    break;
  case SYS_MKDIR:
    f->eax = mkdir(*(argv));
    break;
  case SYS_READDIR:
    f->eax = readdir(*(argv), *(argv + 1));
    break;
  case SYS_ISDIR:
    f->eax = isdir(*(argv));
    break;
  case SYS_INUMBER:
    f->eax = inumber(*(argv));
    break;
  }
}

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}
 
/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. 
static bool
put_user (uint8_t *udst, const uint8_t byte)
{
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
  }*/

static bool
validate_ptr(const void* ptr, int spanSize) {
  if(ptr >= PHYS_BASE) {
    exit(-1);
  }

  if(get_user(ptr) != -1) {
    return true;
  }
  else {
    // invalid read
    exit(-1);
  }
}

/*System Call: void halt (void)
  Terminates Pintos by calling shutdown_power_off() (declared in "threads/init.h"). This should be seldom used, because you lose some information about possible deadlock situations, etc. */

void halt (void) {
  shutdown_power_off();
}

void orphan_threads (struct thread *t, void *aux) {
    int parentTid = *(int*)aux;
    if(t->tid == parentTid) {
        t->parent = NULL;
    }
}

/*  System Call: void exit (int status)
    Terminates the current user program, returning status to the kernel. 
    If the process's parent waits for it (see below), this is the status that will be returned. 
    Conventionally, a status of 0 indicates success and nonzero values indicate errors. 
*/
void exit(int status) {
  printf("%s: exit(%d)\n", thread_current()->name, status);
  thread_current()->returnCode = status;

  if(thread_current()->parent != NULL) {
      struct list_elem* curChild;
      for (curChild = list_begin (&thread_current()->parent->children); curChild != list_end (&thread_current()->parent->children);
           curChild = list_next (curChild))
          {
              struct child_return_info *f = list_entry (curChild, struct child_return_info, elem);
              if(f->tid == thread_current()->tid)
                  {
                      f->hasBeenChecked = true;
                      f->returnCode = status;
                  }
          }
  
      if(thread_current()->parent->waitingOnTid == thread_current()->tid) {
          sema_up(&thread_current()->parent->waitingLock);
      }
  }

  enum intr_level old_level;
  old_level = intr_disable ();
  thread_foreach(&orphan_threads, &thread_current()->tid);
  intr_set_level (old_level);
    
  if(thread_current()->selfOnDisk != NULL) {
    file_allow_write(thread_current()->selfOnDisk);
  }
  thread_exit();
}

/*  System Call: tid_t exec (const char *cmd_line)
    Runs the executable whose name is given in cmd_line, passing any given arguments, and returns the new process's program id (pid). 
    Must return pid -1, which otherwise should not be a valid pid, if the program cannot load or run for any reason. 
    Thus, the parent process cannot return from the exec until it knows whether the child process successfully loaded its executable. 
    You must use appropriate synchronization to ensure this. 
*/
tid_t exec (const char *cmd_line) {
  
  bool validation = validate_ptr((const void*)cmd_line, 1);
  if(!validation){
    exit(-1);
  }

  tid_t pid = process_execute(cmd_line);
  lock_acquire(&file_system_lock);
  sema_down(&thread_current()->childExecLock);
  bool loaded = thread_current()->isChildMadeSuccess;  
  lock_release(&file_system_lock);
  return loaded ? pid : -1;
}

/*  System Call: int wait (tid_t pid)
    Waits for a child process pid and retrieves the child's exit status.

    If pid is still alive, waits until it terminates. 
    Then, returns the status that pid passed to exit. 
    If pid did not call exit(), but was terminated by the kernel (e.g. killed due to an exception), wait(pid) must return -1. 
    It is perfectly legal for a parent process to wait for child processes that have already terminated by the time the parent calls wait, 
    but the kernel must still allow the parent to retrieve its child's exit status, or learn that the child was terminated by the kernel.

    wait must fail and return -1 immediately if any of the following conditions is true:

    pid does not refer to a direct child of the calling process. 
    pid is a direct child of the calling process if and only if the calling process received pid as a return value from a successful call to exec.

    Note that children are not inherited: if A spawns child B and B spawns child process C, then A cannot wait for C, even if B is dead. 
    A call to wait(C) by process A must fail. 
    Similarly, orphaned processes are not assigned to a new parent if their parent process exits before they do.

    The process that calls wait has already called wait on pid. That is, a process may wait for any given child at most once. 

    Processes may spawn any number of children, wait for them in any order, and may even exit without having waited for some or all of their children. 
    Your design should consider all the ways in which waits can occur. 
    All of a process's resources, including its struct thread, must be freed whether its parent ever waits for it or not, 
    and regardless of whether the child exits before or after its parent.

    You must ensure that Pintos does not terminate until the initial process exits. 
    The supplied Pintos code tries to do this by calling process_wait() (in "userprog/process.c") from main() (in "threads/init.c"). 
    We suggest that you implement process_wait() according to the comment at the top of the function and then implement the wait system call in terms of process_wait().

    Implementing this system call requires considerably more work than any of the rest.
*/

int wait (tid_t pid) {
  if(pid < 0) {
    return -1;
  }
  return process_wait(pid);
}

/* System Call: bool create (const char *file, unsigned initial_size)
   Creates a new file called file initially initial_size bytes in size. 
   Returns true if successful, false otherwise. 
   Creating a new file does not open it: opening the new file is a separate operation which would require a open system call. */

bool create (const char *file, unsigned initial_size) {

  bool ret = false;
  lock_acquire(&file_system_lock);

  ret = filesys_create(file, initial_size);
  
  lock_release(&file_system_lock);
  return ret;
}

/* System Call: bool remove (const char *file)
   Deletes the file called file. Returns true if successful, false otherwise. 
   A file may be removed regardless of whether it is open or closed, and removing an open file does not close it. 
   See Removing an Open File, for details. 
*/

bool remove (const char *file) {

  bool ret = false;
  lock_acquire(&file_system_lock);

  ret = filesys_remove(file);
  
  lock_release(&file_system_lock); 
  return ret;
}

/* System Call: int open (const char *file)
   Opens the file called file. Returns a nonnegative integer handle called a "file descriptor" (fd), or -1 if the file could not be opened.

   File descriptors numbered 0 and 1 are reserved for the console: fd 0 (STDIN_FILENO) is standard input, fd 1 (STDOUT_FILENO) is standard output. 
   The open system call will never return either of these file descriptors, which are valid as system call arguments only as explicitly described below.

   Each process has an independent set of file descriptors. File descriptors are not inherited by child processes.

   When a single file is opened more than once, whether by a single process or different processes, each open returns a new file descriptor. 
   Different file descriptors for a single file are closed independently in separate calls to close and they do not share a file position. */

int open (const char *file) {
  lock_acquire(&file_system_lock);
  struct file* openedFile = filesys_open(file);
  if(openedFile == NULL){
    lock_release(&file_system_lock); 
    return -1;
  }
  int ret = thread_add_fd(openedFile);
  lock_release(&file_system_lock); 
  return ret + 2;
}
  
/* System Call: int filesize (int fd)
   Returns the size, in bytes, of the file open as fd. */

int filesize (int fd) {
  if(fd == 1 || fd == 0) {    
    return -1;
  }
  struct thread* t = thread_current();
  if(t->fdCap <= fd-2 || fd < 0) {
    return -1;
  }
  int ret = -1;
  lock_acquire(&file_system_lock);
  if(thread_current()->fdTable[fd-2] != NULL) {
    struct file* fileToWrite = thread_current()->fdTable[fd-2]->ptr.asFile;
    if(fileToWrite != NULL) {
      ret = inode_length(fileToWrite->inode);
    }
  }
  lock_release(&file_system_lock);
  return ret;
}
  
/* System Call: int read (int fd, void *buffer, unsigned size)
   Reads size bytes from the file open as fd into buffer. 
   Returns the number of bytes actually read (0 at end of file), or -1 if the file could not be read (due to a condition other than end of file). 
   Fd 0 reads from the keyboard using input_getc(). */

int read (int fd, void *buffer, unsigned size) {
  set_page_to_accessed(buffer);
  set_page_to_dirty(buffer);
  if(fd == 0) {
    // sysin
    unsigned int indexOfBuffer = 0;
    char currentChar = 0;
    char* bufStr = (char*)buffer;
    while(indexOfBuffer < size - 1) {      
      currentChar = input_getc();
      if(currentChar == 10) {
	bufStr[indexOfBuffer] = '\0';
	return indexOfBuffer;
      }
      bufStr[indexOfBuffer] = currentChar;
      indexOfBuffer++;
    }
    return size;
    
  }
  if(fd == 1 || fd < 0) {
    return -1;
  }
  struct thread* t = thread_current();
  if(t->fdCap <= fd-2 || fd < 0) {
    return -1;
  }
  struct file* fileToRead = thread_current()->fdTable[fd-2]->ptr.asFile;
  int ret = -1;
  lock_acquire(&file_system_lock);
  if(fileToRead != NULL && !fileToRead->deny_write) {
    struct thread* t = thread_current();
    void* bufferEnd = (buffer + size);
    for(void* i = (void*)((uint32_t)buffer & 0xFFFFF000); i <= bufferEnd; i += 4096 ) {
      pin_page(i, t->tid);
    }
    ret = file_read(fileToRead, buffer, size);    
    for(void* i = (void*)((uint32_t)buffer & 0xFFFFF000); i <= bufferEnd; i += 4096 ) {
      unpin_page(i, t->tid);
    }
  }
  lock_release(&file_system_lock);
  
  return ret;
}

/* System Call: int write (int fd, const void *buffer, unsigned size)
   Writes size bytes from buffer to the open file fd. 
   Returns the number of bytes actually written, which may be less than size if some bytes could not be written.

   Writing past end-of-file would normally extend the file, but file growth is not implemented by the basic file system. 
   The expected behavior is to write as many bytes as possible up to end-of-file and return the actual number written, or 0 if no bytes could be written at all.

   Fd 1 writes to the console. Your code to write to the console should write all of buffer in one call to putbuf(), at least as long as size is not bigger than a few hundred bytes. 
   (It is reasonable to break up larger buffers.) Otherwise, lines of text output by different processes may end up interleaved on the console, confusing both human readers and our grading scripts. */

int write (int fd, const void *buffer, unsigned size) {
  set_page_to_accessed(buffer);
  if(fd == 1) {
    putbuf(buffer, size);
    return size;
  }
  if(fd == 0) {
    return -1;
  }

  struct thread* t = thread_current();
  if(t->fdCap <= fd-2 || fd < 0) {
    return -1;
  }
  
  struct file* fileToWrite = thread_current()->fdTable[fd-2]->ptr.asFile;
  int ret = 0;
  lock_acquire(&file_system_lock);
  if(fileToWrite != NULL) {
    if(!fileToWrite->deny_write) {
      ret = file_write (fileToWrite, buffer, size);
    }
  }
  lock_release(&file_system_lock);
  return ret;
}

/*System Call: void seek (int fd, unsigned position)
  Changes the next byte to be read or written in open file fd to position, expressed in bytes from the beginning of the file. (Thus, a position of 0 is the file's start.)

  A seek past the current end of a file is not an error. 
  A later read obtains 0 bytes, indicating end of file. 
  A later write extends the file, filling any unwritten gap with zeros. 
  (However, in Pintos files have a fixed length until project 4 is complete, so writes past end of file will return an error.) 
  These semantics are implemented in the file system and do not require any special effort in system call implementation.*/

void seek (int fd, unsigned position) {
  if(fd == 1 || fd == 0) {    
    return;
  }
  struct thread* t = thread_current();
  if(t->fdCap <= fd-2 || fd < 0) {
    return;
  }
  lock_acquire(&file_system_lock);
  struct file* fileToSeek = thread_current()->fdTable[fd-2]->ptr.asFile;
  fileToSeek->pos = position;  
  lock_release(&file_system_lock);

}

/* System Call: unsigned tell (int fd)
   Returns the position of the next byte to be read or written in open file fd, expressed in bytes from the beginning of the file. */

unsigned tell (int fd) {
  if(fd == 1 || fd == 0) {    
    return 0;
  }
  struct thread* t = thread_current();
  if(t->fdCap <= fd-2 || fd < 0) {
    return 0;
  }
  int ret = 0;
  lock_acquire(&file_system_lock);
  struct file* fileToWrite = thread_current()->fdTable[fd-2]->ptr.asFile;
  if(fileToWrite != NULL) {
    ret = file_tell(fileToWrite);
  }
  lock_release(&file_system_lock);
  return ret;
}

/*System Call: void close (int fd)
  Closes file descriptor fd. Exiting or terminating a process implicitly closes all its open file descriptors, as if by calling this function for each one. */

void close (int fd) {
  if(fd == 1 || fd == 0) {    
    return;
  }
  struct thread* t = thread_current();
  if(t->fdCap <= fd-2 || fd < 0) {
    return;
  }
  if(thread_current()->fdTable[fd-2] == NULL) {
    return;
  }
  lock_acquire(&file_system_lock);
  struct file* fileToWrite = thread_current()->fdTable[fd-2]->ptr.asFile;
  if(fileToWrite != NULL) {
    if(fileToWrite->deny_write) {
      file_allow_write(fileToWrite);
    }
    file_close(fileToWrite);
    thread_current()->fdTable[fd-2] = NULL;
  }
  lock_release(&file_system_lock);
}

bool chdir(const char* dir) {
  struct dir* temp = filesys_get_dir (dir, thread_current()->pwd);
  if(dir != NULL) {
    thread_current()->pwd = temp;
  }
  return temp != NULL;
}

bool mkdir(const char *dir) {
  return filesys_create_dir (dir, thread_current()->pwd);
}

bool readdir(int fd, char* name) {
  return false;
}

bool isdir(int fd)  {
  return false;
}

int inumber(int fd) {
  return -1;
}
