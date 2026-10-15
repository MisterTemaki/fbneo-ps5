// FBNeo PS5's title IDs (ProsperoMigrate.h): the current one and the ones it used before.
//
// SPDX-License-Identifier: MIT
#pragma once

#ifndef FBNEO_TITLE_ID
#define FBNEO_TITLE_ID "PPSA99924"
#endif

#define MIG_APP_NAME "FBNeo PS5"
#define MIG_NEW_TITLE FBNEO_TITLE_ID
// up to 1.8 it was PPSA99012, which the homebrew catalog (homebrew.page) lists for another app
#define MIG_OLD_TITLES "PPSA99012"
// the label of our content ID (UP9000-<title>_00-<label>), the same under every title ID
#define MIG_CONTENT_LABEL "FBNEOPS500000001"
// the host tests' overrides: <prefix>PS5_HOMEBREW, <prefix>SMP_PORT, <prefix>SMP_CONFIG, <prefix>MIGRATE_RETRY_MS
#define MIG_ENV_PREFIX "FBNEO_"
