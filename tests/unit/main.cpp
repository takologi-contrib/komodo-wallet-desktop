// Unit tests for the pure JSON (de)serialization layer touched by
// docs/plans/sia-transaction-history.md. Hand-rolled runner (no framework
// dependency) -- see tests/unit/CMakeLists.txt for what this links and why.

#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "atomicdex/api/kdf/rpc_v2/rpc2.task.enable_sia_coin.init.hpp"
#include "atomicdex/api/kdf/rpc.tx.history.hpp"
#include "atomicdex/api/kdf/transaction.data.hpp"
#include "atomicdex/config/coins.cfg.hpp"
#include "atomicdex/services/kdf/kdf.coin.activation.policy.hpp"

namespace
{
    int g_failures = 0;

    void
    check(bool condition, const std::string& what)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        }
        else
        {
            std::printf("ok: %s\n", what.c_str());
        }
    }

    // ── Test case 1: enable_sia_coin_request::to_json emits tx_history=true ──
    void
    test_enable_sia_coin_request_carries_tx_history()
    {
        using namespace atomic_dex::kdf;
        enable_sia_coin_request request{.coin_name = "SC", .server_url = "https://walletd.example.com", .with_tx_history = true};
        nlohmann::json          j;
        to_json(j, request);
        check(j.at("params").at("tx_history").get<bool>() == true, "enable_sia_coin_request::to_json emits params.tx_history == true");
    }

    // ── Test case 2: tx_history_request::to_json(requires_v2=false) is flat v1 ──
    void
    test_tx_history_request_v1_shape_is_flat()
    {
        using namespace atomic_dex::kdf;
        tx_history_request request{.coin = "SC", .limit = 250, .paging_options = std::nullopt};
        // No "mmrpc" key present -- this is what a v1 (non-mmrpc-2.0) batch
        // request looks like going in, matching rpc.tx.history.cpp's own
        // branch condition (`j.contains("mmrpc") && ... == "2.0"`).
        nlohmann::json j = nlohmann::json::object();
        to_json(j, request);
        check(!j.contains("params"), "tx_history_request v1 shape has no \"params\" wrapper");
        check(!j.contains("mmrpc"), "tx_history_request v1 shape has no \"mmrpc\" key");
        check(j.at("coin").get<std::string>() == "SC", "tx_history_request v1 shape carries coin at the top level");
        check(j.at("limit").get<std::size_t>() == 250, "tx_history_request v1 shape carries limit at the top level");
    }

    // ── Test case 3: transaction_data::from_json parses a Sia-shaped fee ──
    void
    test_transaction_data_parses_sia_fee_shape()
    {
        using namespace atomic_dex::kdf;
        nlohmann::json j = {
            {"block_height", 589200},
            {"coin", "SC"},
            {"from", nlohmann::json::array({"addr1"})},
            {"to", nlohmann::json::array({"addr2"})},
            {"tx_hash", "abcd1234"},
            {"my_balance_change", "-1.00001"},
            {"received_by_me", "0"},
            {"spent_by_me", "1.00001"},
            {"timestamp", 1755000000},
            {"fee_details",
             {{"type", "Sia"}, {"coin", "SC"}, {"policy", {{"type", "pk"}, {"policy", "ed25519:deadbeef"}}}, {"total_amount", "0.00001"}}},
        };
        transaction_data data;
        try
        {
            from_json(j, data);
            check(data.fee_details.normal_fees.has_value(), "transaction_data::from_json routes Sia's total_amount fee shape to fee_regular_coin");
            check(
                data.fee_details.normal_fees.has_value() && data.fee_details.normal_fees->amount == "0.00001",
                "transaction_data::from_json reads Sia's total_amount as the fee amount");
        }
        catch (const std::exception& e)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: transaction_data::from_json threw on a Sia-shaped fixture: %s\n", e.what());
        }
    }

    // ── Test case 4: tx_history_answer parses a full v1 my_tx_history response ──
    void
    test_tx_history_answer_parses_v1_response_with_sia_transaction()
    {
        using namespace atomic_dex::kdf;
        nlohmann::json tx = {
            {"block_height", 589200},
            {"coin", "SC"},
            {"from", nlohmann::json::array({"addr1"})},
            {"to", nlohmann::json::array({"addr2"})},
            {"tx_hash", "abcd1234"},
            {"my_balance_change", "-1.00001"},
            {"received_by_me", "0"},
            {"spent_by_me", "1.00001"},
            {"timestamp", 1755000000},
            {"fee_details",
             {{"type", "Sia"}, {"coin", "SC"}, {"policy", {{"type", "pk"}, {"policy", "ed25519:deadbeef"}}}, {"total_amount", "0.00001"}}},
            {"transaction_type", "SiaV2Transaction"},
        };
        nlohmann::json response = {
            {"result",
             {{"limit", 250},
              {"skipped", 0},
              {"total", 1},
              {"current_block", 589300},
              {"sync_status", {{"state", "Finished"}}},
              {"transactions", nlohmann::json::array({tx})}}},
        };
        tx_history_answer answer;
        try
        {
            from_json(response, answer);
            check(!answer.error.has_value(), "tx_history_answer has no error field on a successful response");
            check(answer.result.has_value(), "tx_history_answer populates result on a successful response");
            check(
                answer.result.has_value() && answer.result->transactions.size() == 1,
                "tx_history_answer parses the one fixture transaction into result.transactions");
            check(
                answer.result.has_value() && answer.result->sync_status.state == "Finished",
                "tx_history_answer parses sync_status.state");
        }
        catch (const std::exception& e)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: tx_history_answer::from_json threw on a fixture v1 response: %s\n", e.what());
        }

        // An unrecognized field (transaction_type here, which
        // transaction_data::from_json does not read at all) must not break
        // parsing -- nlohmann::json's .at()/.get_to() pattern used
        // throughout this parser ignores fields it doesn't ask for.
        for (const std::string& state : {"NotEnabled", "NotStarted", "InProgress"})
        {
            nlohmann::json r2                                    = response;
            r2["result"]["sync_status"]                          = {{"state", state}};
            r2["result"]["transactions"][0]["transaction_type"]  = state == "NotEnabled" ? "SiaV1Transaction" : "SiaMinerPayout";
            tx_history_answer a2;
            try
            {
                from_json(r2, a2);
                check(
                    a2.result.has_value() && a2.result->sync_status.state == state,
                    "tx_history_answer parses sync_status.state == " + state);
            }
            catch (const std::exception& e)
            {
                ++g_failures;
                std::fprintf(stderr, "FAIL: tx_history_answer::from_json threw on sync_status.state == %s: %s\n", state.c_str(), e.what());
            }
        }
    }

    // ── Test case 5: the extracted activation-policy predicates ──
    void
    test_activation_policy_predicates()
    {
        using namespace atomic_dex;

        coin_config_t sia_coin;
        sia_coin.coin_type = CoinType::SIA;
        check(uses_v2_history(sia_coin) == false, "uses_v2_history(SIA) is false (routes through the legacy v1 my_tx_history path)");
        check(uses_task_activation(sia_coin) == true, "uses_task_activation(SIA) is true (unchanged by the v1/v2 history decision)");

        coin_config_t zhtlc_coin;
        zhtlc_coin.coin_type = CoinType::ZHTLC;
        check(uses_v2_history(zhtlc_coin) == true, "uses_v2_history(ZHTLC) is unchanged (still true)");
        check(uses_task_activation(zhtlc_coin) == true, "uses_task_activation(ZHTLC) is unchanged (still true)");

        coin_config_t utxo_coin;
        utxo_coin.coin_type = CoinType::UTXO;
        check(uses_v2_history(utxo_coin) == false, "uses_v2_history(UTXO) is unchanged (still false)");
        check(uses_task_activation(utxo_coin) == false, "uses_task_activation(UTXO) is unchanged (still false)");
    }
} // namespace

int
main()
{
    test_enable_sia_coin_request_carries_tx_history();
    test_tx_history_request_v1_shape_is_flat();
    test_transaction_data_parses_sia_fee_shape();
    test_tx_history_answer_parses_v1_response_with_sia_transaction();
    test_activation_policy_predicates();

    if (g_failures > 0)
    {
        std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
