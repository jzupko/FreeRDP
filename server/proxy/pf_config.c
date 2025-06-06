/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * FreeRDP Proxy Server
 *
 * Copyright 2019 Kobi Mizrachi <kmizrachi18@gmail.com>
 * Copyright 2019 Idan Freiberg <speidy@gmail.com>
 * Copyright 2021,2023 Armin Novak <anovak@thincast.com>
 * Copyright 2021,2023 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <string.h>
#include <winpr/crt.h>
#include <winpr/path.h>
#include <winpr/collections.h>
#include <winpr/cmdline.h>

#include "pf_server.h"
#include <freerdp/server/proxy/proxy_config.h>

#include <freerdp/server/proxy/proxy_log.h>

#include <freerdp/crypto/crypto.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/channels/audin.h>
#include <freerdp/channels/rdpdr.h>
#include <freerdp/channels/disp.h>
#include <freerdp/channels/rail.h>
#include <freerdp/channels/rdpei.h>
#include <freerdp/channels/tsmf.h>
#include <freerdp/channels/video.h>
#include <freerdp/channels/rdpecam.h>

#include "pf_utils.h"

#define TAG PROXY_TAG("config")

#define CONFIG_PRINT_SECTION(section) WLog_INFO(TAG, "\t%s:", section)
#define CONFIG_PRINT_SECTION_KEY(section, key) WLog_INFO(TAG, "\t%s/%s:", section, key)
#define CONFIG_PRINT_STR(config, key) WLog_INFO(TAG, "\t\t%s: %s", #key, (config)->key)
#define CONFIG_PRINT_STR_CONTENT(config, key) \
	WLog_INFO(TAG, "\t\t%s: %s", #key, (config)->key ? "set" : NULL)
#define CONFIG_PRINT_BOOL(config, key) WLog_INFO(TAG, "\t\t%s: %s", #key, boolstr((config)->key))
#define CONFIG_PRINT_UINT16(config, key) WLog_INFO(TAG, "\t\t%s: %" PRIu16 "", #key, (config)->key)
#define CONFIG_PRINT_UINT32(config, key) WLog_INFO(TAG, "\t\t%s: %" PRIu32 "", #key, (config)->key)

static const char* bool_str_true = "true";
static const char* bool_str_false = "false";
static const char* boolstr(BOOL rc)
{
	return rc ? bool_str_true : bool_str_false;
}

static const char* section_server = "Server";
static const char* key_host = "Host";
static const char* key_port = "Port";

static const char* section_target = "Target";
static const char* key_target_fixed = "FixedTarget";
static const char* key_target_user = "User";
static const char* key_target_pwd = "Password";
static const char* key_target_domain = "Domain";
static const char* key_target_tls_seclevel = "TlsSecLevel";

static const char* section_plugins = "Plugins";
static const char* key_plugins_modules = "Modules";
static const char* key_plugins_required = "Required";

static const char* section_channels = "Channels";
static const char* key_channels_gfx = "GFX";
static const char* key_channels_disp = "DisplayControl";
static const char* key_channels_clip = "Clipboard";
static const char* key_channels_mic = "AudioInput";
static const char* key_channels_sound = "AudioOutput";
static const char* key_channels_rdpdr = "DeviceRedirection";
static const char* key_channels_video = "VideoRedirection";
static const char* key_channels_camera = "CameraRedirection";
static const char* key_channels_rails = "RemoteApp";
static const char* key_channels_blacklist = "PassthroughIsBlacklist";
static const char* key_channels_pass = "Passthrough";
static const char* key_channels_intercept = "Intercept";

static const char* section_input = "Input";
static const char* key_input_kbd = "Keyboard";
static const char* key_input_mouse = "Mouse";
static const char* key_input_multitouch = "Multitouch";

static const char* section_security = "Security";
static const char* key_security_server_nla = "ServerNlaSecurity";
static const char* key_security_server_tls = "ServerTlsSecurity";
static const char* key_security_server_rdp = "ServerRdpSecurity";
static const char* key_security_client_nla = "ClientNlaSecurity";
static const char* key_security_client_tls = "ClientTlsSecurity";
static const char* key_security_client_rdp = "ClientRdpSecurity";
static const char* key_security_client_fallback = "ClientAllowFallbackToTls";

static const char* section_certificates = "Certificates";
static const char* key_private_key_file = "PrivateKeyFile";
static const char* key_private_key_content = "PrivateKeyContent";
static const char* key_cert_file = "CertificateFile";
static const char* key_cert_content = "CertificateContent";

WINPR_ATTR_MALLOC(CommandLineParserFree, 1)
static char** pf_config_parse_comma_separated_list(const char* list, size_t* count)
{
	if (!list || !count)
		return NULL;

	if (strlen(list) == 0)
	{
		*count = 0;
		return NULL;
	}

	return CommandLineParseCommaSeparatedValues(list, count);
}

static BOOL pf_config_get_uint16(wIniFile* ini, const char* section, const char* key,
                                 UINT16* result, BOOL required)
{
	int val = 0;
	const char* strval = NULL;

	WINPR_ASSERT(result);

	strval = IniFile_GetKeyValueString(ini, section, key);
	if (!strval && required)
	{
		WLog_ERR(TAG, "key '%s.%s' does not exist.", section, key);
		return FALSE;
	}
	val = IniFile_GetKeyValueInt(ini, section, key);
	if ((val <= 0) || (val > UINT16_MAX))
	{
		WLog_ERR(TAG, "invalid value %d for key '%s.%s'.", val, section, key);
		return FALSE;
	}

	*result = (UINT16)val;
	return TRUE;
}

static BOOL pf_config_get_uint32(wIniFile* ini, const char* section, const char* key,
                                 UINT32* result, BOOL required)
{
	WINPR_ASSERT(result);

	const char* strval = IniFile_GetKeyValueString(ini, section, key);
	if (!strval)
	{
		if (required)
			WLog_ERR(TAG, "key '%s.%s' does not exist.", section, key);
		return !required;
	}

	const int val = IniFile_GetKeyValueInt(ini, section, key);
	if (val < 0)
	{
		WLog_ERR(TAG, "invalid value %d for key '%s.%s'.", val, section, key);
		return FALSE;
	}

	*result = (UINT32)val;
	return TRUE;
}

static BOOL pf_config_get_bool(wIniFile* ini, const char* section, const char* key, BOOL fallback)
{
	int num_value = 0;
	const char* str_value = NULL;

	str_value = IniFile_GetKeyValueString(ini, section, key);
	if (!str_value)
	{
		WLog_WARN(TAG, "key '%s.%s' not found, value defaults to %s.", section, key,
		          fallback ? bool_str_true : bool_str_false);
		return fallback;
	}

	if (_stricmp(str_value, bool_str_true) == 0)
		return TRUE;
	if (_stricmp(str_value, bool_str_false) == 0)
		return FALSE;

	num_value = IniFile_GetKeyValueInt(ini, section, key);

	if (num_value != 0)
		return TRUE;

	return FALSE;
}

static const char* pf_config_get_str(wIniFile* ini, const char* section, const char* key,
                                     BOOL required)
{
	const char* value = NULL;

	value = IniFile_GetKeyValueString(ini, section, key);

	if (!value)
	{
		if (required)
			WLog_ERR(TAG, "key '%s.%s' not found.", section, key);
		return NULL;
	}

	return value;
}

static BOOL pf_config_load_server(wIniFile* ini, proxyConfig* config)
{
	const char* host = NULL;

	WINPR_ASSERT(config);
	host = pf_config_get_str(ini, section_server, key_host, FALSE);

	if (!host)
		return TRUE;

	config->Host = _strdup(host);

	if (!config->Host)
		return FALSE;

	if (!pf_config_get_uint16(ini, section_server, key_port, &config->Port, TRUE))
		return FALSE;

	return TRUE;
}

static BOOL pf_config_load_target(wIniFile* ini, proxyConfig* config)
{
	const char* target_value = NULL;

	WINPR_ASSERT(config);
	config->FixedTarget = pf_config_get_bool(ini, section_target, key_target_fixed, FALSE);

	if (!pf_config_get_uint16(ini, section_target, key_port, &config->TargetPort,
	                          config->FixedTarget))
		return FALSE;

	if (!pf_config_get_uint32(ini, section_target, key_target_tls_seclevel,
	                          &config->TargetTlsSecLevel, FALSE))
		return FALSE;

	if (config->FixedTarget)
	{
		target_value = pf_config_get_str(ini, section_target, key_host, TRUE);
		if (!target_value)
			return FALSE;

		config->TargetHost = _strdup(target_value);
		if (!config->TargetHost)
			return FALSE;
	}

	target_value = pf_config_get_str(ini, section_target, key_target_user, FALSE);
	if (target_value)
	{
		config->TargetUser = _strdup(target_value);
		if (!config->TargetUser)
			return FALSE;
	}

	target_value = pf_config_get_str(ini, section_target, key_target_pwd, FALSE);
	if (target_value)
	{
		config->TargetPassword = _strdup(target_value);
		if (!config->TargetPassword)
			return FALSE;
	}

	target_value = pf_config_get_str(ini, section_target, key_target_domain, FALSE);
	if (target_value)
	{
		config->TargetDomain = _strdup(target_value);
		if (!config->TargetDomain)
			return FALSE;
	}

	return TRUE;
}

static BOOL pf_config_load_channels(wIniFile* ini, proxyConfig* config)
{
	WINPR_ASSERT(config);
	config->GFX = pf_config_get_bool(ini, section_channels, key_channels_gfx, TRUE);
	config->DisplayControl = pf_config_get_bool(ini, section_channels, key_channels_disp, TRUE);
	config->Clipboard = pf_config_get_bool(ini, section_channels, key_channels_clip, FALSE);
	config->AudioOutput = pf_config_get_bool(ini, section_channels, key_channels_mic, TRUE);
	config->AudioInput = pf_config_get_bool(ini, section_channels, key_channels_sound, TRUE);
	config->DeviceRedirection = pf_config_get_bool(ini, section_channels, key_channels_rdpdr, TRUE);
	config->VideoRedirection = pf_config_get_bool(ini, section_channels, key_channels_video, TRUE);
	config->CameraRedirection =
	    pf_config_get_bool(ini, section_channels, key_channels_camera, TRUE);
	config->RemoteApp = pf_config_get_bool(ini, section_channels, key_channels_rails, FALSE);
	config->PassthroughIsBlacklist =
	    pf_config_get_bool(ini, section_channels, key_channels_blacklist, FALSE);
	config->Passthrough = pf_config_parse_comma_separated_list(
	    pf_config_get_str(ini, section_channels, key_channels_pass, FALSE),
	    &config->PassthroughCount);
	config->Intercept = pf_config_parse_comma_separated_list(
	    pf_config_get_str(ini, section_channels, key_channels_intercept, FALSE),
	    &config->InterceptCount);

	return TRUE;
}

static BOOL pf_config_load_input(wIniFile* ini, proxyConfig* config)
{
	WINPR_ASSERT(config);
	config->Keyboard = pf_config_get_bool(ini, section_input, key_input_kbd, TRUE);
	config->Mouse = pf_config_get_bool(ini, section_input, key_input_mouse, TRUE);
	config->Multitouch = pf_config_get_bool(ini, section_input, key_input_multitouch, TRUE);
	return TRUE;
}

static BOOL pf_config_load_security(wIniFile* ini, proxyConfig* config)
{
	WINPR_ASSERT(config);
	config->ServerTlsSecurity =
	    pf_config_get_bool(ini, section_security, key_security_server_tls, TRUE);
	config->ServerNlaSecurity =
	    pf_config_get_bool(ini, section_security, key_security_server_nla, FALSE);
	config->ServerRdpSecurity =
	    pf_config_get_bool(ini, section_security, key_security_server_rdp, TRUE);

	config->ClientTlsSecurity =
	    pf_config_get_bool(ini, section_security, key_security_client_tls, TRUE);
	config->ClientNlaSecurity =
	    pf_config_get_bool(ini, section_security, key_security_client_nla, TRUE);
	config->ClientRdpSecurity =
	    pf_config_get_bool(ini, section_security, key_security_client_rdp, TRUE);
	config->ClientAllowFallbackToTls =
	    pf_config_get_bool(ini, section_security, key_security_client_fallback, TRUE);
	return TRUE;
}

static BOOL pf_config_load_modules(wIniFile* ini, proxyConfig* config)
{
	const char* modules_to_load = NULL;
	const char* required_modules = NULL;

	modules_to_load = pf_config_get_str(ini, section_plugins, key_plugins_modules, FALSE);
	required_modules = pf_config_get_str(ini, section_plugins, key_plugins_required, FALSE);

	WINPR_ASSERT(config);
	config->Modules = pf_config_parse_comma_separated_list(modules_to_load, &config->ModulesCount);

	config->RequiredPlugins =
	    pf_config_parse_comma_separated_list(required_modules, &config->RequiredPluginsCount);
	return TRUE;
}

static char* pf_config_decode_base64(const char* data, const char* name, size_t* pLength)
{
	const char* headers[] = { "-----BEGIN PUBLIC KEY-----", "-----BEGIN RSA PUBLIC KEY-----",
		                      "-----BEGIN CERTIFICATE-----", "-----BEGIN PRIVATE KEY-----",
		                      "-----BEGIN RSA PRIVATE KEY-----" };

	size_t decoded_length = 0;
	char* decoded = NULL;
	if (!data)
	{
		WLog_ERR(TAG, "Invalid base64 data [%p] for %s", data, name);
		return NULL;
	}

	WINPR_ASSERT(name);
	WINPR_ASSERT(pLength);

	const size_t length = strlen(data);

	if (strncmp(data, "-----", 5) == 0)
	{
		BOOL expected = FALSE;
		for (size_t x = 0; x < ARRAYSIZE(headers); x++)
		{
			const char* header = headers[x];

			if (strncmp(data, header, strlen(header)) == 0)
				expected = TRUE;
		}

		if (!expected)
		{
			/* Extract header for log message
			 * expected format is '----- SOMETEXT -----'
			 */
			char hdr[128] = { 0 };
			const char* end = strchr(&data[5], '-');
			if (end)
			{
				while (*end == '-')
					end++;

				const size_t s = MIN(ARRAYSIZE(hdr) - 1ULL, (size_t)(end - data));
				memcpy(hdr, data, s);
			}

			WLog_WARN(TAG, "PEM has unexpected header '%s'. Known supported headers are:", hdr);
			for (size_t x = 0; x < ARRAYSIZE(headers); x++)
			{
				const char* header = headers[x];
				WLog_WARN(TAG, "%s", header);
			}
		}

		*pLength = length + 1;
		return _strdup(data);
	}

	crypto_base64_decode(data, length, (BYTE**)&decoded, &decoded_length);
	if (!decoded || decoded_length == 0)
	{
		WLog_ERR(TAG, "Failed to decode base64 data of length %" PRIuz " for %s", length, name);
		free(decoded);
		return NULL;
	}

	*pLength = strnlen(decoded, decoded_length) + 1;
	return decoded;
}

static BOOL pf_config_load_certificates(wIniFile* ini, proxyConfig* config)
{
	const char* tmp1 = NULL;
	const char* tmp2 = NULL;

	WINPR_ASSERT(ini);
	WINPR_ASSERT(config);

	tmp1 = pf_config_get_str(ini, section_certificates, key_cert_file, FALSE);
	if (tmp1)
	{
		if (!winpr_PathFileExists(tmp1))
		{
			WLog_ERR(TAG, "%s/%s file %s does not exist", section_certificates, key_cert_file,
			         tmp1);
			return FALSE;
		}
		config->CertificateFile = _strdup(tmp1);
		config->CertificatePEM =
		    crypto_read_pem(config->CertificateFile, &config->CertificatePEMLength);
		if (!config->CertificatePEM)
			return FALSE;
		config->CertificatePEMLength += 1;
	}
	tmp2 = pf_config_get_str(ini, section_certificates, key_cert_content, FALSE);
	if (tmp2)
	{
		if (strlen(tmp2) < 1)
		{
			WLog_ERR(TAG, "%s/%s has invalid empty value", section_certificates, key_cert_content);
			return FALSE;
		}
		config->CertificateContent = _strdup(tmp2);
		config->CertificatePEM = pf_config_decode_base64(
		    config->CertificateContent, "CertificateContent", &config->CertificatePEMLength);
		if (!config->CertificatePEM)
			return FALSE;
	}
	if (tmp1 && tmp2)
	{
		WLog_ERR(TAG,
		         "%s/%s and %s/%s are "
		         "mutually exclusive options",
		         section_certificates, key_cert_file, section_certificates, key_cert_content);
		return FALSE;
	}
	else if (!tmp1 && !tmp2)
	{
		WLog_ERR(TAG,
		         "%s/%s or %s/%s are "
		         "required settings",
		         section_certificates, key_cert_file, section_certificates, key_cert_content);
		return FALSE;
	}

	tmp1 = pf_config_get_str(ini, section_certificates, key_private_key_file, FALSE);
	if (tmp1)
	{
		if (!winpr_PathFileExists(tmp1))
		{
			WLog_ERR(TAG, "%s/%s file %s does not exist", section_certificates,
			         key_private_key_file, tmp1);
			return FALSE;
		}
		config->PrivateKeyFile = _strdup(tmp1);
		config->PrivateKeyPEM =
		    crypto_read_pem(config->PrivateKeyFile, &config->PrivateKeyPEMLength);
		if (!config->PrivateKeyPEM)
			return FALSE;
		config->PrivateKeyPEMLength += 1;
	}
	tmp2 = pf_config_get_str(ini, section_certificates, key_private_key_content, FALSE);
	if (tmp2)
	{
		if (strlen(tmp2) < 1)
		{
			WLog_ERR(TAG, "%s/%s has invalid empty value", section_certificates,
			         key_private_key_content);
			return FALSE;
		}
		config->PrivateKeyContent = _strdup(tmp2);
		config->PrivateKeyPEM = pf_config_decode_base64(
		    config->PrivateKeyContent, "PrivateKeyContent", &config->PrivateKeyPEMLength);
		if (!config->PrivateKeyPEM)
			return FALSE;
	}

	if (tmp1 && tmp2)
	{
		WLog_ERR(TAG,
		         "%s/%s and %s/%s are "
		         "mutually exclusive options",
		         section_certificates, key_private_key_file, section_certificates,
		         key_private_key_content);
		return FALSE;
	}
	else if (!tmp1 && !tmp2)
	{
		WLog_ERR(TAG,
		         "%s/%s or %s/%s are "
		         "are required settings",
		         section_certificates, key_private_key_file, section_certificates,
		         key_private_key_content);
		return FALSE;
	}

	return TRUE;
}

proxyConfig* server_config_load_ini(wIniFile* ini)
{
	proxyConfig* config = NULL;

	WINPR_ASSERT(ini);

	config = calloc(1, sizeof(proxyConfig));
	if (config)
	{
		/* Set default values != 0 */
		config->TargetTlsSecLevel = 1;

		/* Load from ini */
		if (!pf_config_load_server(ini, config))
			goto out;

		if (!pf_config_load_target(ini, config))
			goto out;

		if (!pf_config_load_channels(ini, config))
			goto out;

		if (!pf_config_load_input(ini, config))
			goto out;

		if (!pf_config_load_security(ini, config))
			goto out;

		if (!pf_config_load_modules(ini, config))
			goto out;

		if (!pf_config_load_certificates(ini, config))
			goto out;
		config->ini = IniFile_Clone(ini);
		if (!config->ini)
			goto out;
	}
	return config;
out:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	pf_server_config_free(config);
	WINPR_PRAGMA_DIAG_POP

	return NULL;
}

BOOL pf_server_config_dump(const char* file)
{
	BOOL rc = FALSE;
	wIniFile* ini = IniFile_New();
	if (!ini)
		return FALSE;

	/* Proxy server configuration */
	if (IniFile_SetKeyValueString(ini, section_server, key_host, "0.0.0.0") < 0)
		goto fail;
	if (IniFile_SetKeyValueInt(ini, section_server, key_port, 3389) < 0)
		goto fail;

	/* Target configuration */
	if (IniFile_SetKeyValueString(ini, section_target, key_host, "somehost.example.com") < 0)
		goto fail;
	if (IniFile_SetKeyValueInt(ini, section_target, key_port, 3389) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_target, key_target_fixed, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueInt(ini, section_target, key_target_tls_seclevel, 1) < 0)
		goto fail;

	/* Channel configuration */
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_gfx, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_disp, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_clip, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_mic, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_sound, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_rdpdr, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_video, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_camera, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_rails, bool_str_false) < 0)
		goto fail;

	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_blacklist, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_pass, "") < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_channels, key_channels_intercept, "") < 0)
		goto fail;

	/* Input configuration */
	if (IniFile_SetKeyValueString(ini, section_input, key_input_kbd, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_input, key_input_mouse, bool_str_true) < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_input, key_input_multitouch, bool_str_true) < 0)
		goto fail;

	/* Security settings */
	if (IniFile_SetKeyValueString(ini, section_security, key_security_server_tls, bool_str_true) <
	    0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_security, key_security_server_nla, bool_str_false) <
	    0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_security, key_security_server_rdp, bool_str_true) <
	    0)
		goto fail;

	if (IniFile_SetKeyValueString(ini, section_security, key_security_client_tls, bool_str_true) <
	    0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_security, key_security_client_nla, bool_str_true) <
	    0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_security, key_security_client_rdp, bool_str_true) <
	    0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_security, key_security_client_fallback,
	                              bool_str_true) < 0)
		goto fail;

	/* Module configuration */
	if (IniFile_SetKeyValueString(ini, section_plugins, key_plugins_modules,
	                              "module1,module2,...") < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_plugins, key_plugins_required,
	                              "module1,module2,...") < 0)
		goto fail;

	/* Certificate configuration */
	if (IniFile_SetKeyValueString(ini, section_certificates, key_cert_file,
	                              "<absolute path to some certificate file> OR") < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_certificates, key_cert_content,
	                              "<Contents of some certificate file in PEM format>") < 0)
		goto fail;

	if (IniFile_SetKeyValueString(ini, section_certificates, key_private_key_file,
	                              "<absolute path to some private key file> OR") < 0)
		goto fail;
	if (IniFile_SetKeyValueString(ini, section_certificates, key_private_key_content,
	                              "<Contents of some private key file in PEM format>") < 0)
		goto fail;

	/* store configuration */
	if (IniFile_WriteFile(ini, file) < 0)
		goto fail;

	rc = TRUE;

fail:
	IniFile_Free(ini);
	return rc;
}

proxyConfig* pf_server_config_load_buffer(const char* buffer)
{
	proxyConfig* config = NULL;
	wIniFile* ini = NULL;

	ini = IniFile_New();

	if (!ini)
	{
		WLog_ERR(TAG, "IniFile_New() failed!");
		return NULL;
	}

	if (IniFile_ReadBuffer(ini, buffer) < 0)
	{
		WLog_ERR(TAG, "failed to parse ini: '%s'", buffer);
		goto out;
	}

	config = server_config_load_ini(ini);
out:
	IniFile_Free(ini);
	return config;
}

proxyConfig* pf_server_config_load_file(const char* path)
{
	proxyConfig* config = NULL;
	wIniFile* ini = IniFile_New();

	if (!ini)
	{
		WLog_ERR(TAG, "IniFile_New() failed!");
		return NULL;
	}

	if (IniFile_ReadFile(ini, path) < 0)
	{
		WLog_ERR(TAG, "failed to parse ini file: '%s'", path);
		goto out;
	}

	config = server_config_load_ini(ini);
out:
	IniFile_Free(ini);
	return config;
}

static void pf_server_config_print_list(char** list, size_t count)
{
	WINPR_ASSERT(list);
	for (size_t i = 0; i < count; i++)
		WLog_INFO(TAG, "\t\t- %s", list[i]);
}

void pf_server_config_print(const proxyConfig* config)
{
	WINPR_ASSERT(config);
	WLog_INFO(TAG, "Proxy configuration:");

	CONFIG_PRINT_SECTION(section_server);
	CONFIG_PRINT_STR(config, Host);
	CONFIG_PRINT_UINT16(config, Port);

	if (config->FixedTarget)
	{
		CONFIG_PRINT_SECTION(section_target);
		CONFIG_PRINT_STR(config, TargetHost);
		CONFIG_PRINT_UINT16(config, TargetPort);
		CONFIG_PRINT_UINT32(config, TargetTlsSecLevel);

		if (config->TargetUser)
			CONFIG_PRINT_STR(config, TargetUser);
		if (config->TargetDomain)
			CONFIG_PRINT_STR(config, TargetDomain);
	}

	CONFIG_PRINT_SECTION(section_input);
	CONFIG_PRINT_BOOL(config, Keyboard);
	CONFIG_PRINT_BOOL(config, Mouse);
	CONFIG_PRINT_BOOL(config, Multitouch);

	CONFIG_PRINT_SECTION(section_security);
	CONFIG_PRINT_BOOL(config, ServerNlaSecurity);
	CONFIG_PRINT_BOOL(config, ServerTlsSecurity);
	CONFIG_PRINT_BOOL(config, ServerRdpSecurity);
	CONFIG_PRINT_BOOL(config, ClientNlaSecurity);
	CONFIG_PRINT_BOOL(config, ClientTlsSecurity);
	CONFIG_PRINT_BOOL(config, ClientRdpSecurity);
	CONFIG_PRINT_BOOL(config, ClientAllowFallbackToTls);

	CONFIG_PRINT_SECTION(section_channels);
	CONFIG_PRINT_BOOL(config, GFX);
	CONFIG_PRINT_BOOL(config, DisplayControl);
	CONFIG_PRINT_BOOL(config, Clipboard);
	CONFIG_PRINT_BOOL(config, AudioOutput);
	CONFIG_PRINT_BOOL(config, AudioInput);
	CONFIG_PRINT_BOOL(config, DeviceRedirection);
	CONFIG_PRINT_BOOL(config, VideoRedirection);
	CONFIG_PRINT_BOOL(config, CameraRedirection);
	CONFIG_PRINT_BOOL(config, RemoteApp);
	CONFIG_PRINT_BOOL(config, PassthroughIsBlacklist);

	if (config->PassthroughCount)
	{
		WLog_INFO(TAG, "\tStatic Channels Proxy:");
		pf_server_config_print_list(config->Passthrough, config->PassthroughCount);
	}

	if (config->InterceptCount)
	{
		WLog_INFO(TAG, "\tStatic Channels Proxy-Intercept:");
		pf_server_config_print_list(config->Intercept, config->InterceptCount);
	}

	/* modules */
	CONFIG_PRINT_SECTION_KEY(section_plugins, key_plugins_modules);
	for (size_t x = 0; x < config->ModulesCount; x++)
		CONFIG_PRINT_STR(config, Modules[x]);

	/* Required plugins */
	CONFIG_PRINT_SECTION_KEY(section_plugins, key_plugins_required);
	for (size_t x = 0; x < config->RequiredPluginsCount; x++)
		CONFIG_PRINT_STR(config, RequiredPlugins[x]);

	CONFIG_PRINT_SECTION(section_certificates);
	CONFIG_PRINT_STR(config, CertificateFile);
	CONFIG_PRINT_STR_CONTENT(config, CertificateContent);
	CONFIG_PRINT_STR(config, PrivateKeyFile);
	CONFIG_PRINT_STR_CONTENT(config, PrivateKeyContent);
}

void pf_server_config_free(proxyConfig* config)
{
	if (config == NULL)
		return;

	CommandLineParserFree(config->Passthrough);
	CommandLineParserFree(config->Intercept);
	CommandLineParserFree(config->RequiredPlugins);
	CommandLineParserFree(config->Modules);
	free(config->TargetHost);
	free(config->Host);
	free(config->CertificateFile);
	free(config->CertificateContent);
	if (config->CertificatePEM)
		memset(config->CertificatePEM, 0, config->CertificatePEMLength);
	free(config->CertificatePEM);
	free(config->PrivateKeyFile);
	free(config->PrivateKeyContent);
	if (config->PrivateKeyPEM)
		memset(config->PrivateKeyPEM, 0, config->PrivateKeyPEMLength);
	free(config->PrivateKeyPEM);
	IniFile_Free(config->ini);
	free(config);
}

size_t pf_config_required_plugins_count(const proxyConfig* config)
{
	WINPR_ASSERT(config);
	return config->RequiredPluginsCount;
}

const char* pf_config_required_plugin(const proxyConfig* config, size_t index)
{
	WINPR_ASSERT(config);
	if (index >= config->RequiredPluginsCount)
		return NULL;

	return config->RequiredPlugins[index];
}

size_t pf_config_modules_count(const proxyConfig* config)
{
	WINPR_ASSERT(config);
	return config->ModulesCount;
}

const char** pf_config_modules(const proxyConfig* config)
{
	union
	{
		char** ppc;
		const char** cppc;
	} cnv;

	WINPR_ASSERT(config);

	cnv.ppc = config->Modules;
	return cnv.cppc;
}

static BOOL pf_config_copy_string(char** dst, const char* src)
{
	*dst = NULL;
	if (src)
		*dst = _strdup(src);
	return TRUE;
}

static BOOL pf_config_copy_string_n(char** dst, const char* src, size_t size)
{
	*dst = NULL;

	if (src && (size > 0))
	{
		WINPR_ASSERT(strnlen(src, size) == size - 1);
		*dst = calloc(size, sizeof(char));
		if (!*dst)
			return FALSE;
		memcpy(*dst, src, size);
	}

	return TRUE;
}

static BOOL pf_config_copy_string_list(char*** dst, size_t* size, char** src, size_t srcSize)
{
	WINPR_ASSERT(dst);
	WINPR_ASSERT(size);
	WINPR_ASSERT(src || (srcSize == 0));

	*dst = NULL;
	*size = 0;
	if (srcSize > INT32_MAX)
		return FALSE;

	if (srcSize != 0)
	{
		char* csv = CommandLineToCommaSeparatedValues((INT32)srcSize, src);
		*dst = CommandLineParseCommaSeparatedValues(csv, size);
		free(csv);
	}

	return TRUE;
}

BOOL pf_config_clone(proxyConfig** dst, const proxyConfig* config)
{
	proxyConfig* tmp = calloc(1, sizeof(proxyConfig));

	WINPR_ASSERT(dst);
	WINPR_ASSERT(config);

	if (!tmp)
		return FALSE;

	*tmp = *config;

	if (!pf_config_copy_string(&tmp->Host, config->Host))
		goto fail;
	if (!pf_config_copy_string(&tmp->TargetHost, config->TargetHost))
		goto fail;

	if (!pf_config_copy_string_list(&tmp->Passthrough, &tmp->PassthroughCount, config->Passthrough,
	                                config->PassthroughCount))
		goto fail;
	if (!pf_config_copy_string_list(&tmp->Intercept, &tmp->InterceptCount, config->Intercept,
	                                config->InterceptCount))
		goto fail;
	if (!pf_config_copy_string_list(&tmp->Modules, &tmp->ModulesCount, config->Modules,
	                                config->ModulesCount))
		goto fail;
	if (!pf_config_copy_string_list(&tmp->RequiredPlugins, &tmp->RequiredPluginsCount,
	                                config->RequiredPlugins, config->RequiredPluginsCount))
		goto fail;
	if (!pf_config_copy_string(&tmp->CertificateFile, config->CertificateFile))
		goto fail;
	if (!pf_config_copy_string(&tmp->CertificateContent, config->CertificateContent))
		goto fail;
	if (!pf_config_copy_string_n(&tmp->CertificatePEM, config->CertificatePEM,
	                             config->CertificatePEMLength))
		goto fail;
	if (!pf_config_copy_string(&tmp->PrivateKeyFile, config->PrivateKeyFile))
		goto fail;
	if (!pf_config_copy_string(&tmp->PrivateKeyContent, config->PrivateKeyContent))
		goto fail;
	if (!pf_config_copy_string_n(&tmp->PrivateKeyPEM, config->PrivateKeyPEM,
	                             config->PrivateKeyPEMLength))
		goto fail;

	tmp->ini = IniFile_Clone(config->ini);
	if (!tmp->ini)
		goto fail;

	*dst = tmp;
	return TRUE;

fail:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	pf_server_config_free(tmp);
	WINPR_PRAGMA_DIAG_POP
	return FALSE;
}

struct config_plugin_data
{
	proxyPluginsManager* mgr;
	const proxyConfig* config;
};

static const char config_plugin_name[] = "config";
static const char config_plugin_desc[] =
    "A plugin filtering according to proxy configuration file rules";

static BOOL config_plugin_unload(proxyPlugin* plugin)
{
	WINPR_ASSERT(plugin);

	/* Here we have to free up our custom data storage. */
	if (plugin)
	{
		free(plugin->custom);
		plugin->custom = NULL;
	}

	return TRUE;
}

static BOOL config_plugin_keyboard_event(proxyPlugin* plugin, WINPR_ATTR_UNUSED proxyData* pdata,
                                         void* param)
{
	BOOL rc = 0;
	const struct config_plugin_data* custom = NULL;
	const proxyConfig* cfg = NULL;
	const proxyKeyboardEventInfo* event_data = (const proxyKeyboardEventInfo*)(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(event_data);

	WINPR_UNUSED(event_data);

	custom = plugin->custom;
	WINPR_ASSERT(custom);

	cfg = custom->config;
	WINPR_ASSERT(cfg);

	rc = cfg->Keyboard;
	WLog_DBG(TAG, "%s", boolstr(rc));
	return rc;
}

static BOOL config_plugin_unicode_event(proxyPlugin* plugin, WINPR_ATTR_UNUSED proxyData* pdata,
                                        void* param)
{
	BOOL rc = 0;
	const struct config_plugin_data* custom = NULL;
	const proxyConfig* cfg = NULL;
	const proxyUnicodeEventInfo* event_data = (const proxyUnicodeEventInfo*)(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(event_data);

	WINPR_UNUSED(event_data);

	custom = plugin->custom;
	WINPR_ASSERT(custom);

	cfg = custom->config;
	WINPR_ASSERT(cfg);

	rc = cfg->Keyboard;
	WLog_DBG(TAG, "%s", boolstr(rc));
	return rc;
}

static BOOL config_plugin_mouse_event(proxyPlugin* plugin, WINPR_ATTR_UNUSED proxyData* pdata,
                                      void* param)
{
	BOOL rc = 0;
	const struct config_plugin_data* custom = NULL;
	const proxyConfig* cfg = NULL;
	const proxyMouseEventInfo* event_data = (const proxyMouseEventInfo*)(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(event_data);

	WINPR_UNUSED(event_data);

	custom = plugin->custom;
	WINPR_ASSERT(custom);

	cfg = custom->config;
	WINPR_ASSERT(cfg);

	rc = cfg->Mouse;
	return rc;
}

static BOOL config_plugin_mouse_ex_event(proxyPlugin* plugin, WINPR_ATTR_UNUSED proxyData* pdata,
                                         void* param)
{
	BOOL rc = 0;
	const struct config_plugin_data* custom = NULL;
	const proxyConfig* cfg = NULL;
	const proxyMouseExEventInfo* event_data = (const proxyMouseExEventInfo*)(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(event_data);

	WINPR_UNUSED(event_data);

	custom = plugin->custom;
	WINPR_ASSERT(custom);

	cfg = custom->config;
	WINPR_ASSERT(cfg);

	rc = cfg->Mouse;
	return rc;
}

static BOOL config_plugin_client_channel_data(WINPR_ATTR_UNUSED proxyPlugin* plugin,
                                              WINPR_ATTR_UNUSED proxyData* pdata, void* param)
{
	const proxyChannelDataEventInfo* channel = (const proxyChannelDataEventInfo*)(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(channel);

	WLog_DBG(TAG, "%s [0x%04" PRIx16 "] got %" PRIuz, channel->channel_name, channel->channel_id,
	         channel->data_len);
	return TRUE;
}

static BOOL config_plugin_server_channel_data(WINPR_ATTR_UNUSED proxyPlugin* plugin,
                                              WINPR_ATTR_UNUSED proxyData* pdata, void* param)
{
	const proxyChannelDataEventInfo* channel = (const proxyChannelDataEventInfo*)(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(channel);

	WLog_DBG(TAG, "%s [0x%04" PRIx16 "] got %" PRIuz, channel->channel_name, channel->channel_id,
	         channel->data_len);
	return TRUE;
}

static BOOL config_plugin_dynamic_channel_create(proxyPlugin* plugin,
                                                 WINPR_ATTR_UNUSED proxyData* pdata, void* param)
{
	BOOL accept = 0;
	const proxyChannelDataEventInfo* channel = (const proxyChannelDataEventInfo*)(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(channel);

	const struct config_plugin_data* custom = plugin->custom;
	WINPR_ASSERT(custom);

	const proxyConfig* cfg = custom->config;
	WINPR_ASSERT(cfg);

	pf_utils_channel_mode rc = pf_utils_get_channel_mode(cfg, channel->channel_name);
	switch (rc)
	{

		case PF_UTILS_CHANNEL_INTERCEPT:
		case PF_UTILS_CHANNEL_PASSTHROUGH:
			accept = TRUE;
			break;
		case PF_UTILS_CHANNEL_BLOCK:
		default:
			accept = FALSE;
			break;
	}

	if (accept)
	{
		if (strncmp(RDPGFX_DVC_CHANNEL_NAME, channel->channel_name,
		            sizeof(RDPGFX_DVC_CHANNEL_NAME)) == 0)
			accept = cfg->GFX;
		else if (strncmp(RDPSND_DVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(RDPSND_DVC_CHANNEL_NAME)) == 0)
			accept = cfg->AudioOutput;
		else if (strncmp(RDPSND_LOSSY_DVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(RDPSND_LOSSY_DVC_CHANNEL_NAME)) == 0)
			accept = cfg->AudioOutput;
		else if (strncmp(AUDIN_DVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(AUDIN_DVC_CHANNEL_NAME)) == 0)
			accept = cfg->AudioInput;
		else if (strncmp(RDPEI_DVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(RDPEI_DVC_CHANNEL_NAME)) == 0)
			accept = cfg->Multitouch;
		else if (strncmp(TSMF_DVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(TSMF_DVC_CHANNEL_NAME)) == 0)
			accept = cfg->VideoRedirection;
		else if (strncmp(VIDEO_CONTROL_DVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(VIDEO_CONTROL_DVC_CHANNEL_NAME)) == 0)
			accept = cfg->VideoRedirection;
		else if (strncmp(VIDEO_DATA_DVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(VIDEO_DATA_DVC_CHANNEL_NAME)) == 0)
			accept = cfg->VideoRedirection;
		else if (strncmp(RDPECAM_DVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(RDPECAM_DVC_CHANNEL_NAME)) == 0)
			accept = cfg->CameraRedirection;
	}

	WLog_DBG(TAG, "%s [0x%04" PRIx16 "]: %s", channel->channel_name, channel->channel_id,
	         boolstr(accept));
	return accept;
}

static BOOL config_plugin_channel_create(proxyPlugin* plugin, WINPR_ATTR_UNUSED proxyData* pdata,
                                         void* param)
{
	BOOL accept = 0;
	const proxyChannelDataEventInfo* channel = (const proxyChannelDataEventInfo*)(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(channel);

	const struct config_plugin_data* custom = plugin->custom;
	WINPR_ASSERT(custom);

	const proxyConfig* cfg = custom->config;
	WINPR_ASSERT(cfg);

	pf_utils_channel_mode rc = pf_utils_get_channel_mode(cfg, channel->channel_name);
	switch (rc)
	{
		case PF_UTILS_CHANNEL_INTERCEPT:
		case PF_UTILS_CHANNEL_PASSTHROUGH:
			accept = TRUE;
			break;
		case PF_UTILS_CHANNEL_BLOCK:
		default:
			accept = FALSE;
			break;
	}
	if (accept)
	{
		if (strncmp(CLIPRDR_SVC_CHANNEL_NAME, channel->channel_name,
		            sizeof(CLIPRDR_SVC_CHANNEL_NAME)) == 0)
			accept = cfg->Clipboard;
		else if (strncmp(RDPSND_CHANNEL_NAME, channel->channel_name, sizeof(RDPSND_CHANNEL_NAME)) ==
		         0)
			accept = cfg->AudioOutput;
		else if (strncmp(RDPDR_SVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(RDPDR_SVC_CHANNEL_NAME)) == 0)
			accept = cfg->DeviceRedirection;
		else if (strncmp(DISP_DVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(DISP_DVC_CHANNEL_NAME)) == 0)
			accept = cfg->DisplayControl;
		else if (strncmp(RAIL_SVC_CHANNEL_NAME, channel->channel_name,
		                 sizeof(RAIL_SVC_CHANNEL_NAME)) == 0)
			accept = cfg->RemoteApp;
	}

	WLog_DBG(TAG, "%s [static]: %s", channel->channel_name, boolstr(accept));
	return accept;
}

BOOL pf_config_plugin(proxyPluginsManager* plugins_manager, void* userdata)
{
	struct config_plugin_data* custom = NULL;
	proxyPlugin plugin = { 0 };

	plugin.name = config_plugin_name;
	plugin.description = config_plugin_desc;
	plugin.PluginUnload = config_plugin_unload;

	plugin.KeyboardEvent = config_plugin_keyboard_event;
	plugin.UnicodeEvent = config_plugin_unicode_event;
	plugin.MouseEvent = config_plugin_mouse_event;
	plugin.MouseExEvent = config_plugin_mouse_ex_event;
	plugin.ClientChannelData = config_plugin_client_channel_data;
	plugin.ServerChannelData = config_plugin_server_channel_data;
	plugin.ChannelCreate = config_plugin_channel_create;
	plugin.DynamicChannelCreate = config_plugin_dynamic_channel_create;
	plugin.userdata = userdata;

	custom = calloc(1, sizeof(struct config_plugin_data));
	if (!custom)
		return FALSE;

	custom->mgr = plugins_manager;
	custom->config = userdata;

	plugin.custom = custom;
	plugin.userdata = userdata;

	return plugins_manager->RegisterPlugin(plugins_manager, &plugin);
}

const char* pf_config_get(const proxyConfig* config, const char* section, const char* key)
{
	WINPR_ASSERT(config);
	WINPR_ASSERT(config->ini);
	WINPR_ASSERT(section);
	WINPR_ASSERT(key);

	return IniFile_GetKeyValueString(config->ini, section, key);
}
