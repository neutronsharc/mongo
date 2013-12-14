// SSD-Assisted Hybrid Memory.
// Author: Xiangyong Ouyang (neutronsharc@gmail.com)
// Created on: 2011-11-11

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "asyncio_manager.h"
#include "asyncio_request.h"
#include "debug.h"
#include "flash_cache.h"
#include "hash_table.h"
#include "hybrid_memory.h"
#include "hybrid_memory_inl.h"
#include "hybrid_memory_const.h"
#include "ram_cache.h"
#include "page_cache.h"
#include "utils.h"
#include "vaddr_range.h"

bool FlashCache::Init(HybridMemory* hmem,
                      const std::string& name,
                      const std::string& flash_filename,
                      uint64_t max_flash_size) {
  assert(ready_ == false);
  uint64_t total_flash_pages = RoundUpToPageSize(max_flash_size) / PAGE_SIZE;

  uint64_t alignment = PAGE_SIZE;
  uint64_t map_byte_size = total_flash_pages * sizeof(F2VMapItem);
  // Init the F2V mapping table.
  assert(posix_memalign((void**)&f2v_map_, alignment, map_byte_size) == 0);
  assert(mlock(f2v_map_, map_byte_size) == 0);
  for (uint64_t i = 0; i < total_flash_pages; ++i) {
    f2v_map_[i].vaddress_range_id = INVALID_VADDRESS_RANGE_ID;
  }
  // Prepare the aux-buffer.
  aux_buffer_size_ = PAGE_SIZE << VADDRESS_CHUNK_BITS;
  assert(posix_memalign((void**)&aux_buffer_, alignment, aux_buffer_size_)
         == 0);
  assert(mlock(aux_buffer_, aux_buffer_size_) == 0);
  for (uint8_t* buf = aux_buffer_; buf < aux_buffer_ + aux_buffer_size_;
       buf += PAGE_SIZE) {
    aux_buffer_list_.push_back(buf);
  }
  // Init the page allocation table.
  assert(page_allocate_table_.Init(name + "-pg-alloc-table",
                                   total_flash_pages) == true);
  // Init the page access history table.
  assert(page_stats_table_.Init(name + "-pg-stats-table", total_flash_pages) ==
         true);
  // Open the flash-cache file.
  flash_fd_ =
      open(flash_filename.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_DIRECT, 0666);
  if (flash_fd_ <= 0) {
    err("Unable to open flash file: %s\n", flash_filename.c_str());
    perror("Open file error.");
    assert(0);
  }
  flash_file_size_ = total_flash_pages * PAGE_SIZE;
  assert(ftruncate(flash_fd_, flash_file_size_) == 0);
  dbg("Has opened flash-cache file: %s, size = %ld, %ld flash-pages\n",
      flash_filename.c_str(), flash_file_size_, total_flash_pages);

  hybrid_memory_ = hmem;
  flash_filename_ = flash_filename;
  name_ = name;
  total_flash_pages_ = total_flash_pages;
  hits_count_ = 0;
  overflow_pages_ = 0;
  max_evict2hdd_latency_usec_ = 0;
  total_evict2hdd_pages_ = 0;
  ready_ = true;
  return ready_;
}

void FlashCache:: Release() {
  if (ready_) {
    page_allocate_table_.Release();
    page_stats_table_.Release();
    close(flash_fd_);
    free(f2v_map_);
    free(aux_buffer_);
    aux_buffer_ = NULL;
    f2v_map_ = NULL;
    ready_ = false;
    hybrid_memory_ = NULL;
  }
}

static void MoveToHDDWriteCompletion(AsyncIORequest *request, int result,
                                     void *param1, void *param2) {
  if (result != (int)request->size()) {
    // TODO: handle failure.
    err("Write op failed\n");
  }
  std::vector<uint8_t *> *buffer_list =
      static_cast<std::vector<uint8_t *> *>(param2);
  buffer_list->push_back((uint8_t*)(request->buffer()));

  V2HMapMetadata *v2hmap = (V2HMapMetadata *)param1;
  v2hmap->dirty_flash_cache = 0;
  v2hmap->exist_flash_cache = 0;
  v2hmap->exist_hdd_file = 1;
}

static void MoveToHDDReadCompletion(AsyncIORequest *request, int result,
                                    void *param1, void *param2) {
  if (result != (int)request->size()) {
    // TODO: handle failure.
    err("Read op failed\n");
  }
  AsyncIORequest* followup_request = (AsyncIORequest*)param1;
  uint64_t* copy_write_requests = (uint64_t*)param2;
  assert(request->asyncio_manager()->Submit(followup_request) == true);
  ++*copy_write_requests;
}

uint32_t FlashCache::MigrateToHDD(
    std::vector<uint64_t>& flash_pages_writeto_hdd) {
  assert(flash_pages_writeto_hdd.size() > 0);

  // TODO: select the "best" version of the page to write to hdd.
  // From ram-cache? from flash-cache?

  bool support_asyncio = hybrid_memory_->support_asyncio();
  //bool support_asyncio = false;
  AsyncIOManager* aio_manager = NULL;
  if (support_asyncio) {
    aio_manager = hybrid_memory_->asyncio_manager();
  }
  bool use_asyncio = support_asyncio;
  if (support_asyncio &&
      aio_manager->number_free_requests() <= flash_pages_writeto_hdd.size()) {
    use_asyncio = false;
  }
  uint64_t asyncio_copy_read_requests = 0;
  uint64_t asyncio_copy_write_requests = 0;
  uint64_t asyncio_completions = 0;
  std::vector<AsyncIORequest*> requests;

  uint64_t tstart = NowInUsec();
  for (uint64_t i = 0; i < flash_pages_writeto_hdd.size(); ++i) {
    uint64_t flash_page_number = flash_pages_writeto_hdd[i];
    F2VMapItem* f2vmap = &f2v_map_[flash_page_number];
    VAddressRange* vaddress_range =
        GetVAddressRangeFromId(f2vmap->vaddress_range_id);
    uint64_t vaddress_page_number = f2vmap->vaddress_page_offset;
    V2HMapMetadata* v2hmap =
        vaddress_range->GetV2HMapMetadata(vaddress_page_number << PAGE_BITS);
    void* virtual_page_address =
        vaddress_range->address() + (vaddress_page_number << PAGE_BITS);
    uint64_t hdd_file_offset =
        (vaddress_page_number << PAGE_BITS) + vaddress_range->hdd_file_offset();
    uint8_t* data_buffer;
    int io_size = PAGE_SIZE;
    if (flash_page_number == 12288) {
      dbg("%ld: flash-pg#: %ld, virt-page#: %ld at vaddr-range %d\n",
          i,
          flash_page_number,
          vaddress_page_number,
          f2vmap->vaddress_range_id);
    }
    if (v2hmap->dirty_page_cache) {
      // The virt-page has been materialized in page cache, and is being written
      // to. Don't write this page to hdd file because we don't know
      // if the update is complete.
      assert(v2hmap->exist_page_cache);
      dbg("flash page %ld: virt-page %p: exist in page-cache, but its "
          "flash-cache copy will be moved to hdd\n",
          i,
          virtual_page_address);
    } else if (v2hmap->dirty_ram_cache) {
      // This flash page has a copy in RAM-cache.
      // No need to flush it to hdd file.
      assert(v2hmap->exist_ram_cache);
      RAMCacheItem* ram_cache_item =
          hybrid_memory_->GetRAMCache()->GetItem(virtual_page_address);
      assert(ram_cache_item != NULL);
      assert(ram_cache_item->hash_key == virtual_page_address);
      assert(v2hmap == ram_cache_item->v2hmap);
      dbg("virt-page %p: exist in ram-cache, but its flash-cache copy "
          "will be moved to hdd\n",
          virtual_page_address);
#if 0
      data_buffer = ram_cache_item->data;
      assert(pwrite(vaddress_range->hdd_file_fd(),
                    data_buffer,
                    io_size,
                    hdd_file_offset) == io_size);
      hybrid_memory_->GetRAMCache()->Remove(ram_cache_item);
      v2hmap->exist_hdd_file = 1;
      v2hmap->exist_ram_cache = 0;
      v2hmap->dirty_ram_cachee = 0;
#endif
    } else if (v2hmap->dirty_flash_cache) {
      assert(v2hmap->exist_flash_cache);
      assert(aux_buffer_list_.size() > 0);
      data_buffer = aux_buffer_list_.back();
      aux_buffer_list_.pop_back();
      if (flash_page_number == 12288) {
        dbg("flash-page %ld: virt-page-number %ld: exist-dirty in flash-cache, "
            "moved to hdd position %ld\n",
            flash_page_number,
            vaddress_page_number,
            hdd_file_offset);
      }
      if (support_asyncio && use_asyncio) {
        AsyncIORequest *request  = aio_manager->GetRequest();
        AsyncIORequest *followup_request  = aio_manager->GetRequest();
        if (request == NULL || followup_request == NULL) {
          err("Insufficient aio requests.\n");
          assert(0);
        } else {
          request->Prepare(flash_fd_, data_buffer, io_size,
                           flash_page_number << PAGE_BITS, READ);
          followup_request->Prepare(vaddress_range->hdd_file_fd(), data_buffer,
                                    io_size, hdd_file_offset, WRITE);
          request->AddCompletionCallback(MoveToHDDReadCompletion,
                                         (void *)followup_request,
                                         &asyncio_copy_write_requests);
          followup_request->AddCompletionCallback(MoveToHDDWriteCompletion,
                                                  (void *)v2hmap,
                                                  (void *)(&aux_buffer_list_));
          requests.push_back(request);
        }
      } else {
        assert(pread(flash_fd_, data_buffer, io_size,
                     flash_page_number << PAGE_BITS) == io_size);
        assert(pwrite(vaddress_range->hdd_file_fd(), data_buffer, io_size,
                      hdd_file_offset) == io_size);

        aux_buffer_list_.push_back(data_buffer);
        v2hmap->dirty_flash_cache = 0;
        v2hmap->exist_flash_cache = 0;
        v2hmap->exist_hdd_file = 1;
      }
    }
  }
  if (support_asyncio && use_asyncio) {
    assert(aio_manager->Submit(requests) == true);
    asyncio_copy_read_requests += requests.size();
    uint64_t expire_time = NowInUsec() + 2 * 1000000;
    while (asyncio_completions < asyncio_copy_read_requests * 2) {
      // TODO: can usleep(1000) to reduce CPU overhead.
      asyncio_completions += aio_manager->Poll(1);
      if (NowInUsec() > expire_time) {
        break;
      }
    }
    if (asyncio_copy_read_requests + asyncio_copy_write_requests >
        asyncio_completions) {
      dbg("Timeout, issued %ld copy-read, %ld copy-write, got %ld "
          "completions\n",
          asyncio_copy_read_requests, asyncio_copy_write_requests,
          asyncio_completions);
    }
  }
  uint64_t latency_usec = NowInUsec() - tstart;
  if (latency_usec > max_evict2hdd_latency_usec_) {
    max_evict2hdd_latency_usec_ = latency_usec;
    evict2hdd_pages_ =  flash_pages_writeto_hdd.size();
  }
  return flash_pages_writeto_hdd.size();
}

uint32_t FlashCache::EvictItems(uint32_t pages_to_evict) {
  std::vector<uint64_t> pages;
  uint32_t evicted_pages =
      page_stats_table_.FindPagesWithMinCount(pages_to_evict, &pages);
  assert(evicted_pages > 0);

  // If have backing hdd file, shall write dirty pages to back hdd.
  std::vector<uint64_t> flash_pages_writeto_hdd;
  for (uint32_t i = 0; i < evicted_pages; ++i) {
    uint64_t flash_page_number = pages[i];
    // This evicted pages must have be associated to an owner
    // vaddress-range.
    F2VMapItem* f2vmap = &f2v_map_[flash_page_number];
    if (!IsValidVAddressRangeId(f2vmap->vaddress_range_id)) {
      err("flash page %ld: its f2vmap->vaddr_rang_id is invalid: %d\n",
          flash_page_number, f2vmap->vaddress_range_id);
      assert(0);
    }
    VAddressRange* vaddress_range =
        GetVAddressRangeFromId(f2vmap->vaddress_range_id);
    uint64_t vaddress_page_number = f2vmap->vaddress_page_offset;
    V2HMapMetadata* v2hmap =
        vaddress_range->GetV2HMapMetadata(vaddress_page_number << PAGE_BITS);
    if (v2hmap->dirty_flash_cache && (vaddress_range->hdd_file_fd() > 0)) {
      flash_pages_writeto_hdd.push_back(flash_page_number);
    }
  }
  if (flash_pages_writeto_hdd.size() > 0) {
    MigrateToHDD(flash_pages_writeto_hdd);
  }

  for (uint32_t i = 0; i < evicted_pages; ++i) {
    uint64_t flash_page_number = pages[i];
    if (page_allocate_table_.IsPageFree(flash_page_number)) {
      err("will free flash page %ld but it's already freed!\n",
          flash_page_number);
      assert(0);
    }
    F2VMapItem* f2vmap = &f2v_map_[flash_page_number];
    if (!IsValidVAddressRangeId(f2vmap->vaddress_range_id)) {
      err("will free flash page %ld, but its f2vmap->vaddr_rang_id "
          "is invalid: %d\n",
          flash_page_number,
          f2vmap->vaddress_range_id);
      assert(0);
    }
    page_allocate_table_.FreePage(flash_page_number);
    VAddressRange* vaddress_range =
        GetVAddressRangeFromId(f2vmap->vaddress_range_id);
    uint64_t vaddress_page_number = f2vmap->vaddress_page_offset;
    V2HMapMetadata* v2hmap =
        vaddress_range->GetV2HMapMetadata(vaddress_page_number << PAGE_BITS);
    v2hmap->dirty_flash_cache = 0;
    v2hmap->exist_flash_cache = 0;

    f2vmap->vaddress_range_id = INVALID_VADDRESS_RANGE_ID;
    f2vmap->vaddress_page_offset = 0;
  }
  return evicted_pages;
}

bool FlashCache::AddPage(void* page,
                         uint64_t obj_size,
                         bool is_dirty,
                         V2HMapMetadata* v2hmap,
                         uint32_t vaddress_range_id,
                         void* virtual_page_address) {
  assert(IsValidVAddressRangeId(vaddress_range_id));
  assert(obj_size == PAGE_SIZE);
  uint64_t vaddress_page_offset =
      GetPageOffsetInVAddressRange(vaddress_range_id, virtual_page_address);
  V2HMapMetadata* v2h_map = GetV2HMap(vaddress_range_id, vaddress_page_offset);
  assert(v2hmap == v2h_map);

  // A "flash-page number" is relative to the beginning of flash-cache file.
  uint64_t flash_page_number;
  if (v2hmap->exist_flash_cache) {
    assert(v2hmap->flash_page_offset < total_flash_pages_);
    flash_page_number = v2hmap->flash_page_offset;
    F2VMapItem* f2vmap = &f2v_map_[flash_page_number];
    assert(f2vmap->vaddress_page_offset == vaddress_page_offset);
    assert(f2vmap->vaddress_range_id == vaddress_range_id);
    if (flash_page_number == 12288) {
      dbg("^^^^^  pos 1: flash-page %ld for virt-page %ld, "
          "vaddr-range %d, dirty = %d\n",
          flash_page_number,
          vaddress_page_offset,
          vaddress_range_id,
          is_dirty);
    }
  } else {
    if (page_allocate_table_.AllocateOnePage(&flash_page_number) == false) {
      // Evict some pages from flash-cache to make space.
      uint32_t pages_to_evict = 16;
      EvictItems(pages_to_evict);
      if (page_allocate_table_.AllocateOnePage(&flash_page_number) == false) {
        err("Unable to alloc flash page even after evict: virt-page %ld at "
            "vaddr-range-id %d\n",
            vaddress_page_offset,
            vaddress_range_id);
        assert(0);
      }
    }
    if (flash_page_number == 12288) {
      dbg("^^^^^  pos 2: alloc flash-page %ld for virt-page %ld, "
          "vaddr-range %d\n",
          flash_page_number,
          vaddress_page_offset,
          vaddress_range_id);
    }
    assert(
        !IsValidVAddressRangeId(f2v_map_[flash_page_number].vaddress_range_id));
  }
  if (!v2hmap->exist_flash_cache || is_dirty) {
    if (pwrite(flash_fd_, page, obj_size, flash_page_number << PAGE_BITS) !=
        (ssize_t)obj_size) {
      err("Failed to write to flash-cache %s: flash-page: %ld, "
          "virtual-address=%p from vaddr-range %d\n",
          flash_filename_.c_str(),
          flash_page_number,
          virtual_page_address,
          vaddress_range_id);
      perror("flash pwrite failed: ");
      return false;
    }
  }

  f2v_map_[flash_page_number].vaddress_page_offset = vaddress_page_offset;
  f2v_map_[flash_page_number].vaddress_range_id = vaddress_range_id;

  v2hmap->exist_flash_cache = 1;
  v2hmap->dirty_flash_cache = is_dirty;
  v2hmap->flash_page_offset = flash_page_number;
  page_stats_table_.IncreaseAccessCount(flash_page_number, 1);
  if (flash_page_number == 12288) {
    dbg("flash page %ld <=> virt-page %ld: dirty=%d, access-count=%ld, "
        "vaddress-range-id=%d\n",
        flash_page_number,
        vaddress_page_offset,
        is_dirty,
        page_stats_table_.AccessCount(flash_page_number),
        vaddress_range_id);
  }
  return true;
}

bool FlashCache::LoadPage(void* data,
                          uint64_t obj_size,
                          uint64_t flash_page_number,
                          uint32_t vaddress_range_id,
                          uint64_t vaddress_page_offset) {
  F2VMapItem* f2vmap = GetItem(flash_page_number);
  assert(f2vmap->vaddress_range_id == vaddress_range_id);
  assert(f2vmap->vaddress_page_offset == vaddress_page_offset);
  assert((uint64_t)data % 512 == 0);
  assert(obj_size == PAGE_SIZE);

  if (pread(flash_fd_, data, obj_size, flash_page_number << PAGE_BITS) !=
      (ssize_t)obj_size) {
    err("Failed to read flash-cache %s: flash-page %ld, to vaddr-range %d, "
        "page %ld\n",
        flash_filename_.c_str(),
        flash_page_number,
        vaddress_range_id,
        vaddress_page_offset);
    perror("flash pread failed: ");
    return false;
  }
  page_stats_table_.IncreaseAccessCount(flash_page_number, 1);
  return true;
}

bool FlashCache::LoadFromHDDFile(VAddressRange* vaddr_range,
                                 void* page,
                                 V2HMapMetadata* v2hmap,
                                 bool read_ahead) {
  uint64_t hdd_file_offset = (uint64_t)page - (uint64_t)vaddr_range->address() +
                             vaddr_range->hdd_file_offset();
  ssize_t read_size = PAGE_SIZE;
  assert(!read_ahead);  // NOT support read ahead right now.
  if (!read_ahead) {
    if (pread(vaddr_range->hdd_file_fd(), page, read_size, hdd_file_offset) !=
        read_size) {
      err("Failed to read hdd-file at flash-cache %s: vaddr-range %d, "
          "page %p\n",
          name_.c_str(),
          vaddr_range->vaddress_range_id(),
          page);
      perror("flash-cache read hdd-file failed: ");
      return false;
    }
  } else {
    read_size = PAGE_SIZE << VADDRESS_CHUNK_BITS;
    uint64_t virtual_chunk =
        (uint64_t)page & ~((1ULL << (PAGE_BITS + VADDRESS_CHUNK_BITS)) - 1);
    hdd_file_offset = virtual_chunk - (uint64_t)vaddr_range->address() +
                      vaddr_range->hdd_file_offset();
    // TODO: at read-ahead mode, read an entire chunk from hdd file,
    // and save these pages to flash.
  }
  return true;
}

void FlashCache::ShowStats() {
  printf(
      "\n\n*****\tflash-cache: %s, flash-file: %s, total-flash pages %ld,\n"
      "used-flash-pages %ld, available flash pages %ld\n"
      "max-evict-lat %ld usec (write %ld pages)\n",
      name_.c_str(),
      flash_filename_.c_str(),
      total_flash_pages_,
      page_allocate_table_.used_pages(),
      page_allocate_table_.free_pages(),
      max_evict2hdd_latency_usec_,
      evict2hdd_pages_);
}
