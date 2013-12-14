// SSD-Assisted Hybrid Memory.
// Author: Xiangyong Ouyang (neutronsharc@gmail.com)
// Created on: 2011-11-11

#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "avl.h"
#include "hybrid_memory.h"
#include "hybrid_memory_const.h"
#include "vaddr_range.h"
#include "page_cache.h"

bool PageCache::Init(HybridMemory* hmem,
                     const std::string& name,
                     uint64_t max_cache_size) {
  max_cache_size_ = RoundUpToPageSize(max_cache_size);
  assert(max_cache_size_ > 0);
  bool page_align = true;
  bool pin_memory = true;
  uint32_t payload_data = 0;
  assert(item_list_.Init(name + "-itemlist",
                         max_cache_size_ >> PAGE_BITS,
                         payload_data,
                         page_align,
                         pin_memory) == true);
  hybrid_memory_ = hmem;
  name_ = name;
  return true;
}

bool PageCache::Release() {
  if (ready_) {
    if (queue_.size() > 0) {
      // TODO: release pages still in the queue.
      dbg("Has %ld pages in page-cache\n", queue_.size());
    }
    item_list_.Release();
    ready_ = false;
  }
  return true;
}

uint32_t PageCache::EvictItems() {
  // Try to free up this many cached pages.
  uint32_t to_release = 10;
  uint32_t released = 0;
  for (released = 0; queue_.size() > 0 && released < to_release; ++released) {
    PageCacheItem* olditem = (PageCacheItem*)queue_.front();
    queue_.pop();
    assert(olditem != NULL);
    hybrid_memory_->GetRAMCache()->AddPage(olditem->page,
                                           olditem->size,
                                           olditem->v2hmap->dirty_page_cache,
                                           olditem->v2hmap,
                                           olditem->vaddr_range_id);
    olditem->v2hmap->exist_page_cache = 0;
    olditem->v2hmap->dirty_page_cache = 0;
    assert(madvise(olditem->page, olditem->size, MADV_DONTNEED) == 0);
    assert(mprotect(olditem->page, olditem->size, PROT_NONE) == 0);
    item_list_.Free(olditem);
  }
  return released;
}

bool PageCache::AddPage(void* page,
                        uint32_t size,
                        bool is_dirty,
                        V2HMapMetadata* v2hmap,
                        uint32_t vaddr_range_id) {
  PageCacheItem* item = item_list_.New();
  if (item == NULL) {
    assert(EvictItems() > 0);
    item = item_list_.New();
  }
  assert(item != NULL);
  item->page = page;
  item->size = size;
  item->vaddr_range_id = vaddr_range_id;
  item->v2hmap = v2hmap;
  item->v2hmap->exist_page_cache = 1;
  item->v2hmap->dirty_page_cache = is_dirty ? 1 : 0;
  queue_.push(item);
  return true;
}
