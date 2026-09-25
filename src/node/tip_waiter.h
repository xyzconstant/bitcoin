// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_TIP_WAITER_H
#define BITCOIN_NODE_TIP_WAITER_H

#include <node/kernel_notifications.h>
#include <sync.h>
#include <util/time.h>

#include <optional>

class ChainstateManager;
class uint256;

namespace interfaces {
struct BlockRef;
} // namespace interfaces

namespace node {

/**
 * Reads the active chain tip and blocks the calling thread until it changes.
 * Each instance carries its own interrupt flag.
 */
class TipWaiter
{
private:
    ChainstateManager& m_chainman;
    KernelNotifications& m_notifications;
    bool m_interrupt GUARDED_BY(m_notifications.m_tip_block_mutex){false};

    /** @return true if shutting down or interrupted; clears the interrupt. */
    bool CheckInterrupted() EXCLUSIVE_LOCKS_REQUIRED(m_notifications.m_tip_block_mutex);

    /**
     * Wait while the best known header extends the current chain tip AND at
     * least one block is being added to the tip every 3 seconds. If the tip is
     * sufficiently far behind, allow up to 20 seconds for the next tip update.
     *
     * It's not safe to keep waiting, because a malicious miner could announce
     * a header and delay revealing the block, causing all other miners using
     * this software to stall. At the same time, we need to balance between the
     * default waiting time being brief, but not ending the cooldown prematurely
     * when a random block is slow to download (or process).
     *
     * The cooldown only applies to createNewBlock(), which is typically called
     * once per connected client. Subsequent templates are provided by
     * waitNext().
     *
     * @param last_tip tip at the start of the cooldown window.
     *
     * @returns false if interrupted.
     */
    bool CooldownIfHeadersAhead(const interfaces::BlockRef& last_tip)
        EXCLUSIVE_LOCKS_REQUIRED(!m_notifications.m_tip_block_mutex);

public:
    TipWaiter(ChainstateManager& chainman, KernelNotifications& notifications);

    /** Locks cs_main.
     *  @return the active chain tip, or nullopt if none exists. */
    std::optional<interfaces::BlockRef> GetTip();

    /** Wait for the tip to differ from @p current_tip or timeout.
     *  Waits indefinitely during startup for a non-null tip.
     *  @return the current tip, or nullopt if the node is shutting down or
     *  Interrupt() was called (not when the timeout is reached). */
    std::optional<interfaces::BlockRef> WaitTipChanged(const uint256& current_tip, MillisecondsDouble timeout = MillisecondsDouble::max())
        EXCLUSIVE_LOCKS_REQUIRED(!m_notifications.m_tip_block_mutex);

    /** Wait for a tip, then for IBD and the header catch-up cooldown to end.
     *  @return the tip, or nullopt if shutting down or interrupted. */
    std::optional<interfaces::BlockRef> WaitUntilSynced()
        EXCLUSIVE_LOCKS_REQUIRED(!m_notifications.m_tip_block_mutex);

    /** Interrupt a blocking wait. */
    void Interrupt() EXCLUSIVE_LOCKS_REQUIRED(!m_notifications.m_tip_block_mutex);
};
} // namespace node

#endif // BITCOIN_NODE_TIP_WAITER_H
