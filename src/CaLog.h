#pragma once

#include <stddef.h>
#include <string.h>

namespace ca {

struct ShellConfig
{
	const char *logPrefix;
	const char *queueLabel;
};

static const char kDefaultLogPrefix[] = "pinna:";
static const char kDefaultQueueLabel[] = "pinna.audio.control";

inline char *
LogPrefixStore(void)
{
	static char prefix[64];
	return prefix;
}

inline const char *
LogPrefix(void)
{
	const char *store = LogPrefixStore();
	return store[0] ? store : kDefaultLogPrefix;
}

inline bool
SetLogPrefix(const char *prefix)
{
	const char *p = prefix && prefix[0] ? prefix : kDefaultLogPrefix;
	size_t n = strlen(p);
	if(n > 63)
		n = 63;
	const char *now = LogPrefix();
	if(strlen(now) == n && memcmp(now, p, n) == 0)
		return false;
	char *store = LogPrefixStore();
	memcpy(store, p, n);
	store[n] = '\0';
	return true;
}

inline const char *
QueueLabelOf(const ShellConfig *config)
{
	return config && config->queueLabel && config->queueLabel[0] ? config->queueLabel : kDefaultQueueLabel;
}

}
