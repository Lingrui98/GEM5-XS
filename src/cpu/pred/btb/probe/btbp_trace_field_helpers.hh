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

#ifndef __CPU_PRED_BTB_PROBE_BTBP_TRACE_FIELD_HELPERS_HH__
#define __CPU_PRED_BTB_PROBE_BTBP_TRACE_FIELD_HELPERS_HH__

#include "cpu/pred/btb/probe/btbp_trace_event.hh"
#include "mem/request.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

/**
 * Copy the request's nonzero instPrefetchAttemptId into a trace event and mark
 * the field valid. Extracted from Fetch::issueFdipReadyLine so the v2.3
 * attempt-identity repair (contract stage-a v2.3 §4.3) is unit-testable under
 * TARGET_ISA=null. This mirrors the copy already present in
 * Fetch::emitInstPrefetchTrace and in BaseCache, which are left unchanged so
 * EIP identity behavior is unaffected (§5.5).
 */
inline void
applyFdipAttemptIdentity(BtbpTraceEvent &event, const Request::XsMetadata &xs)
{
    if (xs.instPrefetchAttemptId != 0) {
        event.prefetchAttemptId = xs.instPrefetchAttemptId;
        event.prefetchAttemptIdValid = true;
    }
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_PROBE_BTBP_TRACE_FIELD_HELPERS_HH__
