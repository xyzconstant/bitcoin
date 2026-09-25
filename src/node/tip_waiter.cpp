// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/tip_waiter.h>

#include <chain.h>
#include <interfaces/types.h>
#include <node/kernel_notifications.h>
#include <sync.h>
#include <uint256.h>
#include <util/check.h>
#include <util/signalinterrupt.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <optional>

namespace node {

using interfaces::BlockRef;

TipWaiter::TipWaiter(ChainstateManager& chainman, KernelNotifications& notifications)
    : m_chainman(chainman), m_notifications(notifications)
{
}

bool TipWaiter::CheckInterrupted()
{
    AssertLockHeld(m_notifications.m_tip_block_mutex);
    if (!m_chainman.m_interrupt && !m_interrupt) return false;
    m_interrupt = false;
    return true;
}

bool TipWaiter::CooldownIfHeadersAhead(const BlockRef& last_tip)
{
    uint256 last_tip_hash{last_tip.hash};

    while (const std::optional<int> remaining = m_chainman.BlocksAheadOfTip()) {
        const int cooldown_seconds = std::clamp(*remaining, 3, 20);
        const auto cooldown_deadline{MockableSteadyClock::now() + std::chrono::seconds{cooldown_seconds}};

        {
            WAIT_LOCK(m_notifications.m_tip_block_mutex, lock);
            m_notifications.m_tip_block_cv.wait_until(lock, cooldown_deadline, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_notifications.m_tip_block_mutex) {
                const auto tip_block = m_notifications.TipBlock();
                return m_chainman.m_interrupt || m_interrupt || (tip_block && *tip_block != last_tip_hash);
            });
            if (CheckInterrupted()) return false;

            // If the tip changed during the wait, extend the deadline
            const auto tip_block = m_notifications.TipBlock();
            if (tip_block && *tip_block != last_tip_hash) {
                last_tip_hash = *tip_block;
                continue;
            }
        }

        // No tip change and the cooldown window has expired.
        if (MockableSteadyClock::now() >= cooldown_deadline) break;
    }

    return true;
}

void TipWaiter::Interrupt()
{
    LOCK(m_notifications.m_tip_block_mutex);
    m_interrupt = true;
    m_notifications.m_tip_block_cv.notify_all();
}

std::optional<BlockRef> TipWaiter::GetTip()
{
    LOCK(::cs_main);
    CBlockIndex* tip{m_chainman.ActiveChain().Tip()};
    if (!tip) return {};
    return BlockRef{tip->GetBlockHash(), tip->nHeight};
}

std::optional<BlockRef> TipWaiter::WaitTipChanged(const uint256& current_tip, MillisecondsDouble timeout)
{
    Assume(timeout >= 0ms); // No internal callers should use a negative timeout
    if (timeout < 0ms) timeout = 0ms;
    if (timeout > std::chrono::years{100}) timeout = std::chrono::years{100}; // Upper bound to avoid UB in std::chrono
    auto deadline{std::chrono::steady_clock::now() + timeout};
    {
        WAIT_LOCK(m_notifications.m_tip_block_mutex, lock);
        // For callers convenience, wait longer than the provided timeout
        // during startup for the tip to be non-null. That way this function
        // always returns valid tip information when possible and only
        // returns null when shutting down, not when timing out.
        m_notifications.m_tip_block_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_notifications.m_tip_block_mutex) {
            AssertLockHeld(m_notifications.m_tip_block_mutex);
            return m_notifications.TipBlock() || m_chainman.m_interrupt || m_interrupt;
        });
        if (CheckInterrupted()) return {};
        // At this point TipBlock is set, so continue to wait until it is
        // different from `current_tip` provided by caller.
        m_notifications.m_tip_block_cv.wait_until(lock, deadline, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_notifications.m_tip_block_mutex) {
            return Assume(m_notifications.TipBlock()) != current_tip || m_chainman.m_interrupt || m_interrupt;
        });
        if (CheckInterrupted()) return {};
    }

    // Must release m_tip_block_mutex before GetTip() locks cs_main, to
    // avoid deadlocks.
    return GetTip();
}

std::optional<BlockRef> TipWaiter::WaitUntilSynced()
{
    std::optional<BlockRef> tip{WaitTipChanged(uint256::ZERO)};
    if (!tip) return {};

    // Do not return a template during IBD, because it can have long
    // pauses and sometimes takes a while to get started. Although this
    // is useful in general, it's gated behind the cooldown argument,
    // because on regtest and single miner signets this would wait
    // forever if no block was mined in the past day.
    while (m_chainman.IsInitialBlockDownload()) {
        tip = WaitTipChanged(tip->hash, MillisecondsDouble{1000});
        if (!tip || m_chainman.m_interrupt || WITH_LOCK(m_notifications.m_tip_block_mutex, return m_interrupt)) return {};
    }

    // Also wait during the final catch-up moments after IBD.
    if (!CooldownIfHeadersAhead(*tip)) return {};
    return tip;
}
} // namespace node
