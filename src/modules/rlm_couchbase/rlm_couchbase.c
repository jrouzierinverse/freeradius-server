/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Integrate FreeRADIUS with the Couchbase document database.
 * @file rlm_couchbase.c
 *
 * @author Aaron Hurt <ahurt@anbcs.com>
 * @copyright 2013-2014 The FreeRADIUS Server Project.
 */

RCSID("$Id$");

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/libradius.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/rad_assert.h>

#include <libcouchbase/couchbase.h>
#include <json.h>

#include "mod.h"
#include "couchbase.h"
#include "jsonc_missing.h"

/**
 * Client Configuration
 */
static const CONF_PARSER client_config[] = {
	{ "view", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_couchbase_t, client_view), "_design/client/_view/by_name" },
	{NULL, -1, 0, NULL, NULL}     /* end the list */
};

/**
 * Module Configuration
 */
static const CONF_PARSER module_config[] = {
	{ "acct_key", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT, rlm_couchbase_t, acct_key), "radacct_%{%{Acct-Unique-Session-Id}:-%{Acct-Session-Id}}" },
	{ "doctype", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_couchbase_t, doctype), "radacct" },
	{ "server", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_REQUIRED, rlm_couchbase_t, server_raw), NULL },
	{ "bucket", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_REQUIRED, rlm_couchbase_t, bucket), NULL },
	{ "password", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_couchbase_t, password), NULL },
	{ "expire", FR_CONF_OFFSET(PW_TYPE_INTEGER, rlm_couchbase_t, expire), 0 },
	{ "user_key", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT, rlm_couchbase_t, user_key), "raduser_%{md5:%{tolower:%{%{Stripped-User-Name}:-%{User-Name}}}}" },
	{ "read_clients", FR_CONF_OFFSET(PW_TYPE_BOOLEAN, rlm_couchbase_t, read_clients), NULL }, /* NULL defaults to "no" */
	{ "client", FR_CONF_POINTER(PW_TYPE_SUBSECTION, NULL), (void const *) client_config },
	{NULL, -1, 0, NULL, NULL}     /* end the list */
};

/** Initialize the rlm_couchbase module
 *
 * Intialize the module and create the initial Couchbase connection pool.
 *
 * @param  conf     The module configuration.
 * @param  instance The module instance.
 * @return          Returns 0 on success, -1 on error.
 */
static int mod_instantiate(CONF_SECTION *conf, void *instance)
{
	static bool version_done;

	rlm_couchbase_t *inst = instance;   /* our module instance */

	if (!version_done) {
		version_done = true;
		INFO("rlm_couchbase: json-c version: %s", json_c_version());
		INFO("rlm_couchbase: libcouchbase version: %s", lcb_get_version(NULL));
	}

	{
		char *server, *p;
		size_t len, i;
		bool sep = false;

		len = talloc_array_length(inst->server_raw);
		server = p = talloc_array(inst, char, len);
		for (i = 0; i < len; i++) {
			switch (inst->server_raw[i]) {
			case '\t':
			case ' ':
			case ',':
				/* Consume multiple separators occurring in sequence */
				if (sep == true) continue;

				sep = true;
				*p++ = ';';
				break;

			default:
				sep = false;
				*p++ = inst->server_raw[i];
				break;
			}
		}

		*p = '\0';
		inst->server = server;
	}

	/* setup item map */
	if (mod_build_attribute_element_map(conf, inst) != 0) {
		/* fail */
		return -1;
	}

	/* initiate connection pool */
	inst->pool = fr_connection_pool_module_init(conf, inst, mod_conn_create, mod_conn_alive, NULL);

	/* check connection pool */
	if (!inst->pool) {
		ERROR("rlm_couchbase: failed to initiate connection pool");
		/* fail */
		return -1;
	}

	/* load clients if requested */
	if (inst->read_clients) {
		CONF_SECTION *cs; /* conf section */

		/* attempt to find client section */
		cs = cf_section_sub_find(conf, "client");
		if (!cs) {
			ERROR("rlm_couchbase: failed to find client section while loading clients");
			/* fail */
			return -1;
		}


		/* attempt to find attribute subsection */
		cs = cf_section_sub_find(cs, "attribute");
		if (!cs) {
			ERROR("rlm_couchbase: failed to find attribute subsection while loading clients");
			/* fail */
			return -1;
		}

		/* debugging */
		DEBUG("rlm_couchbase: preparing to load client documents");

		/* attempt to load clients */
		if (mod_load_client_documents(inst, cs) != 0) {
			/* fail */
			return -1;
		}
	}

	/* return okay */
	return 0;
}

/** Handle authorization requests using Couchbase document data
 *
 * Attempt to fetch the document assocaited with the requested user by
 * using the deterministic key defined in the configuration.  When a valid
 * document is found it will be parsed and the containing value pairs will be
 * injected into the request.
 *
 * @param  instance The module instance.
 * @param  request  The authorization request.
 * @return          Returns operation status (@p rlm_rcode_t).
 */
static rlm_rcode_t mod_authorize(void *instance, REQUEST *request)
{
	rlm_couchbase_t *inst = instance;       /* our module instance */
	void *handle = NULL;                    /* connection pool handle */
	char dockey[MAX_KEY_SIZE];              /* our document key */
	lcb_error_t cb_error = LCB_SUCCESS;     /* couchbase error holder */

	/* assert packet as not null */
	rad_assert(request->packet != NULL);

	/* attempt to build document key */
	if (radius_xlat(dockey, sizeof(dockey), request, inst->user_key, NULL, NULL) < 0) {
		/* log error */
		RERROR("could not find user key attribute (%s) in packet", inst->user_key);
		/* return */
		return RLM_MODULE_FAIL;
	}

	/* get handle */
	handle = fr_connection_get(inst->pool);

	/* check handle */
	if (!handle) return RLM_MODULE_FAIL;

	/* set handle pointer */
	rlm_couchbase_handle_t *handle_t = handle;

	/* set couchbase instance */
	lcb_t cb_inst = handle_t->handle;

	/* set cookie */
	cookie_t *cookie = handle_t->cookie;

	/* check cookie */
	if (cookie) {
		/* clear cookie */
		memset(cookie, 0, sizeof(cookie_t));
	} else {
		/* log error */
		RERROR("cookie not usable - possibly not allocated");
		/* free connection */
		if (handle) {
			fr_connection_release(inst->pool, handle);
		}
		/* return */
		return RLM_MODULE_FAIL;
	}

	/* reset  cookie error status */
	cookie->jerr = json_tokener_success;

	/* fetch document */
	cb_error = couchbase_get_key(cb_inst, cookie, dockey);

	/* check error */
	if (cb_error != LCB_SUCCESS || cookie->jerr != json_tokener_success || cookie->jobj == NULL) {
		/* log error */
		RERROR("failed to fetch document or parse return");
		/* free json object */
		if (cookie->jobj) {
			json_object_put(cookie->jobj);
		}
		/* release handle */
		if (handle) {
			fr_connection_release(inst->pool, handle);
		}
		/* return */
		return RLM_MODULE_FAIL;
	}

	/* debugging */
	RDEBUG("parsed user document == %s", json_object_to_json_string(cookie->jobj));

	/* inject config value pairs defined in this json oblect */
	mod_json_object_to_value_pairs(cookie->jobj, "config", request);

	/* inject reply value pairs defined in this json oblect */
	mod_json_object_to_value_pairs(cookie->jobj, "reply", request);

	/* free json object */
	if (cookie->jobj) {
		json_object_put(cookie->jobj);
	}

	/* release handle */
	if (handle) {
		fr_connection_release(inst->pool, handle);
	}

	/* return okay */
	return RLM_MODULE_OK;
}

/** Write accounting data to Couchbase documents
 *
 * Handle accounting requests and store the associated data into JSON documents
 * in couchbase mapping attribute names to JSON element names per the module configuration.
 *
 * When an existing document already exists for the same accounting section the new attributes
 * will be merged with the currently existing data.  When conflicts arrise the new attribute
 * value will replace or be added to the existing value.
 *
 * @param instance The module instance.
 * @param request  The accounting request object.
 * @return Returns operation status (@p rlm_rcode_t).
 */
static rlm_rcode_t mod_accounting(void *instance, REQUEST *request)
{
	rlm_couchbase_t *inst = instance;   /* our module instance */
	void *handle = NULL;                /* connection pool handle */
	VALUE_PAIR *vp;                     /* radius value pair linked list */
	char dockey[MAX_KEY_SIZE];          /* our document key */
	char document[MAX_VALUE_SIZE];      /* our document body */
	char element[MAX_KEY_SIZE];         /* mapped radius attribute to element name */
	int status = 0;                     /* account status type */
	int docfound = 0;                   /* document found toggle */
	lcb_error_t cb_error = LCB_SUCCESS; /* couchbase error holder */

	/* assert packet as not null */
	rad_assert(request->packet != NULL);

	/* sanity check */
	if ((vp = pairfind(request->packet->vps, PW_ACCT_STATUS_TYPE, 0, TAG_ANY)) == NULL) {
		/* log debug */
		RDEBUG("could not find status type in packet");
		/* return */
		return RLM_MODULE_NOOP;
	}

	/* set status */
	status = vp->vp_integer;

	/* acknowledge the request but take no action */
	if (status == PW_STATUS_ACCOUNTING_ON || status == PW_STATUS_ACCOUNTING_OFF) {
		/* log debug */
		RDEBUG("handling accounting on/off request without action");
		/* return */
		return RLM_MODULE_OK;
	}

	/* get handle */
	handle = fr_connection_get(inst->pool);

	/* check handle */
	if (!handle) return RLM_MODULE_FAIL;

	/* set handle pointer */
	rlm_couchbase_handle_t *handle_t = handle;

	/* set couchbase instance */
	lcb_t cb_inst = handle_t->handle;

	/* set cookie */
	cookie_t *cookie = handle_t->cookie;

	/* check cookie */
	if (cookie) {
		/* clear cookie */
		memset(cookie, 0, sizeof(cookie_t));
	} else {
		/* log error */
		RERROR("cookie not usable - possibly not allocated");
		/* free connection */
		if (handle) {
			fr_connection_release(inst->pool, handle);
		}
		/* return */
		return RLM_MODULE_FAIL;
	}

	/* attempt to build document key */
	if (radius_xlat(dockey, sizeof(dockey), request, inst->acct_key, NULL, NULL) < 0) {
		/* log error */
		RERROR("could not find accounting key attribute (%s) in packet", inst->acct_key);
		/* release handle */
		if (handle) {
			fr_connection_release(inst->pool, handle);
		}
		/* return */
		return RLM_MODULE_NOOP;
	}

	/* init cookie error status */
	cookie->jerr = json_tokener_success;

	/* attempt to fetch document */
	cb_error = couchbase_get_key(cb_inst, cookie, dockey);

	/* check error */
	if (cb_error != LCB_SUCCESS || cookie->jerr != json_tokener_success) {
		/* log error */
		RERROR("failed to execute get request or parse returned json object");
		/* free json object */
		if (cookie->jobj) {
			json_object_put(cookie->jobj);
		}
	} else {
		/* check cookie json object */
		if (cookie->jobj != NULL) {
			/* set doc found */
			docfound = 1;
			/* debugging */
			RDEBUG("parsed json body from couchbase: %s", json_object_to_json_string(cookie->jobj));
		}
	}

	/* start json document if needed */
	if (docfound != 1) {
		/* debugging */
		RDEBUG("document not found - creating new json document");
		/* create new json object */
		cookie->jobj = json_object_new_object();
		/* set 'docType' element for new document */
		json_object_object_add(cookie->jobj, "docType", json_object_new_string(inst->doctype));
		/* set start and stop times ... ensure we always have these elements */
		json_object_object_add(cookie->jobj, "startTimestamp", json_object_new_string("null"));
		json_object_object_add(cookie->jobj, "stopTimestamp", json_object_new_string("null"));
	}

	/* status specific replacements for start/stop time */
	switch (status) {
		case PW_STATUS_START:
			/* add start time */
			if ((vp = pairfind(request->packet->vps, PW_EVENT_TIMESTAMP, 0, TAG_ANY)) != NULL) {
				/* add to json object */
				json_object_object_add(cookie->jobj, "startTimestamp",
						       mod_value_pair_to_json_object(request, vp));
			}
			break;

		case PW_STATUS_STOP:
			/* add stop time */
			if ((vp = pairfind(request->packet->vps, PW_EVENT_TIMESTAMP, 0, TAG_ANY)) != NULL) {
				/* add to json object */
				json_object_object_add(cookie->jobj, "stopTimestamp",
						       mod_value_pair_to_json_object(request, vp));
			}
			/* check start timestamp and adjust if needed */
			mod_ensure_start_timestamp(cookie->jobj, request->packet->vps);
			break;

		case PW_STATUS_ALIVE:
			/* check start timestamp and adjust if needed */
			mod_ensure_start_timestamp(cookie->jobj, request->packet->vps);
			break;

		default:
			/* we shouldn't get here - free json object */
			if (cookie->jobj) {
				json_object_put(cookie->jobj);
			}
			/* release our connection handle */
			if (handle) {
				fr_connection_release(inst->pool, handle);
			}
			/* return without doing anything */
			return RLM_MODULE_NOOP;
	}

	/* loop through pairs and add to json document */
	for (vp = request->packet->vps; vp; vp = vp->next) {
		/* map attribute to element */
		if (mod_attribute_to_element(vp->da->name, inst->map, &element) == 0) {
			/* debug */
			RDEBUG("mapped attribute %s => %s", vp->da->name, element);
			/* add to json object with mapped name */
			json_object_object_add(cookie->jobj, element, mod_value_pair_to_json_object(request, vp));
		}
	}

	/* copy json string to document and check size */
	if (strlcpy(document, json_object_to_json_string(cookie->jobj), sizeof(document)) >= sizeof(document)) {
		/* this isn't good */
		RERROR("could not write json document - insufficient buffer space");
		/* free json output */
		if (cookie->jobj) {
			json_object_put(cookie->jobj);
		}
		/* release handle */
		if (handle) {
			fr_connection_release(inst->pool, handle);
		}
		/* return */
		return RLM_MODULE_FAIL;
	}

	/* free json output */
	if (cookie->jobj) {
		json_object_put(cookie->jobj);
	}

	/* debugging */
	RDEBUG("setting '%s' => '%s'", dockey, document);

	/* store document/key in couchbase */
	cb_error = couchbase_set_key(cb_inst, dockey, document, inst->expire);

	/* check return */
	if (cb_error != LCB_SUCCESS) {
		RERROR("failed to store document (%s): %s (0x%x)", dockey, lcb_strerror(NULL, cb_error), cb_error);
	}

	/* release handle */
	if (handle) {
		fr_connection_release(inst->pool, handle);
	}

	/* return */
	return RLM_MODULE_OK;
}

/** Detach the module
 *
 * Detach the module instance and free any allocated resources.
 *
 * @param  instance The module instance.
 * @return          Returns 0 (success) in all conditions.
 */
static int mod_detach(void *instance)
{
	rlm_couchbase_t *inst = instance;  /* instance struct */

	/* free json object attribute map */
	if (inst->map) {
		json_object_put(inst->map);
	}

	/* destroy connection pool */
	if (inst->pool) {
		fr_connection_pool_delete(inst->pool);
	}

	/* return okay */
	return 0;
}

/*
 * Hook into the FreeRADIUS module system.
 */
module_t rlm_couchbase = {
	RLM_MODULE_INIT,
	"rlm_couchbase",
	RLM_TYPE_THREAD_SAFE,       /* type */
	sizeof(rlm_couchbase_t),
	module_config,
	mod_instantiate,            /* instantiation */
	mod_detach,                 /* detach */
	{
		NULL,                   /* authentication */
		mod_authorize,          /* authorization */
		NULL,                   /* preaccounting */
		mod_accounting,         /* accounting */
		NULL,                   /* checksimul */
		NULL,                   /* pre-proxy */
		NULL,                   /* post-proxy */
		NULL                    /* post-auth */
	},
};
