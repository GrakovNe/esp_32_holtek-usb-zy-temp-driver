/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 GrakovNe
 */
#include "appconf.h"

#include <string.h>

#include "tinytest.h"

static const char ID[] = "1a2b";

static void test_device_id(void)
{
    const uint8_t mac1[6] = { 0x7c, 0x4f, 0xad, 0xbb, 0x1a, 0x2b };
    char id[5];
    appconf_device_id(mac1, id);
    CHECK_STR(id, "1a2b");

    const uint8_t mac2[6] = { 0, 0, 0, 0, 0xff, 0x0a };
    appconf_device_id(mac2, id);
    CHECK_STR(id, "ff0a");
}

static void test_hostname(void)
{
    char h[APPCONF_HOST_MAX + 1];

    appconf_hostname("AirMonitor-1A2B", ID, h, sizeof h);
    CHECK_STR(h, "airmonitor-1a2b");

    appconf_hostname("  --foo--bar--  ", ID, h, sizeof h);
    CHECK_STR(h, "foo-bar");

    appconf_hostname("-a-", ID, h, sizeof h);
    CHECK_STR(h, "a");

    appconf_hostname("a_b!", ID, h, sizeof h);
    CHECK_STR(h, "a-b");

    appconf_hostname("Ünïcode Spaced Name!", ID, h, sizeof h);
    CHECK_STR(h, "n-code-spaced-name");

    appconf_hostname("МойДатчик", ID, h, sizeof h);
    CHECK_STR(h, "airmonitor-1a2b");

    appconf_hostname("", ID, h, sizeof h);
    CHECK_STR(h, "airmonitor-1a2b");

    appconf_hostname("___", ID, h, sizeof h);
    CHECK_STR(h, "airmonitor-1a2b");

    char long_name[APPCONF_NAME_MAX + 1];
    memset(long_name, 'a', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    appconf_hostname(long_name, ID, h, sizeof h);
    CHECK(strlen(h) == APPCONF_NAME_MAX);
}

static void test_defaults(void)
{
    appconf_t c;
    appconf_defaults(&c, ID);

    CHECK_STR(c.device_name, "AirMonitor-1A2B");
    CHECK_STR(c.hostname, "airmonitor-1a2b");
    CHECK_STR(c.ntp_server, "pool.ntp.org");
    CHECK(c.wifi_ssid[0] == '\0');
    CHECK(c.wifi_password[0] == '\0');
    CHECK(c.mqtt_enabled == false);
    CHECK(c.mqtt_port == APPCONF_MQTT_PORT_PLAIN);
    CHECK(c.mqtt_interval_s == APPCONF_MQTT_INTERVAL_DEFAULT_S);
    CHECK_STR(c.mqtt_co2_topic, "airmonitor/1a2b/co2");
    CHECK_STR(c.mqtt_temp_topic, "airmonitor/1a2b/temperature");

    char err[128];
    CHECK(appconf_validate(&c, err, sizeof err));
}

static void test_derive(void)
{
    appconf_t c;
    appconf_defaults(&c, ID);
    strcpy(c.device_name, "Test Sensor!!");
    appconf_derive(&c, ID);
    CHECK_STR(c.hostname, "test-sensor");
}

static void test_validate_name(void)
{
    appconf_t c;
    char err[128];

    appconf_defaults(&c, ID);
    c.device_name[0] = '\0';
    CHECK(!appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    memset(c.device_name, 'x', APPCONF_NAME_MAX + 1);
    CHECK(!appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    memset(c.device_name, 'x', APPCONF_NAME_MAX);
    CHECK(appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    c.device_name[3] = '\t';
    CHECK(!appconf_validate(&c, err, sizeof err));
}

static void test_validate_wifi(void)
{
    appconf_t c;
    char err[128];

    appconf_defaults(&c, ID);
    strcpy(c.wifi_ssid, "CafeOpen");
    CHECK(appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    strcpy(c.wifi_ssid, "Home");
    strcpy(c.wifi_password, "1234567");
    CHECK(!appconf_validate(&c, err, sizeof err));
    strcpy(c.wifi_password, "12345678");
    CHECK(appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    strcpy(c.wifi_password, "12345678");
    CHECK(!appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    strcpy(c.wifi_ssid, "Home");
    strcpy(c.wifi_password, "пароль123456");
    CHECK(!appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    memset(c.wifi_ssid, 's', APPCONF_SSID_MAX + 1);
    CHECK(!appconf_validate(&c, err, sizeof err));
}

static void test_validate_ntp(void)
{
    appconf_t c;
    char err[128];

    appconf_defaults(&c, ID);
    c.ntp_server[0] = '\0';
    CHECK(!appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    strcpy(c.ntp_server, "time.example.com");
    CHECK(appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    strcpy(c.ntp_server, "2606:4700:f111:0:0:0:0:c633");
    CHECK(appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    strcpy(c.ntp_server, "bad server");
    CHECK(!appconf_validate(&c, err, sizeof err));
}

static void test_validate_mqtt(void)
{
    appconf_t c;
    char err[128];

    appconf_defaults(&c, ID);
    c.mqtt_enabled = true;
    CHECK(!appconf_validate(&c, err, sizeof err));
    strcpy(c.mqtt_host, "192.168.1.10");
    CHECK(appconf_validate(&c, err, sizeof err));

    c.mqtt_port = 0;
    CHECK(!appconf_validate(&c, err, sizeof err));
    c.mqtt_port = APPCONF_MQTT_PORT_TLS;

    c.mqtt_interval_s = APPCONF_MQTT_INTERVAL_MIN_S - 1;
    CHECK(!appconf_validate(&c, err, sizeof err));
    c.mqtt_interval_s = APPCONF_MQTT_INTERVAL_MAX_S + 1;
    CHECK(!appconf_validate(&c, err, sizeof err));
    c.mqtt_interval_s = APPCONF_MQTT_INTERVAL_DEFAULT_S;

    strcpy(c.mqtt_co2_topic, "has space");
    CHECK(!appconf_validate(&c, err, sizeof err));
    strcpy(c.mqtt_co2_topic, "a/#");
    CHECK(!appconf_validate(&c, err, sizeof err));
    strcpy(c.mqtt_co2_topic, "airmonitor/1a2b/co2");

    strcpy(c.mqtt_temp_topic, "");
    CHECK(!appconf_validate(&c, err, sizeof err));
    strcpy(c.mqtt_temp_topic, "airmonitor/1a2b/temperature");
    CHECK(appconf_validate(&c, err, sizeof err));

    appconf_defaults(&c, ID);
    strcpy(c.mqtt_host, "bad host!!");
    c.mqtt_interval_s = 0;
    CHECK(appconf_validate(&c, err, sizeof err));
}

static void test_validate_cert(void)
{
    appconf_t c;
    char err[128];

    appconf_defaults(&c, ID);
    strcpy(c.mqtt_ca_cert, "not a certificate");
    CHECK(!appconf_validate(&c, err, sizeof err));

    strcpy(c.mqtt_ca_cert, "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n");
    CHECK(appconf_validate(&c, err, sizeof err));

    memset(c.mqtt_ca_cert, 'A', APPCONF_CERT_MAX + 1);
    CHECK(!appconf_validate(&c, err, sizeof err));

    c.mqtt_ca_cert[0] = '\0';
    CHECK(appconf_validate(&c, err, sizeof err));
}

int main(void)
{
    test_device_id();
    test_hostname();
    test_defaults();
    test_derive();
    test_validate_name();
    test_validate_wifi();
    test_validate_ntp();
    test_validate_mqtt();
    test_validate_cert();
    REPORT();
}
