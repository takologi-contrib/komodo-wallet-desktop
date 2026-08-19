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

#pragma once

//! Deps
#include "atomicdex/config/coins.cfg.hpp"

//! This header intentionally depends on nothing beyond coin_config_t/CoinType
//! (no live kdf_service, no ECS, no network) so the two predicates below can
//! be exercised by a plain unit test. kdf_service::uses_task_activation and
//! kdf_service::uses_v2_history are thin wrappers around these -- see
//! kdf.service.cpp -- kept so no existing call site inside the class needs
//! to change.

namespace atomic_dex
{
    //! Whether `coin_info` activates via a KDF task-based flow
    //! (`task::enable_*::init` / `::status`) rather than a synchronous
    //! `enable` RPC. Pure function of `coin_info.coin_type`.
    [[nodiscard]] bool uses_task_activation(const coin_config_t& coin_info);

    //! Whether `coin_info`'s transaction history must be fetched through the
    //! mmrpc-2.0 v2 envelope (`my_tx_history_v2`) instead of the legacy v1
    //! `my_tx_history` RPC. Pure function of `coin_info.coin_type`.
    //!
    //! SIA is deliberately *not* included here even though it uses the
    //! task-based activation flow above: GLEEC KDF serves Sia history only
    //! on the legacy v1 tier and rejects an SC request on the mmrpc-2.0
    //! method with `NotSupportedFor` (kdf-reloaded-public's
    //! RELOADED_VS_GLEEC.md), while KDF Reloaded accepts SC on both tiers.
    //! Routing SC through v1 works against either backend; v2 would
    //! silently break history for anyone pointed at GLEEC. See
    //! docs/plans/sia-transaction-history.md, "Design decision: v1, not v2".
    [[nodiscard]] bool uses_v2_history(const coin_config_t& coin_info);
} // namespace atomic_dex
