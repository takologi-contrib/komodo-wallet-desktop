pragma Singleton
import QtQuick 2.15
import AtomicDEX.TradingError 1.0
import AtomicDEX.MarketMode 1.0

QtObject {
    // See https://gs.statcounter.com/screen-resolution-stats/desktop/worldwide
    readonly property int width: 1360
    readonly property int height: 860
    readonly property int minimumWidth: 1360
    readonly property int minimumHeight: 860
    readonly property int max_camo_pw_length: 256
    readonly property int max_std_pw_length: 256
    readonly property int max_pw_length: max_std_pw_length + max_camo_pw_length
    readonly property string os_file_prefix: Qt.platform.os == "windows" ? "file:///" : "file://"
    readonly property string assets_path: "qrc:///"
    readonly property string image_path: assets_path + "assets/images/"
    readonly property string coin_icons_path: image_path + "coins/"
    readonly property string providerIconsPath: image_path + "providers/"

    /* Timers */
    property Timer prevent_coin_disabling: Timer { interval: 5000 }

    function coinIcon(ticker)
    {
        if (ticker.toLowerCase() == "smart chain")
        {
            return coin_icons_path + "smart_chain.png"
        }
        if (ticker.toLowerCase() == "avx")
        {
            return coin_icons_path + "avax.png"
        }
        if (ticker === "" || ticker === "All" || ticker===undefined)
        {
            return ""
        }
        else
        {
            if (['THC-BEP20'].indexOf(ticker) >= 0)
            {
                return coin_icons_path + ticker.toString().toLowerCase().replace('-', '_') + ".png"
            }
            if (['Smart Chain'].indexOf(ticker) >= 0)
            {
                return coin_icons_path + ticker.toString().toLowerCase().replace(' ', '_') + ".png"
            }
            const coin_info = API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker)
            let icon = atomic_qt_utilities.retrieve_main_ticker(ticker.toString()).toLowerCase() + ".png"
            return coin_icons_path + icon
        }
    }

    function getChartID(ticker)
    {
        let coin_info = API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker)
        return coin_info.livecoinwatch_id
        //return coin_info.coinpaprika_id
    }

    function coinWithoutSuffix(ticker)
    {
        if (ticker.search("-") > -1)
        {
            return ticker.split("-")[0]
        }
        else
        {
            return ticker
        }
    }

    function is_testcoin(ticker)
    {
        let coin_info = API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker)
        return coin_info.is_testnet
    }

    function coinName(ticker) {
        return (ticker === "" || ticker === "All" || ticker===undefined) ? "" : API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker).name
    }

    function canSend(ticker, progress=100)
    {
        return !API.app.wallet_pg.send_available ? false : progress < 100 ? false : true
    }

    function isWalletOnly(ticker)
    {
        return API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker).is_wallet_only
    }

    function isFaucetCoin(ticker)
    {
        return API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker).is_faucet_coin
    }

    function isVoteCoin(ticker)
    {
        return API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker).is_vote_coin
    }

    function isCoinWithMemo(ticker)
    {
        return API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker).has_memos
    }

    function getLanguage()
    {
        return API.app.settings_pg.lang
    }

    function isZhtlc(coin)
    {
        return API.app.portfolio_pg.global_cfg_mdl.get_coin_info(coin).is_zhtlc_family
    }
    function isSia(coin)
    {
        return API.app.portfolio_pg.global_cfg_mdl.get_coin_info(coin).is_sia_family
    }

    function isZhtlcReady(coin)
    {
        return !isZhtlc(coin) ? true : (zhtlcActivationProgress(coin) == 100) ? true : false
    }

    function zhtlcActivationProgress(activation_status, coin='ARRR')
    {
        let progress = 100
        if (!isZhtlc(coin)) return progress
        if (!activation_status.hasOwnProperty("result"))
        {
            return progress
        }
        let status = activation_status.result.status
        let details = activation_status.result.details

        if (!status)
        {
            return 0
        }
        else if (status == "Ok")
        {
            if (details.hasOwnProperty("error"))
            {
                console.log("["+coin+"] [zhtlcActivationProgress] Error enabling: " + JSON.stringify(details.error))
                return 0
            }
        }
        else if (status == "InProgress")
        {
            if (details.hasOwnProperty("UpdatingBlocksCache"))
            {
                let current = details.UpdatingBlocksCache.current_scanned_block
                let latest = details.UpdatingBlocksCache.latest_block
                let abs_pct = parseFloat(current/latest)
                progress = parseInt(15 * abs_pct)
                // console.log("["+coin+"] [zhtlcActivationProgress] UpdatingBlocksCache ["+current+"/"+latest+" * "+abs_pct+" | "+progress+"%]: " + JSON.stringify(details.UpdatingBlocksCache))                
            }
            else if (details.hasOwnProperty("BuildingWalletDb"))
            {
                let current = details.BuildingWalletDb.current_scanned_block
                let latest = details.BuildingWalletDb.latest_block
                let abs_pct = parseFloat(current/latest)
                progress = parseInt(98 * abs_pct)
                // console.log("["+coin+"] [zhtlcActivationProgress] BuildingWalletDb ["+current+"/"+latest+" * "+abs_pct+" * 98 | "+progress+"%]: " + JSON.stringify(details.BuildingWalletDb))
                if (progress < 15) {
                    progress = 15
                }
                else if (progress > 98) {
                    progress = 98
                }
            }
            else if (details.hasOwnProperty("RequestingWalletBalance")) progress = 99
            else if (details.hasOwnProperty("ActivatingCoin")) progress = 1
            else
            {
                progress = 2
            }
        }
        else console.log("["+coin+"] [zhtlcActivationProgress] Unexpected status: " + JSON.stringify(status))
        if (progress > 100) {
            progress = 100
        }        
        return progress
    }

    function coinContractAddress(ticker) {
        var cfg = API.app.trading_pg.get_raw_kdf_coin_cfg(ticker)
        if (cfg.hasOwnProperty('protocol')) {
            if (cfg.protocol.hasOwnProperty('protocol_data')) {
                if (cfg.protocol.protocol_data.hasOwnProperty('contract_address')) {
                    return cfg.protocol.protocol_data.contract_address
                }
            }
        }
        return ""
    }

    function coinPlatform(ticker) {
        var cfg = API.app.trading_pg.get_raw_kdf_coin_cfg(ticker)
        if (cfg.hasOwnProperty('protocol')) {
            if (cfg.protocol.hasOwnProperty('protocol_data')) {
                if (cfg.protocol.protocol_data.hasOwnProperty('platform')) {
                    return cfg.protocol.protocol_data.platform
                }
            }
        }
        return ""
    }

    function platformIcon(ticker) {
        if(ticker === "" || ticker === "All" || ticker===undefined) {
            return ""
        } else {
            const coin_info = API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker)
            return coin_icons_path + atomic_qt_utilities.retrieve_main_ticker(ticker.toString()).toLowerCase() + ".png"
        }
    }

    function contractURL(ticker) {
        if(ticker === "" || ticker === "All" || ticker===undefined) {
            return ""
        } else {
            let token_platform = coinPlatform(ticker)
            switch(token_platform) {
                case "BNB":
                    return "https://bscscan.com/token/" + coinContractAddress(ticker)
                case "POL":
                    return "https://polygonscan.com/token/" + coinContractAddress(ticker)
                case "AVAX":
                    return "https://avascan.info/blockchain/c/address/" + coinContractAddress(ticker)
                case "KCS":
                    return "https://explorer.kcc.io/en/token/" + coinContractAddress(ticker)
                case "ETH":
                    return "https://etherscan.io/token/" + coinContractAddress(ticker)
                case "ETH-ARB20":
                    return "https://arbiscan.io/token/" + coinContractAddress(ticker)
                case "ETH-BASE":
                    return "https://basescan.org/token/" + coinContractAddress(ticker)
                case "XDAI":
                    return "https://gnosisscan.io/token/" + coinContractAddress(ticker)
                case "ONE":
                    return "https://explorer.harmony.one/address/" + coinContractAddress(ticker)
                case "MOVR":
                    return "https://moonriver.moonscan.io/token/" + coinContractAddress(ticker)
                default:
                    return ""
            }
        }
    }

    function isIDO(ticker) {
        let IDO_chains = []
        return IDO_chains.includes(ticker)
    }

    // Returns the icon full path of a coin type.
    // If the given coin type has spaces, it will be replaced by '-' characters.
    // If the given coin type is empty, returns an empty string.
    function coinTypeIcon(type) {
        if (type === "") return ""

        var filename = type.toLowerCase().replace(" ", "-");
        return coin_icons_path + filename + ".png"
    }

    // Returns the full path of a provider icon.
    function providerIcon(providerName)
    {
        if (providerName === "") return ""
        return providerIconsPath + providerName + ".png";
    }

    function qaterialIcon(name) {
        return "qrc:/Qaterial/Icons/" + name + ".svg"
    }

    readonly property string cex_icon: 'ⓘ'
    readonly property string download_icon: '📥'
    readonly property string right_arrow_icon: "⮕"
    readonly property string privacy_text: "*****"
    readonly property string version_string: "Desktop v" + API.app.settings_pg.get_version()
    // Empty when the build was configured with DEX_SHOW_COMMIT_HASH=OFF
    // (the default) -- see src/core/atomicdex/version/version.hpp.
    readonly property string commit_hash_string: API.app.settings_pg.get_commit_hash()
    property bool privacy_mode: false
    readonly property var reg_pass_input: /[A-Za-z0-9@#$€£%{}[\]()\/\\'"`~,;:.<>+\-_=!^&*|?]+/
    readonly property var reg_pass_valid_low_security: /^(?=.{1,}).*$/
    readonly property var reg_pass_valid: /^(?=.{16,})(?=.*[a-z])(?=.*[A-Z])(?=.*[0-9])(?=.*[@#$%€£{}[\]()\/\\'"`~,;:.<>+\-_=!^&*|?]).*$/
    readonly property var reg_pass_uppercase: /(?=.*[A-Z])/
    readonly property var reg_pass_lowercase: /(?=.*[a-z])/
    readonly property var reg_pass_numeric: /(?=.*[0-9])/
    readonly property var reg_pass_special: /(?=.*[@#$%{}[\]()\/\\'"`~,€$£;:.<>+\-_=!^&*|?])/
    readonly property var reg_pass_count_low_security: /(?=.{1,})/
    readonly property var reg_pass_count: /(?=.{16,})/
    readonly property double time_toast_important_error: 10000
    readonly property double time_toast_basic_info: 3000
    readonly property var chart_times: (["1m", "3m", "5m", "15m", "30m", "1h", "2h", "4h", "6h", "12h", "1d", "3d"/*, "1w"*/])
    readonly property var time_seconds: ({ "1m": 60, "3m": 180, "5m": 300, "15m": 900, "30m": 1800, "1h": 3600, "2h": 7200, "4h": 14400, "6h": 21600, "12h": 43200, "1d": 86400, "3d": 259200, "1w": 604800 })
    property bool initialized_orderbook_pair: false
    readonly property string default_base: atomic_app_primary_coin
    readonly property string default_rel: atomic_app_secondary_coin

    function timestampToDouble(timestamp) {
        return (new Date(timestamp)).getTime()
    }

    function timestampToString(timestamp) {
        return (new Date(timestamp)).toUTCString()
    }

    function timestampToDate(timestamp) {
        return (new Date(timestamp * 1000))
    }

    function getDuration(total_ms) {
        let delta = Math.abs(total_ms)

        let days = Math.floor(delta / 86400000)
        delta -= days * 86400000

        let hours = Math.floor(delta / 3600000) % 24
        delta -= hours * 3600000

        let minutes = Math.floor(delta / 60000) % 60
        delta -= minutes * 60000

        let seconds = Math.floor(delta / 1000) % 60
        delta -= seconds * 1000

        let milliseconds = Math.floor(delta)

        return { days, hours, minutes, seconds, milliseconds }
    }

    function secondsToTimeLeft(date_now, date_future) {
        const r = getDuration((date_future - date_now)*1000)
        let days = r.days
        let hours = r.hours
        let minutes = r.minutes
        let seconds = r.seconds

        if(hours < 10) hours = '0' + hours
        if(minutes < 10) minutes = '0' + minutes
        if(seconds < 10) seconds = '0' + seconds
        return qsTr("%n day(s)", "", days) + '  ' + hours + ':' + minutes + ':' + seconds
    }

    function durationTextShort(total) {
        if(!General.exists(total))
            return "-"

        const r = getDuration(total)

        let text = ""
        if(r.days > 0) text += qsTr("%nd", "day", r.days) + "  "
        if(r.hours > 0) text += qsTr("%nh", "hours", r.hours) + "  "
        if(r.minutes > 0) text += qsTr("%nm", "minutes", r.minutes) + "  "
        if(r.seconds > 0) text += qsTr("%ns", "seconds", r.seconds) + "  "
        if(text === "" && r.milliseconds > 0) text += qsTr("%nms", "milliseconds", r.milliseconds) + "  "
        if(text === "") text += qsTr("-")

        return text
    }

    function logObject(obj) {
        for (var key in obj) {
            console.log(key + ": " + obj[key]);
        }
    }

    function flipFalse(obj) {
        if (obj === false) return true
        return obj
    }

    function flipTrue(obj) {
        if (obj === true) return false
        return obj
    }

    function getCustomFeeType(ticker_infos)
    {
        if (["ZHTLC", "Moonbeam", "QRC-20"].includes(ticker_infos.type)) return ""
        if (!General.isSpecialToken(ticker_infos) && !General.isParentCoin(ticker_infos.ticker) || ["KMD"].includes(ticker_infos.ticker))
        {
            return "UTXO"
        }
        else
        {
            return "Gas"
        }
    }

    function getFeesDetail(fees) {
        if (privacy_mode) {
            return [
                {"label": privacy_text},
                {"label": privacy_text},
                {"label": privacy_text},
                {"label": privacy_text}
            ]
        } 
        return [
            {"label": qsTr("<b>Taker tx fee:</b> "), "fee": fees.base_transaction_fees, "ticker": fees.base_transaction_fees_ticker},
            {"label": qsTr("<b>Dex tx fee:</b> "), "fee": fees.fee_to_send_taker_fee, "ticker": fees.fee_to_send_taker_fee_ticker},
            {"label": qsTr("<b>Dex fee:</b> "), "fee": fees.trading_fee, "ticker": fees.trading_fee_ticker},
            {"label": qsTr("<b>Maker tx fee:</b> "), "fee": fees.rel_transaction_fees, "ticker": fees.rel_transaction_fees_ticker}
        ]
    }

    function getFeesDetailText(feetype, amount, ticker) {
        if ([feetype, amount, ticker].includes(undefined)) return ""
        let fiat_text = General.getFiatText(amount, ticker, false)
        amount = formatDouble(amount, 8, false).toString()
        return feetype + " " + amount + " " + ticker + " (" + fiat_text + ")"
    }

    function reducedBignum(text, decimals=8, max_length=12) {
        let val = new BigNumber(text).toFixed(decimals)
        if (val.length > max_length)
        {
            return val.substring(0, max_length)
        }
        return val
    }

    function getSimpleFromPlaceholder(selectedTicker, selectedOrder, sell_ticker_balance) {
        if (privacy_mode)
        {
            return "0"
        }
        if (sell_ticker_balance == 0)
        {
            return qsTr("Balance is zero!")
        }
        if (!isZhtlcReady(selectedTicker))
        {
            return qsTr("Activating %1 (%2%)").arg(atomic_qt_utilities.retrieve_main_ticker(selectedTicker)).arg(progress)
        }
        if (API.app.trading_pg.max_volume == 0)
        {
            return qsTr("Loading wallet...")
        }
        if (typeof selectedOrder !== 'undefined')
        {
            return qsTr("Min: %1").arg(API.app.trading_pg.min_trade_vol)
        }
        return qsTr("Enter an amount")
    }

    function arrayExclude(arr, excl) {
        let i = arr.indexOf(excl)
        if (i > -1) arr.splice(i, 1);
        return arr
    }

    function absString(str) {
        return str.replace("-", "")
    }

    function clone(obj) {
        return JSON.parse(JSON.stringify(obj));
    }

    function prettifyJSON(j) {
        const j_obj = typeof j === "string" ? JSON.parse(j) : j
        return JSON.stringify(j_obj, null, 4)
    }

    function addressTxUri(coin_info) {
        if (coin_info.tx_uri == "") return "address/"
            return coin_info.address_uri
    }

    function getTxUri(coin_info) {
        if (coin_info.tx_uri == "") return "tx/"
        return coin_info.tx_uri
    }

    function getBlockUri(coin_info) {
        if (coin_info.block_uri == "") return "block/"
        return coin_info.block_uri
    }

    function getTxExplorerURL(ticker, txid, add_0x=true) {
        if (privacy_mode) return ''
        if(txid !== '') {
            const coin_info = API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker)
            const txid_prefix = (add_0x && coin_info.is_erc_family) ? '0x' : ''
            return coin_info.explorer_url + getTxUri(coin_info) + txid_prefix + txid
        }
    }

    function getAddressExplorerURL(ticker, address) {
        if (privacy_mode) return ''
        if(address !== '') {
            const coin_info = API.app.portfolio_pg.global_cfg_mdl.get_coin_info(ticker)
            return coin_info.explorer_url + addressTxUri(coin_info) + address
        }
        return ""
    }

    function viewTxAtExplorer(ticker, txid, add_0x=true) {
        if (privacy_mode) return ''
        if(txid !== '') {
            Qt.openUrlExternally(getTxExplorerURL(ticker, txid, add_0x))
        }
    }

    function viewAddressAtExplorer(ticker, address) {
        if (privacy_mode) return ''
        if(address !== '') {
            Qt.openUrlExternally(getAddressExplorerURL(ticker, address))
        }
    }

    function diffPrefix(received) {
        return received === "" ? "" : received === true ? "+ " :  "- "
    }

    function filterCoins(list, text, type) {
        return list.filter(c => (c.ticker.indexOf(text.toUpperCase()) !== -1 || c.name.toUpperCase().indexOf(text.toUpperCase()) !== -1) &&
                           (type === undefined || c.type === type)).sort((a, b) => {
                               if(a.ticker < b.ticker) return -1
                               if(a.ticker > b.ticker) return 1
                               return 0
                           })
    }

    function validFiatRates(data, fiat) {
        return data && data.rates && data.rates[fiat]
    }

    function nFormatter(num, digits) {
      if(num < 1E5) return General.formatDouble(num)

      const si = [
        { value: 1, symbol: "" },
        { value: 1E3, symbol: "k" },
        { value: 1E6, symbol: "M" },
        { value: 1E9, symbol: "G" },
        { value: 1E12, symbol: "T" },
        { value: 1E15, symbol: "P" },
        { value: 1E18, symbol: "E" }
      ]
      const rx = /\.0+$|(\.[0-9]*[1-9])0+$/

      let i
      for (i = si.length - 1; i > 0; --i)
        if (num >= si[i].value) break

      return (num / si[i].value).toFixed(digits).replace(rx, "$1") + si[i].symbol
    }

    function convertUsd(v) {
        if (privacy_mode) return ''
        let rate = API.app.get_rate_conversion("USD", API.app.settings_pg.current_currency)
        let value = parseFloat(v) / parseFloat(rate)

        if (API.app.settings_pg.current_fiat == API.app.settings_pg.current_currency) {
            let fiat_rate = API.app.get_fiat_rate(API.app.settings_pg.current_fiat)
            value = parseFloat(v) * parseFloat(fiat_rate)
        }
        return formatFiat("", value, API.app.settings_pg.current_currency)
    }

    function formatFiat(received, amount, fiat, sf=2) {
        if (privacy_mode) return ''
        if (sf == 2 && fiat == "BTC") {
            sf = 8
        }
        return diffPrefix(received) +
                (fiat === API.app.settings_pg.current_fiat ? API.app.settings_pg.current_fiat_sign : API.app.settings_pg.current_currency_sign)
                + " " + (amount < 1E5 ? formatDouble(parseFloat(amount), sf, true) : nFormatter(parseFloat(amount), sf))
    }

    function formatPercent(value, show_prefix=true) {
        if (privacy_mode) return ''
        let prefix = ''
        if(value > 0) prefix = '+ '
        else if(value < 0) {
            prefix = '- '
            value *= -1
        }
        return (show_prefix ? prefix : '') + parseFloat(value).toFixed(3) + ' %'
    }

    function formatCexRates(value) {
        if (value === "0") return "N/A"
        if (parseFloat(value) > 0) {
            return "+"+formatNumber(value, 2)+"%"
        }
        return formatNumber(value, 2)+"%"
    }

    readonly property int defaultPrecision: 8
    readonly property int sliderDigitLimit: 9
    readonly property int recommendedPrecision: -1337

    function getDigitCount(v) {
        return v.toString().replace("-", "").split(".")[0].length
    }

    function getRecommendedPrecision(v, limit) {
        const lim = limit || sliderDigitLimit
        return Math.min(Math.max(lim - getDigitCount(v), 0), defaultPrecision)
    }

    /**
    * Converts a float into a readable string with K, M, B, etc.
    * @param {number} num - The number to format.
    * @param {number} decimals - The number of decimal places to include (default is 2).
    * @param {number} extra_decimals - The number of decimal places to include if no suffix (default is 8).
    * @returns {string} - The formatted string.
    */
    function formatNumber(num, decimals = 8) {
        let r = "0";
        let suffix = "";

        if (isNaN(num) || num === null) {
            return r;
        }

        if (typeof(num) == 'string') {
            num = parseFloat(num)
        }

        const suffixes = ['', 'K', 'M', 'B', 'T']; // Add more as needed for larger numbers
        const tier = Math.floor(Math.log10(Math.abs(num)) / 3); // Determine the tier (e.g., thousands, millions)

        if ([-1, 0].includes(tier)) {
            r = num.toFixed(decimals);
            return r
        }
        if (tier <= suffixes.length - 1) {
            suffix = suffixes[tier]
            if (suffix != '') 
            {
                num = (num / Math.pow(10, tier * 3));
            }
        }
        else {
            suffix = "e" + tier * 3
            num = (num / Math.pow(10, tier * 3));
        }
        r = num.toFixed(decimals) + "" + suffix;
        return r;
    }

    function formatDouble(v, sf = defaultPrecision, trail_zeros = true) {
        if(v === '') return "0"
        if(sf === recommendedPrecision) sf = getRecommendedPrecision(v)

        if(sf === 0) return parseInt(v).toString()

        // Remove more than n decimals, then convert to string without trailing zeros
        const full_double = parseFloat(v).toFixed(sf || defaultPrecision)

        return trail_zeros ? full_double : full_double.replace(/\.?0+$/,"")
    }

    function getComparisonScale(value) {
        return Math.min(Math.pow(10, getDigitCount(parseFloat(value))), 1000000000)
    }

    function limitDigits(value) {
        return parseFloat(formatDouble(value, 2))
    }

    function formatCrypto(received, amount, ticker, fiat_amount, fiat, sf, trail_zeros) {
        if (privacy_mode) {
            return ""
        }
        const prefix = diffPrefix(received)
        return prefix + ticker + " " + formatDouble(amount, sf, trail_zeros) + (fiat_amount ? " (" + formatFiat("", fiat_amount, fiat) + ")" : "")
    }

    function formatFullCrypto(received, amount, ticker, fiat_amount, fiat, use_full_ticker) {
        if (!use_full_ticker) ticker = atomic_qt_utilities.retrieve_main_ticker(ticker)
        return formatCrypto(received, amount, ticker, fiat_amount, fiat)
    }

    function fullCoinName(name, ticker) {
        return name + " (" + ticker + ")"
    }

    function fullNamesOfCoins(coins) {
        return coins.map(c => {
         return { value: c.ticker, text: fullCoinName(c.name, c.ticker) }
        })
    }

    function tickersOfCoins(coins) {
        return coins.map(c => {
            return { value: c.ticker, text: c.ticker }
        })
    }

    function getMinTradeAmount() {
        return formatDouble(API.app.trading_pg.min_trade_vol, 8, false).toString()
    }

    function getReversedMinTradeAmount() {
        if (API.app.trading_pg.market_mode == MarketMode.Buy) {
           return getMinTradeAmount()
        }
        return formatDouble(API.app.trading_pg.orderbook.rel_min_taker_vol, 8, false).toString()
    }

    function hasEnoughFunds(sell, base, rel, price, volume) {
        if(sell) {
            if(volume === "") return true
            return API.app.do_i_have_enough_funds(base, volume)
        }
        else {
            if(price === "") return true
            const needed_amount = parseFloat(price) * parseFloat(volume)
            return API.app.do_i_have_enough_funds(rel, needed_amount)
        }
    }

    function isZero(v) {
        return !isFilled(v) || parseFloat(v) === 0
    }

    function exists(v) {
        return v !== undefined && v !== null
    }

    function isFilled(v) {
        return exists(v) && v !== ""
    }

    function isParentCoinNeeded(ticker, coin_type)
    {
        let enabled_coins = API.app.portfolio_pg.get_all_enabled_coins()
        for (const coin of enabled_coins)
        {
            let c_info = API.app.portfolio_pg.global_cfg_mdl.get_coin_info(coin)
            if(c_info.type === coin_type && c_info.ticker !== ticker) return true
        }
        return false
    }

    function canDisable(ticker) {
        if (prevent_coin_disabling.running) return false
        if (ticker === atomic_app_primary_coin || ticker === atomic_app_secondary_coin) return false
        if (ticker === "ETH") return !General.isParentCoinNeeded("ETH", "ERC-20")
        if (ticker === "GLEEC") return !General.isParentCoinNeeded("GLEEC", "GRC-20")
        if (ticker === "TRX") return !General.isParentCoinNeeded("TRX", "TRC-20")
        if (ticker === "POL") return !General.isParentCoinNeeded("POL", "PLG-20")
        if (ticker === "AVAX") return !General.isParentCoinNeeded("AVAX", "AVX-20")
        if (ticker === "BNB") return !General.isParentCoinNeeded("BNB", "BEP-20")
        if (ticker === "ONE") return !General.isParentCoinNeeded("ONE", "HRC-20")
        if (ticker === "QTUM") return !General.isParentCoinNeeded("QTUM", "QRC-20")
        if (ticker === "KCS") return !General.isParentCoinNeeded("KCS", "KRC-20")
        if (ticker === "MOVR") return !General.isParentCoinNeeded("MOVR", "Moonriver")
        if (ticker === "IRIS") return !General.isParentCoinNeeded("IRIS", "COSMOS")
        if (ticker === "OSMO") return !General.isParentCoinNeeded("OSMO", "COSMOS")
        if (ticker === "ATOM") return !General.isParentCoinNeeded("ATOM", "COSMOS")
        return true
    }

    function tokenUnitName(current_ticker_infos)
    {
        if (current_ticker_infos.type === "TENDERMINT" || current_ticker_infos.type === "TENDERMINTTOKEN")
        {
            return "u" + current_ticker_infos.name.toLowerCase()
        }
        return current_ticker_infos.type === "QRC-20" ? "Satoshi" : "Gwei"
    }

    function isSpecialToken(current_ticker_infos)
    {
        if (current_ticker_infos.hasOwnProperty("has_parent_fees_ticker"))
            return current_ticker_infos.has_parent_fees_ticker
        return false
    }

    function isERC20(current_ticker_infos) {
        return current_ticker_infos.type === "ERC-20"
            || current_ticker_infos.type === "BEP-20"
            || current_ticker_infos.type === "TRC-20"
            || current_ticker_infos.type === "GRC-20"
            || current_ticker_infos.type == "PLG-20"
            || current_ticker_infos.type == "AVX-20"
            || current_ticker_infos.type == "Gnosis"
            || current_ticker_infos.type == "Base"
            || current_ticker_infos.type == "Arbitrum"
    }

    function isParentCoin(ticker) {
        return ["ETH", "ETH-ARB20", "ETH-BASE", "POL", "AVAX", "QTUM", "BNB", "ONE", "KCS", "TRX", "GLEEC", "XDAI"].includes(ticker)
    }

    function getFeesTicker(coin_info) {
        if (coin_info.has_parent_fees_ticker)
            return coin_info.fees_ticker
    }

    function getRandomInt(min, max) {
        min = Math.ceil(min)
        max = Math.floor(max)
        return Math.floor(Math.random() * (max - min + 1)) + min
    }

    function getFiatText(v, ticker, has_info_icon=true) {
        let fiat_from_amount = API.app.get_fiat_from_amount(ticker, v)
        let current_fiat = API.app.settings_pg.current_fiat
        let formatted_fiat = General.formatFiat('', v === '' ? 0 : fiat_from_amount, current_fiat)
        return formatted_fiat + (has_info_icon ? " " +  General.cex_icon : "")
    }

    function hasParentCoinFees(trade_info) {
        return General.isFilled(trade_info.rel_transaction_fees) && parseFloat(trade_info.rel_transaction_fees) > 0
    }

    function feeText(trade_info, base_ticker, has_info_icon=true, has_limited_space=false) {
        if(!trade_info || !trade_info.trading_fee) return ""
        const tx_fee = txFeeText(trade_info, base_ticker, has_info_icon, has_limited_space)
        const trading_fee = tradingFeeText(trade_info, base_ticker, has_info_icon)
        const minimum_amount = minimumtradingFeeText(trade_info, base_ticker, has_info_icon)
        return tx_fee + "\n" + trading_fee +"<br>"+minimum_amount
    }

    function is_swap_safe(checkbox)
    {
        if (checkbox.checked == true || checkbox.visible == false)
        {
            return (!API.app.trading_pg.buy_sell_rpc_busy && API.app.trading_pg.last_trading_error == TradingError.None)
        }
        return false
    }

    function validateWallet(wallet_name) {
        if (wallet_name.length >= 25) return "Wallet name must 25 chars or less"
        return checkIfWalletExists(wallet_name)
    }

    function txFeeText(trade_info, base_ticker, has_info_icon=true, has_limited_space=false) {
        if(!trade_info || !trade_info.trading_fee) return ""
        const has_parent_coin_fees = hasParentCoinFees(trade_info)
         var info =  qsTr('%1 Transaction Fee'.arg(trade_info.base_transaction_fees_ticker))+': '+ trade_info.base_transaction_fees + " (%1)".arg(getFiatText(trade_info.base_transaction_fees, trade_info.base_transaction_fees_ticker, has_info_icon))
        if (has_parent_coin_fees) {
            info = info+"<br>"+qsTr('%1 Transaction Fee'.arg(trade_info.rel_transaction_fees_ticker))+': '+ trade_info.rel_transaction_fees + " (%1)".arg(getFiatText(trade_info.rel_transaction_fees, trade_info.rel_transaction_fees_ticker, has_info_icon))
        }

        return info+"<br>"
//        const main_fee = (qsTr('Transaction Fee') + ': ' + General.formatCrypto("", trade_info.base_transaction_fees, trade_info.base_transaction_fees_ticker)) +
//                                 // Rel Fees
//                                 (has_parent_coin_fees ? " + " + General.formatCrypto("", trade_info.rel_transaction_fees, trade_info.rel_transaction_fees_ticker) : '')

//        let fiat_part = "("
//        fiat_part += getFiatText(trade_info.base_transaction_fees, trade_info.base_transaction_fees_ticker, false)
//        if(has_parent_coin_fees) fiat_part += (has_limited_space ? "\n\t\t+ " : " + ") + getFiatText(trade_info.rel_transaction_fees, trade_info.rel_transaction_fees_ticker, has_info_icon)
//        fiat_part += ")"

//        return main_fee + " " + fiat_part
    }
//    function txFeeText2(trade_info, base_ticker, has_info_icon=true, has_limited_space=false) {
//        if(!trade_info || !trade_info.trading_fee) return ""

//        const has_parent_coin_fees = hasParentCoinFees(trade_info)
//        const main_fee = (qsTr('Transaction Fee') + ': ' + General.formatCrypto("", trade_info.base_transaction_fees, trade_info.base_transaction_fees_ticker)) +
//                                 // Rel Fees
//                                 (has_parent_coin_fees ? " + " + General.formatCrypto("", trade_info.rel_transaction_fees, trade_info.rel_transaction_fees_ticker) : '')

//        let fiat_part = "("
//        fiat_part += getFiatText(trade_info.base_transaction_fees, trade_info.base_transaction_fees_ticker, false)
//        if(has_parent_coin_fees) fiat_part += (has_limited_space ? "\n\t\t+ " : " + ") + getFiatText(trade_info.rel_transaction_fees, trade_info.rel_transaction_fees_ticker, has_info_icon)
//        fiat_part += ")"

//        return main_fee + " " + fiat_part
//    }

    function tradingFeeText(trade_info, base_ticker, has_info_icon=true) {
        if(!trade_info || !trade_info.trading_fee) return ""

        return trade_info.trading_fee_ticker+" "+qsTr('Trading Fee') + ': ' + General.formatCrypto("", trade_info.trading_fee, "") +

                // Fiat part
                (" ("+
                    getFiatText(trade_info.trading_fee, trade_info.trading_fee_ticker, has_info_icon)
                 +")")
    }
    function minimumtradingFeeText(trade_info, base_ticker, has_info_icon=true) {
        if(!trade_info || !trade_info.trading_fee) return ""

        return API.app.trading_pg.market_pairs_mdl.left_selected_coin+" "+qsTr('Minimum Trading Amount') + ': ' + General.formatCrypto("", API.app.trading_pg.min_trade_vol , "") +

                // Fiat part
                (" ("+
                    getFiatText(API.app.trading_pg.min_trade_vol , API.app.trading_pg.market_pairs_mdl.left_selected_coin, has_info_icon)
                 +")")
    }

    function checkIfWalletExists(name)
    {
        if(API.app.wallet_mgr.get_wallets().indexOf(name) !== -1)
            return qsTr("Wallet %1 already exists", "WALLETNAME").arg(name)
        return ""
    }

    function getTradingError(error, fee_info, base_ticker, rel_ticker, left_ticker, right_ticker) {
        switch(error) {
        case TradingError.None:
            return ""
        case TradingError.LeftZhtlcChainNotEnabled:
            return qsTr("Please wait for %1 to fully activate").arg(left_ticker)
        case TradingError.RightZhtlcChainNotEnabled:
            return qsTr("Please wait for %1 to fully activate").arg(right_ticker)
        case TradingError.TotalFeesNotEnoughFunds:
            return qsTr("%1 balance is lower than the fees amount: %2 %3").arg(fee_info.error_fees.coin).arg(fee_info.error_fees.required_balance).arg(fee_info.error_fees.coin)
        case TradingError.BalanceIsLessThanTheMinimalTradingAmount:
            return qsTr("Tradable (after fees) %1 balance is lower than minimum trade amount").arg(base_ticker) + " : " + General.getMinTradeAmount()
        case TradingError.PriceFieldNotFilled:
            return qsTr("Please fill the price field")
        case TradingError.VolumeFieldNotFilled:
            return qsTr("Please fill the volume field")
        case TradingError.VolumeIsLowerThanTheMinimum:
            return qsTr("%1 volume is lower than minimum trade amount").arg(API.app.trading_pg.market_pairs_mdl.left_selected_coin) + " : " + General.getMinTradeAmount()
        case TradingError.ReceiveVolumeIsLowerThanTheMinimum:
            return qsTr("%1 volume is lower than minimum trade amount").arg(rel_ticker) + " : " + General.getReversedMinTradeAmount()
        case TradingError.LeftParentChainNotEnabled:
            return qsTr("%1 needs to be enabled in order to use %2").arg(API.app.portfolio_pg.global_cfg_mdl.get_parent_coin(left_ticker)).arg(left_ticker)
        case TradingError.LeftParentChainNotEnoughBalance:
            return qsTr("%1 balance needs to be funded, a non-zero balance is required to pay the gas of %2 transactions").arg(API.app.portfolio_pg.global_cfg_mdl.get_parent_coin(left_ticker)).arg(left_ticker)
        case TradingError.RightParentChainNotEnabled:
             return qsTr("%1 needs to be enabled in order to use %2").arg(API.app.portfolio_pg.global_cfg_mdl.get_parent_coin(right_ticker)).arg(right_ticker)
        case TradingError.RightParentChainNotEnoughBalance:
             return qsTr("%1 balance needs to be funded, a non-zero balance is required to pay the gas of %2 transactions").arg(API.app.portfolio_pg.global_cfg_mdl.get_parent_coin(right_ticker)).arg(right_ticker)
        default:
            return qsTr("Unknown Error") + ": " + error
        }
    }

    readonly property var zcash_params_filesize: ({
        "sapling-output.params": 3592860,
        "sapling-spend.params": 47958396
    })
}
