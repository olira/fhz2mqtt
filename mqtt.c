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

#include <errno.h>
#include <mosquitto.h>
#include <stddef.h>
#include <stdio.h>

#include "mqtt.h"
#include "fhz.h"

#define S_FHZ "fhz/"
#define S_FHT "fht/"
#define S_SET "set/"

#define TOPIC "/" S_FHZ
#define TOPIC_SUBSCRIBE TOPIC S_SET
#define TOPIC_FHT TOPIC S_FHT

static int mqtt_subscribe(struct mosquitto *mosquitto)
{
	return mosquitto_subscribe(mosquitto, NULL, TOPIC_SUBSCRIBE "#", 0);
}

static void callback(struct mosquitto *mosquitto, void *foo,
		     const struct mosquitto_message *message)
{
	const char *topic = message->topic + sizeof(TOPIC_SUBSCRIBE) - 1;

	if (!strncmp(topic, S_FHT, sizeof(S_FHT) - 1)) {
	}
}

static int mqtt_publish_fht(struct mosquitto *mosquitto, const struct fht_decoded *decoded)
{
	char topic[64];
	char message[64];
	int len;

	switch (decoded->type) {
	case STATUS:
		snprintf(topic, sizeof(topic),
			 TOPIC_FHT "%02u%02u/status/%02x",
			 decoded->hauscode.upper, decoded->hauscode.lower,
			 decoded->status.func);
		len = snprintf(message, sizeof(message), "%02x %02x",
			       decoded->status.status, decoded->status.param);
		break;
	case ACK:
		snprintf(topic, sizeof(topic),
			 TOPIC_FHT "%02u%02u/ack/%02x",
			 decoded->hauscode.upper, decoded->hauscode.lower,
			 decoded->ack.location);
		len = snprintf(message, sizeof(message), "%02x",
			       decoded->ack.byte);
		break;
	default:
		return -EINVAL;
	}


#ifdef DEBUG
	printf("%s: %s\n", topic, message);
#endif
#ifndef NO_SEND
	mosquitto_publish(mosquitto, NULL, topic, len, message, 0, false);
#else
	(void)len; /* surpress compiler warning by pretending use of variable */
#endif

	return 0;
}

int mqtt_publish(struct mosquitto *mosquitto, const struct fhz_decoded *decoded)
{
	switch (decoded->machine) {
	case FHT:
		return mqtt_publish_fht(mosquitto, &decoded->fht);
	default:
		return -EINVAL;
	}
}

int mqtt_handle(struct mosquitto *mosquitto)
{
	int err;

	err = mosquitto_loop(mosquitto, 0, 1);
	if (err == MOSQ_ERR_CONN_LOST || err == MOSQ_ERR_NO_CONN) {
		err = mosquitto_reconnect(mosquitto);
		if (!err)
			err = mqtt_subscribe(mosquitto);
	}
	switch (err) {
	case MOSQ_ERR_SUCCESS:
		break;
	case MOSQ_ERR_CONN_LOST:
		return -ECONNABORTED;
	case MOSQ_ERR_NO_CONN:
		return -ECANCELED;
	case MOSQ_ERR_ERRNO:
		return -errno;
	default:
		return -EINVAL;
	}

	return 0;
}

int mqtt_init(struct mosquitto **handle, const char *host, int port,
	      const char *username, const char *password)
{
	struct mosquitto *mosquitto;
	int err;

	if (!host || !port)
		return -EINVAL;

	if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS)
		return -EINVAL;

	mosquitto = mosquitto_new(NULL, true, NULL);
	if (!mosquitto)
		return -errno;

	if (username && password) {
		err = mosquitto_username_pw_set(mosquitto, username, password);
		if (err)
			goto close_out;
	}

	err = mosquitto_connect(mosquitto, host, port, 120);
	if (err) {
		fprintf(stderr, "mosquitto connect error\n");
		goto close_out;
	}

	err = mqtt_subscribe(mosquitto);
	if (err) {
		fprintf(stderr, "mosquitto subscription error\n");
	}

	mosquitto_message_callback_set(mosquitto, callback);

	*handle = mosquitto;
	return 0;

close_out:
	mosquitto_destroy(mosquitto);
	mosquitto_lib_cleanup();
	return -1;
}

void mqtt_close(struct mosquitto *mosquitto)
{
	mosquitto_destroy(mosquitto);
	mosquitto_lib_cleanup();
}
