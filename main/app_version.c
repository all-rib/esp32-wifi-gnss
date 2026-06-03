#include <stdio.h>

#include "esp_app_desc.h"

#include "app_version.h"

#define APP_VERSION_HASH_HEX_LEN 12

#ifndef APP_VERSION_GIT_TAG
#define APP_VERSION_GIT_TAG "unknown"
#endif

#ifndef APP_VERSION_FLAVOR
#define APP_VERSION_FLAVOR "main"
#endif

const char *app_version_string(void)
{
	static char version[128];

	if (version[0] == '\0') {
		char app_sha[65] = { 0 };

		esp_app_get_elf_sha256(app_sha, sizeof(app_sha));
		app_sha[APP_VERSION_HASH_HEX_LEN] = '\0';

		snprintf(version,
			 sizeof(version),
			 "%s+%s-%s",
			 APP_VERSION_GIT_TAG,
			 app_sha,
			 APP_VERSION_FLAVOR);
	}

	return version;
}
