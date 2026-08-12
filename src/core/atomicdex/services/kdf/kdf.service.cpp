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

#include <algorithm>
#include <unordered_set>
#include <iostream>
#include <sstream>
#include <map>
#include <string>
#include <vector>

#include <async++.h>
#include <boost/thread/thread.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <range/v3/algorithm/any_of.hpp>
#include <QException>
#include <QFile>
#include <QSaveFile>
#include <QProcess>
#include <QSettings>

#include "atomicdex/api/kdf/utxo_merge_params.hpp"
#include "atomicdex/api/kdf/rpc_v1/rpc.electrum.hpp"
#include "atomicdex/api/kdf/rpc_v1/rpc.enable.hpp"
#include "atomicdex/api/kdf/rpc_v1/rpc.min_trading_vol.hpp"
#include "atomicdex/api/kdf/rpc.tx.history.hpp"
#include "atomicdex/api/kdf/rpc_v2/rpc2.z_coin_tx_history.hpp"
#include "atomicdex/api/kdf/rpc_v2/rpc2.task.enable_z_coin.init.hpp"
#include "atomicdex/api/kdf/rpc_v2/rpc2.task.enable_z_coin.status.hpp"
#include "atomicdex/api/kdf/rpc_v2/rpc2.task.enable_sia_coin.init.hpp"
#include "atomicdex/api/kdf/rpc_v2/rpc2.task.enable_sia_coin.status.hpp"
#include "atomicdex/config/kdf.cfg.hpp"
#include "atomicdex/config/coins.cfg.hpp"
#include "atomicdex/constants/dex.constants.hpp"
#include "atomicdex/managers/qt.wallet.manager.hpp"
#include "atomicdex/pages/qt.settings.page.hpp"
#include "atomicdex/services/kdf/kdf.service.hpp"
#include "atomicdex/utilities/qt.utilities.hpp"
#include "atomicdex/utilities/kill.hpp"

namespace ag = antara::gaming;

namespace
{
    void check_for_reconfiguration(const std::string& wallet_name)
    {
        try
        {
            using namespace std::string_literals;

            std::filesystem::path    cfg_path                   = atomic_dex::utils::get_atomic_dex_config_folder();
            std::string filename                   = std::string(atomic_dex::get_precedent_raw_version()) + "-coins." + wallet_name + ".json";
            std::filesystem::path    precedent_version_cfg_path = cfg_path / filename;

            if (std::filesystem::exists(precedent_version_cfg_path))
            {
                //! There is a precedent configuration file
                SPDLOG_INFO("There is a precedent configuration file, upgrading the new one with precedent settings");

                //! Old cfg to ifs
                LOG_PATH("opening previous version coins file: {}", precedent_version_cfg_path);
                QFile ifs;
                ifs.setFileName(atomic_dex::std_path_to_qstring(precedent_version_cfg_path));
                ifs.open(QIODevice::Text | QIODevice::ReadOnly);
                nlohmann::json precedent_config_json_data;
                precedent_config_json_data = nlohmann::json::parse(QString(ifs.readAll()).toStdString());

                //! New cfg to ifs
                std::filesystem::path actual_version_filepath = cfg_path / (std::string(atomic_dex::get_raw_version()) + "-coins."s + wallet_name + ".json"s);
                LOG_PATH("opening new version coins file: {}", actual_version_filepath);
                QFile actual_version_ifs;
                actual_version_ifs.setFileName(atomic_dex::std_path_to_qstring(actual_version_filepath));
                actual_version_ifs.open(QIODevice::Text | QIODevice::ReadOnly);
                nlohmann::json actual_config_data = nlohmann::json::parse(QString(actual_version_ifs.readAll()).toStdString());

                //! Iterate through new config
                for (auto& [key, value]: actual_config_data.items())
                {
                    //! If the coin in new config is present in the old one, copy the contents
                    if (precedent_config_json_data.contains(key))
                    {
                        actual_config_data.at(key)["active"] = precedent_config_json_data.at(key).at("active").get<bool>();
                    }
                }

                LOG_PATH("closing old version coins file: {}", precedent_version_cfg_path);
                ifs.close();
                LOG_PATH("closing new version coins file: {}", actual_version_filepath);
                actual_version_ifs.close();

                //! Write contents
                LOG_PATH("opening new version file: {}", actual_version_filepath);
                QFile ofs;
                ofs.setFileName(atomic_dex::std_path_to_qstring(actual_version_filepath));
                ofs.open(QIODevice::Text | QIODevice::WriteOnly);
                ofs.write(QString::fromStdString(actual_config_data.dump()).toUtf8());

                //! Delete old cfg
                std::error_code ec;
                std::filesystem::remove(precedent_version_cfg_path, ec);
                if (ec)
                {
                    SPDLOG_ERROR("error: {}", ec.message());
                }
                LOG_PATH("closing new version file: {}", actual_version_filepath);
                ofs.close();
            }
        }
        catch (const std::exception& error)
        {
            SPDLOG_ERROR("exception caught: {}", error.what());
        }
    }

    void update_coin_status(const std::string& wallet_name, const std::vector<std::string>& tickers, bool status,
                            atomic_dex::t_coins_registry& registry, std::shared_mutex& registry_mtx, std::string field_name = "active")
    {
        if (wallet_name == "")
        {
            return;
        }
        if (tickers.empty())
        {
            SPDLOG_DEBUG("Tickers list empty, skipping update_coin_status");
            return;
        }
        SPDLOG_INFO("Update coins status to: {} - field_name: {} - tickers: {}", status, field_name, fmt::join(tickers, ", "));
        std::filesystem::path  cfg_path                = atomic_dex::utils::get_atomic_dex_config_folder();
        std::string            filename                = std::string(atomic_dex::get_raw_version()) + "-coins." + wallet_name + ".json";
        std::filesystem::path  filepath                = cfg_path / filename;

        //! Exclusive for the whole read-modify-write. Several coins finish
        //! activation on different threads at once, and each call rewrites the
        //! entire file; a shared lock let them interleave, so one writer's
        //! output could land on top of another's and leave a file that no
        //! longer parses. The registry mutation below needs exclusivity anyway.
        std::unique_lock lock(registry_mtx);

        nlohmann::json config_json_data = atomic_dex::utils::read_json_file(filepath);

        //! `read_json_file` yields a default-constructed (null) json when the
        //! file is missing or does not parse. Indexing that throws, and this
        //! runs inside a Qt event handler where an exception aborts the
        //! process, so refuse the update instead.
        //!
        //! Deliberately not rebuilt from scratch: overwriting an unreadable
        //! config would discard every other coin's saved settings. The
        //! in-memory registry is still updated, so the session behaves
        //! correctly; only persistence is skipped, and loudly.
        if (!config_json_data.is_object())
        {
            SPDLOG_ERROR(
                "Coins config is missing or unreadable, skipping persistence of {}={} for [{}]. Path: {}", field_name, status,
                fmt::join(tickers, ", "), filepath.string());
            for (auto&& ticker: tickers)
            {
                if (field_name == "active")
                {
                    registry[ticker].active = status;
                }
            }
            return;
        }

        for (auto&& ticker: tickers)
        {
            SPDLOG_DEBUG("Setting ticker: {} field {} to {}", ticker, field_name, status);
            //! `at` throws for a ticker the config does not carry; skipping is
            //! the safe reaction, for the same event-handler reason as above.
            if (config_json_data.contains(ticker))
            {
                config_json_data[ticker][field_name] = status;
            }
            else
            {
                SPDLOG_WARN("Ticker {} is absent from the coins config, not persisting {}={}", ticker, field_name, status);
            }

            if (field_name == "active")
            {
                SPDLOG_DEBUG("ticker: {} status active: {}", ticker, status);
                registry[ticker].active = status;
            }
        }

        //! Write contents.
        //!
        //! QSaveFile writes to a temporary file and renames it into place on
        //! commit, so a reader never observes a half-written config and an
        //! interrupted write leaves the previous file intact. A plain QFile
        //! truncates in place, which is how the corruption above arose.
        QSaveFile ofs;
        ofs.setFileName(atomic_dex::std_path_to_qstring(filepath));
        if (!ofs.open(QIODevice::Text | QIODevice::WriteOnly))
        {
            SPDLOG_ERROR("Could not open coins config for writing: {} ({})", filepath.string(), ofs.errorString().toStdString());
            return;
        }
        const std::string serialized = config_json_data.dump();
        if (ofs.write(QString::fromStdString(serialized).toUtf8()) == -1 || !ofs.commit())
        {
            SPDLOG_ERROR("Could not write coins config: {} ({})", filepath.string(), ofs.errorString().toStdString());
            return;
        }

        SPDLOG_DEBUG("Coins file updated to set {}: {} | tickers: [{}]", field_name, status,  fmt::join(tickers, ", "));
    }
}

namespace atomic_dex
{
    std::vector<atomic_dex::coin_config_t> kdf_service::retrieve_coins_informations()
    {
        std::vector<atomic_dex::coin_config_t> cfg;

        check_for_reconfiguration(m_current_wallet_name);
        const auto  cfg_path               = atomic_dex::utils::get_atomic_dex_config_folder();
        std::string filename               = std::string(atomic_dex::get_raw_version()) + "-coins." + m_current_wallet_name + ".json";

        LOG_PATH("Retrieving Wallet information of {}", (cfg_path / filename));
        auto retrieve_cfg_functor = [](std::filesystem::path path) -> std::unordered_map<std::string, atomic_dex::coin_config_t>
        {
            if (exists(path))
            {
                try
                {
                    QFile ifs;
                    ifs.setFileName(atomic_dex::std_path_to_qstring(path));
                    ifs.open(QIODevice::ReadOnly | QIODevice::Text);
                    nlohmann::json config_json_data = nlohmann::json::parse(QString(ifs.readAll()).toStdString());

                    //! Iterate through config
                    for (auto& [key, value]: config_json_data.items())
                    {
                        //! Ensure default coin are marked as active
                        if (is_default_coin(key))
                        {
                            config_json_data.at(key)["active"] = true;
                        }
                    }

                    auto res = config_json_data.get<std::unordered_map<std::string, atomic_dex::coin_config_t>>();
                    return res;
                }
                catch (const std::exception& error)
                {
                    SPDLOG_ERROR("exception in kdf_service::retrieve_coins_informations: {}", error.what());
                }
            }
            else
            {
                SPDLOG_ERROR("Coins file does not exist!");
            }
            return {};
        };

        auto official_cfg = retrieve_cfg_functor(cfg_path / filename);
        if (!official_cfg.empty())
        {
            cfg.reserve(official_cfg.size());
            for (auto&& [key, value]: official_cfg) { cfg.emplace_back(value); }
            {
                std::unique_lock lock(m_coin_cfg_mutex);
                m_coins_informations = std::move(official_cfg);
            }
        }

        return cfg;
    }

    kdf_service::kdf_service(entt::registry& registry, ag::ecs::system_manager& system_manager) : system(registry), m_system_manager(system_manager)
    {
        m_orderbook_clock = std::chrono::high_resolution_clock::now();
        m_orders_clock    = std::chrono::high_resolution_clock::now();
        m_info_clock      = std::chrono::high_resolution_clock::now();
        dispatcher_.sink<gui_enter_trading>().connect<&kdf_service::on_gui_enter_trading>(*this);
        dispatcher_.sink<gui_leave_trading>().connect<&kdf_service::on_gui_leave_trading>(*this);
        dispatcher_.sink<gui_enter_wallet>().connect<&kdf_service::on_gui_enter_wallet>(*this);
        dispatcher_.sink<gui_leave_wallet>().connect<&kdf_service::on_gui_leave_wallet>(*this);
        dispatcher_.sink<refresh_orderbook_model_data>().connect<&kdf_service::on_refresh_orderbook_model_data>(*this);
        dispatcher_.sink<coin_fully_initialized>().connect<&kdf_service::on_coin_fully_initialized_event>(*this);
        SPDLOG_INFO("kdf_service created");
    }

    void kdf_service::update()
    {
        using namespace std::chrono_literals;

        if (not m_kdf_running)
        {
            return;
        }

        const auto now          = std::chrono::high_resolution_clock::now();
        const auto s_orderbook  = std::chrono::duration_cast<std::chrono::seconds>(now - m_orderbook_clock);
        const auto s_info       = std::chrono::duration_cast<std::chrono::seconds>(now - m_info_clock);
        const auto s_activation = std::chrono::duration_cast<std::chrono::seconds>(now - m_activation_clock);
        const auto s_orders     = std::chrono::duration_cast<std::chrono::seconds>(now - m_orders_clock);

        if (s_orderbook >= 13s)
        {
            fetch_current_orderbook_thread(false); // process_orderbook (not a reset) if on trading page
            m_orderbook_clock = std::chrono::high_resolution_clock::now();
        }

        if (s_orders >= 17s)
        {
            batch_fetch_orders_and_swap(); // gets 'my_orders', 'my_recent_swaps' & 'active_swaps'
            m_orders_clock = std::chrono::high_resolution_clock::now();
        }

        if (s_activation >= 4s)
        {
            auto                     coins = this->get_enabled_coins();
            std::vector<std::string> tickers;
            for (auto&& coin: coins)
            {
                if (!coin.active)
                {
                    tickers.push_back(coin.ticker);
                }
            }
            if (!tickers.empty())
            {
                // Mark coins as active internally, and updates the coins file
                SPDLOG_DEBUG("Making sure {} enabled coins are marked as active", tickers.size());
                update_coin_status(this->m_current_wallet_name, tickers, true, m_coins_informations, m_coin_cfg_mutex);
            }

            if (!m_activation_queue.empty())
            {
                std::unique_lock lock(m_activation_mutex);
                SPDLOG_DEBUG("{} coins in the activation queue", m_activation_queue.size());
                t_coins to_enable;
                
                for (size_t i = 0; i < 20 && i < m_activation_queue.size(); ++i) {
                    to_enable.push_back(m_activation_queue[i]);
                }
                activate_coins(to_enable);
                m_activation_queue.erase(m_activation_queue.begin(), m_activation_queue.begin() + to_enable.size());
                m_activation_clock = std::chrono::high_resolution_clock::now();
            }
            else {
                m_activation_clock = std::chrono::high_resolution_clock::now() + std::chrono::duration_cast<std::chrono::seconds>(std::chrono::seconds(4));
            }
        }

        if (s_info >= 43s)
        {
            std::unique_lock lock(m_activation_mutex);
            if (m_activation_queue.empty())
            {
                fetch_infos_thread();
                m_info_clock = std::chrono::high_resolution_clock::now();
            }
        }
    }

    kdf_service::~kdf_service()
    {
        SPDLOG_INFO("destroying kdf service...");
        dispatcher_.sink<gui_enter_trading>().disconnect<&kdf_service::on_gui_enter_trading>(*this);
        dispatcher_.sink<gui_leave_trading>().disconnect<&kdf_service::on_gui_leave_trading>(*this);
        dispatcher_.sink<gui_enter_wallet>().disconnect<&kdf_service::on_gui_enter_wallet>(*this);
        dispatcher_.sink<gui_leave_wallet>().disconnect<&kdf_service::on_gui_leave_wallet>(*this);
        dispatcher_.sink<refresh_orderbook_model_data>().disconnect<&kdf_service::on_refresh_orderbook_model_data>(*this);
        dispatcher_.sink<coin_fully_initialized>().disconnect<&kdf_service::on_coin_fully_initialized_event>(*this);
        SPDLOG_INFO("kdf signals successfully disconnected");
        bool kdf_stopped = false;
        if (m_kdf_running)
        {
            SPDLOG_INFO("preparing kdf stop batch request");
            nlohmann::json stop_request = kdf::template_request("stop");
            nlohmann::json batch        = nlohmann::json::array();
            batch.push_back(stop_request);
            SPDLOG_INFO("processing kdf stop batch request");
            pplx::task<web::http::http_response> resp_task = m_kdf_client.async_rpc_batch_standalone(batch);
            web::http::http_response             resp      = resp_task.get();
            SPDLOG_INFO("kdf stop batch answer received");
            auto answers = kdf::basic_batch_answer(resp);
            if (answers[0].contains("result"))
            {
                kdf_stopped = answers[0].at("result").get<std::string>() == "success";
                SPDLOG_INFO("kdf successfully stopped with rpc stop");
            }
        }
        m_kdf_running = false;
        m_kdf_client.stop();

        if (!kdf_stopped)
        {
            SPDLOG_INFO("kdf didn't stop yet with rpc stop, stopping process manually");
#if defined(_WIN32) || defined(WIN32)
            atomic_dex::kill_executable(atomic_dex::g_dex_api);
#else
            /*const auto ec = m_kdf_instance.stop(stop_actions).second;

            if (ec)
            {
                SPDLOG_ERROR("error when stopping kdf by process: {}", ec.message());
                // std::cerr << "error: " << ec.message() << std::endl;
            }*/
#endif
        }

        if (m_kdf_init_thread.joinable())
        {
            m_kdf_init_thread.join();
            SPDLOG_INFO("kdf init thread destroyed");
        }
        SPDLOG_INFO("kdf service fully destroyed");
    }

    const std::atomic_bool& kdf_service::is_kdf_running() const
    {
        return m_kdf_running;
    }

    t_coins kdf_service::get_enabled_coins() const
    {
        t_coins destination;

        std::shared_lock lock(m_coin_cfg_mutex);
        for (auto&& [key, value]: m_coins_informations)
        {
            if (value.currently_enabled)
            {
                destination.push_back(value);
            }
        }

        return destination;
    }

    t_coins kdf_service::get_active_coins() const
    {
        t_coins destination;

        std::shared_lock lock(m_coin_cfg_mutex);
        for (auto&& [key, value]: m_coins_informations)
        {
            if (value.active)
            {
                destination.emplace_back(value);
            }
        }

        return destination;
    }

    void kdf_service::enable_z_coin_cancel(const std::int8_t task_id)
    {
        t_enable_z_coin_cancel_request request{.task_id = task_id};
        auto                                answer = m_kdf_client.rpc_enable_z_coin_cancel(std::move(request));
        // SPDLOG_DEBUG("kdf_service::enable_z_coin_cancel: [task_id {}]  result: {}", task_id, answer.raw_result);
    }

    bool kdf_service::disable_coin(const std::string& ticker, std::error_code& ec)
    {
        coin_config_t coin_info = get_coin_info(ticker);
        if (not coin_info.currently_enabled)
        {
            SPDLOG_ERROR("kdf_service::disable_coin: {} not currently_enabled", ticker);
            return true;
        }

        t_disable_coin_request request{.coin = ticker};

        auto                   answer = m_kdf_client.rpc_disable_coin(std::move(request));
        // SPDLOG_DEBUG("kdf_service::disable_coin: {} result: {}", ticker, answer.raw_result);
        // kdf_service::disable_coin: BRZ-PLG20 result: {"result":{"coin":"BRZ-PLG20","cancelled_orders":[],"passivized":false}}

        if (answer.error.has_value())
        {
            std::string error = answer.error.value();
            SPDLOG_ERROR("kdf_service::disable_coin disabling {} failed with error: {}", ticker, error);
            if (error.find("such coin") != std::string::npos)
            {
                ec = dextop_error::disable_unknown_coin;
                return false;
            }
            if (error.find("active swaps") != std::string::npos)
            {
                ec = dextop_error::active_swap_is_using_the_coin;
                return false;
            }
            if (error.find("matching orders") != std::string::npos)
            {
                ec = dextop_error::order_is_matched_at_the_moment;
                return false;
            }
            return false;
        }
        coin_info.currently_enabled = false;
        {
            std::unique_lock lock(m_coin_cfg_mutex);
            m_coins_informations[ticker].currently_enabled = false;
        }
        dispatcher_.trigger<coin_disabled>(ticker);
        return true;
    }

    bool kdf_service::enable_default_coins()
    {
        std::atomic<std::size_t> result{1};
        auto                     coins = get_active_coins();
        
        enable_coins(coins);
        this->dispatcher_.trigger<default_coins_enabled>();
        return result.load() == 1;
    }

    void kdf_service::enable_coin(const std::string& ticker)
    {
        enable_coin(get_coin_info(ticker));
    }

    void kdf_service::enable_coin(const coin_config_t& coin_config)
    {
        enable_coins(t_coins{coin_config});
    }
    
    void kdf_service::enable_coins(const std::vector<std::string>& tickers)
    {
        t_coins coins{};
        
        for (const auto& ticker : tickers)
        {
            coins.push_back(get_coin_info(ticker));
        }
        coins.erase(std::unique(coins.begin(), coins.end(), [](auto left, auto right) { return left.ticker == right.ticker; }), coins.end()); // Remove duplicates
        enable_coins(coins);
    }

    void kdf_service::enable_coins(const t_coins& coins)
    {
        t_coins enabled_coins = get_enabled_coins();
        for (const auto& coin : coins)
        {
            if (ranges::any_of(enabled_coins, [&coin](const auto& enabled_coin) { return enabled_coin.ticker == coin.ticker; }))
            {
                SPDLOG_WARN("{} cannot be enabled because it already is or is being enabled.", coin.ticker);
                continue;
            }
            std::unique_lock lock(m_activation_mutex);
            m_activation_queue.push_back(coin);
        }
        m_activation_clock = std::chrono::high_resolution_clock::now() - std::chrono::duration_cast<std::chrono::seconds>(std::chrono::seconds(13));
    }

    void kdf_service::activate_coins(const t_coins& coins)
    {
        t_coins other_coins;
        t_coins sia_coins;
        t_coins erc_family_coins;
        t_coins zhtlc_coins;
        t_coins tendermint_coins;
        t_coins bep20_coins;
        t_coins bep20_testnet_coins;
       
        SPDLOG_INFO(">>>>>>>>>>>>>>>>>>>>>>>>>>> Enabling {} coins <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<", coins.size());
        for (const auto& coin_cfg : coins)
        {
            if (coin_cfg.currently_enabled)
            {
                SPDLOG_WARN("{} cannot be enabled because it already is or is being enabled.", coin_cfg.ticker);
                continue;
            }
            if (coin_cfg.coin_type == CoinType::TENDERMINT || coin_cfg.coin_type == CoinType::TENDERMINTTOKEN)
            {
                tendermint_coins.push_back(coin_cfg);
            }
            else if (coin_cfg.coin_type == CoinType::SIA)
            {
                sia_coins.push_back(coin_cfg);
            }
            else if (coin_cfg.coin_type == CoinType::ZHTLC)
            {
                zhtlc_coins.push_back(coin_cfg);
            }
            else if (coin_cfg.coin_type == CoinType::BEP20)
            {
                coin_cfg.is_testnet.value_or(false) ? bep20_testnet_coins.push_back(coin_cfg) : bep20_coins.push_back(coin_cfg);
            }
            else if (coin_cfg.is_erc_family)
            {
                erc_family_coins.push_back(coin_cfg);
            }
            else
            {
                other_coins.push_back(coin_cfg);
            }
        }
        if (other_coins.size() > 0)
        {
            SPDLOG_INFO(">>>>>>>>>>>>>>>>>>>>>>>>>>> Enabling {} utxo_qrc20_coins <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<", other_coins.size());
            enable_utxo_qrc20_coins(other_coins);
        }
        if (bep20_coins.size() > 0)
        {
            SPDLOG_INFO(">>>>>>>>>>>>>>>>>>>>>>>>>>> Enabling {} BEP20 coins <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<", bep20_coins.size());
            // enable_erc20_coins(bep20_coins, "BNB");
            enable_erc_family_coins(bep20_coins);
        }
        if (bep20_testnet_coins.size() > 0)
        {
            SPDLOG_INFO(">>>>>>>>>>>>>>>>>>>>>>>>>>> Enabling {} bep20_testnet_coins <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<", bep20_testnet_coins.size());
            // enable_erc20_coins(bep20_testnet_coins, "BNBT");
            enable_erc_family_coins(bep20_testnet_coins);
        }
        if (erc_family_coins.size() > 0)
        {
            SPDLOG_INFO(">>>>>>>>>>>>>>>>>>>>>>>>>>> Enabling {} erc_family_coins <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<", erc_family_coins.size());
            enable_erc_family_coins(erc_family_coins);
        }
        if (zhtlc_coins.size() > 0)
        {
            SPDLOG_INFO(">>>>>>>>>>>>>>>>>>>>>>>>>>> Enabling {} zhtlc_coins <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<", zhtlc_coins.size());
            enable_task(zhtlc_coins);
        }
        if (tendermint_coins.size() > 0)
        {
            SPDLOG_INFO(">>>>>>>>>>>>>>>>>>>>>>>>>>> Enabling {} tendermint_coins <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<", tendermint_coins.size());
            for (const auto& [parent_coin, coins_vector] : groupByParentCoin(tendermint_coins)) {
                enable_tendermint_coins(coins_vector, parent_coin);
            }
        }
        if (sia_coins.size() > 0)
        {
            SPDLOG_INFO(">>>>>>>>>>>>>>>>>>>>>>>>>>> Enabling {} sia_coins <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<", sia_coins.size());
            enable_task(sia_coins);
        }
    }

    void kdf_service::enable_erc_family_coin(const coin_config_t& coin_cfg)
    {
        enable_erc_family_coins(t_coins{coin_cfg});
    }

    void kdf_service::enable_erc_family_coins(const t_coins& coins)
    {
        nlohmann::json batch_array = nlohmann::json::array();
        auto callback = [this, coins](const web::http::http_response& resp)
        {
            try
            {
                auto answers = kdf::basic_batch_answer(resp);
                
                if (answers.count("error") == 0)
                {
                    std::size_t                     idx = 0;
                    t_coins                         activated_coins;
                    t_coins                         failed_coins;
                    
                    for (auto&& answer : answers)
                    {
                        auto [res, error] = this->process_batch_enable_answer(answer);
                        if (!res)
                        {
                            if (error.find("is initialized already") != std::string::npos)
                            {
                                SPDLOG_WARN("{} {}: ", coins[idx].ticker, error);
                                activated_coins.push_back(std::move(coins[idx]));
                            }
                            else
                            {
                                failed_coins.push_back(std::move(coins[idx]));
                                this->dispatcher_.trigger<enabling_coin_failed>(coins[idx].ticker, error);
                                SPDLOG_ERROR(error);
                            }
                        }
                        else
                        {
                            activated_coins.push_back(std::move(coins[idx]));
                            this->process_balance_answer(answer);
                        }
                        idx += 1;
                    }
                    std::vector<std::string> tickers;
                    for (auto&& coin: activated_coins)
                    {

                        std::unique_lock lock(m_coin_cfg_mutex);
                        m_coins_informations[coin.ticker].currently_enabled = true;
                        tickers.push_back(coin.ticker);
                    }
                    dispatcher_.trigger<coin_fully_initialized>(tickers);

                    std::vector<std::string> failed_tickers;
                    for (auto&& coin: failed_coins)
                    {
                        std::unique_lock lock(m_coin_cfg_mutex);
                        m_coins_informations[coin.ticker].currently_enabled = false;
                        failed_tickers.push_back(coin.ticker);
                    }
                }
            }
            catch (const std::exception& error)
            {
                SPDLOG_ERROR("exception in kdf_service::enable_erc_family_coins: {}", error.what());
            }
        };
        
        for (const auto& coin_config : coins)
        {
            t_enable_request request
            {
                .coin_name                       = coin_config.ticker,
                .urls                            = coin_config.eth_family_urls.value_or(std::vector<std::string>{}),
                .coin_type                       = coin_config.coin_type,
                .is_testnet                      = coin_config.is_testnet.value_or(false),
                .swap_contract_address           = coin_config.swap_contract_address.value_or(""),
                .with_tx_history                 = false
            };

            if (coin_config.fallback_swap_contract.value_or("") != "")
            {
                request.fallback_swap_contract = coin_config.fallback_swap_contract;
            }

            if (coin_config.wallet_only)
            {
                request.kdf = 0;
            }

            nlohmann::json j = kdf::template_request("enable");
            kdf::to_json(j, request);
            batch_array.push_back(j);
        }
        m_kdf_client.async_rpc_batch_standalone(batch_array)
            .then(callback)
            .then([this, batch_array](pplx::task<void> previous_task) { this->handle_exception_pplx_task(previous_task, "enable_common_coins", batch_array); });
    }

    void kdf_service::enable_utxo_qrc20_coin(coin_config_t coin_config)
    {
        enable_utxo_qrc20_coins(t_coins{std::move(coin_config)});
    }

    void kdf_service::enable_utxo_qrc20_coins(const t_coins& coins)
    {
        auto batch_array = nlohmann::json::array();
        auto callback = [this, coins](const web::http::http_response& resp)
        {
            try
            {
                auto answers = kdf::basic_batch_answer(resp);

                if (answers.count("error") == 0)
                {
                    std::size_t                     idx = 0;
                    t_coins                         activated_coins;
                    t_coins                         failed_coins;
                    
                    for (auto&& answer : answers)
                    {
                        auto [res, error] = this->process_batch_enable_answer(answer);
                        if (!res)
                        {
                            if (error.find("already initialized") != std::string::npos)
                            {
                                SPDLOG_WARN("{} {}: ", coins[idx].ticker, error);
                                activated_coins.push_back(std::move(coins[idx]));
                            }
                            else
                            {
                                failed_coins.push_back(std::move(coins[idx]));
                                this->dispatcher_.trigger<enabling_coin_failed>(coins[idx].ticker, error);
                                SPDLOG_ERROR("Error in kdf_service::enable_utxo_qrc20_coins: {}", error);
                            }
                        }
                        else
                        {
                            this->process_balance_answer(answer);
                            activated_coins.push_back(std::move(coins[idx]));
                        }
                        idx += 1;
                    }

                    std::vector<std::string> tickers;
                    for (auto&& coin: activated_coins)
                    {
                        std::unique_lock lock(m_coin_cfg_mutex);
                        m_coins_informations[coin.ticker].currently_enabled = true;
                        tickers.push_back(coin.ticker);
                    }
                    dispatcher_.trigger<coin_fully_initialized>(tickers);

                    std::vector<std::string> failed_tickers;
                    for (auto&& coin: failed_coins)
                    {
                        std::unique_lock lock(m_coin_cfg_mutex);
                        m_coins_informations[coin.ticker].currently_enabled = false;
                        failed_tickers.push_back(coin.ticker);
                    }
                }
            }
            catch (const std::exception& error)
            {
                SPDLOG_ERROR("exception in kdf_service::enable_utxo_qrc20_coins: {}", error.what());
            }
        };
        
        for (const auto& coin_config : coins)
        {
            nlohmann::json j = kdf::template_request("electrum");
            t_electrum_request request
            {
                .coin_name       = coin_config.ticker,
                .servers         = coin_config.electrum_urls.value_or(get_electrum_server_from_token(coin_config.ticker)),
                .coin_type       = coin_config.coin_type,
                .is_testnet      = coin_config.is_testnet.value_or(false),
                .with_tx_history = true,
                .min_connected   = 1,
                .max_connected   = 1
            };
            if (coin_config.merge_utxos.value_or(false))
            {
                kdf::utxo_merge_params_t  merge_params{.merge_at = 250, .check_every = 300, .max_merge_at_once = 125};
                nlohmann::json            json_merge_params;
                
                kdf::to_json(json_merge_params, merge_params);
                request.merge_params = json_merge_params;
            }
            if (coin_config.swap_contract_address.value_or("") != "")
            {
                request.swap_contract_address = coin_config.swap_contract_address;
            }
            if (coin_config.fallback_swap_contract.value_or("") != "")
            {
                request.fallback_swap_contract = coin_config.fallback_swap_contract;
            }
            kdf::to_json(j, request);
            batch_array.push_back(j);
        }
        m_kdf_client.async_rpc_batch_standalone(batch_array)
            .then(callback)
            .then([this, batch_array](pplx::task<void> previous_task) { this->handle_exception_pplx_task(previous_task, "enable_qrc_family_coins", batch_array); });
    }

    void kdf_service::enable_erc20_coin(coin_config_t coin_config, std::string parent_ticker)
    {
        enable_erc20_coins(t_coins{std::move(coin_config)}, parent_ticker);
    }

    void kdf_service::enable_erc20_coins(const t_coins& coins, const std::string parent_ticker)
    {
        auto callback = [this]<typename RpcRequest>(RpcRequest rpc)
        {
            if (rpc.error)
            {
                SPDLOG_ERROR("{} {}: ", rpc.request.ticker, rpc.error->error_type);
                if (rpc.error->error_type.find("PlatformIsAlreadyActivated") != std::string::npos)
                {
                    SPDLOG_ERROR("{} {}: ", rpc.request.ticker, rpc.error->error_type);
                    std::unique_lock lock(m_coin_cfg_mutex);
                    m_coins_informations[rpc.request.ticker].currently_enabled = true;
                    dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {rpc.request.ticker}});
                    if constexpr (std::is_same_v<RpcRequest, kdf::enable_eth_with_tokens_rpc>)
                    {
                        SPDLOG_ERROR("{} {}: ", rpc.request.ticker, rpc.error->error_type);
                        
                        for (const auto& erc20_coin_info : rpc.request.erc20_tokens_requests)
                        {
                            SPDLOG_ERROR("{} {}: ", erc20_coin_info.ticker, rpc.error->error_type);
                            std::unique_lock lock(m_coin_cfg_mutex);
                            m_coins_informations[erc20_coin_info.ticker].currently_enabled = true;
                            dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {erc20_coin_info.ticker}});
                        }
                    }
                }
                else if (rpc.error->error_type.find("TokenIsAlreadyActivated") != std::string::npos)
                {
                    SPDLOG_ERROR("{} {}: ", rpc.request.ticker, rpc.error->error_type);
                }
                else
                {
                    SPDLOG_ERROR("marking {} as inactive: {}", rpc.request.ticker, rpc.error->error_type);
                    std::unique_lock lock(m_coin_cfg_mutex);
                    m_coins_informations[rpc.request.ticker].currently_enabled = false;
                    this->dispatcher_.trigger<enabling_coin_failed>(rpc.request.ticker, rpc.error->error);
                }
            }
            else
            {
                dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {rpc.request.ticker}});
                std::unique_lock lock(m_coin_cfg_mutex);
                m_coins_informations[rpc.request.ticker].currently_enabled = true;
                SPDLOG_DEBUG("marking {} as active", rpc.request.ticker);
                if constexpr (std::is_same_v<RpcRequest, kdf::enable_eth_with_tokens_rpc>)
                {
                    for (const auto& erc20_address_info : rpc.result->erc20_addresses_infos)
                    {
                        SPDLOG_DEBUG("erc20_address_info.first {}: ", erc20_address_info.first);
                        if (erc20_address_info.second.balances.empty())
                        {
                            SPDLOG_DEBUG("erc20_address_info.second.balances is empty");
                        }
                        else
                        {
                            for (const auto& balance : erc20_address_info.second.balances)
                            {
                                SPDLOG_DEBUG("marking token {} as active", balance.first);
                                dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {balance.first}});
                                process_balance_answer(rpc);
                                std::unique_lock lock(m_coin_cfg_mutex);
                                m_coins_informations[balance.first].currently_enabled = true;
                            }
                        }
                    }
                }
                SPDLOG_DEBUG("process_balance_answer(rpc) done");
            }
        };

        if (!has_coin(parent_ticker))
        {
            static constexpr auto error = "{} is not present in the config. Cannot enable {} tokens.";
            this->dispatcher_.trigger<enabling_coin_failed>(parent_ticker, fmt::format(error, parent_ticker, parent_ticker));
            return;
        }

        auto parent_ticker_info = get_coin_info(parent_ticker);

        if (parent_ticker_info.currently_enabled)
        {
            for (const auto& token_config : coins)
            {
                // SPDLOG_DEBUG("Processing {} token: {}", parent_ticker, token_config.ticker);
                kdf::enable_erc20_rpc rpc{.request={.ticker = token_config.ticker}};

                if (token_config.ticker == parent_ticker_info.ticker)
                {
                    continue;
                }
                m_kdf_client.process_rpc_async<kdf::enable_erc20_rpc>(rpc.request, callback);
            }
        }
        else
        {
            kdf::enable_eth_with_tokens_rpc rpc;
            rpc.request.ticker = parent_ticker_info.ticker;
            rpc.request.nodes = parent_ticker_info.urls.value_or(std::vector<node>{});
            rpc.request.swap_contract_address = parent_ticker_info.swap_contract_address.value_or("");
            if (parent_ticker_info.fallback_swap_contract.value_or("") != "")
            {
                rpc.request.fallback_swap_contract = parent_ticker_info.fallback_swap_contract.value_or("");
            }
            for (const auto& coin_config : coins)
            {
                if (coin_config.ticker == parent_ticker_info.ticker)
                {
                    continue;
                }
                rpc.request.erc20_tokens_requests.push_back({.ticker = coin_config.ticker});
            }
            m_kdf_client.process_rpc_async<kdf::enable_eth_with_tokens_rpc>(rpc.request, callback);
        }
        SPDLOG_DEBUG("kdf_service::enable_erc20_coins done for {}", parent_ticker);
    }

    std::map<std::string, std::vector<coin_config_t>>
    kdf_service::groupByParentCoin(const std::vector<coin_config_t>& coins) {
        std::map<std::string, std::vector<coin_config_t>> groupedCoins;
        for (const auto& coin : coins) {
            groupedCoins[coin.parent_coin].push_back(coin);
        }
        return groupedCoins;
    }

    void kdf_service::enable_tendermint_coin(coin_config_t coin_config)
    {
        enable_tendermint_coins(t_coins{std::move(coin_config)}, coin_config.parent_coin);
    }

    void kdf_service::enable_tendermint_coins(const t_coins& coins, const std::string parent_ticker)
    {
        auto callback = [this]<typename RpcRequest>(RpcRequest rpc)
        {
            if (rpc.error)
            {
                if (rpc.error->error_type.find("PlatformIsAlreadyActivated") != std::string::npos
                    || rpc.error->error_type.find("TokenIsAlreadyActivated") != std::string::npos)
                {
                    SPDLOG_ERROR("{} {}: ", rpc.request.ticker, rpc.error->error_type);
                    std::unique_lock lock(m_coin_cfg_mutex);
                    m_coins_informations[rpc.request.ticker].currently_enabled = true;
                    dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {rpc.request.ticker}});
                    if constexpr (std::is_same_v<RpcRequest, kdf::enable_tendermint_with_assets_rpc>)
                    {
                        for (const auto& tendermint_coin_info : rpc.request.tokens_params)
                        {
                            std::unique_lock lock(m_coin_cfg_mutex);
                            m_coins_informations[tendermint_coin_info.ticker].currently_enabled = true;
                            dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {tendermint_coin_info.ticker}});
                        }
                    }
                }
                else
                {
                    SPDLOG_DEBUG("{} failed to activate", rpc.request.ticker);
                    std::unique_lock lock(m_coin_cfg_mutex);
                    m_coins_informations[rpc.request.ticker].currently_enabled = false;
                    this->dispatcher_.trigger<enabling_coin_failed>(rpc.request.ticker, rpc.error->error);
                }
            }
            else
            {
                dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {rpc.request.ticker}});
                std::unique_lock lock(m_coin_cfg_mutex);
                m_coins_informations[rpc.request.ticker].currently_enabled = true;
                if constexpr (std::is_same_v<RpcRequest, kdf::enable_tendermint_with_assets_rpc>)
                {
                    for (const auto& tendermint_token_addresses_info : rpc.result->tendermint_token_balances_infos)
                    {
                        dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {tendermint_token_addresses_info.first}});
                        m_coins_informations[tendermint_token_addresses_info.first].currently_enabled = true;
                    }
                }
            }
        };

        if (!has_coin(parent_ticker))
        {
            static constexpr auto error = "{} is not present in the config. Cannot enable TENDERMINT tokens.";
            SPDLOG_ERROR(error);
            this->dispatcher_.trigger<enabling_coin_failed>(parent_ticker, fmt::format(error, parent_ticker));
            return;
        }

        auto parent_ticker_info = get_coin_info(parent_ticker);

        if (parent_ticker_info.currently_enabled)
        {
            for (const auto& coin_config : coins)
            {
                kdf::enable_tendermint_token_rpc rpc{.request={.ticker = coin_config.ticker}};

                if (coin_config.ticker == parent_ticker_info.ticker)
                {
                    continue;
                }
                m_kdf_client.process_rpc_async<kdf::enable_tendermint_token_rpc>(rpc.request, callback);
            }
        }
        else
        {
            kdf::enable_tendermint_with_assets_rpc rpc;

            rpc.request.ticker = parent_ticker_info.ticker;
            rpc.request.nodes = parent_ticker_info.rpc_urls.value_or(std::vector<node>{});
            for (const auto& coin_config : coins)
            {
                if (coin_config.ticker == parent_ticker_info.ticker)
                {
                    continue;
                }
                rpc.request.tokens_params.push_back({.ticker = coin_config.ticker});
            }
            m_kdf_client.process_rpc_async<kdf::enable_tendermint_with_assets_rpc>(rpc.request, callback);
        }
    }

    void kdf_service::process_balance_answer(const kdf::enable_erc20_rpc& rpc)
    {
        const auto& answer = rpc.result.value();
        kdf::balance_answer balance_answer;

        balance_answer.address  = answer.balances.begin()->first;
        SPDLOG_DEBUG("balance_answer.address: {}", balance_answer.address);
        balance_answer.balance  = answer.balances.begin()->second.spendable;
        SPDLOG_DEBUG("balance_answer.balance: {}", balance_answer.balance);
        balance_answer.coin     = answer.platform_coin;
        SPDLOG_DEBUG("balance_answer.coin: {}", balance_answer.coin);
        {
            std::unique_lock lock(m_balance_mutex);
            m_balance_informations[balance_answer.coin] = std::move(balance_answer);
        }
        SPDLOG_DEBUG("balance_answer for {} complete", rpc.request.ticker);
    }

    void kdf_service::process_balance_answer(const kdf::enable_eth_with_tokens_rpc& rpc)
    {
        SPDLOG_DEBUG("kdf_service::process_balance_answer(const kdf::enable_eth_with_tokens_rpc& rpc");
        const auto& answer = rpc.result.value();
        {
            kdf::balance_answer balance_answer;
            balance_answer.coin = rpc.request.ticker;
            SPDLOG_DEBUG("balance_answer.coin: {}", balance_answer.coin);
            balance_answer.balance = answer.eth_addresses_infos.begin()->second.balances.spendable;
            SPDLOG_DEBUG("balance_answer.balance: {}", balance_answer.balance);
            balance_answer.address = answer.eth_addresses_infos.begin()->first;
            SPDLOG_DEBUG("balance_answer.address: {}", balance_answer.address);
            {
                std::unique_lock lock(m_balance_mutex);
                m_balance_informations[balance_answer.coin] = std::move(balance_answer);
            }
            SPDLOG_DEBUG("balance_answer for {} complete", rpc.request.ticker);
        }
        if (answer.erc20_addresses_infos.empty())
        {
            SPDLOG_DEBUG("answer.erc20_addresses_infos is empty");
            return;
        }
        SPDLOG_DEBUG("for (auto [address, data] : answer.erc20_addresses_infos) [{}]", answer.erc20_addresses_infos.size());
        for (auto [address, data] : answer.erc20_addresses_infos)
        {
            SPDLOG_DEBUG("for (auto [address, data] : answer.erc20_addresses_infos) address [{}]", address);
            kdf::balance_answer balance_answer;
            balance_answer.address = address;
            if (data.balances.empty())
            {
                SPDLOG_DEBUG("data.balances is empty");
                continue;
            }
            balance_answer.balance = data.balances.begin()->second.spendable;
            SPDLOG_DEBUG("balance_answer.coin: {}", balance_answer.balance);
            balance_answer.coin = data.balances.begin()->first;
            SPDLOG_DEBUG("balance_answer.coin: {}", balance_answer.coin);
            {
                std::unique_lock lock(m_balance_mutex);
                m_balance_informations[balance_answer.coin] = std::move(balance_answer);
            }
        }
        SPDLOG_DEBUG("process_balance_answer for enable_eth_with_tokens_rpc complete");
    }

    void kdf_service::process_balance_answer(const kdf::enable_tendermint_token_rpc& rpc)
    {
        const auto& answer = rpc.result.value();
        kdf::balance_answer balance_answer;
        balance_answer.address  = answer.balances.begin()->first;
        balance_answer.balance  = answer.balances.begin()->second.spendable;
        balance_answer.coin     = answer.platform_coin;

        {
            std::unique_lock lock(m_balance_mutex);
            m_balance_informations[balance_answer.coin] = std::move(balance_answer);
        }
    }

    void kdf_service::process_balance_answer(const kdf::enable_tendermint_with_assets_rpc& rpc)
    {
        const auto& answer = rpc.result.value();
        {
            kdf::balance_answer balance_answer;

            balance_answer.coin = answer.ticker;
            balance_answer.balance = answer.tendermint_balances_infos.balances.spendable;
            balance_answer.address = answer.address;
            {
                std::unique_lock lock(m_balance_mutex);
                m_balance_informations[balance_answer.coin] = std::move(balance_answer);
            }
        }
        if (answer.tendermint_token_balances_infos.empty())
        {
            return;
        }
        for (auto [ticker, data] : answer.tendermint_token_balances_infos)
        {
            kdf::balance_answer balance_answer;

            balance_answer.coin = ticker;
            balance_answer.address = answer.address;
            balance_answer.balance = data.spendable;

            {
                std::unique_lock lock(m_balance_mutex);
                m_balance_informations[balance_answer.coin] = std::move(balance_answer);
            }
        }
    }

    void
    kdf_service::disable_multiple_coins(const std::vector<std::string>& tickers)
    {
        for (const auto& ticker: tickers)
        {
            std::error_code ec;
            disable_coin(ticker, ec);
            if (ec)
            {
                SPDLOG_WARN("{}", ec.message());
            }
        }
        update_coin_status(this->m_current_wallet_name, tickers, false, m_coins_informations, m_coin_cfg_mutex);
    }

    pplx::task<void>
    kdf_service::batch_balance_and_tx(bool is_a_reset, std::vector<std::string> tickers, bool is_during_enabling, bool only_tx)
    {
        (void)tickers;
        (void)is_during_enabling;
        auto&& [batch_array, tickers_idx, tokens_to_fetch] = prepare_batch_balance_and_tx(only_tx);
        return m_kdf_client.async_rpc_batch_standalone(batch_array)
            .then(
                [this, tokens_to_fetch = tokens_to_fetch, is_a_reset, tickers, batch_array = batch_array](web::http::http_response resp)
                {
                    try
                    {
                        auto answers = kdf::basic_batch_answer(resp);
                        if (not answers.contains("error"))
                        {
                            for (auto i = 0ul; i < answers.size(); i++)
                            {
                                auto&       answer = answers[i];
                                std::string ticker;
                                // SPDLOG_DEBUG("batch_balance_and_tx answer: {}", answer.dump(4));
                                
                                if (batch_array[i].contains("mmrpc") && batch_array[i].at("mmrpc") == "2.0")
                                {
                                    if (batch_array[i].at("params").contains("coin"))
                                    {
                                        ticker = batch_array[i].at("params").at("coin");
                                    }
                                    else if (batch_array[i].at("params").contains("ticker"))
                                    {
                                        ticker = batch_array[i].at("params").at("ticker");
                                    }
                                }
                                else
                                {
                                    ticker = batch_array[i].at("coin");
                                }
                                
                                if (answer.contains("balance"))
                                {
                                    this->process_balance_answer(answer);
                                }
                                else if (answer.contains("result"))
                                {
                                    this->process_tx_answer(answer, ticker);
                                }
                                else
                                {
                                    const std::string error = answer.dump(4);
                                    SPDLOG_ERROR("error answer for tx or my_balance: {}", error);
                                    this->dispatcher_.trigger<tx_fetch_finished>(true);
                                    if (error.find("future timed out") != std::string::npos)
                                    {
                                        SPDLOG_WARN("Future timed out error detected, probably a connection issue");
                                        //! Emit error for UI Change
                                    }
                                }
                            }

                            for (auto&& coin: tokens_to_fetch) { process_tx_tokenscan(coin, is_a_reset); }
                        }
                    }
                    catch (const std::exception& error)
                    {
                        SPDLOG_ERROR("exception in kdf_service::batch_balance_and_tx: {}", error.what());
                        this->dispatcher_.trigger<tx_fetch_finished>(true);
                    }
                })
            .then([this, batch = batch_array](pplx::task<void> previous_task)
                  { this->handle_exception_pplx_task(previous_task, "batch_balance_and_tx", batch); });
    }

    bool 
    kdf_service::uses_task_activation(const coin_config_t& coin_info) const
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
    kdf_service::uses_v2_history(const coin_config_t& coin_info) const
    {
        if (coin_info.coin_type == CoinType::ZHTLC)
        {
            return true;
        }
        if (coin_info.coin_type == CoinType::SIA)
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

    std::tuple<nlohmann::json, std::vector<std::string>, std::vector<std::string>>
    kdf_service::prepare_batch_balance_and_tx(bool only_tx) const
    {
        const auto&              enabled_coins = get_enabled_coins();
        nlohmann::json           batch_array   = nlohmann::json::array();
        std::vector<std::string> tickers_idx;
        std::vector<std::string> tokens_to_fetch;
        const auto&              ticker    = get_current_ticker();
        auto                     coin_info = get_coin_info(ticker);

        if (coin_info.is_erc_family)
        {
            tokens_to_fetch.push_back(ticker);
        }
        else
        {
            std::size_t     limit =  250;
            bool            requires_v2 = false;
            std::string     method = "my_tx_history";

            if (uses_v2_history(coin_info))
            {
                requires_v2 = true;
                if (coin_info.is_zhtlc_family)
                {
                    // Don't request balance / history if not completely activated.
                    if (coin_info.activation_status.at("result").at("status") == "Ok")
                    {
                        limit = 100;
                        method = "z_coin_tx_history";
                    }
                    else
                    {
                        return std::make_tuple(batch_array, tickers_idx, tokens_to_fetch);
                    }
                }
            }
            t_tx_history_request request{.coin = ticker, .limit = limit};
            nlohmann::json       j = kdf::template_request(method, requires_v2);
            kdf::to_json(j, request);
            batch_array.push_back(j);
        }

        if (not only_tx)
        {
            for (auto&& coin : enabled_coins)
            {
                coin_info = get_coin_info(ticker);

                if (uses_task_activation(coin_info))
                {
                    // Don't request balance / history if not completely activated.
                    if (coin_info.activation_status.at("result").at("status") != "Ok")
                    {
                        continue;
                    }
                }

                if (is_pin_cfg_enabled())
                {
                    std::shared_lock lock(m_balance_mutex); ///< shared_lock
                    if (m_balance_informations.find(coin.ticker) != m_balance_informations.cend())
                    {
                        continue;
                    }
                }
                SPDLOG_INFO("Getting balance for {} ", coin.ticker);
                t_balance_request balance_request{.coin = coin.ticker};
                nlohmann::json    j = kdf::template_request("my_balance");
                kdf::to_json(j, balance_request);
                batch_array.push_back(j);
                tickers_idx.push_back(coin.ticker);
            }
        }
        return std::make_tuple(batch_array, tickers_idx, tokens_to_fetch);
    }

    std::pair<bool, std::string>
    kdf_service::process_batch_enable_answer(const json& answer)
    {
        std::string error = answer.dump(4);
        std::string data = answer.dump();

        if (answer.contains("error") || answer.contains("Error") || error.find("error") != std::string::npos || error.find("Error") != std::string::npos)
        {
            return {false, error};
        }

        if (answer.contains("coin"))
        {
            auto ticker = answer.at("coin").get<std::string>();
            {
                std::unique_lock lock(m_coin_cfg_mutex);
                m_coins_informations[ticker].currently_enabled = true;
            }
            return {true, ""};
        }

        if (answer.contains("result"))
        {
            if (answer["result"].contains("task_id"))
            {
                return {true, ""};
            }
        }

        return {false, error};
    }

    void kdf_service::enable_task(const t_coins& coins)
    {
        auto request_functor = [this](coin_config_t coin_info) -> std::pair<nlohmann::json, std::vector<std::string>>
        {
            const auto& settings_system  = m_system_manager.get_system<settings_page>();
            nlohmann::json batch = nlohmann::json::array();

            if (coin_info.is_zhtlc_family)
            {
                t_enable_z_coin_request request{
                    .coin_name            = coin_info.ticker,
                    .servers              = coin_info.electrum_urls.value_or(get_electrum_server_from_token(coin_info.ticker)),
                    .z_urls               = coin_info.z_urls.value_or(std::vector<std::string>{}),
                    .coin_type            = coin_info.coin_type,
                    .is_testnet           = coin_info.is_testnet.value_or(false),
                    .with_tx_history      = false}; // Tx history not yet ready for ZHTLC
                bool use_date = settings_system.get_use_sync_date();
                SPDLOG_INFO("use_date: {}", use_date);
                if (use_date)
                {
                    int sync_date = settings_system.get_pirate_sync_date();
                    int sync_height = settings_system.get_pirate_sync_height(
                        sync_date,
                        coin_info.checkpoint_height,
                        coin_info.checkpoint_blocktime
                    );
                    request.sync_height = sync_height;
                }

                nlohmann::json j = kdf::template_request("task::enable_z_coin::init", true);
                kdf::to_json(j, request);
                batch.push_back(j);
                //SPDLOG_INFO("Enable task request: {}", batch.dump(4));
                return {batch, {coin_info.ticker}};
            }

            else if (coin_info.is_sia_family)
            {
                t_enable_sia_coin_request request{
                    .coin_name            = coin_info.ticker,
                    .server_url           = coin_info.sia_family_urls.value().at(0),
                    .with_tx_history      = false}; // NotSupportedFor

                nlohmann::json j = kdf::template_request("task::enable_sia::init", true);
                kdf::to_json(j, request);
                batch.push_back(j);
                //SPDLOG_INFO("Enable task request: {}", batch.dump(4));
                return {batch, {coin_info.ticker}};
            }
        };

        auto answer_functor = [this](coin_config_t coin_info, nlohmann::json batch, std::vector<std::string> tickers)
        {
            m_kdf_client.async_rpc_batch_standalone(batch)
                .then(
                    [this, coin_info, tickers](web::http::http_response resp) mutable
                    {
                        try
                        {
                            auto answers                 = kdf::basic_batch_answer(resp);
                            auto& settings_system  = m_system_manager.get_system<settings_page>();

                            if (answers.count("error") == 0)
                            {
                                std::size_t                     idx = 0;
                                std::unordered_set<std::string> to_remove;
                                for (auto&& answer: answers)
                                {
                                    auto [res, error] = this->process_batch_enable_answer(answer);

                                    if (!res)
                                    {
                                        if (error.find("CoinIsAlreadyActivated") != std::string::npos)
                                        {
                                            SPDLOG_WARN("{} already activated: {}", tickers[idx], error);
                                            std::unique_lock lock(m_coin_cfg_mutex);
                                            m_coins_informations[tickers[idx]].currently_enabled = true;
                                            this->dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {tickers[idx]}});
                                            this->dispatcher_.trigger<enabling_task_status>(tickers[idx], "Complete!");
                                        }
                                        else
                                        {
                                            SPDLOG_ERROR("Error activating {}: {}", tickers[idx], error);
                                            to_remove.emplace(tickers[idx]);
                                            this->dispatcher_.trigger<enabling_coin_failed>(tickers[idx], error);
                                        }
                                    }
                                    else if (answer.contains("result"))
                                    {
                                        if (answer["result"].contains("task_id"))
                                        {
                                            auto task_id = answer.at("result").at("task_id").get<std::int8_t>();
                                            {
                                                using namespace std::chrono_literals;

                                                static std::size_t z_nb_try      = 0;
                                                nlohmann::json     z_error       = nlohmann::json::array();
                                                nlohmann::json     z_batch_array = nlohmann::json::array();

                                                SPDLOG_INFO("{} enable_task Task ID: {}", tickers[idx], task_id);

                                                if (coin_info.is_zhtlc_family)
                                                {
                                                    t_enable_z_coin_status_request z_request{.task_id = task_id};
                                                    nlohmann::json j = kdf::template_request("task::enable_z_coin::status", true);
                                                    kdf::to_json(j, z_request);
                                                    z_batch_array.push_back(j);
                                                }
                                                else if (coin_info.is_sia_family)
                                                {
                                                    t_enable_sia_coin_status_request z_request{.task_id = task_id};
                                                    nlohmann::json j = kdf::template_request("task::enable_sia::status", true);
                                                    kdf::to_json(j, z_request);
                                                    z_batch_array.push_back(j);
                                                }
                                                std::string last_event = "none";
                                                std::string event = "none";

                                                do {
                                                    pplx::task<web::http::http_response> z_resp_task = m_kdf_client.async_rpc_batch_standalone(z_batch_array);
                                                    web::http::http_response             z_resp      = z_resp_task.get();
                                                    auto                                 z_answers   = kdf::basic_batch_answer(z_resp);
                                                    z_error                                          = z_answers;

                                                    std::string status = z_answers[0].at("result").at("status").get<std::string>();

                                                    if (status == "Ok")
                                                    {
                                                        SPDLOG_INFO("{} activation ready, status is {}", tickers[idx], status);
                                                        std::unique_lock lock(m_coin_cfg_mutex);
                                                        m_coins_informations[tickers[idx]].activation_status = z_answers[0];

                                                        if (z_answers[0].at("result").at("details").contains("error"))
                                                        {
                                                            if (z_answers[0].at("result").at("details").at("error").contains("error_type"))
                                                            {
                                                                if (z_answers[0].at("result").at("details").at("error").at("error_type") == "CoinIsAlreadyActivated")
                                                                {
                                                                    continue;
                                                                }
                                                            }
                                                            event = z_answers[0].at("result").at("details").at("error").get<std::string>();
                                                            SPDLOG_ERROR("Enabling [{}] error: {}", tickers[idx], event);
                                                            break;
                                                        }

                                                        m_coins_informations[tickers[idx]].currently_enabled = true;
                                                        this->dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {tickers[idx]}});
                                                        break;
                                                    }
                                                    else if (status == "Error")
                                                    {
                                                        event = z_answers[0].at("result").at("details").at("error_data").at("error").get<std::string>();
                                                        break;
                                                    }
                                                    else
                                                    {
                                                        // TODO: many unused variables
                                                        if (z_answers[0].at("result").at("details").contains("UpdatingBlocksCache"))
                                                        {
                                                            event = "UpdatingBlocksCache";
                                                        }
                                                        else if (z_answers[0].at("result").at("details").contains("BuildingWalletDb"))
                                                        {
                                                            event = "BuildingWalletDb";
                                                        }
                                                        else if (z_answers[0].at("result").at("details").contains("ActivatingCoin"))
                                                        {
                                                            event = "ActivatingCoin";
                                                        }
                                                        else if (z_answers[0].at("result").at("details").contains("TemporaryError"))
                                                        {
                                                            event = "TemporaryError";
                                                        }
                                                        else
                                                        {
                                                            event = z_answers[0].at("result").at("details").get<std::string>();
                                                        }
                                                        SPDLOG_DEBUG("{} activation event [{}]", event, tickers[idx]);

                                                        if (event != last_event)
                                                        {
                                                            SPDLOG_INFO("Waiting for {} to enable [{}: {}]...", tickers[idx], status, event);
                                                            if (!m_coins_informations[tickers[idx]].currently_enabled && event != "ActivatingCoin")
                                                            {
                                                                std::unique_lock lock(m_coin_cfg_mutex);
                                                                m_coins_informations[tickers[idx]].currently_enabled = true;

                                                                dispatcher_.trigger<coin_fully_initialized>(coin_fully_initialized{.tickers = {tickers[idx]}});
                                                            }
                                                            this->dispatcher_.trigger<enabling_task_status>(tickers[idx], event);
                                                            last_event = event;
                                                        }
                                                        // TODO: refactor to a background task
                                                        std::this_thread::sleep_for(4s);
                                                    }
                                                    std::unique_lock lock(m_coin_cfg_mutex);
                                                    m_coins_informations[tickers[idx]].activation_status = z_answers[0];
                                                    settings_system.set_zhtlc_status(z_answers[0]);
                                                    z_nb_try += 1;

                                                } while (z_nb_try < 10000);

                                                try {
                                                    if (z_error[0].at("result").at("details").contains("error"))
                                                    {
                                                        SPDLOG_INFO("Error enabling {}: {} ", tickers[idx], event);
                                                        SPDLOG_INFO(
                                                            "Removing zhtlc from enabling, idx: {}, tickers size: {}, answers size: {}",
                                                            tickers[idx], idx, tickers.size(), answers.size()
                                                        );

                                                        this->dispatcher_.trigger<enabling_task_status>(tickers[idx], event);
                                                        this->dispatcher_.trigger<enabling_coin_failed>(tickers[idx], z_error[0].dump(4));
                                                        to_remove.emplace(tickers[idx]);
                                                    }
                                                    else if (z_nb_try == 10000)
                                                    {
                                                        // TODO: Handle this case.
                                                        // There could be no error message if scanning takes too long.
                                                        // Either we force disable here, or schedule to check on it later
                                                        // If this happens, address will be "Invalid" and balance will be zero.
                                                        // We could save this ticker in a list to try `enable_z_coin_status` again on it periodically until complete.
                                                        SPDLOG_INFO("Exited {} enable loop after 10000 tries ", tickers[idx]);
                                                        SPDLOG_INFO(
                                                            "Bad answer for zhtlc_error: [{}] -> idx: {}, tickers size: {}, answers size: {}", tickers[idx], idx,
                                                            tickers.size(), answers.size()
                                                        );
                                                        this->dispatcher_.trigger<enabling_coin_failed>(tickers[idx], z_error[0].dump(4));
                                                        update_coin_status(this->m_current_wallet_name, tickers, false, m_coins_informations, m_coin_cfg_mutex);
                                                        to_remove.emplace(tickers[idx]);
                                                    }
                                                    else
                                                    {
                                                        SPDLOG_INFO("{} enable loop complete!", tickers[idx]);
                                                        this->dispatcher_.trigger<enabling_task_status>(tickers[idx], "Complete!");
                                                    }
                                                }
                                                catch (const std::exception& error)
                                                {
                                                    SPDLOG_ERROR("exception caught in zhtlc batch_enable_coins: {}", error.what());
                                                }
                                            }
                                        }
                                    }
                                    idx += 1;
                                }

                                if (!to_remove.empty())
                                {
                                    SPDLOG_DEBUG("Removing coins which failed activation...");
                                    std::vector<std::string> disable_coins;
                                    for (auto&& t: to_remove) {
                                        tickers.erase(std::remove(tickers.begin(), tickers.end(), t), tickers.end());
                                        disable_coins.push_back(t);
                                    }
                                    update_coin_status(this->m_current_wallet_name, disable_coins, false, m_coins_informations, m_coin_cfg_mutex);
                                }

                                if (!tickers.empty())
                                {
                                    dispatcher_.trigger<coin_fully_initialized>(tickers);
                                }
                            }
                        }
                        catch (const std::exception& error)
                        {
                            SPDLOG_ERROR("exception caught in batch_enable_coins, update_coin_status to false: {}", error.what());
                            update_coin_status(this->m_current_wallet_name, tickers, false, m_coins_informations, m_coin_cfg_mutex);
                        }
                    })
                .then(
                    [this, tickers, batch](pplx::task<void> previous_task)
                    {
                        this->handle_exception_pplx_task(previous_task, "batch_enable_coins", batch);
                        SPDLOG_DEBUG("update_coin_status of {} to false", fmt::join(tickers, ", "));
                        update_coin_status(this->m_current_wallet_name, tickers, false, m_coins_informations, m_coin_cfg_mutex);
                    });
        };

        for (auto&& coin: coins)
        {
            auto&& [request, coins_to_enable] = request_functor(coin);
            //SPDLOG_INFO("{} {}", request.dump(4), coins_to_enable[0]);
            answer_functor(coin, request, coins_to_enable);
        }
    }

    nlohmann::json kdf_service::get_task_activation_status(const std::string coin) const
    {
        const auto coin_info       = get_coin_info(coin);
        if (coin_info.is_zhtlc_family)
        {
            return coin_info.activation_status;
        }
        return {};
    }

    bool kdf_service::is_task_activation_ready(const std::string coin) const
    {
        const auto coin_info       = get_coin_info(coin);
        if (uses_task_activation(coin_info))
        {
            if (coin_info.activation_status.contains("result"))
            {
                //SPDLOG_DEBUG("kdf_service::is_zhtlc_coin_ready coin_info.activation_status for coin {}: {}", coin, coin_info.activation_status.dump(4));
                if (coin_info.activation_status.at("result").contains("status"))
                {
                    if (coin_info.activation_status.at("result").at("status") == "Ok")
                    {
                        if (coin_info.activation_status.at("result").contains("details"))
                        {
                            if (!coin_info.activation_status.at("result").at("details").contains("error"))
                            {
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        }
        return true;
    }

    coin_config_t kdf_service::get_coin_info(const std::string& ticker) const
    {
        std::shared_lock lock(m_coin_cfg_mutex);
        if (m_coins_informations.find(ticker) == m_coins_informations.cend())
        {
            return {};
        }
        return m_coins_informations.at(ticker);
    }
    
    bool kdf_service::is_coin_enabled(const std::string& ticker) const
    {
        SPDLOG_DEBUG("UNUSED ??");
        return m_coins_informations[ticker].currently_enabled;
    }
    
    bool kdf_service::has_coin(const std::string& ticker) const
    {
        return m_coins_informations.contains(ticker);
    }

    // [smk] Only called by trading_page::process_action()
    kdf::orderbook_result_rpc kdf_service::get_orderbook(t_kdf_ec& ec) const
    {
        auto&& [base, rel]          = this->m_synchronized_ticker_pair.get();
        const std::string pair      = base + "/" + rel;
        auto              orderbook = m_orderbook.get();
        if (orderbook.base.empty() && orderbook.rel.empty())
        {
            SPDLOG_WARN("base/rel/orderbook mismatch: {} != {}", pair, orderbook.base + "/" + rel);
            ec = dextop_error::orderbook_empty;
            return {};
        }
        if (pair != orderbook.base + "/" + rel)
        {
            SPDLOG_WARN("base/rel/orderbook mismatch: {} != {}", pair, orderbook.base + "/" + rel);
            ec = dextop_error::orderbook_ticker_not_found;
            return {};
        }
        return orderbook;
    }

    nlohmann::json generate_req(std::string request_name, auto request, bool is_v2=false)
    {
        nlohmann::json current_request = kdf::template_request(std::move(request_name), is_v2);
        kdf::to_json(current_request, request);
        return current_request;
    }

    void kdf_service::process_orderbook(bool is_a_reset)
    {
        prepare_orderbook(is_a_reset);        
    }

    void kdf_service::prepare_orderbook(bool is_a_reset)
    {
        auto callback = [this, is_a_reset]<typename RpcRequest>(RpcRequest rpc)
        {
            nlohmann::json batch = nlohmann::json::array();
            if (rpc.error)
            {
                SPDLOG_ERROR("error: bad answer json for prepare_orderbook: {}", rpc.error->error);
            }
            else
            {
                if (is_a_reset)
                {
                    nlohmann::json batch = nlohmann::json::array();
                    auto&& [base, rel] = m_synchronized_ticker_pair.get();
                    batch.push_back(generate_req("max_taker_vol", kdf::max_taker_vol_request{.coin = base}));
                    batch.push_back(generate_req("max_taker_vol", kdf::max_taker_vol_request{.coin = rel}));
                    batch.push_back(generate_req("min_trading_vol", t_min_volume_request{.coin = base}));
                    batch.push_back(generate_req("min_trading_vol", t_min_volume_request{.coin = rel}));
                    process_orderbook_extras(batch, is_a_reset);
                }
                m_orderbook = rpc.result.value();
                this->dispatcher_.trigger<process_orderbook_finished>(is_a_reset);
            }
        };

        auto&& [base, rel] = m_synchronized_ticker_pair.get();
        // Avoid segwit coins self pairing, e.g. LTC/LTC-segwit
        std::string base_ticker = boost::replace_all_copy(base, "-segwit", "");
        std::string rel_ticker = boost::replace_all_copy(rel, "-segwit", "");
        if (rel.empty() || base.empty() || base_ticker == rel_ticker)
            SPDLOG_ERROR("Invalid ticker pair while requesting orderbook: {} {}", base, rel);

        kdf::orderbook_rpc rpc{.request={.base = base, .rel = rel}};
        m_kdf_client.process_rpc_async<kdf::orderbook_rpc>(rpc.request, callback);
    }

    void kdf_service::process_orderbook_extras(nlohmann::json batch, bool is_a_reset)
    {
        if (batch.empty())
        {
            SPDLOG_WARN("prepared batch_orderbook is empty, nothing to do");
            return;
        }

        auto answer_functor = [this, is_a_reset](web::http::http_response resp)
        {
            auto answer        = kdf::basic_batch_answer(resp);
            if (answer.is_array())
            {
                if (answer.size() < 1)
                {
                    SPDLOG_ERROR("Answer array did not contain enough elements");
                    return;
                }

                if (is_a_reset)
                {
                    if (answer.size() < 4)
                    {
                        SPDLOG_ERROR("Answer array did not contain enough elements");
                        return;
                    }

                    auto&& [base, rel] = m_synchronized_ticker_pair.get();
                    auto base_max_taker_vol_answer = kdf::rpc_process_answer_batch<kdf::max_taker_vol_answer>(answer[0], "max_taker_vol");
                    if (base_max_taker_vol_answer.rpc_result_code == 200)
                    {
                        if (base == base_max_taker_vol_answer.result->coin)
                        {
                            this->m_synchronized_max_taker_vol->first = base_max_taker_vol_answer.result.value();
                        }
                    }

                    auto rel_max_taker_vol_answer = kdf::rpc_process_answer_batch<kdf::max_taker_vol_answer>(answer[1], "max_taker_vol");
                    if (rel_max_taker_vol_answer.rpc_result_code == 200)
                    {
                        if (rel == rel_max_taker_vol_answer.result->coin)
                        {
                            this->m_synchronized_max_taker_vol->second = rel_max_taker_vol_answer.result.value();
                        }
                    }

                    auto base_min_taker_vol_answer = kdf::rpc_process_answer_batch<t_min_volume_answer>(answer[2], "min_trading_vol");
                    if (base_min_taker_vol_answer.rpc_result_code == 200)
                    {
                        m_synchronized_min_taker_vol->first = base_min_taker_vol_answer.result.value();

                    }

                    auto rel_min_taker_vol_answer = kdf::rpc_process_answer_batch<t_min_volume_answer>(answer[3], "min_trading_vol");
                    if (rel_min_taker_vol_answer.rpc_result_code == 200)
                    {
                        m_synchronized_min_taker_vol->second = rel_min_taker_vol_answer.result.value();
                    }
                }
            }
        };

        m_kdf_client.async_rpc_batch_standalone(batch)
            .then(answer_functor)
            .then([this, batch](pplx::task<void> previous_task) { this->handle_exception_pplx_task(previous_task, "process_orderbook_extras", batch); });
    }

    void kdf_service::fetch_current_orderbook_thread(bool is_a_reset)
    {
        //! If thread is not active ex: we are not on the trading page anymore, we continue sleeping.
        if (!m_orderbook_thread_active)
        {
            return;
        }
        process_orderbook(is_a_reset);
    }

    //! Fetch the balance of a coin that has just become active, so it does not
    //! stay absent until the next `fetch_infos_thread` tick (43 s away, and
    //! skipped entirely while coins are still being enabled).
    //!
    //! Hooked to `coin_fully_initialized` rather than repeated in each enabling
    //! function because the enabling paths do not agree on this: the UTXO/QRC20
    //! and ERC20 batches record the balance from their own answer, but the
    //! task-based path (ZHTLC, Sia), the Tendermint path, and every
    //! "already activated" recovery branch mark the coin enabled without ever
    //! recording one. Anything that reaches the active state is covered here,
    //! including enabling paths added later.
    //!
    //! The work is spawned rather than run inline: this event is dispatched
    //! synchronously, sometimes while the caller holds `m_coin_cfg_mutex`, and
    //! reading the coin config here would then deadlock on a non-recursive
    //! shared_mutex.
    void kdf_service::on_coin_fully_initialized_event(const coin_fully_initialized& evt)
    {
        for (const auto& ticker: evt.tickers)
        {
            async::spawn([this, ticker]() { fetch_single_balance(ticker); });
        }
    }

    void kdf_service::fetch_single_balance(const std::string& ticker)
    {
        const auto coin_info = get_coin_info(ticker);

        //! An unknown ticker has nothing to query, and a coin whose activation
        //! task has not reported success yet has no balance to report -- the
        //! task-based path marks a coin enabled while it is still building its
        //! wallet database. `fetch_infos_thread` picks that one up once it is
        //! ready, as it did before this hook existed.
        if (coin_info.ticker.empty() || !is_task_activation_ready(ticker))
        {
            return;
        }
        fetch_single_balance(coin_info);
    }

    void kdf_service::fetch_single_balance(const coin_config_t& cfg_infos)
    {
        nlohmann::json batch_array = nlohmann::json::array();

        if (is_pin_cfg_enabled())
        {
            std::shared_lock lock(m_balance_mutex);
            if (m_balance_informations.find(cfg_infos.ticker) != m_balance_informations.cend())
            {
                SPDLOG_WARN("m_balance_informations not found for {} ", cfg_infos.ticker);
                return;
            }
        }

        t_balance_request balance_request{.coin = cfg_infos.ticker};
        nlohmann::json    j = kdf::template_request("my_balance");
        kdf::to_json(j, balance_request);
        batch_array.push_back(j);

        auto answer_functor = [this](web::http::http_response resp)
        {
            try
            {
                auto answers = kdf::basic_batch_answer(resp);
                if (!answers.contains("error") && !answers[0].contains("error"))
                {
                    this->process_balance_answer(answers[0]);
                }
            }
            catch (const std::exception& error)
            {
                SPDLOG_ERROR("exception in kdf_service::fetch_single_balance: {}", error.what());
            }
        };

        m_kdf_client.real_async_rpc_batch_standalone(batch_array)
            .then([this, batch = batch_array, answer_functor](web::http::http_response resp) {
                try
                {
                    answer_functor(resp);
                }
                catch (const std::exception& e)
                {
                    this->handle_exception_async_task(std::current_exception(), "fetch_single_balance", batch);
                }
            });
    }

    void kdf_service::fetch_infos_thread()
    {
        const auto& enabled_coins = get_enabled_coins();
        for (const auto& coin : enabled_coins) {
            async::spawn([this, coin]() {
                fetch_single_balance(coin);
            });
        }

        if (m_wallet_page_active) {
            batch_balance_and_tx(true, {}, false, true);
        }
    }

    void kdf_service::spawn_kdf_instance(std::string wallet_name, std::string passphrase, bool with_pin_cfg, std::string rpcpass)
    {
        this->m_balance_factor = utils::determine_balance_factor(with_pin_cfg);
        SPDLOG_DEBUG("balance factor is: {}", m_balance_factor);
        this->m_current_wallet_name = std::move(wallet_name);
        this->dispatcher_.trigger<coin_cfg_parsed>(this->retrieve_coins_informations());
        this->dispatcher_.trigger<force_update_providers>();
        this->dispatcher_.trigger<force_update_defi_stats>();
        kdf_config cfg{
            .passphrase = std::move(passphrase), 
            .rpc_password = std::move(rpcpass) == "" ? std::move(atomic_dex::gen_random_password()) : std::move(rpcpass)
        };

        auto dbdir_parent = std::filesystem::path(utils::get_atomic_dex_data_folder() / "kdf");
        auto old_dbdir_parent = std::filesystem::path(utils::get_atomic_dex_data_folder() / "mm2");
        if (not std::filesystem::exists(dbdir_parent))
        {
            if (std::filesystem::exists(old_dbdir_parent))
            {
                std::filesystem::rename(old_dbdir_parent, dbdir_parent);
            }
        }

        kdf::set_system_manager(m_system_manager);
        kdf::set_rpc_password(cfg.rpc_password);
        json       json_cfg;
        const auto tools_path = ag::core::assets_real_path() / "tools/kdf/";

        nlohmann::to_json(json_cfg, cfg);
        std::filesystem::path kdf_cfg_path = (std::filesystem::temp_directory_path() / "KDF.json");

        QFile ofs;
        ofs.setFileName(std_path_to_qstring(kdf_cfg_path));
        ofs.open(QIODevice::WriteOnly | QIODevice::Text);
        ofs.write(QString::fromStdString(json_cfg.dump()).toUtf8());
        ofs.close();

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("MM_CONF_PATH", std_path_to_qstring(kdf_cfg_path));
        env.insert("MM_LOG", std_path_to_qstring(utils::get_kdf_atomic_dex_current_log_file()));
        env.insert("MM_COINS_PATH", std_path_to_qstring((utils::get_current_configs_path() / "coins.json")));
        QProcess kdf_instance;
        kdf_instance.setProgram(std_path_to_qstring((tools_path / atomic_dex::g_dex_api)));
        kdf_instance.setWorkingDirectory(std_path_to_qstring(tools_path));
        kdf_instance.setProcessEnvironment(env);
        bool started = kdf_instance.startDetached();

        if (!started)
        {
            SPDLOG_ERROR("Couldn't start kdf");
            std::exit(EXIT_FAILURE);
        }

        m_kdf_init_thread = std::thread(
            [this, kdf_cfg_path]()
            {
                using namespace std::chrono_literals;
                auto               check_kdf_alive = []() { return kdf::rpc_version() != "error occured during rpc_version"; };
                static std::size_t nb_try          = 0;

                while (not check_kdf_alive())
                {
                    nb_try += 1;
                    if (nb_try == 30)
                    {
                        SPDLOG_ERROR("KDF not started correctly");
                        //! TODO: emit kdf_failed_initialization
                        std::filesystem::remove(kdf_cfg_path);
                        return;
                    }
                    std::this_thread::sleep_for(1s);
                }

                // m_kdf_client.connect_client();
                std::filesystem::remove(kdf_cfg_path);
                SPDLOG_INFO("kdf is initialized");
                dispatcher_.trigger<kdf_initialized>();
                enable_default_coins();
                m_kdf_running = true;
                dispatcher_.trigger<kdf_started>();
            });
    }

    std::pair<t_transactions, t_tx_state>
    kdf_service::get_tx(t_kdf_ec& ec) const
    {
        const auto& ticker = get_current_ticker();
        const auto underlying_tx_history_map = m_tx_informations.synchronize();
        const auto coin_info                 = get_coin_info(ticker);
        const auto it                        = !(coin_info.is_erc_family) ? underlying_tx_history_map->find("result") : underlying_tx_history_map->find(ticker);
        if (it == underlying_tx_history_map->cend())
        {
            ec = dextop_error::tx_history_of_a_non_enabled_coin;
            return {};
        }
        return it->second;
    }

    t_tx_state
    kdf_service::get_tx_state(t_kdf_ec& ec) const
    {
        return get_tx(ec).second;
    }

    t_transactions
    kdf_service::get_tx_history(t_kdf_ec& ec) const
    {
        return get_tx(ec).first;
    }

    t_float_50
    kdf_service::get_balance_info_f(const std::string& ticker) const
    {
        std::error_code ec;
        std::string     balance_str = get_balance_info(ticker, ec);
        t_float_50      balance_f = safe_float(balance_str);
        return balance_f;
    }

    std::string
    kdf_service::get_balance_info(const std::string& ticker, t_kdf_ec& ec) const
    {
        // This happens quite often
        std::shared_lock lock(m_balance_mutex); ///! read
        auto             it = m_balance_informations.find(ticker);
        
        if (m_coins_informations[ticker].currently_enabled)
        {
            if (it == m_balance_informations.cend())
            {
                if (!is_task_activation_ready(ticker))
                {
                    SPDLOG_WARN("ZHTLC coin {} not ready in kdf_service::get_balance_info", ticker);
                    return "0";
                }
                else
                {
                    SPDLOG_ERROR("kdf_service::get_balance_info not found for enabled coin: {}", ticker);
                }
                ec = dextop_error::balance_of_a_non_enabled_coin;
                return "0";
            }
            else
            {
                return it->second.balance;
            }
        }
        else
        {
            ec = dextop_error::balance_of_a_non_enabled_coin;
            return "0";
        }
    }

    void
    kdf_service::batch_fetch_orders_and_swap(bool after_manual_reset)
    {
        nlohmann::json batch             = nlohmann::json::array();
        nlohmann::json my_orders_request = kdf::template_request("my_orders");
        batch.push_back(my_orders_request);
        //SPDLOG_DEBUG("my_orders_request {}", my_orders_request.dump(4));

        //! Swaps preparation
        [[maybe_unused]]  std::size_t       total           = 0;
        [[maybe_unused]]  std::size_t       nb_active_swaps = 0;
        std::size_t       current_page    = 0;
        std::size_t       limit           = 0;
        t_filtering_infos filter_infos;
        {
            auto value_ptr  = m_orders_and_swaps.synchronize();
            total           = value_ptr->total_swaps;
            nb_active_swaps = value_ptr->active_swaps;
            current_page    = value_ptr->current_page;
            limit           = value_ptr->limit;
            filter_infos    = value_ptr->filtering_infos;
        }

        //! First time fetch or current page
        nlohmann::json            my_swaps = kdf::template_request("my_recent_swaps");
        t_my_recent_swaps_request request{
            .limit          = limit,
            .page_number    = current_page,
            .my_coin        = filter_infos.my_coin,
            .other_coin     = filter_infos.other_coin,
            .from_timestamp = filter_infos.from_timestamp,
            .to_timestamp   = filter_infos.to_timestamp,
        };
        to_json(my_swaps, request);
        batch.push_back(my_swaps);
        //SPDLOG_DEBUG("my_swaps req: {}", my_swaps.dump(4));

        //! Active swaps
        nlohmann::json         active_swaps = kdf::template_request("active_swaps");
        t_active_swaps_request active_swaps_request{.statuses = true};
        to_json(active_swaps, active_swaps_request);
        batch.push_back(active_swaps);
        //SPDLOG_DEBUG("active_swaps req: {}", active_swaps.dump(4));

        auto answer_functor = [this, limit, filter_infos, after_manual_reset](web::http::http_response resp)
        {
            //! Parsing Resp
            orders_and_swaps result;
            auto             answers = kdf::basic_batch_answer(resp);

            //! Extract
            const auto orders_answers      = kdf::rpc_process_answer_batch<t_my_orders_answer>(answers[0], "my_orders");
            const auto swap_answer         = kdf::rpc_process_answer_batch<t_my_recent_swaps_answer>(answers[1], "my_recent_swaps");
            const auto active_swaps_answer = kdf::rpc_process_answer_batch<t_active_swaps_answer>(answers[2], "active_swaps");

            result.orders_and_swaps.reserve(orders_answers.orders.size() + limit);
            result.nb_orders        = orders_answers.orders.size();
            result.orders_and_swaps = std::move(orders_answers.orders);
            result.orders_registry  = std::move(orders_answers.orders_id);
            result.limit            = limit;
            result.filtering_infos  = filter_infos;

            //! Recent swaps
            result.active_swaps = active_swaps_answer.uuids.size();
            for (auto&& cur: active_swaps_answer.swaps)
            {
                const auto uuid = cur.order_id.toStdString();
                result.swaps_registry.emplace(uuid);
                result.orders_and_swaps.emplace_back(std::move(cur));
            }

            //! Swaps
            if (swap_answer.result.has_value())
            {
                const auto& swap_success_answer = swap_answer.result.value();
                result.total_swaps              = swap_success_answer.total;
                result.total_finished_swaps     = result.total_swaps - active_swaps_answer.uuids.size();
                result.current_page             = swap_success_answer.page_number;
                result.nb_pages                 = swap_success_answer.total_pages;
                for (auto&& cur: swap_success_answer.swaps)
                {
                    const auto uuid = cur.order_id.toStdString();
                    if (!result.swaps_registry.contains(uuid))
                    {
                        result.swaps_registry.emplace(uuid);
                        result.orders_and_swaps.emplace_back(std::move(cur));
                    }
                }
                result.average_events_time = std::move(swap_success_answer.average_events_time);
            }

            //! Compute everything
            m_orders_and_swaps = std::move(result);

            this->dispatcher_.trigger<process_swaps_and_orders_finished>(after_manual_reset);
        };

        m_kdf_client.async_rpc_batch_standalone(batch)
            .then(answer_functor)
            .then([this, batch](pplx::task<void> previous_task) { this->handle_exception_pplx_task(previous_task, "batch_fetch_orders_and_swap", batch); });
    }

    void kdf_service::process_tx_tokenscan(const std::string& ticker, [[maybe_unused]] bool is_a_reset)
    {
        std::error_code ec;
        using namespace std::string_literals;
        auto construct_url_functor = [this](
                                         const std::string& main_ticker, const std::string& test_ticker, const std::string& url, const std::string& token_url,
                                         const std::string& ticker, const std::string& address)
        {
            std::string out;
            if (ticker == main_ticker || ticker == test_ticker)
            {
                out = "/api/v2/" + url + "/" + address;
            }
            else
            {
                const std::string contract_address = get_raw_kdf_ticker_cfg(ticker).at("protocol").at("protocol_data").at("contract_address");
                out                                = "/api/v2/" + token_url + "/" + contract_address + "/" + address;
            }
            return out;
        };

        auto retrieve_api_functor = [this, construct_url_functor](const std::string& ticker, const std::string& address) -> std::string
        {
            const auto  coin_info = this->get_coin_info(ticker);
            std::string out;
            switch (coin_info.coin_type)
            {
            case CoinTypeGadget::ERC20:
                out = construct_url_functor("ETH", "ETHR", "eth_tx_history", "erc20_tx_history", ticker, address);
                break;
            case CoinTypeGadget::TRC20:
                out = construct_url_functor("TRX", "TRXT", "trx_tx_history", "trc20_tx_history", ticker, address);
                break;
            case CoinTypeGadget::GRC20:
                out = construct_url_functor("GLEEC", "GLEECT", "gleec_tx_history", "grc20_tx_history", ticker, address);
                break;
            case CoinTypeGadget::BEP20:
                out = construct_url_functor("BNB", "BNBT", "bnb_tx_history", "bep20_tx_history", ticker, address);
                break;
            case CoinTypeGadget::PLG20:
                out = construct_url_functor("POL", "POLTEST", "matic_tx_history", "plg20_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Moonriver:
                out = construct_url_functor("MOVR", "MOVRT", "movr_tx_history", "mvr20_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Moonbeam:
                out = construct_url_functor("GLMR", "GLMRT", "glmr_tx_history", "glmr_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Arbitrum:
                out = construct_url_functor("ETH-ARB20", "ETHR-ARB20", "arb_tx_history", "arb20_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Base:
                out = construct_url_functor("ETH-BASE", "ETHR-BASE", "base_tx_history", "base20_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Gnosis:
                out = construct_url_functor("XDAI", "XDAI", "xdai_tx_history", "gno_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Optimism:
                out = construct_url_functor("ETH-OPT20", "ETHK-OPT20", "opt_tx_history", "opt20_tx_history", ticker, address);
                break;
            case CoinTypeGadget::EthereumClassic:
                out = construct_url_functor("ETC", "ETCT", "etc_tx_history", "etc_tx_history", ticker, address);
                break;
            case CoinTypeGadget::RSK:
                out = construct_url_functor("RBTC", "RBTCT", "rsk_tx_history", "rsk_tx_history", ticker, address);
                break;
            case CoinTypeGadget::AVX20:
                out = construct_url_functor("AVAX", "AVAXT", "avax_tx_history", "avx20_tx_history", ticker, address);
                break;
            default:
                break;
            }
            if (coin_info.is_testnet.value_or(false))
            {
                out += "&testnet=true";
            }
            return out;
        };

        std::string url = retrieve_api_functor(ticker, address(ticker, ec));
        kdf::async_process_rpc_get(kdf::g_etherscan_proxy_http_client, "tx_history", url)
            .then(
                [this, ticker](const web::http::http_response& resp)
                {
                    auto answer = m_kdf_client.rpc_process_answer<kdf::tx_history_answer>(resp, "tx_history");

                    if (answer.rpc_result_code != 200)
                    {
                        // SPDLOG_ERROR("answer.rpc_result_code is {} in kdf::async_process_rpc_get with answer.raw_result: {}", answer.rpc_result_code, answer.raw_result);
                        // answer.rpc_result_code is -1 in kdf::async_process_rpc_get with answer.raw_result: [json.exception.parse_error.101] parse error at line 1, column 1: syntax error while parsing value - invalid literal; last read: 'N'
                        this->dispatcher_.trigger<tx_fetch_finished>(true, ticker);
                    }
                    else if (answer.rpc_result_code not_eq -1 and answer.result.has_value())
                    {
                        t_tx_state state;
                        state.state             = "Finished";
                        state.current_block     = 0;
                        state.blocks_left       = 0;
                        state.transactions_left = 0;

                        if (answer.result.value().sync_status.additional_info.has_value())
                        {
                            if (answer.result.value().sync_status.additional_info.value().erc_infos.has_value())
                            {
                                state.blocks_left = answer.result.value().sync_status.additional_info.value().erc_infos.value().blocks_left;
                            }
                            if (answer.result.value().sync_status.additional_info.value().regular_infos.has_value())
                            {
                                state.transactions_left = answer.result.value().sync_status.additional_info.value().regular_infos.value().transactions_left;
                            }
                        }

                        t_transactions out;
                        out.reserve(answer.result.value().transactions.size());

                        const auto& transactions = answer.result.value().transactions;
                        std::for_each(
                            rbegin(transactions), rend(transactions),
                            [&out, this](auto&& current)
                            {
                                tx_infos current_info{
                                    .am_i_sender       = current.my_balance_change[0] == '-',
                                    .confirmations     = current.confirmations.has_value() ? current.confirmations.value() : 0,
                                    .from              = current.from,
                                    .to                = current.to,
                                    .date              = current.timestamp_as_date,
                                    .timestamp         = current.timestamp,
                                    .tx_hash           = current.tx_hash,
                                    .fees              = current.fee_details.normal_fees.has_value() ? current.fee_details.normal_fees.value().amount
                                                                                                     : current.fee_details.qrc_fees.has_value()
                                                                                                     ? current.fee_details.qrc_fees.value().miner_fee
                                                                                                     : current.fee_details.erc_fees.value().total_fee,
                                    .my_balance_change = current.my_balance_change,
                                    .total_amount      = current.total_amount,
                                    .block_height      = current.block_height,
                                    .ec                = dextop_error::success,
                                };

                                const auto& wallet_manager    = this->m_system_manager.get_system<qt_wallet_manager>();
                                current_info.transaction_note = wallet_manager.retrieve_transactions_notes(current_info.tx_hash);
                                out.push_back(std::move(current_info));
                            });

                        //! History
                        m_tx_informations->insert_or_assign(ticker, std::make_pair(out, state));

                        //! Dispatch
                        this->dispatcher_.trigger<tx_fetch_finished>(false, ticker);
                    }
                })

            .then(
                [this](pplx::task<void> previous_task)
                {
                    this->handle_exception_pplx_task(previous_task, "process_tx_tokenscan", {});
                });
    }

    void
    kdf_service::update_sync_ticker_pair(std::string base, std::string rel)
    {
        this->m_synchronized_ticker_pair = std::make_pair(base, rel);
    }

    void
    kdf_service::on_refresh_orderbook_model_data(const refresh_orderbook_model_data& evt)
    {
        this->m_synchronized_ticker_pair = std::make_pair(evt.base, evt.rel);
        if (this->m_kdf_running)
        {
            process_orderbook(true);
        }
    }

    void
    kdf_service::on_gui_enter_trading([[maybe_unused]] const gui_enter_trading& evt)
    {
        m_orderbook_thread_active = true;
    }

    void
    kdf_service::on_gui_leave_trading([[maybe_unused]] const gui_leave_trading& evt)
    {
        m_orderbook_thread_active = false;
    }

    void kdf_service::on_gui_enter_wallet([[maybe_unused]] const gui_enter_wallet& evt)
    {
        m_wallet_page_active = true;
    }

    void kdf_service::on_gui_leave_wallet([[maybe_unused]] const gui_leave_wallet& evt)
    {
        m_wallet_page_active = false;
    }

    bool
    kdf_service::do_i_have_enough_funds(const std::string& ticker, const t_float_50& amount) const
    {
        t_float_50 funds = get_balance_info_f(ticker);
        return funds >= amount;
    }

    std::string
    kdf_service::address(const std::string& ticker, t_kdf_ec& ec) const
    {
        std::shared_lock lock(m_balance_mutex);
        auto             it = m_balance_informations.find(ticker);

        if (it == m_balance_informations.cend())
        {
            ec = dextop_error::unknown_ticker;
            //SPDLOG_WARN("Invalid Ticker {}", ticker);
            return "Invalid Ticker";
        }

        return it->second.address;
    }

    bool
    kdf_service::is_orderbook_thread_active() const
    {
        return this->m_orderbook_thread_active.load();
    }

    nlohmann::json
    kdf_service::get_raw_kdf_ticker_cfg(const std::string& ticker) const
    {
        nlohmann::json out;

        std::shared_lock lock(m_raw_coin_cfg_mutex);
        const auto       it = m_kdf_raw_coins_cfg.find(ticker);
        if (it != m_kdf_raw_coins_cfg.end())
        {
            atomic_dex::coin_element element = it->second;
            to_json(out, element);
            return out;
        }
        return nlohmann::json::object();
    }

    kdf_service::t_pair_max_vol
    kdf_service::get_taker_vol() const
    {
        return m_synchronized_max_taker_vol.get();
    }

    kdf_service::t_pair_min_vol
    kdf_service::get_min_vol() const
    {
        return m_synchronized_min_taker_vol.get();
    }

    bool
    kdf_service::is_pin_cfg_enabled() const
    {
        return m_balance_factor != 1.0;
    }

    void
    kdf_service::reset_fake_balance_to_zero(const std::string& ticker)
    {
        {
            std::unique_lock lock(m_balance_mutex);
            m_balance_informations.at(ticker).balance = "0";
        }
        this->dispatcher_.trigger<ticker_balance_updated>(std::vector<std::string>{ticker});
    }

    void
    kdf_service::decrease_fake_balance(const std::string& ticker, const std::string& amount)
    {
        t_float_50 balance = get_balance_info_f(ticker);
        t_float_50 amount_f(amount);
        t_float_50 result = balance - amount_f;
        SPDLOG_DEBUG("decreasing {} - {} = {}", balance.str(8, std::ios_base::fixed), amount_f.str(8, std::ios_base::fixed), result.str(8, std::ios_base::fixed));
        if (result < 0)
        {
            reset_fake_balance_to_zero(ticker);
        }
        else
        {
            {
                std::unique_lock lock(m_balance_mutex); //! Write
                m_balance_informations.at(ticker).balance = result.str(8, std::ios_base::fixed);
            }
            this->dispatcher_.trigger<ticker_balance_updated>(std::vector<std::string>{ticker});
        }
    }

    void
    kdf_service::process_tx_answer(const nlohmann::json& answer_json, std::string ticker)
    {
        kdf::tx_history_answer answer;
        kdf::from_json(answer_json, answer);
        t_tx_state state;
        state.state             = answer.result.value().sync_status.state;
        state.current_block     = answer.result.value().current_block;
        state.blocks_left       = 0;
        state.transactions_left = 0;

        if (answer.result.value().sync_status.additional_info.has_value())
        {
            if (answer.result.value().sync_status.additional_info.value().erc_infos.has_value())
            {
                state.blocks_left = answer.result.value().sync_status.additional_info.value().erc_infos.value().blocks_left;
            }
            if (answer.result.value().sync_status.additional_info.value().regular_infos.has_value())
            {
                state.transactions_left = answer.result.value().sync_status.additional_info.value().regular_infos.value().transactions_left;
            }
        }
        t_transactions out;
        out.reserve(answer.result.value().transactions.size());

        for (auto&& current: answer.result.value().transactions)
        {
            tx_infos current_info{

                .am_i_sender       = current.my_balance_change[0] == '-',
                .confirmations     = current.confirmations.has_value() ? current.confirmations.value() : 0,
                .from              = current.from,
                .to                = current.to,
                .date              = current.timestamp_as_date,
                .timestamp         = current.timestamp,
                .tx_hash           = current.tx_hash,
                .my_balance_change = current.my_balance_change,
                .total_amount      = current.total_amount,
                .block_height      = current.block_height,
                .ec                = dextop_error::success,
            };
            if (current.fee_details.normal_fees.has_value())
            {
                current_info.fees = current.fee_details.normal_fees.value().amount;
            }
            else if (current.fee_details.erc_fees.has_value())
            {
                current_info.fees = current.fee_details.erc_fees.value().total_fee;
            }
            else if (current.fee_details.qrc_fees.has_value())
            {
                current_info.fees = current.fee_details.qrc_fees->miner_fee;
            }
            else if (current.transaction_fee.has_value())
            {
                current_info.fees = current.transaction_fee.value();
            }
            if (current_info.timestamp == 0)
            {
                using namespace std::chrono;
                current_info.timestamp   = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
                current_info.date        = utils::to_human_date<std::chrono::seconds>(current_info.timestamp, "%e %b %Y, %H:%M");
                current_info.unconfirmed = true;
            }

            const auto& wallet_manager    = this->m_system_manager.get_system<qt_wallet_manager>();
            current_info.transaction_note = wallet_manager.retrieve_transactions_notes(current_info.tx_hash);

            out.push_back(std::move(current_info));
        }

        //! History
        m_tx_informations->insert_or_assign("result", std::make_pair(out, state));
        this->dispatcher_.trigger<tx_fetch_finished>(false, std::move(ticker));
    }

    void
    kdf_service::process_balance_answer(const nlohmann::json& answer)
    {
        try
        {
            t_balance_answer answer_r;
            
            kdf::from_json(answer, answer_r);
            if (is_pin_cfg_enabled())
            {
                std::shared_lock lock(m_balance_mutex);

                if (m_balance_informations.find(answer_r.coin) != m_balance_informations.end())
                {
                    return;
                }
            }
            t_float_50 result = t_float_50(answer_r.balance) * m_balance_factor;
            answer_r.balance  = result.str(8, std::ios_base::fixed);

            {
                std::unique_lock lock(m_balance_mutex);
                m_balance_informations[answer_r.coin] = std::move(answer_r);
            }
        }
        catch (const std::exception& error)
        {
            SPDLOG_ERROR("exception in kdf_service::process_balance_answer: {}, for answer.dump", error.what(), answer.dump(4));
        }
    }

    kdf::kdf_client& kdf_service::get_kdf_client()
    {
        return m_kdf_client;
    }

    std::string
    kdf_service::get_current_ticker() const
    {
        return m_current_ticker.get();
    }

    bool
    kdf_service::set_current_ticker(const std::string& ticker)
    {
        if (ticker != get_current_ticker())
        {
            m_current_ticker = ticker;
            return true;
        }
        return false;
    }

    bool
    kdf_service::is_this_ticker_present_in_raw_cfg(const std::string& ticker) const
    {
        std::shared_lock lock(m_raw_coin_cfg_mutex);
        return m_kdf_raw_coins_cfg.find(ticker) != m_kdf_raw_coins_cfg.end();
    }

    bool
    kdf_service::is_this_ticker_present_in_normal_cfg(const std::string& ticker) const
    {
        std::shared_lock lock(m_coin_cfg_mutex);
        return m_coins_informations.find(ticker) != m_coins_informations.end();
    }

    std::vector<electrum_server>
    kdf_service::get_electrum_server_from_token(const std::string& ticker)
    {
        std::vector<electrum_server> servers;
        const coin_config_t            cfg = this->get_coin_info(ticker);
        if (cfg.coin_type == CoinType::QRC20)
        {
            if (cfg.is_testnet.value_or(false))
            {
                SPDLOG_INFO("{} is from testnet picking tQTUM electrum", ticker);
                servers = std::move(get_coin_info("tQTUM").electrum_urls.value());
            }
            else
            {
                SPDLOG_INFO("{} is from mainnet picking QTUM electrum", ticker);
                servers = std::move(get_coin_info("QTUM").electrum_urls.value());
            }
        }
        return servers;
    }

    orders_and_swaps
    kdf_service::get_orders_and_swaps() const
    {
        return m_orders_and_swaps.get();
    }

    void
    kdf_service::set_orders_and_swaps_pagination_infos(std::size_t current_page, std::size_t limit, t_filtering_infos filter_infos)
    {
        {
            m_orders_and_swaps = orders_and_swaps{.current_page = current_page, .limit = limit, .filtering_infos = std::move(filter_infos)};
        }
        this->batch_fetch_orders_and_swap(true);
    }

    void
    kdf_service::handle_exception_async_task(std::exception_ptr e, const std::string& from, nlohmann::json request)
    {
        try
        {
            std::rethrow_exception(e);
        }
        catch (const std::exception& ex)
        {
            for (auto&& cur: request) cur["userpass"] = "";
            SPDLOG_ERROR("exception in kdf_service::handle_exception_async_task from {} with request {} and error: {}", from, request.dump(4), ex.what());
            //this->dispatcher_.trigger<fatal_notification>("connection dropped");
            using namespace std::chrono; std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

    void
    kdf_service::handle_exception_pplx_task(pplx::task<void> previous_task, const std::string& from, nlohmann::json request)
    {
        try
        {
            previous_task.wait();
        }
        catch (const std::exception& e)
        {
            for (auto&& cur: request) cur["userpass"] = "";
            SPDLOG_ERROR("exception in kdf_service::handle_exception_pplx_task from {} with request {} and error: {}", from, request.dump(4), e.what());
            //this->dispatcher_.trigger<fatal_notification>("connection dropped");
            using namespace std::chrono; std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
} // namespace atomic_dex
