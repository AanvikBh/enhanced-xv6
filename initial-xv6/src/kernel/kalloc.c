// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  struct run *freelist;
} kmem;

#ifdef COW
struct cow_info page_details[PAGE_COUNT];
struct spinlock page_cow_lock;
#endif

void kinit()
{

  // printf("here\n");
#ifdef COW
  initlock(&page_cow_lock, "page_cow");
  // printf("here\n");
  acquire(&page_cow_lock);
  // printf("here\n");
  for (int i = 0; i < PAGE_COUNT; i++)
  {
    page_details[i].numReferences = 1;
    page_details[i].page_number = i;
    page_details[i].p = 0;
  }
  // printf("here\n");
  release(&page_cow_lock);

#endif
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

#ifdef COW
  acquire(&page_cow_lock);
  int page_num = ((uint64)pa / PGSIZE);
  if (page_details[page_num].numReferences <= 0)
  {
    panic("kfree: no references");
  }
  page_details[page_num].numReferences--;

  if (page_details[page_num].numReferences > 0)
  {
    release(&page_cow_lock);
    return;
  }
  release(&page_cow_lock);
#endif
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r)
  {
    #ifdef COW
      acquire(&page_cow_lock);
      int page_num = ((uint64)r / PGSIZE);
      page_details[page_num].numReferences++;

      // if(page_details[page_num].numReferences>0){
      //   release(&page_cow_lock);
      //   return;
      // }
      release(&page_cow_lock);
    #endif
    memset((char *)r, 5, PGSIZE); // fill with junk
  }

  return (void *)r;
}
