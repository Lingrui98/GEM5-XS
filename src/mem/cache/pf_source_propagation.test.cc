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

#include <gtest/gtest.h>

#include "mem/cache/pf_source_propagation.hh"

namespace gem5
{
namespace {

// BTBP Stage-A v2.5 contract section 4.2 repair (class 1), focused
// negative/positive mechanism test. The repair gates the satisfied/hit
// identity propagation in BaseCache::recvTimingReq so the block's
// XsMetadata.prefetchSource can only fill a request that has no identity of
// its own (PF_NONE), never overwrite a real one.
//
// Parent leg of the negative control: the parent 9f13ca419a has no gate (the
// ungated statement overwrites unconditionally), so this mechanism test
// cannot be built there — the mechanism is introduced by the repair. The
// cell-level parent negative is the frozen v2.4 attempt-2 canary panic
// (fetch.cc:2143 @ tick 3623708331), cited per contract section 5 item 2,
// not rerun. UngatedOverwriteDocumentsParentDefect below pins on real Request
// objects why the ungated statement loses the identity.
//
// Each case drives the exact production call sequence — the production
// predicate pfSourcePropagateFromBlock plus a real Request whose pfSource is
// written only through the production setter — with the block-side value
// supplied as a plain PrefetchSourceType (CacheBlk is a SimObject-adjacent
// type with no unit-test construction pattern in this tree; the predicate
// depends only on the request's own source).

static PrefetchSourceType
propagateLikeProduction(Request &req, PrefetchSourceType blockSource)
{
    // Identical in shape to base.cc's satisfied/hit branch:
    //   if (pfSourcePropagateFromBlock(pkt->req->getPFSource())) {
    //       pkt->req->setPFSource(blk->getXsMetadata().prefetchSource);
    //   }
    if (pfSourcePropagateFromBlock(req.getPFSource())) {
        req.setPFSource(blockSource);
    }
    return req.getPFSource();
}

// Demand completions (req pfSource == PF_NONE) still adopt the block's
// identity: the pre-repair intended behavior is preserved.
TEST(PFSourcePropagation, DemandRequestAdoptsBlockIdentity)
{
    Request req;  // default-constructed: pfSource == PF_NONE
    ASSERT_EQ(req.getPFSource(), PF_NONE);
    EXPECT_TRUE(pfSourcePropagateFromBlock(req.getPFSource()));
    EXPECT_EQ(propagateLikeProduction(req, PF_EIP), PF_EIP);
}

// A demand request against a zero block stays PF_NONE (no-op propagation).
TEST(PFSourcePropagation, DemandRequestAgainstZeroBlockStaysNone)
{
    Request req;
    EXPECT_EQ(propagateLikeProduction(req, PF_NONE), PF_NONE);
}

// The proven v2.4 loss case: an EIP prefetch request (pfSource=16) hitting a
// block whose metadata is PF_NONE must keep its real identity.
TEST(PFSourcePropagation, EipIdentityNotOverwrittenByZeroBlock)
{
    Request req;
    req.setPFSource(PF_EIP);
    EXPECT_FALSE(pfSourcePropagateFromBlock(req.getPFSource()));
    EXPECT_EQ(propagateLikeProduction(req, PF_NONE), PF_EIP);
}

// A real identity is never overwritten by a *different* block identity
// either: the block value is a fallback, not an override.
TEST(PFSourcePropagation, EipIdentityNotOverwrittenByOtherBlockIdentity)
{
    Request req;
    req.setPFSource(PF_EIP);
    EXPECT_EQ(propagateLikeProduction(req, PF_FDIP), PF_EIP);
}

// FDIP completions are likewise preserved (the regression-control arm).
TEST(PFSourcePropagation, FdipIdentityNotOverwrittenByZeroBlock)
{
    Request req;
    req.setPFSource(PF_FDIP);
    EXPECT_FALSE(pfSourcePropagateFromBlock(req.getPFSource()));
    EXPECT_EQ(propagateLikeProduction(req, PF_NONE), PF_FDIP);
}

// Any non-PF_NONE identity (e.g. a hardware prefetcher source) is held; the
// gate keys on PF_NONE alone, not on the BTBP sources specifically.
TEST(PFSourcePropagation, AnyNonNoneIdentityHeld)
{
    Request req;
    req.setPFSource(Berti);
    EXPECT_FALSE(pfSourcePropagateFromBlock(req.getPFSource()));
    EXPECT_EQ(propagateLikeProduction(req, PF_NONE), Berti);
}

// Parent-defect documentation (negative control at object level): the
// parent's ungated statement `req.setPFSource(blockSource)` overwrites
// unconditionally, so an EIP request reaching the hit branch with a zero
// block loses 16 -> 0 while its own xsMeta.prefetchSource would have stayed
// 16 — the exact (pfSrc=0, xsMeta=16) symptom frozen by the v2.4 attempt-3
// REQTRACE. This models the parent line on real Request setters to document
// why the gate is required; the cell-level parent negative remains the
// frozen v2.4 attempt-2 canary panic, cited not rerun.
TEST(PFSourcePropagation, UngatedOverwriteDocumentsParentDefect)
{
    Request req;
    req.setPFSource(PF_EIP);
    req.setPFSource(PF_NONE);  // parent's ungated statement, zero block
    ASSERT_EQ(req.getPFSource(), PF_NONE);  // identity lost on the parent

    // The candidate's gated sequence refuses that same overwrite.
    Request fixed;
    fixed.setPFSource(PF_EIP);
    EXPECT_EQ(propagateLikeProduction(fixed, PF_NONE), PF_EIP);
}

} // namespace
} // namespace gem5
