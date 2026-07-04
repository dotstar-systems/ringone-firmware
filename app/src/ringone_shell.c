/* Ring•One Firmware · Dotstar Consulting · Apache 2.0 */

/*
 * ringone_cred shell command — provision cloud credentials into
 * PSA Protected Storage (CRACEN HUK-encrypted, survives reboots).
 *
 * Usage:
 *   uart:~$ ringone_cred set influx_token <TOKEN>
 *   uart:~$ ringone_cred set mqtt_user    <USERNAME>
 *   uart:~$ ringone_cred set mqtt_pass    <PASSWORD>
 *
 * Changes take effect on next boot (modules load credentials at init).
 * PSA UIDs must stay in sync with influx_telemetry.c and mqtt_client.c.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <psa/protected_storage.h>
#include <string.h>
#include "ringone_sensors.h"

LOG_MODULE_REGISTER(ringone_shell, LOG_LEVEL_INF);

/* Must match definitions in influx_telemetry.c and mqtt_client.c */
#define PS_UID_INFLUX_TOKEN  ((psa_storage_uid_t)0x524E4946U)  /* "RNIF" */
#define PS_UID_MQTT_USERNAME ((psa_storage_uid_t)0x524E4D51U)  /* "RNMQ" */
#define PS_UID_MQTT_PASSWORD ((psa_storage_uid_t)0x524E5057U)  /* "RNPW" */

static int cmd_cred_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 3) {
		shell_error(sh, "usage: ringone_cred set <key> <value>");
		shell_error(sh, "  keys: influx_token  mqtt_user  mqtt_pass");
		return -EINVAL;
	}

	const char *key   = argv[1];
	const char *value = argv[2];
	size_t      vlen  = strlen(value);
	psa_storage_uid_t uid;

	if (strcmp(key, "influx_token") == 0) {
		uid = PS_UID_INFLUX_TOKEN;
	} else if (strcmp(key, "mqtt_user") == 0) {
		uid = PS_UID_MQTT_USERNAME;
	} else if (strcmp(key, "mqtt_pass") == 0) {
		uid = PS_UID_MQTT_PASSWORD;
	} else {
		shell_error(sh, "unknown key '%s'", key);
		shell_error(sh, "valid keys: influx_token  mqtt_user  mqtt_pass");
		return -EINVAL;
	}

	if (vlen == 0) {
		shell_error(sh, "value must not be empty");
		return -EINVAL;
	}

	psa_status_t err = psa_ps_set(uid, vlen, value,
				      PSA_STORAGE_FLAG_NONE);
	if (err != PSA_SUCCESS) {
		shell_error(sh, "PSA write failed (err %d)", (int)err);
		return -EIO;
	}

	shell_print(sh, "stored %s (%zu bytes) — reboot to activate", key, vlen);
	LOG_INF("Credential '%s' written to Protected Storage", key);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ringone_cred_cmds,
	SHELL_CMD_ARG(set, NULL,
		"set <key> <value>  store credential in PSA Protected Storage\n"
		"  influx_token  InfluxDB write token\n"
		"  mqtt_user     HiveMQ username\n"
		"  mqtt_pass     HiveMQ password",
		cmd_cred_set, 3, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(ringone_cred, &ringone_cred_cmds,
	"Provision Ring-One cloud credentials into PSA Protected Storage.",
	NULL);

/*
 * ringone spo2 — manually fetch one raw MAX30101 RED/IR sample plus the
 * current post-processed heart rate / SpO2, for bench debugging
 * independent of the 2 s BLE notify loop.
 */
static int cmd_ringone_spo2(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint32_t red, ir;
	int err = ringone_sensors_read_raw(&red, &ir);

	if (err) {
		shell_error(sh, "MAX30101 read failed (err %d) — not ready yet?", err);
		return err;
	}

	shell_print(sh, "red=%u ir=%u  hr=%u bpm  spo2=%u%%",
		    red, ir, ringone_read_heart_rate(), ringone_read_spo2());
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ringone_cmds,
	SHELL_CMD_ARG(spo2, NULL,
		"Read one raw MAX30101 RED/IR sample + current HR/SpO2",
		cmd_ringone_spo2, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(ringone, &ringone_cmds,
	"Ring-One sensor diagnostics commands.", NULL);
