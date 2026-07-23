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

#ifndef __CPU_PRED_BTB_PROBE_BTBP_TRACE_ROUTE_HH__
#define __CPU_PRED_BTB_PROBE_BTBP_TRACE_ROUTE_HH__

#include "base/logging.hh"
#include "cpu/pred/btb/probe/btbp_trace_event.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

/**
 * The single intentional CPU routing class for a BtbpTraceEvent. This enum is
 * the one source of truth for which `ppBtbpTrace*` probe point a declared
 * event type must reach. It is extracted from `CPU::notifyBtbpTrace` so the
 * route + fail-closed semantics are unit-testable under TARGET_ISA=null (the
 * O3 CPU sources are not compiled there). Production `notifyBtbpTrace`
 * dispatches by this class.
 *
 * Contract stage-a v2.3 §4.1/§4.2: every declared EventType has exactly one
 * intentional routing class, and an unsupported value fails closed instead of
 * being silently dropped (the v2.2 type-14 defect).
 */
enum class BtbpTraceRouteClass : uint8_t
{
    Unsupported = 0,
    MbtbLookup,
    MbtbFill,
    IPrefetchIssue,
    LineLifecycle,
    L1IDemandAccess,
    DecodeBranch,
    BranchDemand,
};

/**
 * Map a raw event type to its single intentional CPU routing class. Values
 * outside the declared enum (including the default-initialized 0) map to
 * `Unsupported`; production must not silently route them.
 */
inline BtbpTraceRouteClass
btbpTraceRouteClass(uint32_t eventType)
{
    switch (eventType) {
      case BtbpTraceEvent::MbtbLookup:
        return BtbpTraceRouteClass::MbtbLookup;
      case BtbpTraceEvent::MbtbFill:
        return BtbpTraceRouteClass::MbtbFill;
      case BtbpTraceEvent::IPrefetchIssue:
      case BtbpTraceEvent::IPrefetchDecisionAccepted:
      case BtbpTraceEvent::IPrefetchTerminal:
      case BtbpTraceEvent::IPrefetchGateAttempt:
        return BtbpTraceRouteClass::IPrefetchIssue;
      case BtbpTraceEvent::LineLifecycle:
      case BtbpTraceEvent::L1IMshrOccupancy:
      case BtbpTraceEvent::L1ILineEvict:
      case BtbpTraceEvent::RoiBegin:
      case BtbpTraceEvent::RoiEnd:
        return BtbpTraceRouteClass::LineLifecycle;
      case BtbpTraceEvent::L1IDemandAccess:
        return BtbpTraceRouteClass::L1IDemandAccess;
      case BtbpTraceEvent::DecodeBranch:
        return BtbpTraceRouteClass::DecodeBranch;
      case BtbpTraceEvent::BranchDemand:
        return BtbpTraceRouteClass::BranchDemand;
      default:
        return BtbpTraceRouteClass::Unsupported;
    }
}

/**
 * Fail closed on a BtbpTraceEvent that has no intentional CPU routing class.
 * Production `notifyBtbpTrace` calls this before dispatch so an
 * unsupported/uninitialized eventType aborts loudly instead of being silently
 * dropped. Contract stage-a v2.3 §4.2.
 */
inline void
requireBtbpTraceRouteable(uint32_t eventType)
{
    if (btbpTraceRouteClass(eventType) == BtbpTraceRouteClass::Unsupported) {
        panic("BtbpTraceEvent: unsupported eventType %u routed to CPU",
              eventType);
    }
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_PROBE_BTBP_TRACE_ROUTE_HH__
