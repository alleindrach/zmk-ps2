/*
 * Copyright (c) 2019 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gpio_ps2

#include <errno.h>
#include <device.h>
#include <drivers/ps2.h>
#include <drivers/gpio.h>

// #if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#include <logging/log.h>

#define LOG_LEVEL CONFIG_PS2_LOG_LEVEL
LOG_MODULE_REGISTER(ps2_gpio);

// Settings
#define PS2_GPIO_ENABLE_POST_WRITE_LOG false
#define PS2_GPIO_INTERRUPT_LOG_WRITE_ENABLE false

// Timeout for blocking read using the zephyr PS2 ps2_read() function
#define PS2_GPIO_TIMEOUT_READ K_SECONDS(2)

// Timeout for blocking write using the zephyr PS2 ps2_write() function
#define PS2_GPIO_TIMEOUT_WRITE K_MSEC(1000)

// Max time we allow the device to send the next clock signal during reads
// and writes.
//
// PS2 uses a frequency between 10 kHz and 16.7 kHz. So clocks should arrive
// within 60-100us.
#define PS2_GPIO_TIMEOUT_READ_SCL K_USEC(100)
#define PS2_GPIO_TIMEOUT_WRITE_SCL K_USEC(100)

// But after inhibiting the clock line, sometimes clocks take a little longer
// to start. So we allow a bit more time for the first write clock.
#define PS2_GPIO_TIMEOUT_WRITE_SCL_START K_USEC(1000)

#define PS2_GPIO_WRITE_INHIBIT_SLC_DURATION K_USEC(300)

#define PS2_GPIO_POS_START 0
// 1-8 are the data bits
#define PS2_GPIO_POS_PARITY 9
#define PS2_GPIO_POS_STOP 10
#define PS2_GPIO_POS_ACK 11  // Write mode only

#define PS2_GPIO_GET_BIT(data, bit_pos) ( (data >> bit_pos) & 0x1 )
#define PS2_GPIO_SET_BIT(data, bit_val, bit_pos) ( data |= (bit_val) << bit_pos )

int ps2_gpio_write_byte_async(uint8_t byte);

typedef enum
{
    PS2_GPIO_MODE_READ,
    PS2_GPIO_MODE_WRITE
} ps2_gpio_mode;

// Used to keep track of blocking write status
typedef enum
{
    PS2_GPIO_WRITE_STATUS_INACTIVE,
    PS2_GPIO_WRITE_STATUS_ACTIVE,
	PS2_GPIO_WRITE_STATUS_SUCCESS,
	PS2_GPIO_WRITE_STATUS_FAILURE,
} ps2_gpio_write_status;

struct ps2_gpio_config {
	const char *scl_gpio_name;
	gpio_pin_t scl_pin;
	gpio_dt_flags_t scl_flags;

	const char *sda_gpio_name;
	gpio_pin_t sda_pin;
	gpio_dt_flags_t sda_flags;
};

struct ps2_gpio_data {
	const struct device *scl_gpio;	/* GPIO used for PS2 SCL line */
	const struct device *sda_gpio;	/* GPIO used for PS2 SDA line */

	struct gpio_callback scl_cb_data;

	ps2_callback_t callback_isr;
	bool callback_enabled;
	struct k_fifo data_queue;

	ps2_gpio_mode mode;

	uint8_t cur_read_byte;
	int cur_read_pos;
	struct k_work_delayable read_scl_timout;

	uint16_t write_buffer;
	uint8_t cur_write_byte;
	int cur_write_pos;
	ps2_gpio_write_status cur_write_status;
	struct k_sem write_lock;
	struct k_work_delayable write_inhibition_wait;
	struct k_work_delayable write_scl_timout;

	bool dbg_post_write_log;
	int dbg_post_write_pos;
	int64_t dbg_last_clock_ticks;
};


static const struct ps2_gpio_config ps2_gpio_config = {
    .scl_gpio_name = DT_INST_GPIO_LABEL(0, scl_gpios),
    .scl_pin = DT_INST_GPIO_PIN(0, scl_gpios),
    .scl_flags = DT_INST_GPIO_FLAGS(0, scl_gpios),

    .sda_gpio_name = DT_INST_GPIO_LABEL(0, sda_gpios),
    .sda_pin = DT_INST_GPIO_PIN(0, sda_gpios),
    .sda_flags = DT_INST_GPIO_FLAGS(0, sda_gpios),
};

static struct ps2_gpio_data ps2_gpio_data = {
	.scl_gpio = NULL,
	.sda_gpio = NULL,

    .callback_isr = NULL,
	.callback_enabled = false,
	.mode = PS2_GPIO_MODE_READ,

	.cur_read_byte = 0x0,
	.cur_read_pos = 0,

	.write_buffer = 0x0,
	.cur_write_pos = 0,
	.cur_write_status = PS2_GPIO_WRITE_STATUS_INACTIVE,

	.dbg_post_write_log = false,
	.dbg_post_write_pos = 0,
	.dbg_last_clock_ticks = 0,
};


/*
 * Helpers functions
 */

void ps2_gpio_interrupt_log_add(char *format, ...);

int ps2_gpio_get_scl()
{
	const struct ps2_gpio_data *data = &ps2_gpio_data;
	const struct ps2_gpio_config *config = &ps2_gpio_config;
	int rc = gpio_pin_get(data->scl_gpio, config->scl_pin);

	return rc;
}

int ps2_gpio_get_sda()
{
	const struct ps2_gpio_data *data = &ps2_gpio_data;
	const struct ps2_gpio_config *config = &ps2_gpio_config;
	int rc = gpio_pin_get(data->sda_gpio, config->sda_pin);

	return rc;
}

void ps2_gpio_set_scl(int state)
{
	const struct ps2_gpio_data *data = &ps2_gpio_data;
	const struct ps2_gpio_config *config = &ps2_gpio_config;

	// LOG_INF("Setting scl to %d", state);
	gpio_pin_set(data->scl_gpio, config->scl_pin, state);
}

void ps2_gpio_set_sda(int state)
{
	const struct ps2_gpio_data *data = &ps2_gpio_data;
	const struct ps2_gpio_config *config = &ps2_gpio_config;

	// LOG_INF("Seting sda to %d", state);
	gpio_pin_set(data->sda_gpio, config->sda_pin, state);
}

int ps2_gpio_set_scl_callback_enabled(bool enabled)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;
	int err;

	if(enabled) {
		err = gpio_add_callback(data->scl_gpio, &data->scl_cb_data);
		if (err) {
			LOG_ERR(
				"failed to enable interrupt callback on "
				"SCL GPIO pin (err %d)", err
			);
		}
	} else {
		err = gpio_remove_callback(data->scl_gpio, &data->scl_cb_data);
		if (err) {
			LOG_ERR(
				"failed to disable interrupt callback on "
				"SCL GPIO pin (err %d)", err
			);
		}
	}

	return err;
}


int ps2_gpio_configure_pin_scl(gpio_flags_t flags, char *descr)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;
	const struct ps2_gpio_config *config = &ps2_gpio_config;
	int err;

	err = gpio_pin_configure(
		data->scl_gpio,
		config->scl_pin,
		flags
	);
	if (err) {
		LOG_ERR("failed to configure SCL GPIO pin to %s (err %d)", descr, err);
	}

	return err;
}

int ps2_gpio_configure_pin_scl_input()
{
	return ps2_gpio_configure_pin_scl(
		(GPIO_INPUT),
		"input"
	);
}

int ps2_gpio_configure_pin_scl_output()
{
	return ps2_gpio_configure_pin_scl(
		(GPIO_OUTPUT),
		"output"
	);
}

int ps2_gpio_configure_pin_sda(gpio_flags_t flags, char *descr)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;
	const struct ps2_gpio_config *config = &ps2_gpio_config;
	int err;

	err = gpio_pin_configure(
		data->sda_gpio,
		config->sda_pin,
		flags
	);
	if (err) {
		LOG_ERR("failed to configure SDA GPIO pin to %s (err %d)", descr, err);
	}

	return err;
}

int ps2_gpio_configure_pin_sda_input()
{
	return ps2_gpio_configure_pin_sda(
		(GPIO_INPUT),
		"input"
	);
}

int ps2_gpio_configure_pin_sda_output()
{
	return ps2_gpio_configure_pin_sda(
		(GPIO_OUTPUT),
		"output"
	);
}

void ps2_gpio_send_cmd_resend()
{
	uint8_t cmd = 0xfe;
	LOG_INF("Requesting resend of data with command: 0x%x", cmd);
	ps2_gpio_write_byte_async(cmd);
}

uint8_t ps2_gpio_data_queue_get_next(uint8_t *dst_byte, k_timeout_t timeout)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	uint8_t *queue_byte = k_fifo_get(&data->data_queue, timeout);
	if(queue_byte == NULL) {
		return -ETIMEDOUT;
	}

	*dst_byte =  *queue_byte;

	k_free(queue_byte);

	return 0;
}

void ps2_gpio_data_queue_empty()
{
	while(true) {
		uint8_t byte;
		int err = ps2_gpio_data_queue_get_next(&byte, K_NO_WAIT);
		if(err) {  // No more items in queue
			break;
		}
	}
}

void ps2_gpio_data_queue_add(uint8_t byte)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	uint8_t *byte_heap = (uint8_t *) k_malloc(sizeof(byte));
	if(byte_heap == NULL) {
		LOG_WRN(
			"Could not allocate heap space to add byte to fifo. "
			"Clearing fifo."
		);

		// TODO: Define max amount for read data queue instead of emptying it
		// when memory runs out.
		// But unfortunately it seems like there is no official way to query
		// how many items are currently in the fifo.
		ps2_gpio_data_queue_empty();

		byte_heap = (uint8_t *) k_malloc(sizeof(byte));
		if(byte_heap == NULL) {
			LOG_ERR(
				"Could not allocate heap space after clearing fifo. "
				"Losing received byte 0x%x", byte
			);
			return;
		}
	}

	*byte_heap = byte;
	k_fifo_alloc_put(&data->data_queue, byte_heap);
}


/*
 * Logging Helpers
 */

char *ps2_gpio_get_mode_str() {
	struct ps2_gpio_data *data = &ps2_gpio_data;

	if(data->mode == PS2_GPIO_MODE_READ) {
		return "r";
	} else {
		return "w";
	}
}

char *ps2_gpio_get_pos_str() {
	struct ps2_gpio_data *data = &ps2_gpio_data;

	int pos;
	if(data->mode == PS2_GPIO_MODE_READ) {
		pos = data->cur_read_pos;
	} else {
		pos = data->cur_write_pos;
	}

	char *pos_names[] = {
		"start",
		"data_1",
		"data_2",
		"data_3",
		"data_4",
		"data_5",
		"data_6",
		"data_7",
		"data_8",
		"parity",
		"stop",
		"ack",
	};

	if(pos >= (sizeof(pos_names) / sizeof(pos_names[0]))) {
		return "unk";
	} else {
		return pos_names[pos];
	}
}

/*
 * Reading PS/2 data
 */

void ps2_gpio_read_scl_timeout(struct k_work *item);
void ps2_gpio_abort_read(bool should_resend);
void ps2_gpio_process_received_byte(uint8_t byte);
void ps2_gpio_read_finish();
bool ps2_gpio_check_parity(uint8_t byte, int parity_bit_val);

// Reading doesn't need to be initiated. It happens automatically whenever
// the device sends data.
// Once a full byte has been received successfully it is processed in
// ps2_gpio_process_received_byte, which decides what should happen with it.
void ps2_gpio_scl_interrupt_handler_read()
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	k_work_cancel_delayable(&data->read_scl_timout);

	ps2_gpio_interrupt_log_add("Read interrupt");

	int sda_val = ps2_gpio_get_sda();

	if(data->cur_read_pos == PS2_GPIO_POS_START) {
		// The first bit of every transmission should be 0.
		// If it is not, it means we are out of sync with the device.
		// So we abort the transmission and start from scratch.
		if(sda_val != 0) {
			ps2_gpio_interrupt_log_add(
				"Ignoring read interrupt due to invalid start bit."
			);

			// We don't request a resend here, because sometimes after writes
			// devices send some unintended interrupts. If this is a "real
			// transmission" and we are out of sync, we will catch it with the
			// parity and stop bits and then request a resend.
			ps2_gpio_abort_read(false);
			return;
		}
	} else if(data->cur_read_pos == PS2_GPIO_POS_PARITY) {
		if(ps2_gpio_check_parity(data->cur_read_byte, sda_val) != true) {
			ps2_gpio_interrupt_log_add(
				"Requesting re-send due to invalid parity bit."
			);

			// If we got to the parity bit and it's incorrect then we
			// are definitly in a transmission and out of sync. So we
			// request a resend.
			ps2_gpio_abort_read(true);
			return;
		}
	} else if(data->cur_read_pos == PS2_GPIO_POS_STOP) {
		if(sda_val != 1) {
			ps2_gpio_interrupt_log_add(
				"Requesting re-send due to invalid stop bit."
			);

			// If we got to the stop bit and it's incorrect then we
			// are definitly in a transmission and out of sync. So we
			// request a resend.
			ps2_gpio_abort_read(true);
			return;
		}

		ps2_gpio_process_received_byte(data->cur_read_byte);

		return;
	} else { // Data Bits

		// Current position, minus start bit
		int bit_pos = data->cur_read_pos - 1;
		PS2_GPIO_SET_BIT(data->cur_read_byte, sda_val, bit_pos);
	}

 	data->cur_read_pos += 1;
	k_work_schedule(&data->read_scl_timout, PS2_GPIO_TIMEOUT_READ_SCL);
}

void ps2_gpio_read_scl_timeout(struct k_work *item)
{
	// Once we are receiving a transmission we expect the device to
	// to send a new clock/interrupt within 100us.
	// If we don't receive the next interrupt within that timeframe,
	// we abort the read.
	struct ps2_gpio_data *data = CONTAINER_OF(
		item,
		struct ps2_gpio_data,
		read_scl_timout
	);

	ps2_gpio_interrupt_log_add("Read SCL timeout");

	// We don't request a resend if the timeout happens in the early
	// stage of the transmission.
	//
	// Because, sometimes after writes devices send some unintended
	// interrupts and start the "real response" after one or two cycles.
	//
	// If we are really out of sync the parity and stop bits should catch
	// it and request a re-transmission.
	if(data->cur_read_pos <= 3) {
		ps2_gpio_abort_read(false);
	} else {
		ps2_gpio_abort_read(true);
	}
}

void ps2_gpio_abort_read(bool should_resend)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	if(should_resend == true) {
		ps2_gpio_interrupt_log_add("Aborting read with resend request.");
	} else {
		ps2_gpio_interrupt_log_add("Aborting read without resend request.");
	}

	ps2_gpio_read_finish();

	k_work_cancel_delayable(&data->read_scl_timout);

	if(should_resend == true) {
		ps2_gpio_send_cmd_resend();
	}
}

void ps2_gpio_process_received_byte(uint8_t byte)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	ps2_gpio_interrupt_log_add("Successfully received value: 0x%x", byte);

	ps2_gpio_read_finish();

	// If no callback is set, we add the data to a fifo queue
	// that can be read later with the read using `ps2_read`
	if(data->callback_isr != NULL && data->callback_enabled) {

		data->callback_isr(NULL, byte);
	} else {
		ps2_gpio_data_queue_add(byte);
	}
}

void ps2_gpio_read_finish()
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	data->cur_read_pos = PS2_GPIO_POS_START;
	data->cur_read_byte = 0x0;

	k_work_cancel_delayable(&data->read_scl_timout);
}

bool ps2_gpio_check_parity(uint8_t byte, int parity_bit_val)
{
	int byte_parity = __builtin_parity(byte);

	// gcc parity returns 1 if there is an odd number of bits in byte
	// But the PS2 protocol sets the parity bit to 0 if there is an odd number
	if(byte_parity == parity_bit_val) {
		return 0;  // Do not match
	}

	return 1;  // Match
}


/*
 * Writing PS2 data
 */

int ps2_gpio_write_byte_blocking(uint8_t byte);
int ps2_gpio_write_byte_async(uint8_t byte);
void ps2_gpio_write_inhibition_wait(struct k_work *item);
void ps2_gpio_scl_interrupt_handler_write_send_bit();
void ps2_gpio_scl_interrupt_handler_write_check_ack();
void ps2_gpio_finish_write(bool successful);
void ps2_gpio_write_scl_timeout(struct k_work *item);
bool ps2_gpio_get_byte_parity(uint8_t byte);

void ps2_gpio_interrupt_log_write_start(uint8_t byte);


int ps2_gpio_write_byte_blocking(uint8_t byte)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;
	int err;

	LOG_INF("ps2_gpio_write_byte_blocking called with byte=0x%x", byte);

	err = ps2_gpio_write_byte_async(byte);
    if (err) {
		LOG_ERR("Could not initiate writing of byte.");
		return err;
	}

	// The async `write_byte_async` function takes the only available semaphor.
	// This causes the `k_sem_take` call below to block until
	// `ps2_gpio_scl_interrupt_handler_write_check_ack` gives it back.
	err = k_sem_take(&data->write_lock, PS2_GPIO_TIMEOUT_WRITE);
    if (err) {
		LOG_ERR("Blocking write failed due to semaphore timeout: %d", err);
		return err;
	}

	if(data->cur_write_status == PS2_GPIO_WRITE_STATUS_SUCCESS) {
		LOG_INF("Blocking write finished successfully for byte 0x%x", byte);
		err = 0;
	} else {
		LOG_ERR(
			"Blocking write finished with failure for byte 0x%x status: %d",
			byte, data->cur_write_status
		);
		err = -data->cur_write_status;
	}

	data->cur_write_status = PS2_GPIO_WRITE_STATUS_INACTIVE;

	return err;
}

int ps2_gpio_write_byte_async(uint8_t byte) {
	struct ps2_gpio_data *data = &ps2_gpio_data;
	int err;

	LOG_INF("ps2_gpio_write_byte_async called with byte=0x%x", byte);

	if(data->mode == PS2_GPIO_MODE_WRITE) {
		LOG_ERR(
			"Preventing write off byte 0x%x: "
			"Another write in progress for 0x%x",
			byte, data->cur_write_byte
		);

		return -EBUSY;
	}

	// Take semaphore so that when `ps2_gpio_write_byte_blocking` attempts
	// taking it, the process gets blocked.
	// It is released in `ps2_gpio_scl_interrupt_handler_write_check_ack`.
	err = k_sem_take(&data->write_lock, K_NO_WAIT);
    if (err != 0 && err != -EBUSY) {
		LOG_ERR("ps2_gpio_write_byte_async could not take semaphore: %d", err);

		return err;
	}

	// Change mode and set write_pos so that the read interrupt handler
	// doesn't trigger when we bring the clock line low.
	data->mode = PS2_GPIO_MODE_WRITE;
	data->cur_write_pos = PS2_GPIO_POS_START;
	data->cur_write_byte = byte;

	// Initiating a send aborts any in-progress reads, so we
	// reset the current read byte
	data->cur_write_status = PS2_GPIO_WRITE_STATUS_ACTIVE;
	if(data->cur_read_pos != PS2_GPIO_POS_START ||
	   data->cur_read_byte != 0x0)
	{
		LOG_WRN("Aborting in-progress read due to write of byte 0x%x", byte);
		ps2_gpio_abort_read(false);
	}

	// Configure data and clock lines for output
	ps2_gpio_configure_pin_scl_output();
	ps2_gpio_configure_pin_sda_output();

	ps2_gpio_interrupt_log_add("Starting write of byte 0x%x", byte);

	// Disable interrupt so that we don't trigger it when we
	// pull the clock low to inhibit the line
	ps2_gpio_set_scl_callback_enabled(false);

	// Inhibit the line by setting clock low and data high
	// LOG_INF("Pulling clock line low to start write process.");
	ps2_gpio_set_scl(0);
	ps2_gpio_set_sda(1);

	ps2_gpio_interrupt_log_add("Inhibited clock line");

	// Keep the line inhibited for at least 100 microseconds
	k_work_schedule(
		&data->write_inhibition_wait,
		PS2_GPIO_WRITE_INHIBIT_SLC_DURATION
	);

	// The code continues in ps2_gpio_write_inhibition_wait
	return 0;
}

void ps2_gpio_write_inhibition_wait(struct k_work *item)
{
	ps2_gpio_interrupt_log_add("Inhibition timer finished");

	struct ps2_gpio_data *data = CONTAINER_OF(
		item,
		struct ps2_gpio_data,
		write_inhibition_wait
	);

	// Enable the scl interrupt again
	ps2_gpio_set_scl_callback_enabled(true);

	// Set data to value of start bit
	ps2_gpio_set_sda(0);

	ps2_gpio_interrupt_log_add("Set sda to start bit");

	// The start bit was sent by setting sda to low
	// So the next scl interrupt will be for the first
	// data bit.
	data->cur_write_pos += 1;

	// Release the clock line and configure it as input
	// This let's the device take control of the clock again
	ps2_gpio_set_scl(1);
	ps2_gpio_configure_pin_scl_input();

	ps2_gpio_interrupt_log_add("Released clock");

	k_work_schedule(&data->write_scl_timout, PS2_GPIO_TIMEOUT_WRITE_SCL_START);

	// From here on the device takes over the control of the clock again
	// Every time it is ready for the next bit to be trasmitted, it will...
	//  - Pull the clock line low
	//  - Which will trigger our `scl_interrupt_handler_write`
	//  - Which will send the correct bit
	//  - After all bits are sent `scl_interrupt_handler_write_check_ack` is
	//    called, which verifies if the transaction was successful
}

void ps2_gpio_scl_interrupt_handler_write()
{
	// After initiating writing, the device takes over
	// the clock and asks us for a new bit of data on
	// each falling edge.
	struct ps2_gpio_data *data = &ps2_gpio_data;

	if(data->cur_write_pos == PS2_GPIO_POS_START)
	{
		// This should not be happening, because the PS2_GPIO_POS_START bit
		// is sent in ps2_gpio_write_byte_async during inhibition
		ps2_gpio_interrupt_log_add("Write interrupt");
		return;
	}

	k_work_cancel_delayable(&data->write_scl_timout);

	if(data->cur_write_pos > PS2_GPIO_POS_START &&
	   data->cur_write_pos < PS2_GPIO_POS_PARITY)
	{
		// Set it to the data bit corresponding to the current
		// write position (subtract start bit postion)
		ps2_gpio_set_sda(
			PS2_GPIO_GET_BIT(data->cur_write_byte, (data->cur_write_pos - 1))
		);
	} else if(data->cur_write_pos == PS2_GPIO_POS_PARITY)
	{
		ps2_gpio_set_sda(
			ps2_gpio_get_byte_parity(data->cur_write_byte)
		);
	} else if(data->cur_write_pos == PS2_GPIO_POS_STOP)
	{
		// Send the stop bit (always 1)
		ps2_gpio_set_sda(1);

		// Give control over data pin back to device after sending stop bit
		// so that we can receive the ack bit from the device
		ps2_gpio_configure_pin_sda_input();
	} else if(data->cur_write_pos == PS2_GPIO_POS_ACK)
	{
		ps2_gpio_interrupt_log_add("Write interrupt");
		int ack_val = ps2_gpio_get_sda();

		if(ack_val == 0) {
			ps2_gpio_interrupt_log_add("Write was successful with ack: %d", ack_val);
			ps2_gpio_finish_write(true);
		} else {
			ps2_gpio_interrupt_log_add("Write failed with ack: %d", ack_val);
			ps2_gpio_finish_write(false);
		}

		return;
	} else {
		ps2_gpio_interrupt_log_add(
			"Invalid write clock triggered with pos=%d", data->cur_write_pos
		);

		return;
	}

	ps2_gpio_interrupt_log_add("Write interrupt");

	data->cur_write_pos += 1;
	k_work_schedule(&data->write_scl_timout, PS2_GPIO_TIMEOUT_WRITE_SCL);
}

void ps2_gpio_write_scl_timeout(struct k_work *item)
{
	ps2_gpio_interrupt_log_add("Write SCL timeout");
	ps2_gpio_finish_write(false);
}

void ps2_gpio_finish_write(bool successful)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	k_work_cancel_delayable(&data->write_scl_timout);

	if(successful) {
		ps2_gpio_interrupt_log_add(
			"Write was successful for value 0x%x",
			data->cur_write_byte
		);
		data->cur_write_status = PS2_GPIO_WRITE_STATUS_SUCCESS;
	} else {
		ps2_gpio_interrupt_log_add(
			"Aborting write of value 0x%x at pos=%d",
			data->cur_write_byte, data->cur_write_pos
		);
	 	data->cur_write_status = PS2_GPIO_WRITE_STATUS_FAILURE;
	}

	data->mode = PS2_GPIO_MODE_READ;
	data->cur_read_pos = PS2_GPIO_POS_START;
	data->cur_write_pos = PS2_GPIO_POS_START;
	data->cur_write_byte = 0x0;

	// Give back control over data and clock line if we still hold on to it
	ps2_gpio_configure_pin_sda_input();
	ps2_gpio_configure_pin_scl_input();

	// Give the semaphore to allow write_byte_blocking to continue
	k_sem_give(&data->write_lock);
}

bool ps2_gpio_get_byte_parity(uint8_t byte)
{
	int byte_parity = __builtin_parity(byte);

	// gcc parity returns 1 if there is an odd number of bits in byte
	// But the PS2 protocol sets the parity bit to 0 if there is an odd number
	return !byte_parity;
}


/*
 * Interrupt logging
 */

#define PS2_GPIO_INTERRUPT_LOG_SCL_TIMEOUT K_SECONDS(1)
#define PS2_GPIO_INTERRUPT_LOG_MAX_ITEMS 1000

struct interrupt_log {
	int64_t uptime_ticks;
	char msg[50];
	int scl;
	int sda;
	ps2_gpio_mode mode;
	int pos;
};

int interrupt_log_offset = 0;
int interrupt_log_idx = 0;
struct interrupt_log interrupt_log[PS2_GPIO_INTERRUPT_LOG_MAX_ITEMS];

void ps2_gpio_interrupt_log_add(char *format, ...);
void ps2_gpio_interrupt_log_print();
void ps2_gpio_interrupt_log_clear();

void ps2_gpio_interrupt_log_add(char *format, ...)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;
	struct interrupt_log l;

	l.uptime_ticks = k_uptime_ticks();

	va_list arglist;
    va_start(arglist, format);
    vsnprintf(l.msg, sizeof(l.msg) - 1, format, arglist);
    va_end(arglist);

	l.scl = ps2_gpio_get_scl();
	l.sda = ps2_gpio_get_sda();
	l.mode = data->mode;
	if(data->mode == PS2_GPIO_MODE_READ) {
		l.pos = data->cur_read_pos;
	} else {
		l.pos = data->cur_write_pos;
	}

	if(interrupt_log_idx >= (PS2_GPIO_INTERRUPT_LOG_MAX_ITEMS)) {
		ps2_gpio_interrupt_log_print();
		ps2_gpio_interrupt_log_clear();
		return;
	}

	interrupt_log[interrupt_log_idx] = l;

	interrupt_log_idx += 1;
}

void ps2_gpio_interrupt_log_clear()
{
	memset(&interrupt_log,	0x0, sizeof(interrupt_log));
	interrupt_log_offset = interrupt_log_idx;
	interrupt_log_idx = 0;
}

void ps2_gpio_interrupt_log_get_pos_str(int pos,
										char *pos_str,
										int pos_str_size)
{
	char *pos_names[] = {
		"start",
		"data_1",
		"data_2",
		"data_3",
		"data_4",
		"data_5",
		"data_6",
		"data_7",
		"data_8",
		"parity",
		"stop",
		"ack",
	};

	if(pos >= (sizeof(pos_names) / sizeof(pos_names[0]))) {
		snprintf(pos_str, pos_str_size - 1, "%d", pos);
	} else {
		strncpy(pos_str, pos_names[pos], pos_str_size - 1);
	}
}

char *ps2_gpio_interrupt_log_get_mode_str() {
	struct ps2_gpio_data *data = &ps2_gpio_data;

	if(data->mode == PS2_GPIO_MODE_READ) {
		return "r";
	} else if(data->mode == PS2_GPIO_MODE_WRITE) {
		return "w";
	} else {
		return "?";
	}
}

void ps2_gpio_interrupt_log_print()
{

	LOG_INF("===== Interrupt Log =====");
	for(int i = 0; i < interrupt_log_idx; i++) {
		struct interrupt_log *l = &interrupt_log[i];
		char pos_str[50];

		ps2_gpio_interrupt_log_get_pos_str(l->pos, pos_str, sizeof(pos_str));

		LOG_INF(
			"%d - %" PRIu64 ": %s "
			"(mode=%s, pos=%s, scl=%d, sda=%d)" ,
			interrupt_log_offset + i + 1, l->uptime_ticks, l->msg,
			 ps2_gpio_interrupt_log_get_mode_str(), pos_str, l->scl, l->sda
		);
		k_sleep(K_MSEC(10));
	}
	LOG_INF("======== End Log ========");
}


/*
 * Write Logging
 */

struct k_work_delayable interrupt_log_write_inhibition_wait;
struct k_work_delayable interrupt_log_write_scl_timout;

void ps2_gpio_interrupt_log_write_start(uint8_t byte)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;
	data->cur_write_byte = byte;

	ps2_gpio_interrupt_log_add("start write 0x%x", byte);


	// Configure data and clock lines for output
	ps2_gpio_configure_pin_scl_output();
	ps2_gpio_configure_pin_sda_output();

	// Disable scl interrupt so that we don't trigger it when we
	// pull the clock low to inhibit the line
	ps2_gpio_set_scl_callback_enabled(false);

	// Inhibit the line by setting clock low and data high
	// LOG_INF("Pulling clock line low to start write process.");
	ps2_gpio_set_sda(1);
	ps2_gpio_set_scl(0);

	// Enable the scl interrupt again
	ps2_gpio_set_scl_callback_enabled(true);

	ps2_gpio_interrupt_log_add("inhibit line");

	// Keep the line inhibited for at least 100 microseconds
	k_work_schedule(
		&interrupt_log_write_inhibition_wait,
		PS2_GPIO_WRITE_INHIBIT_SLC_DURATION
	);
}

void ps2_gpio_interrupt_log_write_inhibition_wait(struct k_work *item)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	ps2_gpio_interrupt_log_add("inhibit timer done");

	// Set data to value of start bit
	ps2_gpio_set_sda(0);

	// The start bit was sent by setting sda to low
	data->cur_write_pos += 1;

	// Release the clock line and configure it as input
	// This let's the device take control of the clock again
	ps2_gpio_set_scl(1);
	ps2_gpio_configure_pin_scl_input();

	ps2_gpio_interrupt_log_add("inhibit release clock");

	k_work_schedule(
		&interrupt_log_write_scl_timout,
		PS2_GPIO_INTERRUPT_LOG_SCL_TIMEOUT
	);
}


void ps2_gpio_interrupt_log_write_interrupt_handler()
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	k_work_cancel_delayable(&interrupt_log_write_scl_timout);

	if(data->cur_write_pos == PS2_GPIO_POS_START) {
		// Do nothing
	} else if(data->cur_write_pos > PS2_GPIO_POS_START &&
	   data->cur_write_pos < PS2_GPIO_POS_PARITY)
	{

		ps2_gpio_set_sda(
			PS2_GPIO_GET_BIT(data->cur_write_byte, (data->cur_write_pos - 1))
		);
	} else if(data->cur_write_pos == PS2_GPIO_POS_PARITY)
	{
		ps2_gpio_set_sda(ps2_gpio_get_byte_parity(data->cur_write_byte));
	} else if(data->cur_write_pos == PS2_GPIO_POS_STOP)
	{
		// Send the stop bit
		ps2_gpio_set_sda(1);

		// Give control over data pin back to device after sending stop bit
		// so that we can receive the ack bit from the device
		ps2_gpio_configure_pin_sda_input();
	} else if(data->cur_write_pos == PS2_GPIO_POS_ACK)
	{
		int sda_val = ps2_gpio_get_sda();
		if(sda_val == 0) {
			LOG_INF("Successful write for 0x%x", data->cur_write_byte);
			ps2_gpio_interrupt_log_add(
				"successful write for 0x%x",
				data->cur_write_byte
			);
		} else {
			LOG_ERR("Failed write for 0x%x", data->cur_write_byte);

			ps2_gpio_interrupt_log_add(
				"failed write for 0x%x",
				data->cur_write_byte
			);
		}
		data->cur_write_pos = 0;
		data->mode = PS2_GPIO_MODE_READ;
		k_work_schedule(
			&interrupt_log_write_scl_timout,
			PS2_GPIO_INTERRUPT_LOG_SCL_TIMEOUT
		);
	} else {
		ps2_gpio_interrupt_log_add("interrupt write invalid pos");
	}

	ps2_gpio_interrupt_log_add("interrupt write");

	data->cur_write_pos += 1;

	k_work_schedule(
		&interrupt_log_write_scl_timout,
		PS2_GPIO_INTERRUPT_LOG_SCL_TIMEOUT
	);
}

void ps2_gpio_interrupt_log_write_scl_timeout(struct k_work *item)
{
	ps2_gpio_interrupt_log_print();
	ps2_gpio_interrupt_log_clear();
}


/*
 * Interrupt Handler
 */

void ps2_gpio_scl_interrupt_handler_log()
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	int scl_val = ps2_gpio_get_scl();
	int sda_val = ps2_gpio_get_sda();

	LOG_INF(
		"ps2_gpio_scl_interrupt_handler_log called with position=%d; scl=%d; sda=%d",
		data->dbg_post_write_pos, scl_val, sda_val
	);

	data->dbg_post_write_pos += 1;
}

void ps2_gpio_scl_interrupt_handler(const struct device *dev,
						   struct gpio_callback *cb,
						   uint32_t pins)
{
	struct ps2_gpio_data *data = &ps2_gpio_data;

	k_work_cancel_delayable(&interrupt_log_write_scl_timout);

	if(data->mode == PS2_GPIO_MODE_READ) {
		ps2_gpio_scl_interrupt_handler_read();
	} else {
		ps2_gpio_scl_interrupt_handler_write();
	}

	k_work_schedule(
		&interrupt_log_write_scl_timout,
		PS2_GPIO_INTERRUPT_LOG_SCL_TIMEOUT
	);
}


/*
 * Zephyr PS/2 driver interface
 */
static int ps2_gpio_enable_callback(const struct device *dev);

static int ps2_gpio_configure(const struct device *dev,
			     ps2_callback_t callback_isr)
{
	LOG_ERR("In ps2_gpio_configure");
	struct ps2_gpio_data *data = dev->data;

	if (!callback_isr) {
		return -EINVAL;
	}

	data->callback_isr = callback_isr;
	ps2_gpio_enable_callback(dev);

	return 0;
}

int ps2_gpio_read(const struct device *dev, uint8_t *value)
{
	// TODO: Add a way to not return old queue items
	// Maybe only bytes that were received within past 10 seconds.
	LOG_INF("In ps2_gpio_read...");

	uint8_t queue_byte;
	int err = ps2_gpio_data_queue_get_next(&queue_byte, PS2_GPIO_TIMEOUT_READ);
	if(err) {  // Timeout due to no data to read in data queue
		LOG_ERR("ps2_gpio_read: Fifo timed out...");

		return -ETIMEDOUT;
	}

	LOG_DBG("ps2_gpio_read: Returning 0x%x", queue_byte);
	*value =  queue_byte;

	return 0;
}

static int ps2_gpio_write(const struct device *dev, uint8_t value)
{
	return ps2_gpio_write_byte_blocking(value);
}

static int ps2_gpio_disable_callback(const struct device *dev)
{
	struct ps2_gpio_data *data = dev->data;

	// Make sure there are no stale items in the data queue
	// from before the callback was disabled.
	ps2_gpio_data_queue_empty();

	data->callback_enabled = false;

	LOG_INF("Disabled PS2 callback.");

	return 0;
}

static int ps2_gpio_enable_callback(const struct device *dev)
{
	struct ps2_gpio_data *data = dev->data;
	data->callback_enabled = true;

	LOG_INF("Enabled PS2 callback.");

	ps2_gpio_data_queue_empty();

	return 0;
}

static const struct ps2_driver_api ps2_gpio_driver_api = {
	.config = ps2_gpio_configure,
	.read = ps2_gpio_read,
	.write = ps2_gpio_write,
	.disable_callback = ps2_gpio_disable_callback,
	.enable_callback = ps2_gpio_enable_callback,
};

/*
 * PS/2 GPIO Driver Init
 */

int ps2_gpio_configure_scl_pin(struct ps2_gpio_data *data,
							   const struct ps2_gpio_config *config)
{
	int err;

	// Configure PIN
	data->scl_gpio = device_get_binding(config->scl_gpio_name);
	if (!data->scl_gpio) {
		LOG_ERR("failed to get SCL GPIO device");
		return -EINVAL;
	}

	ps2_gpio_configure_pin_scl_input();

	// Interrupt for clock line
	// Almost all actions happen on the falling edge, but at the end of a write
	// the device sends an ack bit on the rising edge. Setting up both edges
	// allows us to detect it.
	err = gpio_pin_interrupt_configure(
		data->scl_gpio,
		config->scl_pin,
		(GPIO_INT_EDGE_FALLING)
	);
	if (err) {
		LOG_ERR(
			"failed to configure interrupt on "
			"SCL GPIO pin (err %d)", err
		);
		return err;
	}

	gpio_init_callback(
		&data->scl_cb_data,
		ps2_gpio_scl_interrupt_handler,
		BIT(config->scl_pin)
	);

	ps2_gpio_set_scl_callback_enabled(true);

	return 0;
}

int ps2_gpio_configure_sda_pin(struct ps2_gpio_data *data,
							   const struct ps2_gpio_config *config)
{
	data->sda_gpio = device_get_binding(config->sda_gpio_name);
	if (!data->sda_gpio) {
		LOG_ERR("failed to get SDA GPIO device");
		return -EINVAL;
	}

	ps2_gpio_configure_pin_sda_input();

	return 0;
}

static int ps2_gpio_init(const struct device *dev)
{
	LOG_INF("Inside ps2_gpio_init");

	struct ps2_gpio_data *data = dev->data;
	const struct ps2_gpio_config *config = dev->config;
	int err;

	err = ps2_gpio_configure_scl_pin(data, config);
	if (err) {
		return err;
	}
	err = ps2_gpio_configure_sda_pin(data, config);
	if (err) {
		return err;
	}

	// Check if this stuff is needed
	// TODO: Figure out why this is requiered.
	ps2_gpio_set_sda(1);
	ps2_gpio_set_scl(1);

	LOG_INF("Finished configuring ps2_gpio.");

	// Init fifo for synchronous read operations
	k_fifo_init(&data->data_queue);

	// Init semaphore for blocking writes
	k_sem_init(&data->write_lock, 0, 1);

	// Timeouts for clock pulses during read and write
    k_work_init_delayable(&data->read_scl_timout, ps2_gpio_read_scl_timeout);
    k_work_init_delayable(&data->write_scl_timout, ps2_gpio_write_scl_timeout);
    k_work_init_delayable(&data->write_inhibition_wait, ps2_gpio_write_inhibition_wait);

    k_work_init_delayable(&interrupt_log_write_inhibition_wait, ps2_gpio_interrupt_log_write_inhibition_wait);
    k_work_init_delayable(&interrupt_log_write_scl_timout, ps2_gpio_interrupt_log_write_scl_timeout);

	return 0;
}

DEVICE_DT_INST_DEFINE(
	0,
	&ps2_gpio_init,
	NULL,
	&ps2_gpio_data, &ps2_gpio_config,
	POST_KERNEL, CONFIG_PS2_INIT_PRIORITY,
	&ps2_gpio_driver_api
);

// #endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
