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

#include <string>

// Set by CMake from DEX_GIT_COMMIT_HASH (root CMakeLists.txt): the build's
// short git commit hash when DEX_SHOW_COMMIT_HASH is ON, empty otherwise.
// The #ifndef guard covers any translation unit built outside the `core`
// target (which is where the compile definition is actually attached).
#ifndef DEX_GIT_COMMIT_HASH
    #define DEX_GIT_COMMIT_HASH ""
#endif

namespace atomic_dex
{
    constexpr const char*
    get_version()
    {
        return "0.9.9";
    }

    /// The build's short git commit hash, or an empty string when
    /// DEX_SHOW_COMMIT_HASH was OFF at configure time (the default -- a
    /// build's own commit is not meaningful to an end user and stays out of
    /// the log/UI unless a release build explicitly turns it on).
    constexpr const char*
    get_commit_hash()
    {
        return DEX_GIT_COMMIT_HASH;
    }

    constexpr int
    get_num_version() noexcept
    {
        return 99;
    }

    constexpr const char*
    get_raw_version()
    {
        return "0.9.9";
    }

    constexpr const char*
    get_precedent_raw_version()
    {
        return "0.8.2";
    }

    /// "0.9.9" or "0.9.9 (abc123456)", depending on whether the commit hash
    /// is shown for this build. Used identically by the startup log line
    /// and the main-screen UI so both always agree.
    inline std::string
    get_version_display_string()
    {
        const std::string hash = get_commit_hash();
        if (hash.empty())
        {
            return get_version();
        }
        return std::string(get_version()) + " (" + hash + ")";
    }
} // namespace atomic_dex
