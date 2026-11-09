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
	static char prefix[64] = "pinna:";
	return prefix;
}

inline const char *
LogPrefix(void)
{
	return LogPrefixStore();
}

inline void
SetLogPrefix(const char *prefix)
{
	const char *p = prefix && prefix[0] ? prefix : kDefaultLogPrefix;
	char *store = LogPrefixStore();
	size_t n = strlen(p);
	if(n > 63)
		n = 63;
	memcpy(store, p, n);
	store[n] = '\0';
}

inline const char *
QueueLabelOf(const ShellConfig *config)
{
	return config && config->queueLabel && config->queueLabel[0] ? config->queueLabel : kDefaultQueueLabel;
}

}
