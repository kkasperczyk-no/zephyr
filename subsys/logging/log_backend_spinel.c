#include <logging/log_backend.h>
#include <logging/log_output.h>
#include "log_backend_std.h"

#ifndef CONFIG_LOG_BACKEND_SPINEL_BUFFER_SIZE
#define CONFIG_LOG_BACKEND_SPINEL_BUFFER_SIZE 0
#endif

static u8_t char_buf[CONFIG_LOG_BACKEND_SPINEL_BUFFER_SIZE];
static bool panic_mode;

static int write(u8_t *data, size_t length, void *ctx);

LOG_OUTPUT_DEFINE(log_output, write,
		  char_buf, sizeof(char_buf));

static void put(const struct log_backend *const backend,
		struct log_msg *msg)
{
    log_backend_std_put(&log_output, 0, msg);
}

static void sync_string(const struct log_backend *const backend,
		     struct log_msg_ids src_level, u32_t timestamp,
		     const char *fmt, va_list ap)
{
    log_backend_std_sync_string(&log_output, 0, src_level,
				    timestamp, fmt, ap);
}

static void sync_hexdump(const struct log_backend *const backend,
			 struct log_msg_ids src_level, u32_t timestamp,
			 const char *metadata, const u8_t *data, u32_t length)
{
    log_backend_std_sync_hexdump(&log_output, 0, src_level,
				     timestamp, metadata, data, length);
}

static void log_backend_spinel_init(void)
{
    memset(char_buf, '\0', sizeof(char_buf));
}

static void panic(struct log_backend const *const backend)
{
	log_backend_std_panic(&log_output);
	panic_mode = true;
}

static void dropped(const struct log_backend *const backend, u32_t cnt)
{
	ARG_UNUSED(backend);

	log_backend_std_dropped(&log_output, cnt);
}

__attribute__((weak)) void otPlatLog(int aLogLevel, int aLogRegion, const char *aFormat, ...)
{
    ARG_UNUSED(aLogLevel);
    ARG_UNUSED(aLogRegion);
    ARG_UNUSED(aFormat);
}

static int write(u8_t *data, size_t length, void *ctx){
    // FIXME: log region
    otPlatLog(1, 5, "%s", data);
	// make sure that buffer will be clean in next attempt
	memset(char_buf, '\0', length);
    return length;
}

const struct log_backend_api log_backend_spinel_api = {
	.put = IS_ENABLED(CONFIG_LOG_IMMEDIATE) ? NULL : put,
	.put_sync_string = IS_ENABLED(CONFIG_LOG_IMMEDIATE) ?
			sync_string : NULL,
	.put_sync_hexdump = IS_ENABLED(CONFIG_LOG_IMMEDIATE) ?
			sync_hexdump : NULL,
	.panic = panic,
	.init = log_backend_spinel_init,
	.dropped = IS_ENABLED(CONFIG_LOG_IMMEDIATE) ? NULL : dropped,
};

LOG_BACKEND_DEFINE(log_backend_spinel, log_backend_spinel_api, true);