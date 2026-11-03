// Output sink: how the engine hands outputs and reports to whoever owns the rollup device.
// Streaming (rather than returning a vector) lets an emission failure (-ENOBUFS on an echo of a
// payload too large for the TX buffer) be observed by the engine, and a queueing sink is all the
// application-engine.h shim needs for drain_output().
#pragma once

#include "types.h"

namespace qawallet {

class OutputSink {
public:
    virtual ~OutputSink() = default;
    // Each returns 0 on success or a negative errno (libcmt convention).
    virtual int notice(bytes_view payload) = 0;
    virtual int voucher(const Address &destination, const U256 &value, bytes_view payload) = 0;
    virtual int report(bytes_view payload) = 0;
};

} // namespace qawallet
