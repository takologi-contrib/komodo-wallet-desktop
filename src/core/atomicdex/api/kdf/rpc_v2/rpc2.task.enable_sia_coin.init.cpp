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

//! Deps
#include <nlohmann/json.hpp>

//! Project Headers
#include "atomicdex/api/kdf/rpc_v2/rpc2.task.enable_sia_coin.init.hpp"

//! Implementation 2.0 RPC [enable_sia_coin]
namespace atomic_dex::kdf
{
    //! Serialization
    void to_json(nlohmann::json& j, const enable_sia_coin_request& request)
    {
        j["params"]["ticker"]                                                          = request.coin_name;
        j["params"]["activation_params"]["client_conf"]["server_url"]                  = request.server_url;
        // KDF's InitStandaloneCoinReq<T> only has "ticker" and
        // "activation_params" fields -- an unrecognized sibling key (this
        // used to be "params.tx_history") is silently dropped by serde, so
        // KDF never actually saw tx_history=true and the coin activated
        // with history tracking off regardless of what this wallet
        // requested. tx_history belongs to SiaCoinActivationRequest, i.e.
        // inside activation_params, matching the shape a task::enable_sia::init
        // request actually needs (confirmed against a real wire capture).
        j["params"]["activation_params"]["tx_history"]                                = request.with_tx_history;
    }

    //! Deserialization
    void from_json(const nlohmann::json& j, enable_sia_coin_answer& answer)
    {
        j.at("task_id").get_to(answer.task_id);
    }
} // namespace atomic_dex::kdf
