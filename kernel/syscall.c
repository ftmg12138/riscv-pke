/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "proc_file.h"

#include "spike_interface/spike_utils.h"

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in direct mapping).
  assert( current );
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)buf);
  sprint(pa);
  return 0;
}


// 将相对路径转换为绝对路径
void sys_user_relative_path_to_absolute(char *relativepath, char *absolutepath) {
  struct dentry* current_dir = current->pfiles->cwd; // 当前工作目录

  if (relativepath[0] == '.' && relativepath[1] == '.') {
    // 处理相对路径以../开头的情况，向上一级目录
    current_dir = current_dir->parent;
    relativepath += 2; // 移动相对路径指针
  }

  // 初始化绝对路径
  memset(absolutepath, '\0', MAX_DENTRY_NAME_LEN);
  strcpy(absolutepath, "/"); // 初始绝对路径为根目录

  while (current_dir) {
    // 拷贝当前目录名到绝对路径中
    strcat(absolutepath, current_dir->name);

    if (current_dir->parent != NULL)
      strcat(absolutepath, "/");

    // 移动到父目录
    current_dir = current_dir->parent;
  }

  // 处理相对路径中的./和../
  if (relativepath[0] == '.') {
    if (relativepath[1] == '.') {
      // ../ 相对路径
      strcat(absolutepath, relativepath + 3);
    } else {
      // ./ 相对路径
      strcat(absolutepath, relativepath + 2);
    }
  } else {
    // 处理其他相对路径
    strcat(absolutepath, relativepath);
  }
}



//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  // reclaim the current process, and reschedule. added @lab3_1
  free_process( current );
  schedule();
  return 0;
}

//
// maybe, the simplest implementation of malloc in the world ... added @lab2_2
//
uint64 sys_user_allocate_page() {
  void* pa = alloc_page();
  uint64 va;
  // if there are previously reclaimed pages, use them first (this does not change the
  // size of the heap)
  if (current->user_heap.free_pages_count > 0) {
    va =  current->user_heap.free_pages_address[--current->user_heap.free_pages_count];
    assert(va < current->user_heap.heap_top);
  } else {
    // otherwise, allocate a new page (this increases the size of the heap by one page)
    va = current->user_heap.heap_top;
    current->user_heap.heap_top += PGSIZE;

    current->mapped_info[HEAP_SEGMENT].npages++;
  }
  user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
         prot_to_type(PROT_WRITE | PROT_READ, 1));

  return va;
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_free_page(uint64 va) {
  user_vm_unmap((pagetable_t)current->pagetable, va, PGSIZE, 1);
  // add the reclaimed page to the free page list
  current->user_heap.free_pages_address[current->user_heap.free_pages_count++] = va;
  return 0;
}

//
// kerenl entry point of naive_fork
//
ssize_t sys_user_fork() {
  sprint("User call fork.\n");
  return do_fork( current );
}

//
// kerenl entry point of yield. added @lab3_2
//
ssize_t sys_user_yield() {
  // TODO (lab3_2): implment the syscall of yield.
  // hint: the functionality of yield is to give up the processor. therefore,
  // we should set the status of currently running process to READY, insert it in
  // the rear of ready queue, and finally, schedule a READY process to run.
  current->status = READY;
  insert_to_ready_queue(current);
  schedule();
  return 0;

  return 0;
}

//
// open file
//
ssize_t sys_user_open(char *pathva, int flags) {
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), pathva);

  if (pa[0] == '.') {
    // 如果路径以"."开头，将其转换为绝对路径
    char absolute[MAX_DENTRY_NAME_LEN];
    memset(absolute, '\0', MAX_DENTRY_NAME_LEN); 
    sys_user_relative_path_to_absolute(pa, absolute);
    return do_open(absolute, flags);
  } else {
    // 否则，直接使用给定的路径
    return do_open(pa, flags);
  }
}


//
// read file
//
ssize_t sys_user_read(int fd, char *bufva, uint64 count) {
  int i = 0;
  while (i < count) { // count can be greater than page size
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_read(fd, (char *)pa + off, len);
    i += r; if (r < len) return i;
  }
  return count;
}

//
// write file
//
ssize_t sys_user_write(int fd, char *bufva, uint64 count) {
  int i = 0;
  while (i < count) { // count can be greater than page size
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_write(fd, (char *)pa + off, len);
    i += r; if (r < len) return i;
  }
  return count;
}

//
// lseek file
//
ssize_t sys_user_lseek(int fd, int offset, int whence) {
  return do_lseek(fd, offset, whence);
}

//
// read vinode
//
ssize_t sys_user_stat(int fd, struct istat *istat) {
  struct istat * pistat = (struct istat *)user_va_to_pa((pagetable_t)(current->pagetable), istat);
  return do_stat(fd, pistat);
}

//
// read disk inode
//
ssize_t sys_user_disk_stat(int fd, struct istat *istat) {
  struct istat * pistat = (struct istat *)user_va_to_pa((pagetable_t)(current->pagetable), istat);
  return do_disk_stat(fd, pistat);
}

//
// close file
//
ssize_t sys_user_close(int fd) {
  return do_close(fd);
}

//
// lib call to opendir
//
ssize_t sys_user_opendir(char * pathva){
  char * pathpa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), pathva);
  return do_opendir(pathpa);
}

//
// lib call to readdir
//
ssize_t sys_user_readdir(int fd, struct dir *vdir){
  struct dir * pdir = (struct dir *)user_va_to_pa((pagetable_t)(current->pagetable), vdir);
  return do_readdir(fd, pdir);
}

//
// lib call to mkdir
//
ssize_t sys_user_mkdir(char * pathva){
  char * pathpa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), pathva);
  return do_mkdir(pathpa);
}

//
// lib call to closedir
//
ssize_t sys_user_closedir(int fd){
  return do_closedir(fd);
}

//
// lib call to link
//
ssize_t sys_user_link(char * vfn1, char * vfn2){
  char * pfn1 = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)vfn1);
  char * pfn2 = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)vfn2);
  return do_link(pfn1, pfn2);
}


// 这个函数用于将当前工作目录路径转换成绝对路径并存储在参数 path_now 中
void sys_user_rcwd(char * path_now) {
  // 将用户态地址转换为内核态地址
  char * pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), path_now);

  // 初始化用于构建路径的临时变量
  char path[MAX_DEVICE_NAME_LEN];
  memset(path, '\0', MAX_DEVICE_NAME_LEN);

  // 获取当前工作目录
  struct dentry* cwd = current->pfiles->cwd;

  // 初始化绝对路径字符串
  memset(pa, '\0', MAX_DENTRY_NAME_LEN);

  // 如果当前工作目录是根目录
  if (cwd->parent == NULL) {
    strcpy(pa, "/");
  } else {
    // 循环向上遍历目录树以构建绝对路径
    while (cwd->parent != NULL) {
      // 将目录名添加到路径中
      strcat(path, "/");
      strcat(path, cwd->name);
      cwd = cwd->parent;
    }
    // 将构建好的绝对路径拷贝到 pa 中
    strcpy(pa, path);
  }
}

// 这个函数用于切换当前工作目录到指定路径
void sys_user_ccwd(char * path_to) {
  // 将用户态地址转换为内核态地址
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), path_to);

  // 初始化用于存储绝对路径的变量
  char path[MAX_DEVICE_NAME_LEN];
  memset(path, '\0', MAX_DEVICE_NAME_LEN);

  // 将相对路径转换为绝对路径
  sys_user_relative_path_to_absolute(pa, path);

  // 打开新目录
  int tmp = do_opendir(path);

  // 将当前工作目录切换到新路径
  current->pfiles->cwd = current->pfiles->opened_files[tmp].f_dentry;

  // 关闭目录句柄
  do_closedir(tmp);
}



//
// lib call to unlink
//
ssize_t sys_user_unlink(char * vfn){
  char * pfn = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)vfn);
  return do_unlink(pfn);
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    // added @lab2_2
    case SYS_user_allocate_page:
      return sys_user_allocate_page();
    case SYS_user_free_page:
      return sys_user_free_page(a1);
    case SYS_user_fork:
      return sys_user_fork();
    case SYS_user_yield:
      return sys_user_yield();
    // added @lab4_1
    case SYS_user_open:
      return sys_user_open((char *)a1, a2);
    case SYS_user_read:
      return sys_user_read(a1, (char *)a2, a3);
    case SYS_user_write:
      return sys_user_write(a1, (char *)a2, a3);
    case SYS_user_lseek:
      return sys_user_lseek(a1, a2, a3);
    case SYS_user_stat:
      return sys_user_stat(a1, (struct istat *)a2);
    case SYS_user_disk_stat:
      return sys_user_disk_stat(a1, (struct istat *)a2);
    case SYS_user_close:
      return sys_user_close(a1);
    // added @lab4_2
    case SYS_user_opendir:
      return sys_user_opendir((char *)a1);
    case SYS_user_readdir:
      return sys_user_readdir(a1, (struct dir *)a2);
    case SYS_user_mkdir:
      return sys_user_mkdir((char *)a1);
    case SYS_user_closedir:
      return sys_user_closedir(a1);
    // added @lab4_3
    case SYS_user_link:
      return sys_user_link((char *)a1, (char *)a2);
    case SYS_user_unlink:
      return sys_user_unlink((char *)a1);
    case SYS_user_rcwd:
      sys_user_rcwd((char *)a1);
      return 0;
    case SYS_user_ccwd:
      sys_user_ccwd((char *)a1);
      return 0;
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
