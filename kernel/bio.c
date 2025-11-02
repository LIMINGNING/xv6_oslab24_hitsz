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

#define NBUCKET 13  // 使用质数减少哈希冲突

struct bucket {
  struct spinlock lock;
  struct buf head;     // 哈希桶链表头节点
};

struct {
  struct spinlock lock;      // 全局分配锁，用于跨桶操作
  struct buf buf[NBUF];      // 所有缓冲区
  struct bucket buckets[NBUCKET];  // 哈希桶数组
} bcache;

// 静态存储锁名
static char bucket_names[NBUCKET][16];

// 哈希函数：使用blockno对桶数取模
static int
hash(uint blockno)
{
  return blockno % NBUCKET;
}

void
binit(void)
{
  struct buf *b;
  int i;

  // 初始化全局分配锁
  initlock(&bcache.lock, "bcache");

  // 初始化每个哈希桶
  for(i = 0; i < NBUCKET; i++) {
    snprintf(bucket_names[i], sizeof(bucket_names[i]), "bcache_%d", i);
    initlock(&bcache.buckets[i].lock, bucket_names[i]);
    
    // 初始化桶的头节点
    bcache.buckets[i].head.prev = &bcache.buckets[i].head;
    bcache.buckets[i].head.next = &bcache.buckets[i].head;
  }

  // 将所有缓冲区分配到第0个桶中
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.buckets[0].head.next;
    b->prev = &bcache.buckets[0].head;
    initsleeplock(&b->lock, "buffer");
    bcache.buckets[0].head.next->prev = b;
    bcache.buckets[0].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct bucket *bucket;
  int bucket_id;

  bucket_id = hash(blockno);
  bucket = &bcache.buckets[bucket_id];

  // 首先在目标桶中查找
  acquire(&bucket->lock);
  
  // 检查块是否已在缓存中
  for(b = bucket->head.next; b != &bucket->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 未找到，需要分配新的缓冲区
  // 先在当前桶中查找空闲缓冲区
  for(b = bucket->head.prev; b != &bucket->head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 当前桶没有空闲缓冲区，需要从其他桶窃取
  release(&bucket->lock);

  // 使用全局锁进行跨桶操作，避免死锁
  acquire(&bcache.lock);
  
  // 重新检查目标桶（可能在等待期间已被其他进程添加）
  acquire(&bucket->lock);
  for(b = bucket->head.next; b != &bucket->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket->lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bucket->lock);

  // 从其他桶窃取空闲缓冲区
  for(int i = 0; i < NBUCKET; i++) {
    if(i == bucket_id) continue;
    
    struct bucket *src_bucket = &bcache.buckets[i];
    acquire(&src_bucket->lock);
    
    for(b = src_bucket->head.prev; b != &src_bucket->head; b = b->prev){
      if(b->refcnt == 0) {
        // 从源桶中移除
        b->next->prev = b->prev;
        b->prev->next = b->next;
        
        // 设置新的设备和块号
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        
        release(&src_bucket->lock);
        
        // 添加到目标桶
        acquire(&bucket->lock);
        b->next = bucket->head.next;
        b->prev = &bucket->head;
        bucket->head.next->prev = b;
        bucket->head.next = b;
        release(&bucket->lock);
        
        release(&bcache.lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&src_bucket->lock);
  }
  
  release(&bcache.lock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
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
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  struct bucket *bucket;
  int bucket_id;

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  bucket_id = hash(b->blockno);
  bucket = &bcache.buckets[bucket_id];

  acquire(&bucket->lock);
  b->refcnt--;
  if (b->refcnt == 0) { // refcnt 引用数
    // 将缓冲区移到链表头部（最近使用）
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bucket->head.next;
    b->prev = &bucket->head;
    bucket->head.next->prev = b;
    bucket->head.next = b;
  }
  release(&bucket->lock);
}

void
bpin(struct buf *b) {
  struct bucket *bucket;
  int bucket_id;

  bucket_id = hash(b->blockno);
  bucket = &bcache.buckets[bucket_id];

  acquire(&bucket->lock);
  b->refcnt++;
  release(&bucket->lock);
}

void
bunpin(struct buf *b) {
  struct bucket *bucket;
  int bucket_id;

  bucket_id = hash(b->blockno);
  bucket = &bcache.buckets[bucket_id];

  acquire(&bucket->lock);
  b->refcnt--;
  release(&bucket->lock);
}


