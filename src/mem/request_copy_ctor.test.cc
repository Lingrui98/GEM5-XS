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

#include "mem/request.hh"

namespace gem5
{
namespace {

// BTBP Stage-A v2.4 section 4.2 repair: Request(const Request&) now copies
// the previously-omitted classification fields misalignedFetch, reqNum,
// pfSource, pfDepth, and firstReqAfterSquash (the _xsMetadata field was
// already copied). This test pins that a copy-constructed Request preserves
// those fields. Before the repair the copy ctor silently reset them to
// their defaults (false / 1 / PF_NONE / 0 / false), so a copied Request lost
// its BTBP classification. We build the original via the default constructor
// plus the inline setters, then copy-construct and assert field-for-field.
TEST(RequestCopyCtorTest, PreservesBtbpClassificationFields)
{
    Request orig;
    orig.setMisalignedFetch();
    orig.setReqNum(7);
    orig.setPFSource(PF_EIP);
    orig.setPFDepth(3);
    orig.setFirstReqAfterSquash();

    // Sanity: the setters wrote the expected values onto the original.
    ASSERT_TRUE(orig.isMisalignedFetch());
    ASSERT_EQ(orig.getReqNum(), 7);
    ASSERT_EQ(orig.getPFSource(), PF_EIP);
    ASSERT_EQ(orig.getPFDepth(), 3);
    ASSERT_TRUE(orig.isFirstReqAfterSquash());

    // The repair under test: the copy constructor must propagate the
    // classification fields instead of dropping them to defaults.
    Request copy(orig);

    ASSERT_TRUE(copy.isMisalignedFetch());
    ASSERT_EQ(copy.getReqNum(), 7);
    ASSERT_EQ(copy.getPFSource(), PF_EIP);
    ASSERT_EQ(copy.getPFDepth(), 3);
    ASSERT_TRUE(copy.isFirstReqAfterSquash());

    // Regression guard: the pre-existing _xsMetadata + privateFlags copy
    // path (VALID_XS_METADATA) must remain intact after the repair. The
    // copy ctor already copied _xsMetadata; confirm it still does, so the
    // repair did not regress the established behavior.
    Request::XsMetadata xs(PF_EIP);
    ASSERT_TRUE(xs.validXsMetadata);
    orig.setXsMetadata(xs);
    Request copyWithXs(orig);
    EXPECT_TRUE(copyWithXs.hasXsMetadata());
    EXPECT_EQ(copyWithXs.getXsMetadata().validXsMetadata, true);
    EXPECT_EQ(copyWithXs.getXsMetadata().prefetchSource, PF_EIP);
}

} // namespace
} // namespace gem5
