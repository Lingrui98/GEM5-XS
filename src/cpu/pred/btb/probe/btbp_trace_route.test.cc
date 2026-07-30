/*
 * Copyright (c) 2024 The Chinese University of Hong Kong
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtest/gtest.h>

#include "cpu/pred/btb/probe/btbp_trace_event.hh"
#include "cpu/pred/btb/probe/btbp_trace_field_helpers.hh"
#include "cpu/pred/btb/probe/btbp_trace_route.hh"
#include "mem/request.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{
namespace {

using Route = BtbpTraceRouteClass;

// Contract stage-a v2.3 §4.1/§4.2 + §5.3: every declared EventType routes to
// exactly one intentional CPU probe class. The v2.2 defect was that
// IPrefetchGateAttempt (14) had no routing case and was silently dropped by
// the default branch; these tests pin the repair and prevent regression.

TEST(BtbpTraceRouteTest, GateAttemptRoutesToIPrefetchIssueClass)
{
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::IPrefetchGateAttempt),
              Route::IPrefetchIssue);
}

TEST(BtbpTraceRouteTest, IPrefetchFamilySharesOneRouteClass)
{
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::IPrefetchIssue),
              Route::IPrefetchIssue);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::IPrefetchDecisionAccepted),
              Route::IPrefetchIssue);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::IPrefetchTerminal),
              Route::IPrefetchIssue);
}

TEST(BtbpTraceRouteTest, EveryOtherDeclaredTypeHasItsOwnRouteClass)
{
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::MbtbLookup),
              Route::MbtbLookup);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::MbtbFill),
              Route::MbtbFill);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::LineLifecycle),
              Route::LineLifecycle);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::L1IMshrOccupancy),
              Route::LineLifecycle);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::L1ILineEvict),
              Route::LineLifecycle);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::RoiBegin),
              Route::LineLifecycle);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::RoiEnd),
              Route::LineLifecycle);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::L1IDemandAccess),
              Route::L1IDemandAccess);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::DecodeBranch),
              Route::DecodeBranch);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::BranchDemand),
              Route::BranchDemand);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::LookupTerminal),
              Route::MbtbLookup);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::FetchRequestOpen),
              Route::L1IDemandAccess);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::FetchRequestTerminal),
              Route::L1IDemandAccess);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::L1IDemandIssue),
              Route::L1IDemandAccess);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::L1IDemandAttempt),
              Route::L1IDemandAccess);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::L1IDemandTerminal),
              Route::L1IDemandAccess);
    EXPECT_EQ(btbpTraceRouteClass(BtbpTraceEvent::DecodeConsume),
              Route::L1IDemandAccess);
}

// Contract §4.2/§5.3: an unsupported or uninitialized eventType must not be
// silently routed. Production CPU::notifyBtbpTrace calls
// requireBtbpTraceRouteable (which panics on Unsupported) before dispatch, so
// these values fail closed rather than being dropped. This test pins the
// classification that drives that fail-closed path; the panic fire itself is
// corroborated by the §5.2 route inventory and the §6 first-cell census.
TEST(BtbpTraceRouteTest, UnsupportedValuesAreNotRoutable)
{
    EXPECT_EQ(btbpTraceRouteClass(0), Route::Unsupported);
    EXPECT_EQ(btbpTraceRouteClass(99), Route::Unsupported);
    EXPECT_EQ(btbpTraceRouteClass(22), Route::Unsupported);
}

// Contract §4.3 + §5.4: FDIP issue attempt-identity repair.
// Fetch::issueFdipReadyLine delegates to applyFdipAttemptIdentity; these tests
// pin that the request's nonzero instPrefetchAttemptId is copied and the
// adjacent decision/source/identity fields are not clobbered.

TEST(BtbpTraceFieldTest, FdipAttemptIdCopiedWhenNonzero)
{
    Request::XsMetadata xs;
    xs.instPrefetchAttemptId = 11;
    BtbpTraceEvent event;
    applyFdipAttemptIdentity(event, xs);
    EXPECT_TRUE(event.prefetchAttemptIdValid);
    EXPECT_EQ(event.prefetchAttemptId, 11u);
}

TEST(BtbpTraceFieldTest, FdipAttemptIdLeftUnsetWhenZero)
{
    Request::XsMetadata xs; // instPrefetchAttemptId defaults to 0
    BtbpTraceEvent event;
    applyFdipAttemptIdentity(event, xs);
    EXPECT_FALSE(event.prefetchAttemptIdValid);
}

TEST(BtbpTraceFieldTest, DoesNotClobberAdjacentFields)
{
    Request::XsMetadata xs;
    xs.instPrefetchAttemptId = 7;
    BtbpTraceEvent event;
    event.eventType = BtbpTraceEvent::IPrefetchIssue;
    event.prefetchDecisionId = 42;
    event.prefetchDecisionIdValid = true;
    applyFdipAttemptIdentity(event, xs);
    EXPECT_EQ(event.eventType, BtbpTraceEvent::IPrefetchIssue);
    EXPECT_EQ(event.prefetchDecisionId, 42u);
    EXPECT_TRUE(event.prefetchDecisionIdValid);
    EXPECT_EQ(event.prefetchAttemptId, 7u);
    EXPECT_TRUE(event.prefetchAttemptIdValid);
}

} // namespace
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
