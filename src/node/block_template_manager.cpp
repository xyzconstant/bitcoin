// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/block_template_manager.h>

#include <chain.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <interfaces/types.h>
#include <kernel/chainparams.h>
#include <node/miner.h>
#include <node/mining_args.h>
#include <node/tip_waiter.h>
#include <primitives/block.h>
#include <sync.h>
#include <uint256.h>
#include <util/check.h>
#include <util/signalinterrupt.h>
#include <validation.h>
#include <validationinterface.h>

#include <algorithm>
#include <chrono>
#include <compare>
#include <numeric>
#include <utility>
#include <vector>

namespace node {

using interfaces::BlockRef;

BlockTemplateManager::BlockTemplateManager(CTxMemPool& mempool, ChainstateManager& chainman,
                                           BlockCreateOptions block_create_args)
    : m_mempool(mempool), m_chainman(chainman), m_block_create_args(std::move(block_create_args))
{
}

std::unique_ptr<CBlockTemplate> BlockTemplateManager::CreateNewTemplate(const BlockCreateOptions& options)
{
    return BlockAssembler{
        m_chainman.ActiveChainstate(),
        &m_mempool,
        MergeMiningOptions(options, m_block_create_args),
    }.CreateNewBlock();
}

namespace {
class SubmitBlockStateCatcher final : public CValidationInterface
{
public:
    uint256 m_hash;
    bool m_found{false};
    BlockValidationState m_state;

    explicit SubmitBlockStateCatcher(const uint256& hash) : m_hash{hash} {}

protected:
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state) override
    {
        if (block->GetHash() != m_hash) return;
        // ProcessNewBlock emits BlockChecked synchronously while holding cs_main,
        // so SubmitBlock can read these fields after ProcessNewBlock returns
        // without extra synchronization.
        m_found = true;
        m_state = state;
    }
};
} // namespace

bool BlockTemplateManager::SubmitBlock(const std::shared_ptr<const CBlock>& block, std::string& reason, std::string& debug)
{
    reason.clear();
    debug.clear();

    // This follows the submitblock RPC's validation-state capture pattern, but
    // is intentionally kept separate from the RPC implementation. The RPC entry
    // point decodes hex, formats BIP22/JSONRPC results, and calls
    // UpdateUncommittedBlockStructures() for legacy witness handling. IPC
    // callers submit already-formed blocks and need bool + reason/debug
    // results.
    auto sc = std::make_shared<SubmitBlockStateCatcher>(block->GetHash());
    CHECK_NONFATAL(m_chainman.m_options.signals)->RegisterSharedValidationInterface(sc);
    bool new_block;
    bool accepted = m_chainman.ProcessNewBlock(block, /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&new_block);
    // No queue drain is needed. The BlockChecked notification used above is
    // emitted synchronously by ProcessNewBlock, unlike most validation signals.
    CHECK_NONFATAL(m_chainman.m_options.signals)->UnregisterSharedValidationInterface(sc);

    if (!new_block && accepted) {
        reason = "duplicate";
    } else if (!accepted && (!sc->m_found || sc->m_state.IsValid())) {
        // ProcessNewBlock can fail without a validation result, for example
        // from an activation or system error. It can also fail after a valid
        // BlockChecked result. In these cases the validation result is
        // inconclusive.
        reason = "inconclusive";
    } else if (!sc->m_found) {
        // The block was accepted but not connected, for example if it does not
        // have more work than the current tip.
        reason = "inconclusive";
    } else if (!sc->m_state.IsValid()) {
        reason = sc->m_state.GetRejectReason();
        debug = sc->m_state.GetDebugMessage();
    }
    const bool result{accepted && new_block && reason.empty()};
    CHECK_NONFATAL(result == reason.empty());
    return result;
}

std::unique_ptr<CBlockTemplate> BlockTemplateManager::WaitAndCreateNewBlock(
    TipWaiter& tip_waiter,
    const std::unique_ptr<CBlockTemplate>& block_template,
    const BlockWaitOptions& wait_options,
    const BlockCreateOptions& create_options)
{
    // Delay calculating the current template fees, just in case a new block
    // comes in before the next tick.
    CAmount current_fees = -1;

    // Alternate waiting for a new tip and checking if fees have risen.
    // The latter check is expensive so we only run it once per second.
    auto now{NodeClock::now()};
    const auto deadline = now + wait_options.timeout;
    const MillisecondsDouble tick{1000};
    const bool allow_min_difficulty{m_chainman.GetParams().GetConsensus().fPowAllowMinDifficultyBlocks};

    do {
        const std::optional<BlockRef> tip{tip_waiter.WaitTipChanged(block_template->block.hashPrevBlock, std::min(tick, MillisecondsDouble{deadline - now}))};
        if (!tip) return nullptr;
        bool tip_changed{tip->hash != block_template->block.hashPrevBlock};

        // At this point the tip changed, a full tick went by or we reached
        // the deadline.

        // Must release m_tip_block_mutex before locking cs_main, to avoid deadlocks.
        LOCK(::cs_main);

        // On test networks return a minimum difficulty block after 20 minutes
        if (!tip_changed && allow_min_difficulty) {
            const NodeClock::time_point tip_time{std::chrono::seconds{m_chainman.ActiveChain().Tip()->GetBlockTime()}};
            if (now > tip_time + 20min) {
                tip_changed = true;
            }
        }

        /**
         * We determine if fees increased compared to the previous template by generating
         * a fresh template. There may be more efficient ways to determine how much
         * (approximate) fees for the next block increased, perhaps more so after
         * Cluster Mempool.
         *
         * We'll also create a new template if the tip changed during this iteration.
         */
        if (wait_options.fee_threshold < MAX_MONEY || tip_changed) {
            auto new_tmpl{CreateNewTemplate(create_options)};

            // If the tip changed, return the new template regardless of its fees.
            if (tip_changed) return new_tmpl;

            // Calculate the original template total fees if we haven't already
            if (current_fees == -1) {
                current_fees = std::accumulate(block_template->vTxFees.begin(), block_template->vTxFees.end(), CAmount{0});
            }

            // Check if fees increased enough to return the new template
            const CAmount new_fees = std::accumulate(new_tmpl->vTxFees.begin(), new_tmpl->vTxFees.end(), CAmount{0});
            Assume(wait_options.fee_threshold != MAX_MONEY);
            if (new_fees >= current_fees + wait_options.fee_threshold) return new_tmpl;
        }

        now = NodeClock::now();
    } while (now < deadline);

    return nullptr;
}

} // namespace node
