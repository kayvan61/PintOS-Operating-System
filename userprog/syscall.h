#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/init.h"
#include "lib/syscall-nr.h"

void syscall_init (void);

void halt (void);
void exit(int status);
int wait (tid_t pid);
int write (int fd, const void *buffer, unsigned size);
tid_t exec (const char *cmd_line);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned size);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);
bool chdir(const char* dir);
bool mkdir(const char* dir);
bool readdir(int fd, char* name);
bool isdir(int fd);
int inumber(int fd);

#endif /* userprog/syscall.h */
