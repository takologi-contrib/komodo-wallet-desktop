#include <nlohmann/json.hpp>

#include "atomicdex/api/kdf/transaction.data.hpp"
#include "atomicdex/utilities/global.utilities.hpp"

namespace atomic_dex::kdf
{
    void from_json(const nlohmann::json& j, fee_regular_coin& cfg)
    {
        // KDF's TxFeeDetails is `#[serde(tag = "type")]`, and most variants
        // (Utxo, Slp, Solana) carry the amount as "amount" -- but Sia's own
        // shape ({"type":"Sia","coin":...,"policy":...,"total_amount":...})
        // names the same single-number fee "total_amount" instead. Both are
        // exactly one plain amount with nothing else worth displaying, so
        // both are handled by this same struct rather than adding a
        // Sia-specific one.
        if (j.contains("amount"))
        {
            j.at("amount").get_to(cfg.amount);
        }
        else
        {
            j.at("total_amount").get_to(cfg.amount);
        }
    }

    void from_json(const nlohmann::json& j, fee_erc_coin& cfg)
    {
        j.at("coin").get_to(cfg.coin);
        j.at("gas").get_to(cfg.gas);
        j.at("gas_price").get_to(cfg.gas_price);
        j.at("total_fee").get_to(cfg.total_fee);
    }

    void from_json(const nlohmann::json& j, fee_tendermint_coin& cfg)
    {
        j.at("coin").get_to(cfg.coin);
        j.at("type").get_to(cfg.type);
        j.at("amount").get_to(cfg.amount);
        j.at("gas_limit").get_to(cfg.gas_limit);
    }

    void from_json(const nlohmann::json& j, fee_qrc_coin& cfg)
    {
        j.at("coin").get_to(cfg.coin);
        j.at("gas_limit").get_to(cfg.gas_limit);
        j.at("gas_price").get_to(cfg.gas_price);
        j.at("miner_fee").get_to(cfg.miner_fee);
        j.at("total_gas_fee").get_to(cfg.total_gas_fee);
    }

    void from_json(const nlohmann::json& j, fees_data& cfg)
    {
        // Sia's TxFeeDetails (see fee_regular_coin::from_json above) carries
        // "total_amount" instead of "amount" -- route it to the same
        // single-amount struct rather than falling through to the ERC-gas
        // branch below, which threw ("key 'gas' not found") the moment a
        // real Sia withdraw first exercised this path: SC has no "amount"
        // key, isn't QTUM, and the ERC shape is not optional-field-tolerant.
        if (j.count("amount") == 1 || j.count("total_amount") == 1)
        {
            cfg.normal_fees = fee_regular_coin{};
            from_json(j, cfg.normal_fees.value());
        }
        else if (j.value("coin", std::string{}) == "QTUM" || j.value("coin", std::string{}) == "tQTUM")
        {
            cfg.qrc_fees = fee_qrc_coin{};
            from_json(j, cfg.qrc_fees.value());
        }
        else
        {
            // Falls through here for Eth/Qrc20-non-platform/Tron shapes.
            // Eth is genuinely gas-shaped and this is correct for it. Tron's
            // TxFeeDetails has no "coin" field and a completely different
            // shape (total_fee_sun/bandwidth/energy) that nothing here
            // parses yet; TRX/TRC20 cannot currently be activated through
            // this wallet's `enable` flow anyway (a separate, known gap), so
            // this stays a latent mismatch rather than a reachable crash for
            // now. The `.value(...)` above (rather than `.at("coin")`) at
            // least keeps a coinless fee shape like Tron's from throwing on
            // this check specifically.
            cfg.erc_fees = fee_erc_coin{};
            from_json(j, cfg.erc_fees.value());
        }
    }

    void from_json(const nlohmann::json& j, transaction_data& cfg)
    {
        j.at("block_height").get_to(cfg.block_height);
        j.at("coin").get_to(cfg.coin);
        j.at("from").get_to(cfg.from);
        j.at("to").get_to(cfg.to);
        j.at("tx_hash").get_to(cfg.tx_hash);
        j.at("my_balance_change").get_to(cfg.my_balance_change);
        j.at("received_by_me").get_to(cfg.received_by_me);
        j.at("spent_by_me").get_to(cfg.spent_by_me);
        j.at("timestamp").get_to(cfg.timestamp);

        // internal_id is numeric for ZHTLC - needs conversion
        //if (j.contains("internal_id"))
        //{
        //    cfg.internal_id = j.at("internal_id").get<std::string>();
        //}
        if (j.at("timestamp").get<std::size_t>() != 0)
        {
            cfg.timestamp = j.at("timestamp").get<std::size_t>();
        }
        else
        {
            using namespace std::chrono;
            cfg.timestamp      = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
        }

        if (j.contains("confirmations"))
        {
            cfg.confirmations = j.at("confirmations").get<std::size_t>();
        }

        // API returns null if no memo
        if (j.contains("memo"))
        {
            try
            {
                cfg.memo = j.at("memo").get<std::string>();
            }
            catch (const std::exception& ex)
            {
                [[maybe_unused]] auto err = ex.what();
                //SPDLOG_ERROR("Error parsing memo: {}", err);
                cfg.memo = "";
            }
        }

        if (cfg.from.empty())
        {
            if (cfg.coin == "FIRO")
            {
                cfg.from.emplace_back("Lelantusjsplit (Hidden)");
            }
            else
            {
                cfg.from.emplace_back("Shielded");
            }
        }

        if (j.contains("transaction_type"))
        {
            // Check if the "transaction_type" is an object for tendermint
            if (j.at("transaction_type").is_object())
            {
                for (auto& [k, v] : j.at("transaction_type").items()) {
                    cfg.tendermint_transaction_type = k;
                    cfg.tendermint_transaction_type_hash = v;
                }
            }
            // Check if "transaction_type" is a string
            else if (j.at("transaction_type").is_string())
            {
                cfg.transaction_type = j.at("transaction_type").get<std::string>();
            }
            else
            {
                // Handle the case where "transaction_type" is neither a string nor an object
                SPDLOG_ERROR("Unexpected type for transaction_type in JSON");
            }
        }

        // transaction_fee only in ZHTLC response
        if (j.contains("transaction_fee"))
        {
            cfg.transaction_fee = j.at("transaction_fee").get<std::string>();
        }
        else if (j.contains("fee_details"))
        {
            j.at("fee_details").get_to(cfg.fee_details);
        }

        // total_amount not in ZHTLC response
        if (j.contains("total_amount"))
        {
            j.at("total_amount").get_to(cfg.total_amount);
        }

        // tx_hex not in ZHTLC response
        if (j.contains("tx_hex"))
        {
            j.at("tx_hex").get_to(cfg.tx_hex);
        }

        std::string s         = atomic_dex::utils::to_human_date<std::chrono::seconds>(cfg.timestamp, "%e %b %Y, %H:%M");
        cfg.timestamp_as_date = std::move(s);
    }
} // namespace atomic_dex::kdf
