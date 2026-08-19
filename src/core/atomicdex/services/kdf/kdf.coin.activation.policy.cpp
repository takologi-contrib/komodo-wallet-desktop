/******************************************************************************
* Copyright © 2013-2024 The Komodo Platform Developers.                      *
*                                                                            *
* See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
* the top-level directory of this distribution for the individual copyright  *
* holder information and the developer policies on copyright and licensing.  *
*                                                                            *
* Unless otherwise agreed in a custom licensing agreement, no part of the    *
* Komodo Platform software, including this file may be copied, modified,     *
* propagated or distributed except according to the terms contained in the   *
* LICENSE file                                                               *
*                                                                            *
* Removal or modification of this copyright notice is prohibited.            *
*                                                                            *
******************************************************************************/

#include "atomicdex/services/kdf/kdf.coin.activation.policy.hpp"

namespace atomic_dex
{
    bool
    uses_task_activation(const coin_config_t& coin_info)
    {
        if (coin_info.coin_type == CoinType::ZHTLC)
        {
            return true;
        }
        if (coin_info.coin_type == CoinType::SIA)
        {
            return true;
        }
        return false;
    }

    bool
    uses_v2_history(const coin_config_t& coin_info)
    {
        if (coin_info.coin_type == CoinType::ZHTLC)
        {
            return true;
        }
        if (coin_info.coin_type == CoinType::TENDERMINT)
        {
            return true;
        }
        if (coin_info.coin_type == CoinType::TENDERMINTTOKEN)
        {
            return true;
        }
        return false;
    }
} // namespace atomic_dex
