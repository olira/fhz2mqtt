/*
 * fhz2mqtt, a FHZ to MQTT bridge
 *
 * Copyright (c) Ralf Ramsauer, 2018
 *
 * Authors:
 *  Ralf Ramsauer <ralf@ramses-pyramidenbau.de>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#include "fhz.h"

#define FHT_YEAR_BASE 2000

#define FHT_TEMP_OFF 5.5
#define FHT_TEMP_ON 30.5

#define FHT_IS_VALVE 0x00
#define FHT_VALVE_1 0x01
#define FHT_VALVE_2 0x02
#define FHT_VALVE_3 0x03
#define FHT_VALVE_4 0x04
#define FHT_VALVE_5 0x05
#define FHT_VALVE_6 0x06
#define FHT_VALVE_7 0x07
#define FHT_VALVE_8 0x08
#define FHT_MODE 0x3e
#define  FHT_MODE_AUTO 0
#define  FHT_MODE_MANU 1
#define  FHT_MODE_HOLI 2
#define FHT_DESIRED_TEMP 0x41
#define FHT_IS_TEMP_LOW 0x42
#define FHT_IS_TEMP_HIGH 0x43
#define FHT_STATUS 0x44
#define FHT_MANU_TEMP 0x45
#define FHT_ACK 0x4b
#define FHT_YEAR 0x60
#define FHT_MONTH 0x61
#define FHT_DAY 0x62
#define FHT_HOUR 0x63
#define FHT_MINUTE 0x64
#define FHT_ACK2 0x69
#define FHT_START_XMIT 0x7d
#define FHT_END_XMIT 0x7e
#define FHT_DAY_TEMP 0x82
#define FHT_NIGHT_TEMP 0x84
#define FHT_WINDOW_OPEN_TEMP 0x8a

#define __report_printf(__message, __no, __field, ...) \
	snprintf(__message->report[__no].__field, \
		 sizeof(__message->report[__no].__field), \
		 __VA_ARGS__)

#define report_printf_topic(__message, __no, ...) \
	__report_printf(__message, __no, topic, __VA_ARGS__)

#define report_printf_value(__message, __no, ...) \
	__report_printf(__message, __no, value, __VA_ARGS__)

struct fht_message_raw {
	unsigned char cmd;
	unsigned char subfun;
	unsigned char status;
	unsigned char value;
};

struct fht_command {
	unsigned char function_id;
	const char *name;
	int (*input_conversion)(const char *payload);
	int (*output_conversion)(struct fht_message *message,
				 const struct fht_message_raw *raw);
};

#define for_each_fht_command(commands, command, counter) \
	for((counter) = 0, (command) = (commands); \
	    (counter) < ARRAY_SIZE((commands)); \
	    (counter)++, (command)++)

static const char s_mode_auto[] = "auto";
static const char s_mode_holiday[] = "holiday";
static const char s_mode_manual[] = "manual";

static unsigned char temp_low;

static int payload_to_fht_temp(const char *payload)
{
	float temp;

	if (!strcasecmp(payload, "off")) {
		temp = FHT_TEMP_OFF;
		goto temp_out;
	} else if (!strcasecmp(payload, "on")) {
		temp = FHT_TEMP_ON;
		goto temp_out;
	} else if (sscanf(payload, "%f", &temp) != 1)
		return -EINVAL;

	if (temp < FHT_TEMP_OFF || temp > FHT_TEMP_ON)
		return -ERANGE;

temp_out:
	return (unsigned char)(temp/0.5);
}

static int fht_temp_to_str(struct fht_message *message,
			   const struct fht_message_raw *raw)
{
	report_printf_value(message, 0, "%0.1f", (float)raw->value * 0.5);
	return 0;
}

static int payload_to_mode(const char *payload)
{
	if (!strcasecmp(payload, s_mode_auto))
		return FHT_MODE_AUTO;
	if (!strcasecmp(payload, s_mode_manual))
		return FHT_MODE_MANU;
	if (!strcasecmp(payload, s_mode_manual))
		return FHT_MODE_HOLI;

	return -EINVAL;
}

static int mode_to_str(struct fht_message *message,
		       const struct fht_message_raw *raw)
{
	const char *src;
	int err = 0;

	switch (raw->value) {
	case FHT_MODE_AUTO:
		src = s_mode_auto;
		break;
	case FHT_MODE_MANU:
		src = s_mode_manual;
		break;
	case FHT_MODE_HOLI:
		src = s_mode_holiday;
		break;
	default:
		src = "unknown";
		err = -EINVAL;
		break;
	}

	report_printf_value(message, 0, src);

	return err;
}

static int input_not_accepted(const char *payload)
{
	return -EPERM;
}

static int fht_is_temp_low(struct fht_message *message,
			   const struct fht_message_raw *raw)
{
	temp_low = raw->value;
	return -EAGAIN;
}

static int fht_is_temp_high_to_str(struct fht_message *message,
				   const struct fht_message_raw *raw)
{
	report_printf_value(message, 0, "%0.2f",
			    ((float)temp_low + (float)raw->value* 256)/10.0);
	return 0;
}

static int payload_to_fht_year(const char *payload)
{
	unsigned int year;

	if (sscanf(payload, "%u", &year) != 1)
		return -EINVAL;

	return year - FHT_YEAR_BASE;
}

static int fht_year_to_str(struct fht_message *message,
			   const struct fht_message_raw *raw)
{
	report_printf_value(message, 0, "%u", FHT_YEAR_BASE + raw->value);
	return 0;
}

static int payload_to_fht_month(const char *payload)
{
	unsigned int month;

	if (sscanf(payload, "%u", &month) != 1)
		return -EINVAL;

	if (month > 12)
		return -EINVAL;

	return month;
}

static int fht_month_to_str(struct fht_message *message,
			   const struct fht_message_raw *raw)
{
	if (raw->value > 12)
		return -EINVAL;

	report_printf_value(message, 0, "%u", raw->value);
	return 0;
}

static int payload_to_fht_day(const char *payload)
{
	unsigned int day;

	if (sscanf(payload, "%u", &day) != 1)
		return -EINVAL;

	if (day > 31)
		return -EINVAL;

	return day;
}

static int fht_day_to_str(struct fht_message *message,
			  const struct fht_message_raw *raw)
{
	if (raw->value > 31)
		return -EINVAL;

	report_printf_value(message, 0, "%u", raw->value);
	return 0;
}

static int payload_to_fht_hour(const char *payload)
{
	unsigned int hour;

	if (sscanf(payload, "%u", &hour) != 1)
		return -EINVAL;

	if (hour > 24)
		return -EINVAL;

	return hour;
}

static int fht_hour_to_str(struct fht_message *message,
			   const struct fht_message_raw *raw)
{
	if (raw->value > 24)
		return -EINVAL;

	report_printf_value(message, 0, "%u", raw->value);
	return 0;
}

static int payload_to_fht_minute(const char *payload)
{
	unsigned int minute;

	if (sscanf(payload, "%u", &minute) != 1)
		return -EINVAL;

	if (minute > 59)
		return -EINVAL;

	return minute;
}

static int fht_minute_to_str(struct fht_message *message,
			     const struct fht_message_raw *raw)
{
	if (raw->value > 59)
		return -EINVAL;

	report_printf_value(message, 0, "%u", raw->value);
	return 0;
}

static int fht_percentage_to_str(struct fht_message *message,
				 const struct fht_message_raw *raw)
{
	unsigned char l, r;
	unsigned char valve = raw->value;

	l = (raw->status >> 4) & 0x0f;
	r = raw->status & 0x0f;

	/* actuator changed state. e.g., the valve */
	if (l == 0x2) {
	 /* actuator didn't change */
	} else if (l == 0xa) {
	}

	switch (r) {
	case 0x1: /* 30.5 or ON on fht80b */
		valve = 0xff;
		break;
	case 0x2: /* 5.5 or OFF on fht80b */
		valve = 0;
		break;
	case 0x0: /* value contains valve state */
	case 0x6:
		break;
	case 0x8: /* value contains OFFSET setting */
		report_printf_topic(message, 0, "valve/%u/offset", raw->cmd);
		report_printf_value(message, 0, "%s%u",
				    raw->value & 0x80 ? "-" : "",
				    raw->value & 0x7f);
		return 0;
		break;
	case 0xa: /* lime-protection */
		/* lime-protection bug, value contains valve setting */
		if (l == 0xa || l == 0xb)
			break;
		/* else l == 0x2 or l == 0x3 */
		return -EINVAL;
		/* TBD: submit lime-protection */
		break;
	case 0xc: /* synctime */
		report_printf_topic(message, 0, "synctime");
		report_printf_value(message, 0, "%u", (raw->value / 2) - 1);
		return 0;
		break;
	case 0xe: /* TEST */
		return -EINVAL;
		break;
	case 0xf: /* pair */
		report_printf_topic(message, 1, "valve/%u", raw->cmd);
		report_printf_value(message, 1, "paired");
		break;
	}

	report_printf_value(message, 0, "%0.1f", (float)valve * 100 / 255);
	return 0;
}

static int fht_status_to_str(struct fht_message *message,
			     const struct fht_message_raw *raw)
{
	report_printf_topic(message, 0, "window");
	report_printf_value(message, 0, "%s",
			    raw->value & (1 << 5) ? "open" : "close");

	report_printf_topic(message, 1, "battery");
	report_printf_value(message, 1, "%s",
			    raw->value & (1 << 0) ? "empty" : "ok");
	return 0;
}

static int fht_ignore(struct fht_message *message,
		      const struct fht_message_raw *raw)
{
	printf("ignored %02x: %02x %02x %02x\n", raw->cmd, raw->subfun,
	       raw->status, raw->value);
	return 0;
}

#define DEFINE_VALVE(__no) \
	{ \
		.function_id = FHT_VALVE_##__no, \
		.name = "valve/" __stringify(__no), \
		.input_conversion = input_not_accepted, \
		.output_conversion = fht_percentage_to_str, \
	}

#define DEFINE_IGNORE(__no) \
	{ \
		.function_id = __no, \
		.input_conversion = input_not_accepted, \
		.output_conversion = fht_ignore, \
	}

static const struct fht_command fht_commands[] = {
	/* is valve */ {
		.function_id = FHT_IS_VALVE,
		.name = "is-valve",
		.input_conversion = input_not_accepted,
		.output_conversion = fht_percentage_to_str,
	},
	DEFINE_VALVE(1),
	DEFINE_VALVE(2),
	DEFINE_VALVE(3),
	DEFINE_VALVE(4),
	DEFINE_VALVE(5),
	DEFINE_VALVE(6),
	DEFINE_VALVE(7),
	DEFINE_VALVE(8),
	/* mode */ {
		.function_id = FHT_MODE,
		.name = "mode",
		.input_conversion = payload_to_mode,
		.output_conversion = mode_to_str,
	},
	/* desired temp */ {
		.function_id = FHT_DESIRED_TEMP,
		.name = "desired-temp",
		.input_conversion = payload_to_fht_temp,
		.output_conversion = fht_temp_to_str,
	},
	/* is temp low */ {
		.function_id = FHT_IS_TEMP_LOW,
		.input_conversion = input_not_accepted,
		.output_conversion = fht_is_temp_low,
	},
	/* is temp high */ {
		.function_id = FHT_IS_TEMP_HIGH,
		.name = "is-temp",
		.input_conversion = input_not_accepted,
		.output_conversion = fht_is_temp_high_to_str,
	},
	/* status */ {
		.function_id = FHT_STATUS,
		.name = "status",
		.input_conversion = input_not_accepted,
		.output_conversion = fht_status_to_str,
	},
	/* manu temp */ {
		.function_id = FHT_MANU_TEMP,
		.name = "manu-temp",
		.input_conversion = payload_to_fht_temp,
		.output_conversion = fht_temp_to_str,
	},
	/* ack, ack2, {start,end}-xmit, we don't forward this */
	DEFINE_IGNORE(FHT_ACK),
	/* year */ {
		.function_id = FHT_YEAR,
		.name = "year",
		.input_conversion = payload_to_fht_year,
		.output_conversion = fht_year_to_str,
	},
	/* month */ {
		.function_id = FHT_MONTH,
		.name = "month",
		.input_conversion = payload_to_fht_month,
		.output_conversion = fht_month_to_str,
	},
	/* day */ {
		.function_id = FHT_DAY,
		.name = "day",
		.input_conversion = payload_to_fht_day,
		.output_conversion = fht_day_to_str,
	},
	/* hour */ {
		.function_id = FHT_HOUR,
		.name = "hour",
		.input_conversion = payload_to_fht_hour,
		.output_conversion = fht_hour_to_str,
	},
	/* minute */ {
		.function_id = FHT_MINUTE,
		.name = "minute",
		.input_conversion = payload_to_fht_minute,
		.output_conversion = fht_minute_to_str,
	},
	DEFINE_IGNORE(FHT_ACK2),
	DEFINE_IGNORE(FHT_START_XMIT),
	DEFINE_IGNORE(FHT_END_XMIT),
	/* day temp */ {
		.function_id = FHT_DAY_TEMP,
		.name = "day-temp",
		.input_conversion = payload_to_fht_temp,
		.output_conversion = fht_temp_to_str,
	},
	/* night temp */ {
		.function_id = FHT_NIGHT_TEMP,
		.name = "night-temp",
		.input_conversion = payload_to_fht_temp,
		.output_conversion = fht_temp_to_str,
	},
	/* window open temp */ {
		.function_id = FHT_WINDOW_OPEN_TEMP,
		.name = "window-open-temp",
		.input_conversion = payload_to_fht_temp,
		.output_conversion = fht_temp_to_str,
	},
};

int fht_decode(const struct payload *payload, struct fht_message *message)
{
	static const unsigned char magic_ack[] = {0x83, 0x09, 0x83, 0x01};
	static const unsigned char magic_status[] = {0x09, 0x09, 0xa0, 0x01};
	struct fht_message_raw fht_message_raw = {0, 0, 0, 0};
	const struct fht_command *fht_command;
	int i;

	memset(message, 0, sizeof(*message));

	if (payload->len < 9)
		return -EINVAL;

	if (!memcmp(payload->data, magic_ack, sizeof(magic_ack))) {
		message->type = ACK;
		fht_message_raw.value = payload->data[7];
	} else if (!memcmp(payload->data, magic_status, sizeof(magic_status))) {
		if (payload->len != 10)
			return -EINVAL;
		message->type = STATUS;
		fht_message_raw.subfun = payload->data[7];
		fht_message_raw.status = payload->data[8];
		fht_message_raw.value = payload->data[9];
	} else
		return -EINVAL;
	fht_message_raw.cmd = payload->data[6];

	message->hauscode = *(const struct hauscode*)(payload->data + 4);

	for_each_fht_command(fht_commands, fht_command, i) {
		if (fht_command->function_id != fht_message_raw.cmd)
			continue;
		if (fht_command->name)
			strncpy(message->report[0].topic, fht_command->name,
				sizeof(message->report[0].topic));
		return fht_command->output_conversion(message,
						      &fht_message_raw);
	}

	return -EINVAL;
}

static int fht_send(int fd, const struct hauscode *hauscode,
		    unsigned char memory, unsigned char value)
{
	const struct payload payload = {
		.tt = 0x04,
		.len = 7,
		.data = {0x02, 0x01, 0x83, hauscode->upper, hauscode->lower,
			 memory, value},
	};

	return fhz_send(fd, &payload);
}

int fht_set(int fd, const struct hauscode *hauscode,
	    const char *command, const char *payload)
{
	const struct fht_command *fht_command;
	unsigned char fht_val;
	bool found = false;
	int i, err;

	for_each_fht_command(fht_commands, fht_command, i)
		if (fht_command->name && !strcmp(fht_command->name, command)) {
			found = true;
			break;
		}

	if (!found)
		return -EINVAL;

	err = fht_command->input_conversion(payload);
	if (err < 0)
		return err;

	fht_val = err;

	return fht_send(fd, hauscode, fht_command->function_id, fht_val);
}
