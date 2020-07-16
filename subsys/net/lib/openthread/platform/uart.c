/*
 * Copyright (c) 2020 Tridonic GmbH & Co KG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_LEVEL CONFIG_OPENTHREAD_LOG_LEVEL
#define LOG_MODULE_NAME net_otPlat_uart

#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>

#include <drivers/uart.h>

#include <sys/ring_buffer.h>
#include <sys/atomic.h>

#ifdef CONFIG_OPENTHREAD_NCP_SPINEL_ON_UART_ACM
#include <usb/usb_device.h>
#endif

#include <openthread-system.h>
#include <openthread/platform/uart.h>

#include "platform-zephyr.h"

struct openthread_uart {
	struct ring_buf *rx_ringbuf;
	struct device *dev;
	atomic_t tx_busy;
	atomic_t tx_finished;
};

#define OT_UART_DEFINE(_name, _ringbuf_size) \
	RING_BUF_DECLARE(_name##_rx_ringbuf, _ringbuf_size); \
	static struct openthread_uart _name = { \
		.rx_ringbuf = &_name##_rx_ringbuf, \
	}

OT_UART_DEFINE(ot_uart, CONFIG_OPENTHREAD_NCP_UART_RING_BUFFER_SIZE);

#define RX_FIFO_SIZE 128

static bool is_panic_mode;
static const uint8_t *s_write_buffer = NULL;
static uint16_t       s_write_length = 0;

static void uart_rx_handle(void)
{
	u8_t *data;
	u32_t len;
	u32_t rd_len;
	bool new_data = false;

	do {
		len = ring_buf_put_claim(
			ot_uart.rx_ringbuf, &data,
			ot_uart.rx_ringbuf->size);
		if (len > 0) {
			rd_len = uart_fifo_read(
				ot_uart.dev, data, len);

			if (rd_len > 0) {
				new_data = true;
			}

			int err = ring_buf_put_finish(
				ot_uart.rx_ringbuf, rd_len);
			(void)err;
			__ASSERT_NO_MSG(err == 0);
		} else {
			u8_t dummy;

			/* No space in the ring buffer - consume byte. */
			LOG_WRN("RX ring buffer full.");

			rd_len = uart_fifo_read(
				ot_uart.dev, &dummy, 1);
		}
	} while (rd_len && (rd_len == len));

	if (new_data) {
		otSysEventSignalPending();
	}
}

static void uart_tx_handle(void)
{
	u32_t len;

	if (s_write_length) {
		len = uart_fifo_fill(ot_uart.dev, s_write_buffer, s_write_length);
		s_write_buffer += len;
		s_write_length -= len;
	} else {
		uart_irq_tx_disable(ot_uart.dev);
		ot_uart.tx_busy = 0;
		atomic_set(&(ot_uart.tx_finished), 1);
		otSysEventSignalPending();
	}
}

static void uart_callback(void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(ot_uart.dev) &&
	       uart_irq_is_pending(ot_uart.dev)) {

		if (uart_irq_rx_ready(ot_uart.dev)) {
			uart_rx_handle();
		}

		if (uart_irq_tx_ready(ot_uart.dev)) {
			uart_tx_handle();
		}
	}
}

void platformUartProcess(otInstance *aInstance)
{
	u32_t len = 0;
	const u8_t *data;

	/* Process UART RX */
	while ((len = ring_buf_get_claim(
			ot_uart.rx_ringbuf,
			(u8_t **)&data,
			ot_uart.rx_ringbuf->size)) > 0) {
		int err;

		otPlatUartReceived(data, len);
		err = ring_buf_get_finish(
				ot_uart.rx_ringbuf,
				len);
		(void)err;
		__ASSERT_NO_MSG(err == 0);
	}

	/* Process UART TX */
	if (ot_uart.tx_finished) {
		LOG_DBG("UART TX done");
		otPlatUartSendDone();
		ot_uart.tx_finished = 0;
	}
};

otError otPlatUartEnable(void)
{
	ot_uart.dev = device_get_binding(
		CONFIG_OPENTHREAD_NCP_SPINEL_ON_UART_DEV_NAME);

	if ((&ot_uart)->dev == NULL) {
		LOG_ERR("UART device not found");
		return OT_ERROR_FAILED;
	}

#ifdef CONFIG_OPENTHREAD_NCP_SPINEL_ON_UART_ACM
	int ret = usb_enable(NULL);
	u32_t baudrate = 0U;

	if (ret != 0) {
		LOG_ERR("Failed to enable USB");
		return OT_ERROR_FAILED;
	}

	LOG_INF("Wait for host to settle");
	k_sleep(K_SECONDS(1));

	ret = uart_line_ctrl_get(ot_uart.dev,
				 UART_LINE_CTRL_BAUD_RATE,
				 &baudrate);
	if (ret) {
		LOG_WRN("Failed to get baudrate, ret code %d", ret);
	} else {
		LOG_INF("Baudrate detected: %d", baudrate);
	}
#endif

	uart_irq_callback_user_data_set(
		ot_uart.dev,
		uart_callback,
		(void *)&ot_uart);
	uart_irq_rx_enable(ot_uart.dev);

	return OT_ERROR_NONE;
};

otError otPlatUartDisable(void)
{
#ifdef CONFIG_OPENTHREAD_NCP_SPINEL_ON_UART_ACM
	int ret = usb_disable();

	if (ret) {
		LOG_WRN("Failed to disable USB, ret code %d", ret);
	}
#endif

	uart_irq_tx_disable(ot_uart.dev);
	uart_irq_rx_disable(ot_uart.dev);
	return OT_ERROR_NONE;
};


otError otPlatUartSend(const u8_t *aBuf, u16_t aBufLength)
{
	if (NULL == aBuf) {
		return OT_ERROR_FAILED;
	}

	s_write_buffer = aBuf;
	s_write_length = aBufLength;

	if (atomic_set(&(ot_uart.tx_busy), 1) == 0) {
		if (is_panic_mode) {
			/* In panic mode all data have to be send immediately
			 * without using interrupts
		 	 */
			otPlatUartFlush();
		} else {
			uart_irq_tx_enable(ot_uart.dev);
		}
	}

	return OT_ERROR_NONE;
};

otError otPlatUartFlush(void)
{
	u32_t len;
	otError result = OT_ERROR_NONE;

	if (s_write_length) {
		for (size_t i = 0; i < len; i++) {
		     uart_poll_out(ot_uart.dev, s_write_buffer+i);
		}
	}

	ot_uart.tx_busy = 0;
	atomic_set(&(ot_uart.tx_finished), 1);
	otSysEventSignalPending();
	return result;
}

void platformUartPanic(void)
{
	is_panic_mode = true;
	/* In panic mode data are send without using interrupts.
	 * Reception in this mode is not supported.
	 */
	uart_irq_tx_disable(ot_uart.dev);
	uart_irq_rx_disable(ot_uart.dev);
}
