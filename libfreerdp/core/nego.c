/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RDP Protocol Security Negotiation
 *
 * Copyright 2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include <freerdp/constants.h>
#include <freerdp/utils/memory.h>
#include <freerdp/utils/unicode.h>

#include "tpkt.h"

#include "nego.h"

#include "transport.h"

static const char* const NEGO_STATE_STRINGS[] =
{
	"NEGO_STATE_INITIAL",
	"NEGO_STATE_NLA",
	"NEGO_STATE_TLS",
	"NEGO_STATE_RDP",
	"NEGO_STATE_FAIL",
	"NEGO_STATE_FINAL"
};

static const char PROTOCOL_SECURITY_STRINGS[3][4] =
{
	"RDP",
	"TLS",
	"NLA"
};

BOOL nego_security_connect(rdpNego* nego);

/**
 * Negotiate protocol security and connect.
 * @param nego
 * @return
 */

BOOL nego_connect(rdpNego* nego)
{
	if (nego->state == NEGO_STATE_INITIAL)
	{
		if (nego->enabled_protocols[PROTOCOL_NLA] > 0)
			nego->state = NEGO_STATE_NLA;
		else if (nego->enabled_protocols[PROTOCOL_TLS] > 0)
			nego->state = NEGO_STATE_TLS;
		else if (nego->enabled_protocols[PROTOCOL_RDP] > 0)
			nego->state = NEGO_STATE_RDP;
		else
		{
			DEBUG_NEGO("No security protocol is enabled");
			nego->state = NEGO_STATE_FAIL;
		}

		if (!nego->security_layer_negotiation_enabled)
		{
			DEBUG_NEGO("Security Layer Negotiation is disabled");
			/* attempt only the highest enabled protocol (see nego_attempt_*) */
			nego->enabled_protocols[PROTOCOL_NLA] = 0;
			nego->enabled_protocols[PROTOCOL_TLS] = 0;
			nego->enabled_protocols[PROTOCOL_RDP] = 0;
			if(nego->state == NEGO_STATE_NLA)
			{
				nego->enabled_protocols[PROTOCOL_NLA] = 1;
				nego->selected_protocol = PROTOCOL_NLA;
			}
			else if (nego->state == NEGO_STATE_TLS)
			{
				nego->enabled_protocols[PROTOCOL_TLS] = 1;
				nego->selected_protocol = PROTOCOL_TLS;
			}
			else if (nego->state == NEGO_STATE_RDP)
			{
				nego->enabled_protocols[PROTOCOL_RDP] = 1;
				nego->selected_protocol = PROTOCOL_RDP;
			}
		}

		if(!nego_send_preconnection_pdu(nego))
		{
			DEBUG_NEGO("Failed to send preconnection information");
			nego->state = NEGO_STATE_FINAL;
			return FALSE;
		}
	}

	do
	{
		DEBUG_NEGO("state: %s", NEGO_STATE_STRINGS[nego->state]);

		nego_send(nego);

		if (nego->state == NEGO_STATE_FAIL)
		{
			DEBUG_NEGO("Protocol Security Negotiation Failure");
			nego->state = NEGO_STATE_FINAL;
			return FALSE;
		}
	}
	while (nego->state != NEGO_STATE_FINAL);

	DEBUG_NEGO("Negotiated %s security", PROTOCOL_SECURITY_STRINGS[nego->selected_protocol]);

	/* update settings with negotiated protocol security */
	nego->transport->settings->requested_protocols = nego->requested_protocols;
	nego->transport->settings->selected_protocol = nego->selected_protocol;
	nego->transport->settings->negotiationFlags = nego->flags;

	if(nego->selected_protocol == PROTOCOL_RDP)
	{
		nego->transport->settings->encryption = TRUE;
		nego->transport->settings->encryption_method = ENCRYPTION_METHOD_40BIT | ENCRYPTION_METHOD_128BIT | ENCRYPTION_METHOD_FIPS;
		nego->transport->settings->encryption_level = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;
	}

	/* finally connect security layer (if not already done) */
	if(!nego_security_connect(nego))
	{
		DEBUG_NEGO("Failed to connect with %s security", PROTOCOL_SECURITY_STRINGS[nego->selected_protocol]);
		return FALSE;
	}

	return TRUE;
}

/* connect to selected security layer */
BOOL nego_security_connect(rdpNego* nego)
{
	if (!nego->tcp_connected)
	{
		nego->security_connected = FALSE;
	}
	else if (!nego->security_connected)
	{
		if (nego->selected_protocol == PROTOCOL_NLA)
		{
			DEBUG_NEGO("nego_security_connect with PROTOCOL_NLA");
			nego->security_connected = transport_connect_nla(nego->transport);
		}
		else if (nego->selected_protocol == PROTOCOL_TLS)
		{
			DEBUG_NEGO("nego_security_connect with PROTOCOL_TLS");
			nego->security_connected = transport_connect_tls(nego->transport);
		}
		else if (nego->selected_protocol == PROTOCOL_RDP)
		{
			DEBUG_NEGO("nego_security_connect with PROTOCOL_RDP");
			nego->security_connected = transport_connect_rdp(nego->transport);
		}
		else
		{
			DEBUG_NEGO("cannot connect security layer because no protocol has been selected yet.");
		}
	}

	return nego->security_connected;
}

/**
 * Connect TCP layer.
 * @param nego
 * @return
 */

BOOL nego_tcp_connect(rdpNego* nego)
{
	if (!nego->tcp_connected)
		nego->tcp_connected = transport_connect(nego->transport, nego->hostname, nego->port);

	return nego->tcp_connected;
}

/**
 * Connect TCP layer. For direct approach, connect security layer as well.
 * @param nego
 * @return
 */

BOOL nego_transport_connect(rdpNego* nego)
{
	nego_tcp_connect(nego);

	if (nego->tcp_connected && !nego->security_layer_negotiation_enabled)
		return nego_security_connect(nego);

	return nego->tcp_connected;
}

/**
 * Disconnect TCP layer.
 * @param nego
 * @return
 */

int nego_transport_disconnect(rdpNego* nego)
{
	if (nego->tcp_connected)
		transport_disconnect(nego->transport);

	nego->tcp_connected = 0;
	nego->security_connected = 0;

	return 1;
}

/**
 * Send preconnection information if enabled.
 * @param nego
 * @return
 */

BOOL nego_send_preconnection_pdu(rdpNego* nego)
{
	STREAM* s;
	UINT32 cbSize;
	UINT16 cchPCB = 0;
	WCHAR* wszPCB = NULL;

	if (!nego->send_preconnection_pdu)
		return TRUE;

	DEBUG_NEGO("Sending preconnection PDU");

	if (!nego_tcp_connect(nego))
		return FALSE;

	/* it's easier to always send the version 2 PDU, and it's just 2 bytes overhead */
	cbSize = PRECONNECTION_PDU_V2_MIN_SIZE;

	if (nego->preconnection_blob)
	{
		cchPCB = (UINT16) freerdp_AsciiToUnicodeAlloc(nego->preconnection_blob, &wszPCB, 0);
		cchPCB += 1; /* zero-termination */
		cbSize += cchPCB * 2;
	}

	s = transport_send_stream_init(nego->transport, cbSize);
	stream_write_UINT32(s, cbSize); /* cbSize */
	stream_write_UINT32(s, 0); /* Flags */
	stream_write_UINT32(s, PRECONNECTION_PDU_V2); /* Version */
	stream_write_UINT32(s, nego->preconnection_id); /* Id */
	stream_write_UINT16(s, cchPCB); /* cchPCB */

	if (wszPCB)
	{
		stream_write(s, wszPCB, cchPCB * 2); /* wszPCB */
		free(wszPCB);
	}

	if (transport_write(nego->transport, s) < 0)
		return FALSE;

	return TRUE;
}

/**
 * Attempt negotiating NLA + TLS security.
 * @param nego
 */

void nego_attempt_nla(rdpNego* nego)
{
	nego->requested_protocols = PROTOCOL_NLA | PROTOCOL_TLS;

	DEBUG_NEGO("Attempting NLA security");

	if (!nego_transport_connect(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_send_negotiation_request(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_recv_response(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	DEBUG_NEGO("state: %s", NEGO_STATE_STRINGS[nego->state]);
	if (nego->state != NEGO_STATE_FINAL)
	{
		nego_transport_disconnect(nego);

		if (nego->enabled_protocols[PROTOCOL_TLS] > 0)
			nego->state = NEGO_STATE_TLS;
		else if (nego->enabled_protocols[PROTOCOL_RDP] > 0)
			nego->state = NEGO_STATE_RDP;
		else
			nego->state = NEGO_STATE_FAIL;
	}
}

/**
 * Attempt negotiating TLS security.
 * @param nego
 */

void nego_attempt_tls(rdpNego* nego)
{
	nego->requested_protocols = PROTOCOL_TLS;

	DEBUG_NEGO("Attempting TLS security");

	if (!nego_transport_connect(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_send_negotiation_request(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_recv_response(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (nego->state != NEGO_STATE_FINAL)
	{
		nego_transport_disconnect(nego);

		if (nego->enabled_protocols[PROTOCOL_RDP] > 0)
			nego->state = NEGO_STATE_RDP;
		else
			nego->state = NEGO_STATE_FAIL;
	}
}

/**
 * Attempt negotiating standard RDP security.
 * @param nego
 */

void nego_attempt_rdp(rdpNego* nego)
{
	nego->requested_protocols = PROTOCOL_RDP;

	DEBUG_NEGO("Attempting RDP security");

	if (!nego_transport_connect(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_send_negotiation_request(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_recv_response(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}
}

/**
 * Wait to receive a negotiation response
 * @param nego
 */

BOOL nego_recv_response(rdpNego* nego)
{
	STREAM* s = transport_recv_stream_init(nego->transport, 1024);

	if (transport_read(nego->transport, s) < 0)
		return FALSE;

	return nego_recv(nego->transport, s, nego);
}

/**
 * Receive protocol security negotiation message.\n
 * @msdn{cc240501}
 * @param transport transport
 * @param s stream
 * @param extra nego pointer
 */

BOOL nego_recv(rdpTransport* transport, STREAM* s, void* extra)
{
	BYTE li;
	BYTE type;
	rdpNego* nego = (rdpNego*) extra;

	if (tpkt_read_header(s) == 0)
		return FALSE;

	li = tpdu_read_connection_confirm(s);

	if (li > 6)
	{
		/* rdpNegData (optional) */

		stream_read_BYTE(s, type); /* Type */

		switch (type)
		{
			case TYPE_RDP_NEG_RSP:
				nego_process_negotiation_response(nego, s);

				DEBUG_NEGO("selected_protocol: %d", nego->selected_protocol);

				/* enhanced security selected ? */

				if (nego->selected_protocol)
				{
					if ((nego->selected_protocol == PROTOCOL_NLA) &&
							(!nego->enabled_protocols[PROTOCOL_NLA]))
					{
						nego->state = NEGO_STATE_FAIL;
					}
					if ((nego->selected_protocol == PROTOCOL_TLS) &&
						(!nego->enabled_protocols[PROTOCOL_TLS]))
					{
						nego->state = NEGO_STATE_FAIL;
					}
				}
				else if (!nego->enabled_protocols[PROTOCOL_RDP])
				{
					nego->state = NEGO_STATE_FAIL;
				}
				break;

			case TYPE_RDP_NEG_FAILURE:
				nego_process_negotiation_failure(nego, s);
				break;
		}
	}
	else
	{
		DEBUG_NEGO("no rdpNegData");

		if (!nego->enabled_protocols[PROTOCOL_RDP])
			nego->state = NEGO_STATE_FAIL;
		else
			nego->state = NEGO_STATE_FINAL;
	}

	return TRUE;
}

/**
 * Read protocol security negotiation request message.\n
 * @param nego
 * @param s stream
 */

BOOL nego_read_request(rdpNego* nego, STREAM* s)
{
	BYTE li;
	BYTE c;
	BYTE type;

	tpkt_read_header(s);
	li = tpdu_read_connection_request(s);

	if (li != stream_get_left(s) + 6)
	{
		printf("Incorrect TPDU length indicator.\n");
		return FALSE;
	}

	if (stream_get_left(s) > 8)
	{
		/* Optional routingToken or cookie, ending with CR+LF */
		while (stream_get_left(s) > 0)
		{
			stream_read_BYTE(s, c);

			if (c != '\x0D')
				continue;

			stream_peek_BYTE(s, c);

			if (c != '\x0A')
				continue;

			stream_seek_BYTE(s);
			break;
		}
	}

	if (stream_get_left(s) >= 8)
	{
		/* rdpNegData (optional) */

		stream_read_BYTE(s, type); /* Type */

		if (type != TYPE_RDP_NEG_REQ)
		{
			printf("Incorrect negotiation request type %d\n", type);
			return FALSE;
		}

		nego_process_negotiation_request(nego, s);
	}

	return TRUE;
}

/**
 * Send protocol security negotiation message.
 * @param nego
 */

void nego_send(rdpNego* nego)
{
	if (nego->state == NEGO_STATE_NLA)
		nego_attempt_nla(nego);
	else if (nego->state == NEGO_STATE_TLS)
		nego_attempt_tls(nego);
	else if (nego->state == NEGO_STATE_RDP)
		nego_attempt_rdp(nego);
	else
		DEBUG_NEGO("invalid negotiation state for sending");
}

/**
 * Send RDP Negotiation Request (RDP_NEG_REQ).\n
 * @msdn{cc240500}\n
 * @msdn{cc240470}
 * @param nego
 */

BOOL nego_send_negotiation_request(rdpNego* nego)
{
	STREAM* s;
	int length;
	BYTE *bm, *em;
	int cookie_length;

	s = transport_send_stream_init(nego->transport, 256);
	length = TPDU_CONNECTION_REQUEST_LENGTH;
	stream_get_mark(s, bm);
	stream_seek(s, length);

	if (nego->RoutingToken != NULL)
	{
		stream_write(s, nego->RoutingToken, nego->RoutingTokenLength);
		length += nego->RoutingTokenLength;
	}
	else if (nego->cookie != NULL)
	{
		cookie_length = strlen(nego->cookie);

		if (cookie_length > (int) nego->cookie_max_length)
			cookie_length = nego->cookie_max_length;

		stream_write(s, "Cookie: mstshash=", 17);
		stream_write(s, (BYTE*) nego->cookie, cookie_length);
		stream_write_BYTE(s, 0x0D); /* CR */
		stream_write_BYTE(s, 0x0A); /* LF */
		length += cookie_length + 19;
	}

	DEBUG_NEGO("requested_protocols: %d", nego->requested_protocols);

	if (nego->requested_protocols > PROTOCOL_RDP)
	{
		/* RDP_NEG_DATA must be present for TLS and NLA */
		stream_write_BYTE(s, TYPE_RDP_NEG_REQ);
		stream_write_BYTE(s, 0); /* flags, must be set to zero */
		stream_write_UINT16(s, 8); /* RDP_NEG_DATA length (8) */
		stream_write_UINT32(s, nego->requested_protocols); /* requestedProtocols */
		length += 8;
	}

	stream_get_mark(s, em);
	stream_set_mark(s, bm);
	tpkt_write_header(s, length);
	tpdu_write_connection_request(s, length - 5);
	stream_set_mark(s, em);

	if (transport_write(nego->transport, s) < 0)
		return FALSE;

	return TRUE;
}

/**
 * Process Negotiation Request from Connection Request message.
 * @param nego
 * @param s
 */

void nego_process_negotiation_request(rdpNego* nego, STREAM* s)
{
	BYTE flags;
	UINT16 length;

	DEBUG_NEGO("RDP_NEG_REQ");

	stream_read_BYTE(s, flags);
	stream_read_UINT16(s, length);
	stream_read_UINT32(s, nego->requested_protocols);

	DEBUG_NEGO("requested_protocols: %d", nego->requested_protocols);

	nego->state = NEGO_STATE_FINAL;
}

/**
 * Process Negotiation Response from Connection Confirm message.
 * @param nego
 * @param s
 */

void nego_process_negotiation_response(rdpNego* nego, STREAM* s)
{
	UINT16 length;

	DEBUG_NEGO("RDP_NEG_RSP");

	stream_read_BYTE(s, nego->flags);
	stream_read_UINT16(s, length);
	stream_read_UINT32(s, nego->selected_protocol);

	nego->state = NEGO_STATE_FINAL;
}

/**
 * Process Negotiation Failure from Connection Confirm message.
 * @param nego
 * @param s
 */

void nego_process_negotiation_failure(rdpNego* nego, STREAM* s)
{
	BYTE flags;
	UINT16 length;
	UINT32 failureCode;

	DEBUG_NEGO("RDP_NEG_FAILURE");

	stream_read_BYTE(s, flags);
	stream_read_UINT16(s, length);
	stream_read_UINT32(s, failureCode);

	switch (failureCode)
	{
		case SSL_REQUIRED_BY_SERVER:
			DEBUG_NEGO("Error: SSL_REQUIRED_BY_SERVER");
			break;
		case SSL_NOT_ALLOWED_BY_SERVER:
			DEBUG_NEGO("Error: SSL_NOT_ALLOWED_BY_SERVER");
			break;
		case SSL_CERT_NOT_ON_SERVER:
			DEBUG_NEGO("Error: SSL_CERT_NOT_ON_SERVER");
			break;
		case INCONSISTENT_FLAGS:
			DEBUG_NEGO("Error: INCONSISTENT_FLAGS");
			break;
		case HYBRID_REQUIRED_BY_SERVER:
			DEBUG_NEGO("Error: HYBRID_REQUIRED_BY_SERVER");
			break;
		default:
			DEBUG_NEGO("Error: Unknown protocol security error %d", failureCode);
			break;
	}

	nego->state = NEGO_STATE_FAIL;
}

/**
 * Send RDP Negotiation Response (RDP_NEG_RSP).\n
 * @param nego
 */

BOOL nego_send_negotiation_response(rdpNego* nego)
{
	STREAM* s;
	BYTE* bm;
	BYTE* em;
	int length;
	BOOL status;
	rdpSettings* settings;

	status = TRUE;
	settings = nego->transport->settings;

	s = transport_send_stream_init(nego->transport, 256);
	length = TPDU_CONNECTION_CONFIRM_LENGTH;
	stream_get_mark(s, bm);
	stream_seek(s, length);

	if (nego->selected_protocol > PROTOCOL_RDP)
	{
		/* RDP_NEG_DATA must be present for TLS and NLA */
		stream_write_BYTE(s, TYPE_RDP_NEG_RSP);
		stream_write_BYTE(s, EXTENDED_CLIENT_DATA_SUPPORTED); /* flags */
		stream_write_UINT16(s, 8); /* RDP_NEG_DATA length (8) */
		stream_write_UINT32(s, nego->selected_protocol); /* selectedProtocol */
		length += 8;
	}
	else if (!settings->rdp_security)
	{
		stream_write_BYTE(s, TYPE_RDP_NEG_FAILURE);
		stream_write_BYTE(s, 0); /* flags */
		stream_write_UINT16(s, 8); /* RDP_NEG_DATA length (8) */
		/*
		 * TODO: Check for other possibilities,
		 *       like SSL_NOT_ALLOWED_BY_SERVER.
		 */
		printf("nego_send_negotiation_response: client supports only Standard RDP Security\n");
		stream_write_UINT32(s, SSL_REQUIRED_BY_SERVER);
		length += 8;
		status = FALSE;
	}

	stream_get_mark(s, em);
	stream_set_mark(s, bm);
	tpkt_write_header(s, length);
	tpdu_write_connection_confirm(s, length - 5);
	stream_set_mark(s, em);

	if (transport_write(nego->transport, s) < 0)
		return FALSE;

	if (status)
	{
		/* update settings with negotiated protocol security */
		settings->requested_protocols = nego->requested_protocols;
		settings->selected_protocol = nego->selected_protocol;

		if (settings->selected_protocol == PROTOCOL_RDP)
		{
			settings->tls_security = FALSE;
			settings->nla_security = FALSE;
			settings->rdp_security = TRUE;

			if (!settings->local)
			{
				settings->encryption = TRUE;
				settings->encryption_method = ENCRYPTION_METHOD_40BIT | ENCRYPTION_METHOD_128BIT | ENCRYPTION_METHOD_FIPS;
				settings->encryption_level = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;
			}

			if (settings->encryption && settings->server_key == NULL && settings->rdp_key_file == NULL)
				return FALSE;
		}
		else if (settings->selected_protocol == PROTOCOL_TLS)
		{
			settings->tls_security = TRUE;
			settings->nla_security = FALSE;
			settings->rdp_security = FALSE;
			settings->encryption = FALSE;
			settings->encryption_method = ENCRYPTION_METHOD_NONE;
			settings->encryption_level = ENCRYPTION_LEVEL_NONE;
		}
		else if (settings->selected_protocol == PROTOCOL_NLA)
		{
			settings->tls_security = TRUE;
			settings->nla_security = TRUE;
			settings->rdp_security = FALSE;
			settings->encryption = FALSE;
			settings->encryption_method = ENCRYPTION_METHOD_NONE;
			settings->encryption_level = ENCRYPTION_LEVEL_NONE;
		}
	}

	return status;
}

/**
 * Initialize NEGO state machine.
 * @param nego
 */

void nego_init(rdpNego* nego)
{
	nego->state = NEGO_STATE_INITIAL;
	nego->requested_protocols = PROTOCOL_RDP;
	nego->transport->recv_callback = nego_recv;
	nego->transport->recv_extra = (void*) nego;
	nego->cookie_max_length = DEFAULT_COOKIE_MAX_LENGTH;
	nego->flags = 0;
}

/**
 * Create a new NEGO state machine instance.
 * @param transport
 * @return
 */

rdpNego* nego_new(struct rdp_transport * transport)
{
	rdpNego* nego = (rdpNego*) xzalloc(sizeof(rdpNego));

	if (nego != NULL)
	{
		nego->transport = transport;
		nego_init(nego);
	}

	return nego;
}

/**
 * Free NEGO state machine.
 * @param nego
 */

void nego_free(rdpNego* nego)
{
	free(nego);
}

/**
 * Set target hostname and port.
 * @param nego
 * @param hostname
 * @param port
 */

void nego_set_target(rdpNego* nego, char* hostname, int port)
{
	nego->hostname = hostname;
	nego->port = port;
}

/**
 * Enable security layer negotiation.
 * @param nego pointer to the negotiation structure
 * @param enable_rdp whether to enable security layer negotiation (TRUE for enabled, FALSE for disabled)
 */

void nego_set_negotiation_enabled(rdpNego* nego, BOOL security_layer_negotiation_enabled)
{
	DEBUG_NEGO("Enabling security layer negotiation: %s", security_layer_negotiation_enabled ? "TRUE" : "FALSE");
	nego->security_layer_negotiation_enabled = security_layer_negotiation_enabled;
}

/**
 * Enable RDP security protocol.
 * @param nego pointer to the negotiation structure
 * @param enable_rdp whether to enable normal RDP protocol (TRUE for enabled, FALSE for disabled)
 */

void nego_enable_rdp(rdpNego* nego, BOOL enable_rdp)
{
	DEBUG_NEGO("Enabling RDP security: %s", enable_rdp ? "TRUE" : "FALSE");
	nego->enabled_protocols[PROTOCOL_RDP] = enable_rdp;
}

/**
 * Enable TLS security protocol.
 * @param nego pointer to the negotiation structure
 * @param enable_tls whether to enable TLS + RDP protocol (TRUE for enabled, FALSE for disabled)
 */
void nego_enable_tls(rdpNego* nego, BOOL enable_tls)
{
	DEBUG_NEGO("Enabling TLS security: %s", enable_tls ? "TRUE" : "FALSE");
	nego->enabled_protocols[PROTOCOL_TLS] = enable_tls;
}


/**
 * Enable NLA security protocol.
 * @param nego pointer to the negotiation structure
 * @param enable_nla whether to enable network level authentication protocol (TRUE for enabled, FALSE for disabled)
 */

void nego_enable_nla(rdpNego* nego, BOOL enable_nla)
{
	DEBUG_NEGO("Enabling NLA security: %s", enable_nla ? "TRUE" : "FALSE");
	nego->enabled_protocols[PROTOCOL_NLA] = enable_nla;
}

/**
 * Set routing token.
 * @param nego
 * @param RoutingToken
 * @param RoutingTokenLength
 */

void nego_set_routing_token(rdpNego* nego, BYTE* RoutingToken, DWORD RoutingTokenLength)
{
	nego->RoutingToken = RoutingToken;
	nego->RoutingTokenLength = RoutingTokenLength;
}

/**
 * Set cookie.
 * @param nego
 * @param cookie
 */

void nego_set_cookie(rdpNego* nego, char* cookie)
{
	nego->cookie = cookie;
}

/**
 * Set cookie maximum length
 * @param nego
 * @param cookie_max_length
 */

void nego_set_cookie_max_length(rdpNego* nego, UINT32 cookie_max_length)
{
	nego->cookie_max_length = cookie_max_length;
}

/**
 * Enable / disable preconnection PDU.
 * @param nego
 * @param send_pcpdu
 */

void nego_set_send_preconnection_pdu(rdpNego* nego, BOOL send_pcpdu)
{
	nego->send_preconnection_pdu = send_pcpdu;
}

/**
 * Set preconnection id.
 * @param nego
 * @param id
 */

void nego_set_preconnection_id(rdpNego* nego, UINT32 id)
{
	nego->preconnection_id = id;
}

/**
 * Set preconnection blob.
 * @param nego
 * @param blob
 */

void nego_set_preconnection_blob(rdpNego* nego, char* blob)
{
	nego->preconnection_blob = blob;
}
