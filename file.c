//
// File descriptors
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "fs.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE)
    pipeclose(ff.pipe, ff.writable);
  else if(ff.type == FD_INODE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
int
filestat(struct file *f, struct stat *st)
{
  if(f->type == FD_INODE){
    ilock(f->ip);
    stati(f->ip, st);
    iunlock(f->ip);
    return 0;
  }
  return -1;
}

// Read from file f.
int
fileread(struct file *f, char *addr, int n)
{
  int r;

  if(f->readable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return piperead(f->pipe, addr, n);
  if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
    return r;
  }
  panic("fileread");
}

//PAGEBREAK!
// Write to file f.
int
filewrite(struct file *f, char *addr, int n)
{
  int r;

  if(f->writable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return pipewrite(f->pipe, addr, n);
  if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * 512;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r < 0)
        break;
      if(r != n1)
        panic("short filewrite");
      i += r;
    }
    return i == n ? n : -1;
  }
  panic("filewrite");
}

struct sleeplock offsetlock;

int pread(int fd, void *addr, int n, int off) {
  struct proc *p = myproc();
  int old_offset;
  int retval;

  if(fd < 0 || fd >= NOFILE || (p->ofile[fd]) == 0)
    return -1;

  acquiresleep(&offsetlock);
  // save old offset
  old_offset = p->ofile[fd]->off;

  // set temp offset
  p->ofile[fd]->off = off;

  // read
  retval = fileread(p->ofile[fd], (char*)addr, n);

  // restore offset
  p->ofile[fd]->off = old_offset;
  releasesleep(&offsetlock);

  return retval;
}

int pwrite(int fd, void *addr, int n, int off) {
  struct proc *p = myproc();
  int old_offset;
  int retval;
  
  if(fd < 0 || fd >= NOFILE || (p->ofile[fd]) == 0)
    return -1;

  acquiresleep(&offsetlock);
  // save old offset
  old_offset = p->ofile[fd]->off;

  // set temp offset
  p->ofile[fd]->off = off;

  // write
  retval = filewrite(p->ofile[fd], (char*)addr, n);

  // restore offset
  p->ofile[fd]->off = old_offset;
  releasesleep(&offsetlock);

  return retval;
}

int sys_pread(void) {
  int fd, addr, n, off;
  if(argint(0, &fd) < 0 || argint(1, &addr) < 0 || argint(2, &n) < 0 ||
      argint(3, &off) < 0) {
      return -1;
      }
  return pread(fd, (void*)addr, n, off);
}

int sys_pwrite(void) {
  int fd, addr, n, off;
  if(argint(0, &fd) < 0 || argint(1, &addr) < 0 || argint(2, &n) < 0 ||
      argint(3, &off) < 0) {
      return -1;
      }
  return pwrite(fd, (void*)addr, n, off);
}