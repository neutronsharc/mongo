// SSD-Assisted Hybrid Memory.
// Author: Xiangyong Ouyang (neutronsharc@gmail.com)
// Created on: 2011-11-11

#ifndef HYBRID_MEMORY_INL_H_
#define HYBRID_MEMORY_INL_H_

#include <stdint.h>
#include <string>

struct V2HMapMetadata;
class VAddressRange;

uint64_t NumberOfPageFaults();

uint64_t FoundPages();

uint64_t UnFoundPages();

uint64_t GetPageOffsetInVAddressRange(uint32_t vaddress_range_id, void* page);

V2HMapMetadata* GetV2HMap(uint32_t vaddress_range_id, uint64_t page_offset);

VAddressRange* GetVAddressRangeFromId(uint32_t vaddress_range_id);

void HybridMemoryStats();

#endif  // HYBRID_MEMORY_INL_H_
