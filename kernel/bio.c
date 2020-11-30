// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"


#define NBUCKETS 13

struct {
  struct spinlock lock[NBUCKETS];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // head.next is most recently used.
  struct buf head[NBUCKETS];
} bcache;

void
binit(void)
{
  struct buf *b;
  int n = NBUF / NBUCKETS;
  int cur = 0;

  for(int i=0;i<NBUCKETS;i++)
    initlock(&bcache.lock[i], "bcache");

  // Create linked list of buffers
  for(int i=0; i<NBUCKETS; i++){
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }

  // Create linked list of buffers
  for(int i=0;i<NBUCKETS-1;i++){
      bcache.head[i].prev = &bcache.head[i];
      bcache.head[i].next = &bcache.head[i];
      for(b = &bcache.buf[i*n];b<&bcache.buf[(i+1)*n];b++){
        // printf("here!\n");
        b->next = bcache.head[i].next;
        b->prev = &bcache.head[i];
        initsleeplock(&b->lock, "buffer");
        bcache.head[i].next->prev = b;
        bcache.head[i].next = b;
      }
      cur = i+1;
  }
  for(;b<bcache.buf+NBUF;b++){
    b->next = bcache.head[cur].next;
    b->prev = &bcache.head[cur];
    initsleeplock(&b->lock,"buffer");
    bcache.head[cur].next->prev = b;
    bcache.head[cur].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  // printf("bget\n");
  struct buf *b;
  int key = blockno % NBUCKETS;  //blockno 对 NBUCKETS取余，得到哈希KEY

  acquire(&bcache.lock[key]);
  // printf("aquire key lock0\n");

  // Is the block already cached?
  for(b = bcache.head[key].next; b != &bcache.head[key]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[key]);
      // printf("release key lock0\n");
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached; recycle an unused buffer.
  for(b = bcache.head[key].prev; b != &bcache.head[key] ; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      // printf("release key lock1\n");
      release(&bcache.lock[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  //如果没有找到这样的buf，就从其它桶中窃取
 for(int i = (key+1) % NBUCKETS;i != key; i = (i+1) % NBUCKETS){
    acquire(&bcache.lock[i]);
    for(b = bcache.head[i].prev; b != &bcache.head[i];b = b->prev){
      if(b->refcnt == 0){
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        //从当前桶移除
        b->prev->next = b->next;
        b->next->prev = b->prev;
        release(&bcache.lock[i]);
        //加入当前key对应的桶
        b->prev = &bcache.head[key];
        b->next = bcache.head[key].next;
        bcache.head[key].next->prev = b;
        bcache.head[key].next = b;
        // printf("release key lock 2\n");
        release(&bcache.lock[key]);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.lock[i]);
  }
  // printf("release key lock3\n");
  release(&bcache.lock[key]);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b->dev, b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b->dev, b, 1);
}

// Release a locked buffer.
// Move to the head of the MRU list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  int key = b->blockno % NBUCKETS;

  acquire(&bcache.lock[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev; //remove from list
    b->prev->next = b->next;
    b->next = bcache.head[key].next;
    b->prev = &bcache.head[key];
    bcache.head[key].next->prev = b;
    bcache.head[key].next = b;
  }
  
  release(&bcache.lock[key]);
}

void
bpin(struct buf *b) {
  int key = b->blockno % NBUCKETS;
  acquire(&bcache.lock[key]);
  b->refcnt++;
  release(&bcache.lock[key]);
}

void
bunpin(struct buf *b) {
  int key = b->blockno % NBUCKETS;
  acquire(&bcache.lock[key]);
  b->refcnt--;
  release(&bcache.lock[key]);
}


