/*
 * Copyright (c) 2026 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_CACHE_PF_SOURCE_PROPAGATION_HH__
#define __MEM_CACHE_PF_SOURCE_PROPAGATION_HH__

#include "mem/request.hh"

namespace gem5
{

// BTBP Stage-A v2.5 contract section 4.2 repair (class 1): the satisfied/hit
// identity propagation in BaseCache::recvTimingReq may adopt the block's
// XsMetadata.prefetchSource only when the request carries no prefetch
// identity of its own. A real identity (PF_EIP, PF_FDIP, or any other
// non-PF_NONE source) must never be overwritten by block metadata; the block
// value is a fallback for unidentified requests only.
//
// Proven defect (v2.4 attempt-3 REQTRACE): an EIP prefetch request reached
// the hit branch with pfSource=PF_EIP while the block's
// XsMetadata.prefetchSource was PF_NONE; the ungated
// pkt->req->setPFSource(blk->getXsMetadata().prefetchSource) overwrote the
// real identity 16->0 (the request's own xsMeta.prefetchSource stayed 16),
// the completion missed the fetch.cc:2132 EIP classification, fell to the
// demand path, and tripped assert(isMisalignedFetch()) at fetch.cc:2143.
inline bool
pfSourcePropagateFromBlock(PrefetchSourceType reqSource)
{
    return reqSource == PF_NONE;
}

} // namespace gem5

#endif // __MEM_CACHE_PF_SOURCE_PROPAGATION_HH__
