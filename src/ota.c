/* OTA firmware update from an HTTP URL into the MCUboot upgrade slot. */

/*
 * SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wallabmc_ota, LOG_LEVEL_INF);

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/shell/shell.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/http/parser_url.h>

#include "ota.h"

#define OTA_HTTP_TIMEOUT_MS 60000

struct ota_state {
	struct flash_img_context img;
	size_t total;
	int err;
};

static int ota_resolve(const char *host, uint16_t port, struct sockaddr *addr,
		       socklen_t *addrlen)
{
	/*
	 * Parse the URL host as a literal IPv4 dotted quad. DNS would
	 * require enabling the resolver subsystem in this build, which
	 * the README flags as destabilising the websocket console — so
	 * keep it to URLs of the form http://<a.b.c.d>[:port]/path.
	 */
	struct sockaddr_in *sin = (struct sockaddr_in *)addr;

	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_port = htons(port);
	if (zsock_inet_pton(AF_INET, host, &sin->sin_addr) != 1) {
		LOG_ERR("OTA URL host must be a dotted-quad IPv4 (got %s)", host);
		return -EINVAL;
	}
	*addrlen = sizeof(*sin);
	return 0;
}

static int ota_response_cb(struct http_response *rsp,
			   enum http_final_call final_data, void *user_data)
{
	struct ota_state *s = user_data;
	int ret;

	if (s->err) {
		return 0;
	}

	if (rsp->http_status_code && rsp->http_status_code / 100 != 2) {
		LOG_ERR("OTA HTTP status %d (%s)", rsp->http_status_code,
			rsp->http_status);
		s->err = -EIO;
		return 0;
	}

	/*
	 * Treat rsp->processed (the http_parser's running body byte count)
	 * as the source of truth, since on_body can fire multiple times per
	 * recv. body_frag_start points at the first body byte in this recv;
	 * the contiguous body span in the recv buffer ends `processed - last_processed`
	 * bytes later. body_frag_len only reflects the latest on_body chunk
	 * in this recv, so summing it loses fragments when the parser splits
	 * a buffer across more than one chunk.
	 */
	if (rsp->body_frag_start && rsp->processed > s->total) {
		size_t take = rsp->processed - s->total;
		const uint8_t *src = rsp->body_frag_start;

		ret = flash_img_buffered_write(&s->img, src, take,
					       final_data == HTTP_DATA_FINAL);
		if (ret) {
			LOG_ERR("flash_img write failed: %d", ret);
			s->err = ret;
			return 0;
		}
		s->total += take;
	} else if (final_data == HTTP_DATA_FINAL) {
		/* No new body bytes but message done: flush partial block. */
		ret = flash_img_buffered_write(&s->img, NULL, 0, true);
		if (ret) {
			LOG_ERR("flash_img flush failed: %d", ret);
			s->err = ret;
			return 0;
		}
	}

	if (final_data == HTTP_DATA_FINAL && rsp->content_length &&
	    s->total != rsp->content_length) {
		LOG_ERR("OTA size mismatch: wrote %zu, content-length %zu",
			s->total, rsp->content_length);
		s->err = -EIO;
	}

	return 0;
}

static int ota_do(const char *url, struct ota_state *s)
{
	struct http_parser_url parsed;
	struct http_request req = {0};
	struct sockaddr addr;
	socklen_t addrlen;
	char host[64];
	char path[128];
	uint8_t *recv_buf = NULL;
	uint16_t port = 80;
	uint16_t hlen, plen;
	int sock = -1;
	int ret;

	http_parser_url_init(&parsed);
	ret = http_parser_parse_url(url, strlen(url), 0, &parsed);
	if (ret) {
		LOG_ERR("Bad URL");
		return -EINVAL;
	}

	if (!(parsed.field_set & (1U << UF_HOST))) {
		LOG_ERR("URL missing host");
		return -EINVAL;
	}

	hlen = parsed.field_data[UF_HOST].len;
	if (hlen >= sizeof(host)) {
		return -ENOMEM;
	}
	memcpy(host, url + parsed.field_data[UF_HOST].off, hlen);
	host[hlen] = '\0';

	if (parsed.field_set & (1U << UF_PORT)) {
		port = parsed.port;
	}

	if (parsed.field_set & (1U << UF_PATH)) {
		plen = parsed.field_data[UF_PATH].len;
		if (plen >= sizeof(path)) {
			return -ENOMEM;
		}
		memcpy(path, url + parsed.field_data[UF_PATH].off, plen);
		path[plen] = '\0';
	} else {
		strcpy(path, "/");
	}

	ret = ota_resolve(host, port, &addr, &addrlen);
	if (ret) {
		return ret;
	}

	ret = flash_img_init(&s->img);
	if (ret) {
		LOG_ERR("flash_img_init failed: %d", ret);
		return ret;
	}

	recv_buf = k_malloc(CONFIG_APP_OTA_HTTP_BUF_SIZE);
	if (!recv_buf) {
		return -ENOMEM;
	}

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("socket: %d", errno);
		ret = -errno;
		goto out;
	}

	ret = zsock_connect(sock, &addr, addrlen);
	if (ret < 0) {
		LOG_ERR("connect: %d", errno);
		ret = -errno;
		goto out;
	}

	LOG_INF("OTA: GET %s from %s:%u", path, host, port);

	req.method = HTTP_GET;
	req.url = path;
	req.host = host;
	req.protocol = "HTTP/1.1";
	req.response = ota_response_cb;
	req.recv_buf = recv_buf;
	req.recv_buf_len = CONFIG_APP_OTA_HTTP_BUF_SIZE;

	ret = http_client_req(sock, &req, OTA_HTTP_TIMEOUT_MS, s);
	if (ret < 0) {
		LOG_ERR("http_client_req: %d", ret);
		goto out;
	}

	if (s->err) {
		ret = s->err;
		goto out;
	}

	if (s->total == 0) {
		LOG_ERR("OTA empty body");
		ret = -EIO;
		goto out;
	}

	LOG_INF("OTA: wrote %zu bytes to upgrade slot", s->total);
	ret = 0;
out:
	if (sock >= 0) {
		zsock_close(sock);
	}
	k_free(recv_buf);
	return ret;
}

int ota_download_and_stage(const char *url)
{
	struct ota_state s = {0};
	int ret;

	ret = ota_do(url, &s);
	if (ret) {
		return ret;
	}

	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret) {
		LOG_ERR("boot_request_upgrade failed: %d", ret);
		return ret;
	}

	LOG_INF("OTA staged. Reboot to install.");
	return 0;
}

static int cmd_ota_url(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc != 2) {
		shell_error(sh, "usage: bmc ota url http://<ip>[:port]/<path>");
		return -EINVAL;
	}

	shell_print(sh, "OTA: starting download from %s", argv[1]);
	ret = ota_download_and_stage(argv[1]);
	if (ret) {
		shell_error(sh, "OTA failed: %d", ret);
		return ret;
	}

	shell_print(sh, "OTA: staged image, rebooting in 1s...");
	k_sleep(K_MSEC(1000));
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

static int cmd_ota_confirm(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (boot_is_img_confirmed()) {
		shell_print(sh, "Image is already confirmed.");
		return 0;
	}

	ret = boot_write_img_confirmed();
	if (ret) {
		shell_error(sh, "boot_write_img_confirmed failed: %d", ret);
		return ret;
	}

	shell_print(sh, "Image confirmed.");
	return 0;
}

/*
 * Compile-time tag that lets a smoke test verify whether an OTA actually
 * replaced the running image. Override on the cmake line with e.g.
 *     -DAPP_OTA_BUILD_TAG='"V2"'
 * Defaults to "V1" so a tagless build is still obviously distinguishable
 * from a tagged one.
 */
#ifndef APP_OTA_BUILD_TAG
#define APP_OTA_BUILD_TAG "V1"
#endif

static int cmd_ota_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "OTA build tag: %s", APP_OTA_BUILD_TAG);
	shell_print(sh, "Image confirmed: %s",
		    boot_is_img_confirmed() ? "yes" : "no");
	shell_print(sh, "Pending swap type: %d", mcuboot_swap_type());
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ota_subcmds,
	SHELL_CMD_ARG(url, NULL,
		"Download image from URL into MCUboot slot1 and reboot.\n"
		"  Usage: bmc ota url http://<ip>[:port]/<path>",
		cmd_ota_url, 2, 0),
	SHELL_CMD(confirm, NULL,
		"Mark the currently running image as confirmed (permanent).",
		cmd_ota_confirm),
	SHELL_CMD(status, NULL, "Show OTA / MCUboot image status.",
		cmd_ota_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(ota, &ota_subcmds, "OTA firmware update", NULL);
