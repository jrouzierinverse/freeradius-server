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
 * @brief VALUE_PAIR template functions
 * @file main/tmpl.c
 *
 * @ingroup AVP
 *
 * @copyright 2014 The FreeRADIUS server project
 */
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/rad_assert.h>

#include <ctype.h>

const FR_NAME_NUMBER pair_lists[] = {
	{ "request",		PAIR_LIST_REQUEST },
	{ "reply",		PAIR_LIST_REPLY },
	{ "control",		PAIR_LIST_CONTROL },		/* New name should have priority */
	{ "config",		PAIR_LIST_CONTROL },
#ifdef WITH_PROXY
	{ "proxy-request",	PAIR_LIST_PROXY_REQUEST },
	{ "proxy-reply",	PAIR_LIST_PROXY_REPLY },
#endif
#ifdef WITH_COA
	{ "coa",		PAIR_LIST_COA },
	{ "coa-reply",		PAIR_LIST_COA_REPLY },
	{ "disconnect",		PAIR_LIST_DM },
	{ "disconnect-reply",	PAIR_LIST_DM_REPLY },
#endif
	{  NULL , -1 }
};

const FR_NAME_NUMBER request_refs[] = {
	{ "outer",		REQUEST_OUTER },
	{ "current",		REQUEST_CURRENT },
	{ "parent",		REQUEST_PARENT },
	{  NULL , -1 }
};

/** Resolve attribute name to a list.
 *
 * Check the name string for qualifiers that specify a list and return
 * an pair_lists_t value for that list. This value may be passed to
 * radius_list, along with the current request, to get a pointer to the
 * actual list in the request.
 *
 * If qualifiers were consumed, write a new pointer into name to the
 * char after the last qualifier to be consumed.
 *
 * radius_list_name should be called before passing a name string that
 * may contain qualifiers to dict_attrbyname.
 *
 * @see dict_attrbyname
 *
 * @param[in,out] name of attribute.
 * @param[in] default_list the list to return if no qualifiers were found.
 * @return PAIR_LIST_UNKOWN if qualifiers couldn't be resolved to a list.
 */
pair_lists_t radius_list_name(char const **name, pair_lists_t default_list)
{
	char const *p = *name;
	char const *q;
	pair_lists_t output;

	/* This should never be a NULL pointer or zero length string */
	rad_assert(name && *name);

	/*
	 *	Unfortunately, ':' isn't a definitive separator for
	 *	the list name.  We may have numeric tags, too.
	 */
	q = strchr(p, ':');
	if (q) {
		/*
		 *	Check for tagged attributes.  They have
		 *	"name:tag", where tag is a decimal number.
		 *	Valid tags are invalid attributes, so that's
		 *	OK.
		 *
		 *	Also allow "name:tag[#]" as a tag.
		 *
		 *	However, "request:" is allowed, too, and
		 *	shouldn't be interpreted as a tag.
		 *
		 *	We do this check first rather than just
		 *	looking up the request name, because this
		 *	check is cheap, and looking up the request
		 *	name is expensive.
		 */
		if (isdigit((int) q[1])) {
			char const *d = q + 1;

			while (isdigit((int) *d)) {
				d++;
			}

			/*
			 *	Return the DEFAULT list as supplied by
			 *	the caller.  This is usually
			 *	PAIRLIST_REQUEST.
			 */
			if (!*d || (*d == '[')) {
				return default_list;
			}
		}

		/*
		 *	If the first part is a list name, then treat
		 *	it as a list.  This means that we CANNOT have
		 *	an attribute which is named "request",
		 *	"reply", etc.  Allowing a tagged attribute
		 *	"request:3" would just be insane.
		 */
		output = fr_substr2int(pair_lists, p, PAIR_LIST_UNKNOWN, (q - p));
		if (output != PAIR_LIST_UNKNOWN) {
			*name = (q + 1);	/* Consume the list and delimiter */
			return output;
		}

		/*
		 *	It's not a known list, say so.
		 */
		return PAIR_LIST_UNKNOWN;
	}

	/*
	 *	The input string may be just a list name,
	 *	e.g. "request".  Check for that.
	 */
	q = (p + strlen(p));
	output = fr_substr2int(pair_lists, p, PAIR_LIST_UNKNOWN, (q - p));
	if (output != PAIR_LIST_UNKNOWN) {
		*name = q;
		return output;
	}

	/*
	 *	It's just an attribute name.  Return the default list
	 *	as supplied by the caller.
	 */
	return default_list;
}

/** Resolve attribute pair_lists_t value to an attribute list.
 *
 * The value returned is a pointer to the pointer of the HEAD of the list
 * in the REQUEST. If the head of the list changes, the pointer will still
 * be valid.
 *
 * @param[in] request containing the target lists.
 * @param[in] list pair_list_t value to resolve to VALUE_PAIR list.
 *	Will be NULL if list name couldn't be resolved.
 */
VALUE_PAIR **radius_list(REQUEST *request, pair_lists_t list)
{
	if (!request) return NULL;

	switch (list) {
	case PAIR_LIST_UNKNOWN:
	default:
		break;

	case PAIR_LIST_REQUEST:
		return &request->packet->vps;

	case PAIR_LIST_REPLY:
		return &request->reply->vps;

	case PAIR_LIST_CONTROL:
		return &request->config_items;

#ifdef WITH_PROXY
	case PAIR_LIST_PROXY_REQUEST:
		if (!request->proxy) break;
		return &request->proxy->vps;

	case PAIR_LIST_PROXY_REPLY:
		if (!request->proxy) break;
		return &request->proxy_reply->vps;
#endif
#ifdef WITH_COA
	case PAIR_LIST_COA:
		if (request->coa &&
		    (request->coa->proxy->code == PW_CODE_COA_REQUEST)) {
			return &request->coa->proxy->vps;
		}
		break;

	case PAIR_LIST_COA_REPLY:
		if (request->coa && /* match reply with request */
		    (request->coa->proxy->code == PW_CODE_COA_REQUEST) &&
		    request->coa->proxy_reply) {
			return &request->coa->proxy_reply->vps;
		}
		break;

	case PAIR_LIST_DM:
		if (request->coa &&
		    (request->coa->proxy->code == PW_CODE_DISCONNECT_REQUEST)) {
			return &request->coa->proxy->vps;
		}
		break;

	case PAIR_LIST_DM_REPLY:
		if (request->coa && /* match reply with request */
		    (request->coa->proxy->code == PW_CODE_DISCONNECT_REQUEST) &&
		    request->coa->proxy_reply) {
			return &request->coa->proxy->vps;
		}
		break;
#endif
	}

	RWDEBUG2("List \"%s\" is not available",
		fr_int2str(pair_lists, list, "<INVALID>"));

	return NULL;
}

/** Get the correct TALLOC ctx for a list
 *
 * Returns the talloc context associated with an attribute list.
 *
 * @param[in] request containing the target lists.
 * @param[in] list_name pair_list_t value to resolve to TALLOC_CTX.
 * @return a TALLOC_CTX on success, else NULL
 */
TALLOC_CTX *radius_list_ctx(REQUEST *request, pair_lists_t list_name)
{
	if (!request) return NULL;

	switch (list_name) {
	case PAIR_LIST_REQUEST:
		return request->packet;

	case PAIR_LIST_REPLY:
		return request->reply;

	case PAIR_LIST_CONTROL:
		return request;

#ifdef WITH_PROXY
	case PAIR_LIST_PROXY_REQUEST:
		return request->proxy;

	case PAIR_LIST_PROXY_REPLY:
		return request->proxy_reply;
#endif

#ifdef WITH_COA
	case PAIR_LIST_COA:
		if (!request->coa) return NULL;
		rad_assert(request->coa->proxy != NULL);
		if (request->coa->proxy->code != PW_CODE_COA_REQUEST) return NULL;
		return request->coa->proxy;

	case PAIR_LIST_COA_REPLY:
		if (!request->coa) return NULL;
		rad_assert(request->coa->proxy != NULL);
		if (request->coa->proxy->code != PW_CODE_COA_REQUEST) return NULL;
		return request->coa->proxy_reply;

	case PAIR_LIST_DM:
		if (!request->coa) return NULL;
		rad_assert(request->coa->proxy != NULL);
		if (request->coa->proxy->code != PW_CODE_DISCONNECT_REQUEST) return NULL;
		return request->coa->proxy;

	case PAIR_LIST_DM_REPLY:
		if (!request->coa) return NULL;
		rad_assert(request->coa->proxy != NULL);
		if (request->coa->proxy->code != PW_CODE_DISCONNECT_REQUEST) return NULL;
		return request->coa->proxy_reply;
#endif

	default:
		break;
	}

	return NULL;
}

/** Resolve attribute name to a request.
 *
 * Check the name string for qualifiers that reference a parent request and
 * write the pointer to this request to 'request'.
 *
 * If qualifiers were consumed, write a new pointer into name to the
 * char after the last qualifier to be consumed.
 *
 * radius_ref_request should be called before radius_list_name.
 *
 * @see radius_list_name
 * @param[in,out] name of attribute.
 * @param[in] def default request ref to return if no request qualifier is present.
 * @return one of the REQUEST_* definitions or REQUEST_UNKOWN
 */
request_refs_t radius_request_name(char const **name, request_refs_t def)
{
	char *p;
	int request;

	p = strchr(*name, '.');
	if (!p) {
		return def;
	}

	/*
	 *	We may get passed "127.0.0.1".
	 */
	request = fr_substr2int(request_refs, *name, REQUEST_UNKNOWN, p - *name);

	/*
	 *	If we get a valid name, skip it.
	 */
	if (request != REQUEST_UNKNOWN) {
		*name = p + 1;
		return request;
	}

	/*
	 *	Otherwise leave it alone, and return the caller's
	 *	default.
	 */
	return def;
}

/** Resolve request to a request.
 *
 * Resolve name to a current request.
 *
 * @see radius_list
 * @param[in,out] context Base context to use, and to write the result back to.
 * @param[in] name (request) to resolve to.
 * @return 0 if request is valid in this context, else -1.
 */
int radius_request(REQUEST **context, request_refs_t name)
{
	REQUEST *request = *context;

	switch (name) {
	case REQUEST_CURRENT:
		return 0;

	case REQUEST_PARENT:	/* for future use in request chaining */
	case REQUEST_OUTER:
		if (!request->parent) {
			return -1;
		}
		*context = request->parent;
		break;

	case REQUEST_UNKNOWN:
	default:
		rad_assert(0);
		return -1;
	}

	return 0;
}

#ifdef WITH_VERIFY_PTR
static uint8_t const *not_zeroed(uint8_t const *ptr, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (ptr[i] != 0x00) return ptr + i;
	}

	return NULL;
}
#define CHECK_ZEROED(_x) not_zeroed((uint8_t const *)&_x + sizeof(_x), sizeof(vpt->data) - sizeof(_x))


/** Verify fields of a value_pair_tmpl_t make sense
 *
 */
void tmpl_verify(char const *file, int line, value_pair_tmpl_t const *vpt)
{
	if (vpt->type == TMPL_TYPE_UNKNOWN) {
		FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: value_pair_tmpl_t type was "
			     "TMPL_TYPE_UNKNOWN (uninitialised)", file, line);
		fr_assert(0);
		fr_exit_now(1);
	}

	if (vpt->type > TMPL_TYPE_NULL) {
		FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: value_pair_tmpl_t type was %i "
			     "(outside range of TMPL_TYPEs)", file, line, vpt->type);
		fr_assert(0);
		fr_exit_now(1);
	}

	/*
	 *  Do a memcmp of the bytes after where the space allocated for
	 *  the union member should have ended and the end of the union.
	 *  These should always be zero if the union has been initialised
	 *  properly.
	 *
	 *  If they're still all zero, do TMPL_TYPE specific checks.
	 */
	switch (vpt->type) {
	case TMPL_TYPE_NULL:
		if (not_zeroed((uint8_t const *)&vpt->data, sizeof(vpt->data))) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_NULL "
				     "has non-zero bytes in its data union", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_LITERAL:
		if (not_zeroed((uint8_t const *)&vpt->data, sizeof(vpt->data))) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_LITERAL "
				     "has non-zero bytes in its data union", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_XLAT:
	case TMPL_TYPE_XLAT_STRUCT:
		break;

/* @todo When regexes get converted to xlat the flags field of the regex union is used
	case TMPL_TYPE_XLAT:
		if (not_zeroed((uint8_t const *)&vpt->data, sizeof(vpt->data))) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_XLAT "
				     "has non-zero bytes in its data union", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_XLAT_STRUCT:
		if (CHECK_ZEROED(vpt->data.xlat)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_XLAT_STRUCT "
				     "has non-zero bytes after the data.xlat pointer in the union", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}
		break;
*/

	case TMPL_TYPE_EXEC:
		if (not_zeroed((uint8_t const *)&vpt->data, sizeof(vpt->data))) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_EXEC "
				     "has non-zero bytes in its data union", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_ATTR_UNKNOWN:
		rad_assert(vpt->tmpl_da == NULL);
		break;

	case TMPL_TYPE_ATTR:
		if (CHECK_ZEROED(vpt->data.attribute)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_ATTR "
				     "has non-zero bytes after the data.attribute struct in the union",
				     file, line);
			fr_assert(0);
			fr_exit_now(1);
		}

		if (vpt->tmpl_da->flags.is_unknown) {
			if (vpt->tmpl_da != (DICT_ATTR *)&vpt->data.attribute.fugly.unknown) {
				FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_ATTR "
					     "da is marked as unknown, but does not point to the template's "
					     "unknown da buffer", file, line);
				fr_assert(0);
				fr_exit_now(1);
			}

		} else {
			DICT_ATTR const *da;

			da = dict_attrbyvalue(vpt->tmpl_da->attr, vpt->tmpl_da->vendor);
			if (da != vpt->tmpl_da) {
				FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_ATTR "
					     "da pointer and global dictionary pointer for attribute \"%s\" differ",
					     file, line, vpt->tmpl_da->name);
				fr_assert(0);
				fr_exit_now(1);
			}
		}
		break;

	case TMPL_TYPE_LIST:
		if (CHECK_ZEROED(vpt->data.attribute)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_LIST"
				     "has non-zero bytes after the data.attribute struct in the union", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}

		if (vpt->tmpl_da != NULL) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_LIST da pointer was NULL", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_DATA:
		if (CHECK_ZEROED(vpt->data.literal)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA "
				     "has non-zero bytes after the data.literal struct in the union",
				     file, line);
			fr_assert(0);
			fr_exit_now(1);
		}

		if (vpt->tmpl_data_type == PW_TYPE_INVALID) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA type was "
				     "PW_TYPE_INVALID (uninitialised)", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}

		if (vpt->tmpl_data_type >= PW_TYPE_MAX) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA type was "
				     "%i (outside the range of PW_TYPEs)", file, line, vpt->tmpl_data_type);
			fr_assert(0);
			fr_exit_now(1);
		}

		if (vpt->tmpl_data_value == NULL) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA has a NULL data field",
				     file, line);
			fr_assert(0);
			fr_exit_now(1);
		}

		/*
		 *	Unlike VALUE_PAIRs we can't guarantee that VALUE_PAIR_TMPL buffers will
		 *	be talloced. They may be allocated on the stack or in global variables.
		 */
		switch (vpt->tmpl_data_type) {
		case PW_TYPE_STRING:
		if (vpt->tmpl_data_value->strvalue[vpt->tmpl_data_length] != '\0') {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA char buffer not \\0 "
				     "terminated", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}
			break;

		case PW_TYPE_TLV:
		case PW_TYPE_OCTETS:
			break;

		default:
			if (vpt->tmpl_data_length == 0) {
				FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA data pointer not NULL "
				             "but len field is zero", file, line);
				fr_assert(0);
				fr_exit_now(1);
			}
		}

		break;

	case TMPL_TYPE_REGEX:
		/*
		 *	iflag field is used for non compiled regexes too.
		 */
		if (CHECK_ZEROED(vpt->data.preg)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX "
				     "has non-zero bytes after the data.preg struct in the union", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}

		if (vpt->tmpl_preg != NULL) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX "
				     "preg field was not nULL", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}

		if ((vpt->tmpl_iflag != true) && (vpt->tmpl_iflag != false)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX "
				     "iflag field was neither true or false", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}

		break;

	case TMPL_TYPE_REGEX_STRUCT:
		if (CHECK_ZEROED(vpt->data.preg)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX_STRUCT "
				     "has non-zero bytes after the data.preg struct in the union", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}

		if (vpt->tmpl_preg == NULL) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX_STRUCT "
				     "comp field was NULL", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}

		if ((vpt->tmpl_iflag != true) && (vpt->tmpl_iflag != false)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX_STRUCT "
				     "iflag field was neither true or false", file, line);
			fr_assert(0);
			fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_UNKNOWN:
		rad_assert(0);
	}
}
#endif

/** Initialise stack allocated value_pair_tmpl_t
 *
 */
value_pair_tmpl_t *tmpl_init(value_pair_tmpl_t *vpt, tmpl_type_t type, char const *name, ssize_t len)
{
	rad_assert(vpt);
	rad_assert(type != TMPL_TYPE_UNKNOWN);
	rad_assert(type <= TMPL_TYPE_NULL);

	memset(vpt, 0, sizeof(value_pair_tmpl_t));
	vpt->type = type;

	if (name) {
		vpt->name = name;
		vpt->len = len < 0 ? strlen(name) :
				     (size_t) len;
	}
	return vpt;
}

/** Allocate and initialise heap allocated value_pair_tmpl_t
 *
 */
value_pair_tmpl_t *tmpl_alloc(TALLOC_CTX *ctx, tmpl_type_t type, char const *name, ssize_t len)
{
	value_pair_tmpl_t *vpt;

	rad_assert(type != TMPL_TYPE_UNKNOWN);
	rad_assert(type <= TMPL_TYPE_NULL);

	vpt = talloc_zero(ctx, value_pair_tmpl_t);
	if (!vpt) return NULL;
	vpt->type = type;
	if (name) {
		vpt->name = len < 0 ? talloc_strdup(ctx, name) :
				      talloc_strndup(ctx, name, len);
		vpt->len = talloc_array_length(vpt->name) - 1;
	}

	return vpt;
}

/** Parse qualifiers to convert attrname into a value_pair_tmpl_t.
 *
 * VPTs are used in various places where we need to pre-parse configuration
 * sections into attribute mappings.
 *
 * @note The name field is just a copy of the input pointer, if you know that
 * string might be freed before you're done with the vpt use tmpl_afrom_attr_str
 * instead.
 *
 * @param[out] vpt to modify.
 * @param[in] name of attribute including qualifiers.
 * @param[in] request_def The default request to insert unqualified attributes into.
 * @param[in] list_def The default list to insert unqualified attributes into.
 * @return <= 0 on error (offset as negative integer), > 0 on success (number of bytes parsed)
 */
ssize_t tmpl_from_attr_substr(value_pair_tmpl_t *vpt, char const *name,
			      request_refs_t request_def, pair_lists_t list_def)
{
	bool force_attr = false;
	char const *p;
	long num;
	char *q;
	tmpl_type_t type = TMPL_TYPE_ATTR;

	value_pair_tmpl_attr_t attr;	/* So we don't fill the tmpl with junk and then error out */

	memset(vpt, 0, sizeof(*vpt));
	memset(&attr, 0, sizeof(attr));

	p = name;
	if (*p == '&') {
		force_attr = true;
		p++;
	}

	attr.request = radius_request_name(&p, request_def);
	if (attr.request == REQUEST_UNKNOWN) {
		fr_strerror_printf("Invalid request qualifier");
		return -(p - name);
	}

	attr.list = radius_list_name(&p, list_def);
	if (attr.list == PAIR_LIST_UNKNOWN) {
		fr_strerror_printf("Invalid list qualifier");
		return -(p - name);
	}

	if (*p == '\0') {
		type = TMPL_TYPE_LIST;
		goto finish;
	}

	attr.tag = TAG_ANY;
	attr.num = NUM_ANY;

	attr.da = dict_attrbyname_substr(&p);
	if (!attr.da) {
		/*
		 *	Attr-1.2.3.4 is OK.
		 */
		if (dict_unknown_from_substr((DICT_ATTR *)&attr.fugly.unknown, &p) == 0) {
			attr.da = (DICT_ATTR *)&attr.fugly.unknown;
			goto skip_tag; /* unknown attributes can't have tags */
		}

		/*
		 *	Can't parse it as an attribute, it must be a literal string.
		 */
		if (!force_attr) {
			fr_strerror_printf("Should be re-parsed as bare word (shouldn't see me)");
			return -(p - name);
		}


		/*
		 *	Copy the name to a field for later evaluation
		 */
		type = TMPL_TYPE_ATTR_UNKNOWN;
		for (q = attr.fugly.name; dict_attr_allowed_chars[(int) *p]; *q++ = *p++) {
			if (q >= (attr.fugly.name + sizeof(attr.fugly.name) - 1)) {
				fr_strerror_printf("Attribute name is too long");
				return -(p - name);
			}
		}
		*q = '\0';

		goto skip_tag;
	}
	type = TMPL_TYPE_ATTR;

	/*
	 *	The string MIGHT have a tag.
	 */
	if (*p == ':') {
		if (!attr.da->flags.has_tag) {
			fr_strerror_printf("Attribute '%s' cannot have a tag", attr.da->name);
			return -(p - name);
		}

		num = strtol(p + 1, &q, 10);
		if ((num > 0x1f) || (num < 0)) {
			fr_strerror_printf("Invalid tag value '%li' (should be between 0-31)", num);
			return -((p + 1)- name);
		}

		attr.tag = num;
		p = q;
	}

skip_tag:
	if (*p == '\0') goto finish;

	if (*p == '[') {
		p++;

		if (*p != '*') {
			num = strtol(p, &q, 10);
			if (p == q) {
				fr_strerror_printf("Array index is not an integer");
				return -(p - name);
			}

			if ((num > 1000) || (num < 0)) {
				fr_strerror_printf("Invalid array reference '%li' (should be between 0-1000)", num);
				return -(p - name);
			}
			attr.num = num;
			p = q;
		} else {
			attr.num = NUM_ALL;
			p++;
		}

		if (*p != ']') {
			fr_strerror_printf("No closing ']' for array index");
			return -(p - name);
		}
		p++;
	}

finish:
	vpt->type = type;
	vpt->name = name;
	vpt->len = p - name;

	/*
	 *	Copy over the attribute definition, now we're
	 *	sure what we were passed is valid.
	 */
	memcpy(&vpt->data.attribute, &attr, sizeof(vpt->data.attribute));
	if ((vpt->type == TMPL_TYPE_ATTR) && attr.da->flags.is_unknown) {
		vpt->tmpl_da = (DICT_ATTR *)&vpt->data.attribute.fugly.unknown;
	}

	VERIFY_TMPL(vpt);

	return vpt->len;
}

/** Parse qualifiers to convert an attrname into a value_pair_tmpl_t.
 *
 * VPTs are used in various places where we need to pre-parse configuration
 * sections into attribute mappings.
 *
 * @note The name field is just a copy of the input pointer, if you know that
 * string might be freed before you're done with the vpt use tmpl_afrom_attr_str
 * instead.
 *
 * @param[out] vpt to modify.
 * @param[in] name attribute name including qualifiers.
 * @param[in] request_def The default request to insert unqualified attributes into.
 * @param[in] list_def The default list to insert unqualified attributes into.
 * @return <= 0 on error (offset as negative integer), > 0 on success (number of bytes parsed)
 */
ssize_t tmpl_from_attr_str(value_pair_tmpl_t *vpt, char const *name, request_refs_t request_def,
			   pair_lists_t list_def)
{
	ssize_t slen;

	slen = tmpl_from_attr_substr(vpt, name, request_def, list_def);
	if (slen <= 0) return slen;
	if (name[slen] != '\0') {
		fr_strerror_printf("Unexpected text after attribute name");
		return -slen;
	}

	VERIFY_TMPL(vpt);

	return slen;
}

/** Parse qualifiers to convert attrname into a value_pair_tmpl_t.
 *
 * VPTs are used in various places where we need to pre-parse configuration
 * sections into attribute mappings.
 *
 * @param[in] ctx for talloc
 * @param[out] out Where to write the pointer to the new value_pair_tmpl_t.
 * @param[in] name attribute name including qualifiers.
 * @param[in] request_def The default request to insert unqualified
 *	attributes into.
 * @param[in] list_def The default list to insert unqualified attributes into.
 * @return <= 0 on error (offset as negative integer), > 0 on success (number of bytes parsed)
 */
ssize_t tmpl_afrom_attr_str(TALLOC_CTX *ctx, value_pair_tmpl_t **out, char const *name,
			    request_refs_t request_def, pair_lists_t list_def)
{
	ssize_t slen;
	value_pair_tmpl_t *vpt;

	MEM(vpt = talloc(ctx, value_pair_tmpl_t)); /* tmpl_from_attr_substr zeros it */

	slen = tmpl_from_attr_substr(vpt, name, request_def, list_def);
	if (slen <= 0) {
		tmpl_free(&vpt);
		return slen;
	}
	if (name[slen] != '\0') {
		fr_strerror_printf("Unexpected text after attribute name");
		tmpl_free(&vpt);
		return -slen;
	}
	vpt->name = talloc_strndup(vpt, vpt->name, vpt->len);

	VERIFY_TMPL(vpt);

	*out = vpt;

	return slen;
}

/** Release memory allocated to value pair template.
 *
 * @param[in,out] tmpl to free.
 */
void tmpl_free(value_pair_tmpl_t **tmpl)
{
	if (*tmpl == NULL) return;

	if ((*tmpl)->type != TMPL_TYPE_UNKNOWN) VERIFY_TMPL(*tmpl);

	dict_attr_free(&((*tmpl)->tmpl_da));

	talloc_free(*tmpl);

	*tmpl = NULL;
}

/**  Print a template to a string
 *
 * @param[out] buffer for the output string
 * @param[in] bufsize of the buffer
 * @param[in] vpt to print
 * @param[in] values Used for integer attributes only. DICT_ATTR to use when mapping integer values to strings.
 * @return the size of the string written to the output buffer.
 */
size_t tmpl_prints(char *buffer, size_t bufsize, value_pair_tmpl_t const *vpt, DICT_ATTR const *values)
{
	size_t len;
	char c;
	char const *p;
	char *q = buffer;
	char *end;

	if (!vpt) {
		*buffer = '\0';
		return 0;
	}

	VERIFY_TMPL(vpt);

	switch (vpt->type) {
	default:
		return 0;

	case TMPL_TYPE_REGEX:
	case TMPL_TYPE_REGEX_STRUCT:
		c = '/';
		break;

	case TMPL_TYPE_XLAT:
	case TMPL_TYPE_XLAT_STRUCT:
		c = '"';
		break;

	case TMPL_TYPE_LIST:
	case TMPL_TYPE_LITERAL:	/* single-quoted or bare word */
		/*
		 *	Hack
		 */
		for (p = vpt->name; *p != '\0'; p++) {
			if (*p == ' ') break;
			if (*p == '\'') break;
			if (!dict_attr_allowed_chars[(int) *p]) break;
		}

		if (!*p) {
			strlcpy(buffer, vpt->name, bufsize);
			return strlen(buffer);
		}

		c = '\'';
		break;

	case TMPL_TYPE_EXEC:
		c = '`';
		break;

	case TMPL_TYPE_ATTR:
		buffer[0] = '&';
		if (vpt->tmpl_request == REQUEST_CURRENT) {
			if (vpt->tmpl_list == PAIR_LIST_REQUEST) {
				strlcpy(buffer + 1, vpt->tmpl_da->name, bufsize - 1);
			} else {
				snprintf(buffer + 1, bufsize - 1, "%s:%s",
					 fr_int2str(pair_lists, vpt->tmpl_list, ""),
					 vpt->tmpl_da->name);
			}

		} else {
			snprintf(buffer + 1, bufsize - 1, "%s.%s:%s",
				 fr_int2str(request_refs, vpt->tmpl_request, ""),
				 fr_int2str(pair_lists, vpt->tmpl_list, ""),
				 vpt->tmpl_da->name);
		}

		len = strlen(buffer);

		if ((vpt->tmpl_tag == TAG_ANY) && (vpt->tmpl_num == NUM_ANY)) {
			return len;
		}

		q = buffer + len;
		bufsize -= len;

		if (vpt->tmpl_tag != TAG_ANY) {
			snprintf(q, bufsize, ":%d", vpt->tmpl_tag);
			len = strlen(q);
			q += len;
			bufsize -= len;
		}

		if (vpt->tmpl_num != NUM_ANY) {
			snprintf(q, bufsize, "[%i]", vpt->tmpl_num);
			len = strlen(q);
			q += len;
		}

		return (q - buffer);

	case TMPL_TYPE_ATTR_UNKNOWN:
		buffer[0] = '&';
		if (vpt->tmpl_request == REQUEST_CURRENT) {
			if (vpt->tmpl_list == PAIR_LIST_REQUEST) {
				strlcpy(buffer + 1, vpt->tmpl_unknown_name, bufsize - 1);
			} else {
				snprintf(buffer + 1, bufsize - 1, "%s:%s",
					 fr_int2str(pair_lists, vpt->tmpl_list, ""),
					 vpt->tmpl_unknown_name);
			}

		} else {
			snprintf(buffer + 1, bufsize - 1, "%s.%s:%s",
				 fr_int2str(request_refs, vpt->tmpl_request, ""),
				 fr_int2str(pair_lists, vpt->tmpl_list, ""),
				 vpt->tmpl_unknown_name);
		}

		len = strlen(buffer);

		if (vpt->tmpl_num == NUM_ANY) {
			return len;
		}

		q = buffer + len;
		bufsize -= len;

		if (vpt->tmpl_num != NUM_ANY) {
			snprintf(q, bufsize, "[%i]", vpt->tmpl_num);
			len = strlen(q);
			q += len;
		}

		return (q - buffer);

	case TMPL_TYPE_DATA:
		if (vpt->tmpl_data_value) {
			return vp_data_prints_value(buffer, bufsize, vpt->tmpl_data_type,
						    vpt->tmpl_data_value, vpt->tmpl_data_length, values, '\'');
		} else {
			*buffer = '\0';
			return 0;
		}
	}

	if (bufsize <= 3) {
	no_room:
		*buffer = '\0';
		return 0;
	}

	p = vpt->name;
	*(q++) = c;
	end = buffer + bufsize - 3; /* quotes + EOS */

	while (*p && (q < end)) {
		if (*p == c) {
			if ((end - q) < 4) goto no_room; /* escape, char, quote, EOS */
			*(q++) = '\\';
			*(q++) = *(p++);
			continue;
		}

		switch (*p) {
		case '\\':
			if ((end - q) < 4) goto no_room;
			*(q++) = '\\';
			*(q++) = *(p++);
			break;

		case '\r':
			if ((end - q) < 4) goto no_room;
			*(q++) = '\\';
			*(q++) = 'r';
			p++;
			break;

		case '\n':
			if ((end - q) < 4) goto no_room;
			*(q++) = '\\';
			*(q++) = 'r';
			p++;
			break;

		case '\t':
			if ((end - q) < 4) goto no_room;
			*(q++) = '\\';
			*(q++) = 't';
			p++;
			break;

		default:
			*(q++) = *(p++);
			break;
		}
	}

	*(q++) = c;
	*q = '\0';

	return q - buffer;
}

/** Convert module specific attribute id to value_pair_tmpl_t.
 *
 * @note Unlike tmpl_afrom_attr_str return code 0 doesn't indicate failure, just means it parsed a 0 length string.
 *
 * @param[in] ctx for talloc.
 * @param[out] out Where to write the pointer to the new value_pait_tmpl_t.
 * @param[in] name string to convert.
 * @param[in] type Type of quoting around value.
 * @param[in] request_def The default request to insert unqualified
 *	attributes into.
 * @param[in] list_def The default list to insert unqualified attributes into.
 * @return < 0 on error (offset as negative integer), >= 0 on success (number of bytes parsed)
 */
ssize_t tmpl_afrom_str(TALLOC_CTX *ctx, value_pair_tmpl_t **out, char const *name, FR_TOKEN type,
		       request_refs_t request_def, pair_lists_t list_def)
{
	char const *p;
	ssize_t slen;
	value_pair_tmpl_t *vpt;

	switch (type) {
	case T_BARE_WORD:
		/*
		 *	If we can parse it as an attribute, it's an attribute.
		 *	Otherwise, treat it as a literal.
		 */
		slen = tmpl_afrom_attr_str(ctx, &vpt, name, request_def, list_def);
		if ((name[0] == '&') && (slen <= 0)) return slen;
		if (slen > 0) break;
		/* FALL-THROUGH */

	case T_SINGLE_QUOTED_STRING:
		vpt = tmpl_alloc(ctx, TMPL_TYPE_LITERAL, name, -1);
		slen = vpt->len;
		break;

	case T_DOUBLE_QUOTED_STRING:
		p = name;
		while (*p) {
			if (*p == '\\') {
				if (!p[1]) break;
				p += 2;
				continue;
			}

			if (*p == '%') break;

			p++;
		}

		/*
		 *	If the double quoted string needs to be
		 *	expanded at run time, make it an xlat
		 *	expansion.  Otherwise, convert it to be a
		 *	literal.
		 */
		if (*p) {
			vpt = tmpl_alloc(ctx, TMPL_TYPE_XLAT, name, -1);
		} else {
			vpt = tmpl_alloc(ctx, TMPL_TYPE_LITERAL, name, -1);
		}
		slen = vpt->len;
		break;

	case T_BACK_QUOTED_STRING:
		vpt = tmpl_alloc(ctx, TMPL_TYPE_EXEC, name, -1);
		slen = vpt->len;
		break;

	case T_OP_REG_EQ: /* hack */
		vpt = tmpl_alloc(ctx, TMPL_TYPE_REGEX, name, -1);
		slen = vpt->len;
		break;

	default:
		rad_assert(0);
		return 0;	/* 0 is an error here too */
	}

	VERIFY_TMPL(vpt);

	*out = vpt;

	return slen;
}

/** Convert a tmpl containing literal data, to the type specified by da.
 *
 * @param[in,out] vpt the template to modify
 * @param[in] da the dictionary attribute to case it to
 * @return true for success, false for failure.
 */
bool tmpl_cast_in_place(value_pair_tmpl_t *vpt, DICT_ATTR const *da)
{
	VALUE_PAIR *vp;
	value_data_t *data;

	VERIFY_TMPL(vpt);

	rad_assert(vpt != NULL);
	rad_assert(da != NULL);
	rad_assert(vpt->type == TMPL_TYPE_LITERAL);

	vp = pairalloc(vpt, da);
	if (!vp) return false;

	if (pairparsevalue(vp, vpt->name, 0) < 0) {
		pairfree(&vp);
		return false;
	}
	memset(&vpt->data, 0, sizeof(vpt->data));

	vpt->tmpl_data_value = data = talloc(vpt, value_data_t);
	if (!vpt->tmpl_data_value) return false;

	vpt->type = TMPL_TYPE_DATA;
	vpt->tmpl_data_length = vp->length;
	vpt->tmpl_data_type = da->type;

	if (vp->da->flags.is_pointer) {
		data->ptr = talloc_steal(vpt, vp->data.ptr);
		vp->data.ptr = NULL;
	} else {
		memcpy(data, &vp->data, sizeof(*data));
	}

	pairfree(&vp);

	VERIFY_TMPL(vpt);

	return true;
}

/** Expand a template to a string, parse it as type of "cast", and create a VP from the data.
 */
int tmpl_cast_to_vp(VALUE_PAIR **out, REQUEST *request,
		    value_pair_tmpl_t const *vpt, DICT_ATTR const *cast)
{
	int rcode;
	VALUE_PAIR *vp;
	char *str;

	VERIFY_TMPL(vpt);

	*out = NULL;

	vp = pairalloc(request, cast);
	if (!vp) return -1;

	if (vpt->type == TMPL_TYPE_DATA) {
		VERIFY_VP(vp);
		rad_assert(vp->da->type == vpt->tmpl_data_type);
		pairdatacpy(vp, vpt->tmpl_data_type, vpt->tmpl_data_value, vpt->tmpl_data_length);
		*out = vp;
		return 0;
	}

	rcode = radius_expand_tmpl(&str, request, vpt);
	if (rcode < 0) {
		pairfree(&vp);
		return rcode;
	}

	if (pairparsevalue(vp, str, 0) < 0) {
		talloc_free(str);
		pairfree(&vp);
		return rcode;
	}

	*out = vp;
	return 0;
}

/** Initialise a vp_cursor_t to the VALUE_PAIR specified by a value_pair_tmpl_t
 *
 * This makes iterating over the one or more VALUE_PAIRs specified by a value_pair_tmpl_t
 * significantly easier.
 *
 * @see tmpl_cursor_next
 *
 * @param err Will be set to -1 if VP could not be found, -2 if list could not be found,
 *	-3 if context could not be found and NULL will be returned. Will be 0 on success.
 * @param cursor to store iterator state.
 * @param request The current request.
 * @param vpt specifying the VALUE_PAIRs to iterate over.
 * @return the first VALUE_PAIR specified by the value_pair_tmpl_t, NULL if no matching VALUE_PAIRs exist,
 * 	and NULL on error.
 */
VALUE_PAIR *tmpl_cursor_init(int *err, vp_cursor_t *cursor, REQUEST *request, value_pair_tmpl_t const *vpt)
{
	VALUE_PAIR **vps, *vp = NULL;

	VERIFY_TMPL(vpt);

	rad_assert((vpt->type == TMPL_TYPE_ATTR) || (vpt->type == TMPL_TYPE_LIST));

	if (err) *err = 0;

	if (radius_request(&request, vpt->tmpl_request) < 0) {
		if (err) *err = -3;
		return NULL;
	}
	vps = radius_list(request, vpt->tmpl_list);
	if (!vps) {
		if (err) *err = -2;
		return NULL;
	}
	(void) fr_cursor_init(cursor, vps);

	switch (vpt->type) {
	/*
	 *	May not may not be found, but it *is* a known name.
	 */
	case TMPL_TYPE_ATTR:
	{
		int num;

		if (vpt->tmpl_num == NUM_ANY) {
			vp = fr_cursor_next_by_da(cursor, vpt->tmpl_da, vpt->tmpl_tag);
			if (!vp) {
				if (err) *err = -1;
				return NULL;
			}
			VERIFY_VP(vp);
			break;
		}

		(void) fr_cursor_init(cursor, vps);
		num = vpt->tmpl_num;
		while ((vp = fr_cursor_next_by_da(cursor, vpt->tmpl_da, vpt->tmpl_tag))) {
			VERIFY_VP(vp);
			if (num-- <= 0) goto finish;
		}

		if (err) *err = -1;
		return NULL;
	}

	case TMPL_TYPE_LIST:
		vp = fr_cursor_init(cursor, vps);
		break;

	default:
		rad_assert(0);
	}

finish:
	return vp;
}

/** Gets the next VALUE_PAIR specified by value_pair_tmpl_t
 *
 * Returns the next VALUE_PAIR matching a value_pair_tmpl_t
 *
 * @param cursor initialised with tmpl_cursor_init.
 * @param vpt specifying the VALUE_PAIRs to iterate over.
 * @return NULL if no more matching VALUE_PAIRs found.
 */
VALUE_PAIR *tmpl_cursor_next(vp_cursor_t *cursor, value_pair_tmpl_t const *vpt)
{
	rad_assert((vpt->type == TMPL_TYPE_ATTR) || (vpt->type == TMPL_TYPE_LIST));

	VERIFY_TMPL(vpt);

	switch (vpt->type) {
	/*
	 *	May not may not be found, but it *is* a known name.
	 */
	case TMPL_TYPE_ATTR:
		if (vpt->tmpl_num != NUM_ALL) return NULL;
		return fr_cursor_next_by_da(cursor, vpt->tmpl_da, vpt->tmpl_tag);

	case TMPL_TYPE_LIST:
		return fr_cursor_next(cursor);

	default:
		rad_assert(0);
	}
}

/** Copy pairs matching a VPT in the current request
 *
 * @param ctx to allocate new VALUE_PAIRs under.
 * @param out Where to write the copied vps.
 * @param request current request.
 * @param vpt specifying the VALUE_PAIRs to iterate over.
 * @return -1 if VP could not be found, -2 if list could not be found, -3 if context could not be found,
 *	-4 on memory allocation error.
 */
int tmpl_copy_vps(TALLOC_CTX *ctx, VALUE_PAIR **out, REQUEST *request, value_pair_tmpl_t const *vpt)
{
	VALUE_PAIR *vp;
	vp_cursor_t from, to;

	VERIFY_TMPL(vpt);

	int err;

	rad_assert((vpt->type == TMPL_TYPE_ATTR) || (vpt->type == TMPL_TYPE_LIST));

	*out = NULL;

	fr_cursor_init(&to, out);

	for (vp = tmpl_cursor_init(&err, &from, request, vpt);
	     vp;
	     vp = tmpl_cursor_next(&from, vpt)) {
		vp = paircopyvp(ctx, vp);
		if (!vp) {
			pairfree(out);
			return -4;
		}
		fr_cursor_insert(&to, vp);
	}

	return err;
}

/** Gets the first VP from a value_pair_tmpl_t
 *
 * @param out where to write the retrieved vp.
 * @param request current request.
 * @param vpt specifying the VALUE_PAIRs to iterate over.
 * @return -1 if VP could not be found, -2 if list could not be found, -3 if context could not be found.
 */
int tmpl_find_vp(VALUE_PAIR **out, REQUEST *request, value_pair_tmpl_t const *vpt)
{
	vp_cursor_t cursor;
	VALUE_PAIR *vp;

	VERIFY_TMPL(vpt);

	int err;

	vp = tmpl_cursor_init(&err, &cursor, request, vpt);
	if (out) *out = vp;

	return err;
}

bool tmpl_define_unknown_attr(value_pair_tmpl_t *vpt)
{
	DICT_ATTR const *da;

	if (!vpt) return false;

	VERIFY_TMPL(vpt);

	if ((vpt->type != TMPL_TYPE_ATTR) &&
	    (vpt->type != TMPL_TYPE_DATA)) {
		return true;
	}

	if (!vpt->tmpl_da->flags.is_unknown) return true;

	da = dict_unknown_add(vpt->tmpl_da);
	if (!da) return false;
	vpt->tmpl_da = da;
	return true;
}
