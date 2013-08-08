/**
 * @file sipe-ucs.c
 *
 * pidgin-sipe
 *
 * Copyright (C) 2013 SIPE Project <http://sipe.sourceforge.net/>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * Implementation for Unified Contact Store [MS-OXWSCOS]
 *  <http://msdn.microsoft.com/en-us/library/jj194130.aspx>
 */

#include <string.h>

#include <glib.h>

#include "sipe-backend.h"
#include "sipe-common.h"
#include "sipe-core.h"
#include "sipe-core-private.h"
#include "sipe-digest.h"
#include "sipe-ews-autodiscover.h"
#include "sipe-http.h"
#include "sipe-ucs.h"
#include "sipe-utils.h"
#include "sipe-xml.h"

typedef void (ucs_callback)(struct sipe_core_private *sipe_private,
			    const sipe_xml *xml,
			    gpointer callback_data);

struct ucs_deferred {
	ucs_callback *cb;
	gpointer cb_data;
	gchar *body;
};

struct ucs_request {
	ucs_callback *cb;
	gpointer cb_data;
	struct sipe_http_request *request;
};

struct sipe_ucs {
	gchar *ews_url;
	GSList *deferred_requests;
	GSList *pending_requests;
	gboolean shutting_down;
};

static void sipe_ucs_deferred_free(struct sipe_core_private *sipe_private,
				   struct ucs_deferred *data)
{
	if (data->cb)
		/* Callback: aborted */
		(*data->cb)(sipe_private, NULL, data->cb);
	g_free(data->body);
	g_free(data);
}

static void sipe_ucs_request_free(struct sipe_core_private *sipe_private,
				  struct ucs_request *data)
{
	if (data->request)
		sipe_http_request_cancel(data->request);
	if (data->cb)
		/* Callback: aborted */
		(*data->cb)(sipe_private, NULL, data->cb);
	g_free(data);
}

static void sipe_ucs_http_response(struct sipe_core_private *sipe_private,
				   guint status,
				   SIPE_UNUSED_PARAMETER GSList *headers,
				   const gchar *body,
				   gpointer callback_data)
{
	struct ucs_request *data = callback_data;
	struct sipe_ucs *ucs = sipe_private->ucs;

	SIPE_DEBUG_INFO("sipe_ucs_http_response: code %d", status);
	data->request = NULL;

	if ((status == SIPE_HTTP_STATUS_OK) && body) {
		sipe_xml *xml = sipe_xml_parse(body, strlen(body));
		/* Callback: success */
		(*data->cb)(sipe_private, xml, data->cb_data);
		sipe_xml_free(xml);
	} else {
		/* Callback: failed */
		(*data->cb)(sipe_private, NULL, data->cb_data);
	}

	/* already been called */
	data->cb = NULL;

	ucs->pending_requests = g_slist_remove(ucs->pending_requests,
					       data);
	sipe_ucs_request_free(sipe_private, data);
}

static gboolean sipe_ucs_http_request(struct sipe_core_private *sipe_private,
				      const gchar *body,
				      ucs_callback *callback,
				      gpointer callback_data)
{
	struct sipe_ucs *ucs = sipe_private->ucs;
	gboolean success = FALSE;

	if (ucs->shutting_down) {
		SIPE_DEBUG_ERROR("sipe_ucs_http_request: new UCS request during shutdown: THIS SHOULD NOT HAPPEN! Debugging information:\n"
				 "Body:   %s\n",
				 body ? body : "<EMPTY>");

	} else if (ucs->ews_url) {
		struct ucs_request *data = g_new0(struct ucs_request, 1);
		gchar *soap = g_strdup_printf("<?xml version=\"1.0\"?>\r\n"
					      "<soap:Envelope"
					      " xmlns:m=\"http://schemas.microsoft.com/exchange/services/2006/messages\""
					      " xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\""
					      " xmlns:t=\"http://schemas.microsoft.com/exchange/services/2006/types\""
					      " >"
					      " <soap:Header>"
					      "  <t:RequestServerVersion Version=\"Exchange2013\" />"
					      " </soap:Header>"
					      " <soap:Body>"
					      "  %s"
					      " </soap:Body>"
					      "</soap:Envelope>",
					      body);
		struct sipe_http_request *request = sipe_http_request_post(sipe_private,
									   ucs->ews_url,
									   NULL,
									   soap,
									   "text/xml; charset=UTF-8",
									   sipe_ucs_http_response,
									   data);
		g_free(soap);

		if (request) {
			data->cb      = callback;
			data->cb_data = callback_data;
			data->request = request;

			ucs->pending_requests = g_slist_prepend(ucs->pending_requests,
								data);

			sipe_core_email_authentication(sipe_private,
						       request);
			sipe_http_request_allow_redirect(request);
			sipe_http_request_ready(request);

			success = TRUE;
		} else {
			SIPE_DEBUG_ERROR_NOFORMAT("sipe_ucs_http_request: failed to create HTTP connection");
			g_free(data);
		}

	} else {
		struct ucs_deferred *data = g_new0(struct ucs_deferred, 1);
		data->cb      = callback;
		data->cb_data = callback_data;
		data->body    = g_strdup(body);

		ucs->deferred_requests = g_slist_prepend(ucs->deferred_requests,
							 data);
		success = TRUE;
	}

	return(success);
}

static void sipe_ucs_get_user_photo_response(struct sipe_core_private *sipe_private,
					     const sipe_xml *xml,
					     gpointer callback_data)
{
	gchar *uri = callback_data;
	const sipe_xml *node = sipe_xml_child(xml,
					      "Body/GetUserPhotoResponse/PictureData");

	if (node) {
		gchar *base64;
		gsize photo_size;
		guchar *photo;
		guchar digest[SIPE_DIGEST_SHA1_LENGTH];
		gchar *digest_string;

		/* decode photo data */
		base64 = sipe_xml_data(node);
		photo = g_base64_decode(base64, &photo_size);
		g_free(base64);

		/* EWS doesn't provide a hash -> calculate SHA-1 digest */
		sipe_digest_sha1(photo, photo_size, digest);
		digest_string = buff_to_hex_str(digest,
						SIPE_DIGEST_SHA1_LENGTH);

		/* backend frees "photo" */
		sipe_backend_buddy_set_photo(SIPE_CORE_PUBLIC,
					     uri,
					     photo,
					     photo_size,
					     digest_string);
		g_free(digest_string);
	}

	g_free(uri);
}

void sipe_ucs_get_photo(struct sipe_core_private *sipe_private,
			const gchar *uri)
{
	gchar *payload = g_strdup(uri);
	gchar *body = g_strdup_printf("<m:GetUserPhoto>"
				      " <m:Email>%s</m:Email>"
				      " <m:SizeRequested>HR48x48</m:SizeRequested>"
				      "</m:GetUserPhoto>",
				      sipe_get_no_sip_uri(uri));

	if (!sipe_ucs_http_request(sipe_private,
				   body,
				   sipe_ucs_get_user_photo_response,
				   payload))
		g_free(payload);

	g_free(body);
}

static void sipe_ucs_get_im_item_list_response(struct sipe_core_private *sipe_private,
					       const sipe_xml *xml,
					     gpointer callback_data)
{
	/* temporary */
	(void)sipe_private;
	(void)xml;
	(void)callback_data;
}

static void ucs_ews_autodiscover_cb(struct sipe_core_private *sipe_private,
				    const struct sipe_ews_autodiscover_data *ews_data,
				    SIPE_UNUSED_PARAMETER gpointer callback_data)
{
	struct sipe_ucs *ucs = sipe_private->ucs;
	const gchar *ews_url = ews_data->ews_url;

	if (!ucs)
		return;

	if (is_empty(ews_url)) {
		SIPE_DEBUG_ERROR_NOFORMAT("ucs_ews_autodiscover_cb: can't detect EWS URL, contact list operations will not work!");
		return;
	}

	SIPE_DEBUG_INFO("ucs_ews_autodiscover_cb: EWS URL '%s'", ews_url);
	ucs->ews_url = g_strdup(ews_url);

	sipe_ucs_http_request(sipe_private,
			      "<m:GetImItemList/>",
			      sipe_ucs_get_im_item_list_response,
			      NULL);

	/* EWS URL is valid, send all deferred requests now */
	if (ucs->deferred_requests) {
		GSList *entry = ucs->deferred_requests;
		while (entry) {
			struct ucs_deferred *data = entry->data;

			sipe_ucs_http_request(sipe_private,
					      data->body,
					      data->cb,
					      data->cb_data);

			/* callback & data has been forwarded */
			data->cb = NULL;
			sipe_ucs_deferred_free(sipe_private, data);

			entry = entry->next;
		}
		g_slist_free(ucs->deferred_requests);
		ucs->deferred_requests = NULL;
	}
}

void sipe_ucs_init(struct sipe_core_private *sipe_private)
{
	if (sipe_private->ucs)
		return;

	sipe_private->ucs = g_new0(struct sipe_ucs, 1);

	sipe_ews_autodiscover_start(sipe_private,
				    ucs_ews_autodiscover_cb,
				    NULL);
}

void sipe_ucs_free(struct sipe_core_private *sipe_private)
{
	struct sipe_ucs *ucs = sipe_private->ucs;

	if (!ucs)
		return;

	/* UCS stack is shutting down: reject all new requests */
	ucs->shutting_down = TRUE;

	if (ucs->deferred_requests) {
		GSList *entry = ucs->deferred_requests;
		while (entry) {
			sipe_ucs_deferred_free(sipe_private, entry->data);
			entry = entry->next;
		}
		g_slist_free(ucs->deferred_requests);
	}

	if (ucs->pending_requests) {
		GSList *entry = ucs->pending_requests;
		while (entry) {
			sipe_ucs_request_free(sipe_private, entry->data);
			entry = entry->next;
		}
		g_slist_free(ucs->pending_requests);
	}

	g_free(ucs->ews_url);
	g_free(ucs);
	sipe_private->ucs = NULL;
}

/*
  Local Variables:
  mode: c
  c-file-style: "bsd"
  indent-tabs-mode: t
  tab-width: 8
  End:
*/