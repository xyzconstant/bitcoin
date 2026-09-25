// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCK_TEMPLATE_MANAGER_H
#define BITCOIN_NODE_BLOCK_TEMPLATE_MANAGER_H

#include <node/mining_types.h>
#include <util/time.h>

#include <memory>
#include <string>

class CBlock;
class ChainstateManager;
class CTxMemPool;

namespace interfaces {
struct BlockRef;
} // namespace interfaces

namespace node {
class TipWaiter;
struct CBlockTemplate;

/**
 * Creates block templates and submits solved blocks.
 * Owns the init-time block creation args.
 */
class BlockTemplateManager
{
private:
    CTxMemPool& m_mempool;
    ChainstateManager& m_chainman;
    const BlockCreateOptions m_block_create_args;

public:
    explicit BlockTemplateManager(CTxMemPool& mempool,
                                  ChainstateManager& chainman,
                                  BlockCreateOptions block_create_args = {});

    /** @return the block creation args set during node init. */
    const BlockCreateOptions& BlockCreateArgs() const { return m_block_create_args; }

    /** Create a fresh block template, applying init-time defaults to any unset options. */
    std::unique_ptr<CBlockTemplate> CreateNewTemplate(const BlockCreateOptions& options);

    /** Submit a block via ProcessNewBlock and capture validation state.
     *  @return whether the block was accepted as a new valid block. */
    bool SubmitBlock(const std::shared_ptr<const CBlock>& block, std::string& reason, std::string& debug);

    /** Return a new block template when fees rise to a certain threshold or
     *  after a new tip; return nullptr if timeout is reached. */
    std::unique_ptr<CBlockTemplate> WaitAndCreateNewBlock(
        TipWaiter& tip_waiter,
        const std::unique_ptr<CBlockTemplate>& block_template,
        const BlockWaitOptions& wait_options,
        const BlockCreateOptions& create_options);
};
} // namespace node

#endif // BITCOIN_NODE_BLOCK_TEMPLATE_MANAGER_H
