#pragma once

/**
 * @file block_failures.h
 * @brief Fork: a count of the DSP blocks whose work() failed in this process, for both block
 * kinds (dsp::Block and satdump::ndsp::Block).
 *
 * A block whose work() throws logs the error and ends its part of the processing without
 * stopping SatDump, so the processing around it finishes as if its input had ended. Whoever
 * ran that processing checks this count to report that the output is incomplete (the command
 * line does: see core/cli/cli.cpp).
 */

#include <cstdint>
#include <string>

namespace dsp
{
    /**
     * @brief Note that a DSP block's work() failed. Called by the block, after it logged it.
     * @param block the block's name or ID
     * @param error what work() threw
     */
    void record_block_failure(const std::string &block, const std::string &error);

    /**
     * @brief How many DSP blocks failed in this process so far
     * @return the number of failures
     */
    uint64_t block_failure_count();

    /**
     * @brief The most recent failure
     * @return "block: error", empty if no block failed
     */
    std::string last_block_failure();
} // namespace dsp
