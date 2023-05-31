/* SPDX-License-Identifier: BSD-3-Clause */

#include <fnmatch.h>
#include <stdbool.h>
#include <jansson.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "core.h"
#include "dagger.h"
#include "lyx.h"
#include "srx_module.h"
#include "srx_val.h"

#define ERR_IFACE(_iface, _err, _fmt, ...)				\
	({								\
		ERROR("%s: " _fmt, lydx_get_cattr(_iface, "name"),	\
		      ##__VA_ARGS__);					\
		_err;							\
	})

#define DEBUG_IFACE(_iface, _fmt, ...)				\
	DEBUG("%s: " _fmt, lydx_get_cattr(_iface, "name"), ##__VA_ARGS__)

static const char *iffeat[] = {
	"if-mib",
	NULL
};

static const char *ifextfeat[] = {
	"sub-interfaces",
	NULL
};

static const struct srx_module_requirement ietf_if_reqs[] = {
	{ .dir = YANG_PATH_, .name = "ietf-interfaces", .rev = "2018-02-20", .features = iffeat },
	{ .dir = YANG_PATH_, .name = "iana-if-type", .rev = "2023-01-26" },
	{ .dir = YANG_PATH_, .name = "ietf-if-extensions", .rev = "2023-01-26", .features = ifextfeat },
	{ .dir = YANG_PATH_, .name = "ieee802-dot1q-types", .rev = "2022-10-29" },
	{ .dir = YANG_PATH_, .name = "ietf-if-vlan-encapsulation", .rev = "2023-01-26" },
	{ .dir = YANG_PATH_, .name = "ietf-ip", .rev = "2018-02-22" },
	{ .dir = YANG_PATH_, .name = "infix-ip", .rev = "2023-04-24" },

	{ NULL }
};

static bool iface_is_phys(const char *ifname)
{
	bool is_phys = false;
	json_error_t jerr;
	const char *attr;
	json_t *link;
	FILE *proc;

	proc = popenf("re", "ip -d -j link show dev %s 2>/dev/null", ifname);
	if (!proc)
		goto out;

	link = json_loadf(proc, 0, &jerr);
	pclose(proc);

	if (!link)
		goto out;

	if (json_unpack(link, "[{s:s}]", "link_type", &attr))
		goto out_free;

	if (strcmp(attr, "ether"))
		goto out_free;

	if (!json_unpack(link, "[{s: { s:s }}]", "linkinfo", "info_kind", &attr))
		goto out_free;

	is_phys = true;

out_free:
	json_decref(link);
out:
	return is_phys;
}

static int ifchange_cand_infer_type(sr_session_ctx_t *session, const char *xpath)
{
	sr_val_t inferred = { .type = SR_STRING_T };
	sr_error_t err = SR_ERR_OK;
	char *ifname, *type;

	type = srx_get_str(session, "%s/type", xpath);
	if (type) {
		free(type);
		return SR_ERR_OK;
	}

	ifname = srx_get_str(session, "%s/name", xpath);
	if (!ifname)
		return SR_ERR_INTERNAL;

	if (iface_is_phys(ifname))
		inferred.data.string_val = "iana-if-type:ethernetCsmacd";
	else if (!fnmatch("br+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "iana-if-type:bridge";
	else if (!fnmatch("lag+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "iana-if-type:ieee8023adLag";
	else if (!fnmatch("vlan+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "iana-if-type:l2vlan";
	else if (!fnmatch("*.+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "iana-if-type:l2vlan";

	free(ifname);

	if (inferred.data.string_val)
		err = srx_set_item(session, &inferred, 0, "%s/type", xpath);

	return err;
}

static int ifchange_cand(sr_session_ctx_t *session, uint32_t sub_id, const char *module,
			 const char *xpath, sr_event_t event, unsigned request_id, void *priv)
{
	sr_change_iter_t *iter;
	sr_change_oper_t op;
	sr_val_t *old, *new;
	sr_error_t err;

	if (event != SR_EV_UPDATE)
		return SR_ERR_OK;

	err = sr_dup_changes_iter(session, "/ietf-interfaces:interfaces/interface", &iter);
	if (err)
		return err;

	while (sr_get_change_next(session, iter, &op, &old, &new) == SR_ERR_OK) {
		if (op != SR_OP_CREATED)
			continue;

		err = ifchange_cand_infer_type(session, new->xpath);
		if (err)
			break;
	}

	sr_free_change_iter(iter);
	return SR_ERR_OK;
}

static int netdag_exit_reload(struct dagger *net)
{
	FILE *initctl;

	/* We may end up writing this file multiple times, e.g. if
	 * multiple services are disabled in the same config cycle,
	 * but since the contents of the file are static it doesn't
	 * matter.
	 */
	initctl = dagger_fopen_current(net, "exit", "@post",
				       90, "reload.sh");
	if (!initctl)
		return -EIO;

	fputs("initctl -bnq reload\n", initctl);
	fclose(initctl);
	return 0;
}

static bool is_std_lo_addr(const char *ifname, const char *ip, const char *pf)
{
	struct in6_addr in6, lo6;
	struct in_addr in4;

	if (strcmp(ifname, "lo"))
		return false;

	if (inet_pton(AF_INET, ip, &in4) == 1)
		return (ntohl(in4.s_addr) == INADDR_LOOPBACK) && !strcmp(pf, "8");

	if (inet_pton(AF_INET6, ip, &in6) == 1) {
		inet_pton(AF_INET6, "::1", &lo6);

		return !memcmp(&in6, &lo6, sizeof(in6))
			&& !strcmp(pf, "128");
	}

	return false;
}

static int netdag_gen_ipvx_addr(FILE *ip, const char *ifname,
				struct lyd_node *addr)
{
	enum lydx_op op = lydx_get_op(addr);
	struct lyd_node *adr, *pfx;
	struct lydx_diff adrd, pfxd;
	const char *addcmd = "add";

	adr = lydx_get_child(addr, "ip");
	pfx = lydx_get_child(addr, "prefix-length");
	if (!adr || !pfx)
		return -EINVAL;

	lydx_get_diff(adr, &adrd);
	lydx_get_diff(pfx, &pfxd);

	if (op != LYDX_OP_CREATE) {
		fprintf(ip, "address delete %s/%s dev %s\n",
			adrd.old, pfxd.old, ifname);

		if (op == LYDX_OP_DELETE)
			return 0;
	}

	/* When bringing up loopback, the kernel will automatically
	 * add the standard addresses, so don't treat the existance of
	 * these as an error.
	 */
	if ((op == LYDX_OP_CREATE) &&
	    is_std_lo_addr(ifname, adrd.new, pfxd.new))
		addcmd = "replace";

	fprintf(ip, "address %s %s/%s dev %s\n", addcmd,
		adrd.new, pfxd.new, ifname);
	return 0;
}

static int netdag_gen_ipvx_addrs(FILE *ip, const char *ifname,
				 struct lyd_node *ipvx)
{
	struct lyd_node *addr;
	int err = 0;

	LYX_LIST_FOR_EACH(lyd_child(ipvx), addr, "address") {
		err = netdag_gen_ipvx_addr(ip, ifname, addr);
		if (err)
			break;
	}

	return err;
}

static int netdag_gen_ipv4_addrs(FILE *ip, struct lyd_node *dif)
{
	struct lyd_node *ipv4 = lydx_get_child(dif, "ipv4");
	const char *ifname = lydx_get_cattr(dif, "name");

	if (!ipv4)
		return 0;

	return netdag_gen_ipvx_addrs(ip, ifname, ipv4);
}

static int netdag_gen_ipv6_addrs(FILE *ip, struct lyd_node *dif)
{
	struct lyd_node *ipv6 = lydx_get_child(dif, "ipv6");
	const char *ifname = lydx_get_cattr(dif, "name");

	if (!ipv6)
		return 0;

	return netdag_gen_ipvx_addrs(ip, ifname, ipv6);
}

static int netdag_gen_ipv6_autoconf(FILE *ip, struct lyd_node *dif)
{
	struct lyd_node *node;
	struct lydx_diff nd;

	node = lydx_get_descendant(lyd_child(dif),
				   "ipv6",
				   "autoconf",
				   "create-global-addresses",
				   NULL);
	if (!node)
		return 0;

	if (!lydx_get_diff(node, &nd))
		return 0;

	if (!nd.new || !strcmp(nd.val, "true"))
		fputs(" addrgenmode eui64", ip);
	else
		fputs(" addrgenmode none", ip);

	return 0;
}

static int netdag_gen_ipv4_autoconf(struct dagger *net,
				    struct lyd_node *dif)
{
	const char *ifname = lydx_get_cattr(dif, "name");
	struct lyd_node *node;
	struct lydx_diff nd;
	FILE *initctl;
	int err = 0;

	node = lydx_get_descendant(lyd_child(dif),
				   "ipv4",
				   "autoconf",
				   "enabled",
				   NULL);
	if (!node)
		return 0;

	if (!lydx_get_diff(node, &nd))
		return 0;

	if (nd.new && !strcmp(nd.val, "true")) {
		initctl = dagger_fopen_next(net, "init", ifname,
					    60, "zeroconf-up.sh");
		if (!initctl)
			return -EIO;

		fprintf(initctl,
			"initctl -bnq enable zeroconf@%s.conf\n", ifname);
	} else {
		initctl = dagger_fopen_current(net, "exit", ifname,
					       40, "zeroconf-down.sh");
		if (!initctl)
			return -EIO;

		fprintf(initctl,
			"initctl -bnq disable zeroconf@%s.conf\n", ifname);

		err = netdag_exit_reload(net);
	}

	fclose(initctl);
	return err;
}

static int netdag_gen_sysctl_bool(struct dagger *net,
				  const char *ifname, FILE **fpp,
				  struct lyd_node *node,
				  const char *fmt, ...)
{
	struct lydx_diff nd;
	va_list ap;

	if (!node)
		return 0;

	if (!lydx_get_diff(node, &nd))
		return 0;

	*fpp = *fpp ? : dagger_fopen_next(net, "init", ifname,
					  60, "init.sysctl");
	if (!*fpp)
		return -EIO;

	va_start(ap, fmt);
	vfprintf(*fpp, fmt, ap);
	va_end(ap);
	fprintf(*fpp, " = %u\n", (nd.new && !strcmp(nd.val, "true")) ? 1 : 0);
	return 0;
}
static int netdag_gen_sysctl(struct dagger *net,
			     struct lyd_node *dif)
{
	const char *ifname = lydx_get_cattr(dif, "name");
	struct lyd_node *node;
	FILE *sysctl = NULL;
	int err = 0;

	node = lydx_get_descendant(lyd_child(dif), "ipv4", "forwarding", NULL);
	err = err ? : netdag_gen_sysctl_bool(net, ifname, &sysctl, node,
					     "net.ipv4.conf.%s.forwarding",
					     ifname);

	node = lydx_get_descendant(lyd_child(dif), "ipv6", "forwarding", NULL);
	err = err ? : netdag_gen_sysctl_bool(net, ifname, &sysctl, node,
					     "net.ipv6.conf.%s.forwarding",
					     ifname);

	if (sysctl)
		fclose(sysctl);

	return err;
}

static int netdag_gen_vlan(struct dagger *net, struct lyd_node *dif,
			   struct lyd_node *cif, FILE *ip)
{
	const char *parent, *ifname = lydx_get_cattr(cif, "name");
	struct lydx_diff typed, vidd;
	struct lyd_node *otag;
	const char *proto;
	int err;

	DEBUG("");

	parent = lydx_get_cattr(cif, "parent-interface");
	fprintf(ip, " link %s", parent);

	err = dagger_add_dep(net, ifname, parent);
	if (err)
		return ERR_IFACE(cif, err, "Unable to add dep \"%s\"", parent);

	otag = lydx_get_descendant(lyd_child(dif ? : cif),
				   "encapsulation",
				   "dot1q-vlan",
				   "outer-tag",
				   NULL);
	if (!otag)
		return 0;

	fprintf(ip, " type vlan");

	if (lydx_get_diff(lydx_get_child(otag, "tag-type"), &typed)) {
		if (!strcmp(typed.new, "ieee802-dot1q-types:c-vlan"))
			proto = "802.1Q";
		else if (!strcmp(typed.new, "ieee802-dot1q-types:s-vlan"))
			proto = "802.1ad";
		else
			return ERR_IFACE(cif, -ENOSYS, "Unsupported tag type \"%s\"",
					 typed.new);

		fprintf(ip, " proto %s", proto);
	}

	if (lydx_get_diff(lydx_get_child(otag, "vlan-id"), &vidd))
		fprintf(ip, " id %s", vidd.new);

	DEBUG("");
	return 0;
}

static int netdag_gen_afspec_add(struct dagger *net, struct lyd_node *dif,
				 struct lyd_node *cif, FILE *ip)
{
	const char *ifname = lydx_get_cattr(cif, "name");
	const char *iftype = lydx_get_cattr(cif, "type");
	int err;

	DEBUG_IFACE(dif, "");

	fprintf(ip, "link add dev %s down", ifname);

	if (iftype && !strcmp(iftype, "iana-if-type:l2vlan")) {
		err = netdag_gen_vlan(net, NULL, cif, ip);
	} else {
		ERROR("Unable to add unsupported interface type \"%s\"",
		      iftype);
		return -ENOSYS;
	}

	if (err)
		return err;

	fputc('\n', ip);
	return 0;
}

static int netdag_gen_afspec_set(struct dagger *net, struct lyd_node *dif,
				 struct lyd_node *cif, FILE *ip)
{
	const char *iftype = lydx_get_cattr(cif, "type");

	DEBUG_IFACE(dif, "");

	if (iftype && !strcmp(iftype, "iana-if-type:l2vlan"))
		return netdag_gen_vlan(net, dif, cif, ip);

	ERROR("Unable to configure unsupported interface type \"%s\"", iftype);
	return -ENOSYS;
}

static bool netdag_must_del(struct lyd_node *dif, struct lyd_node *cif)
{
	const char *iftype = lydx_get_cattr(cif, "type");

	if (iftype && !strcmp(iftype, "iana-if-type:l2vlan"))
		return lydx_get_cattr(dif, "parent-interface") ||
			lydx_get_descendant(lyd_child(dif),
					    "encapsulation",
					    "dot1q-vlan",
					    "outer-tag",
					    NULL);

	return false;
}

static int netdag_gen_iface_del(struct dagger *net, struct lyd_node *dif,
				       struct lyd_node *cif, bool fixed)
{
	const char *ifname = lydx_get_cattr(dif, "name");
	FILE *ip;

	DEBUG_IFACE(dif, "");

	ip = dagger_fopen_current(net, "exit", ifname, 50, "exit.ip");
	if (!ip)
		return -EIO;

	if (fixed) {
		fprintf(ip, "link set dev %s down\n", ifname);
		fprintf(ip, "addr flush dev %s\n", ifname);
	} else {
		fprintf(ip, "link del dev %s\n", ifname);
	}

	fclose(ip);
	return 0;
}

static sr_error_t netdag_gen_iface(struct dagger *net,
				   struct lyd_node *dif, struct lyd_node *cif)
{
	const char *ifname = lydx_get_cattr(dif, "name");
	enum lydx_op op = lydx_get_op(dif);
	const char *attr;
	int err = 0;
	bool fixed;
	FILE *ip;

	fixed = iface_is_phys(ifname) || !strcmp(ifname, "lo");

	DEBUG("%s(%s) %s", ifname, fixed ? "fixed" : "dynamic",
	      (op == LYDX_OP_NONE) ? "mod" : ((op == LYDX_OP_CREATE) ? "add" : "del"));

	if (op == LYDX_OP_DELETE) {
		err = netdag_gen_iface_del(net, dif, cif, fixed);
		goto err;
	}

	/* Although, from a NETCONF perspective, we are handling a
	 * modification, we may have to remove the interface and
	 * recreate it from scratch.  E.g. Linux can't modify the
	 * parent ("link") of an existing interface, but this is
	 * perfectly legal according to the YANG model.
	 */
	if (op != LYDX_OP_CREATE && netdag_must_del(dif, cif)) {
		DEBUG_IFACE(dif, "Must delete");

		err = netdag_gen_iface_del(net, dif, cif, fixed);
		if (err)
			goto err;

		/* Interface has been removed, convert the config to a
		 * diff, so that all settings/addresses are applied
		 * again.
		 */
		lyd_new_meta(cif->schema->module->ctx, cif, NULL,
			     "yang:operation", "create", false, NULL);
		dif = cif;
		op = LYDX_OP_CREATE;
	}

	ip = dagger_fopen_next(net, "init", ifname, 50, "init.ip");
	if (!ip) {
		err = -EIO;
		goto err;
	}

	if (!fixed && op == LYDX_OP_CREATE) {
		err = netdag_gen_afspec_add(net, dif, cif, ip);
		if (err)
			goto err_close_ip;
	}

	fprintf(ip, "link set dev %s down", ifname);

	/* Set generic link attributes */
	err = err ? : netdag_gen_ipv4_autoconf(net, dif);
	err = err ? : netdag_gen_ipv6_autoconf(ip, dif);
	if (err)
		goto err_close_ip;

	fputc('\n', ip);

	/* Set type specific attributes */
	if (!fixed && op != LYDX_OP_CREATE) {
		err = netdag_gen_afspec_set(net, dif, cif, ip);
		if (err)
			goto err_close_ip;
	}

	/* Set Addresses */
	err = err ? : netdag_gen_ipv4_addrs(ip, dif);
	err = err ? : netdag_gen_ipv6_addrs(ip, dif);
	if (err)
		goto err_close_ip;

	/* Bring interface back up, if enabled */
	attr = lydx_get_cattr(cif, "enabled");
	if (!attr || !strcmp(attr, "true"))
		fprintf(ip, "link set dev %s up\n", ifname);

	err = err ? : netdag_gen_sysctl(net, dif);

err_close_ip:
	fclose(ip);
err:
	if (err)
		ERROR("Failed to setup %s: %d\n", ifname, err);

	return err ? SR_ERR_INTERNAL : SR_ERR_OK;
}

static sr_error_t netdag_init(struct dagger *net, struct lyd_node *cifs,
			      struct lyd_node *difs)
{
	struct lyd_node *iface;

	LYX_LIST_FOR_EACH(cifs, iface, "interface")
		if (dagger_add_node(net, lydx_get_cattr(iface, "name")))
			return SR_ERR_INTERNAL;

	LYX_LIST_FOR_EACH(difs, iface, "interface")
		if (dagger_add_node(net, lydx_get_cattr(iface, "name")))
			return SR_ERR_INTERNAL;

	return SR_ERR_OK;
}

static int ifchange(sr_session_ctx_t *session, uint32_t sub_id, const char *module,
		    const char *xpath, sr_event_t event, unsigned request_id, void *_confd)
{
	struct lyd_node *diff, *cifs, *difs, *cif, *dif;
	struct confd *confd = _confd;
	sr_data_t *cfg;
	sr_error_t err;

	switch (event) {
	case SR_EV_CHANGE:
		break;
	case SR_EV_ABORT:
		return dagger_abandon(&confd->netdag);
	case SR_EV_DONE:
		return dagger_evolve_or_abandon(&confd->netdag);
	default:
		return SR_ERR_OK;
	}

	err = dagger_claim(&confd->netdag, "/run/net");
	if (err)
		return err;

	err = sr_get_data(session, "/interfaces/interface", 0, 0, 0, &cfg);
	if (err)
		goto err_abandon;

	err = srx_get_diff(session, (struct lyd_node **)&diff);
	if (err)
		goto err_release_data;

	cifs = lydx_get_descendant(cfg->tree, "interfaces", "interface", NULL);
	difs = lydx_get_descendant(diff, "interfaces", "interface", NULL);

	err = netdag_init(&confd->netdag, cifs, difs);
	if (err)
		goto err_free_diff;

	LYX_LIST_FOR_EACH(difs, dif, "interface") {
		LYX_LIST_FOR_EACH(cifs, cif, "interface")
			if (!strcmp(lydx_get_cattr(dif, "name"),
				    lydx_get_cattr(cif, "name")))
				break;

		err = netdag_gen_iface(&confd->netdag, dif, cif);
		if (err)
			break;
	}

err_free_diff:
	lyd_free_tree(diff);
err_release_data:
	sr_release_data(cfg);
err_abandon:
	if (err)
		dagger_abandon(&confd->netdag);

	return err;
}

int ietf_interfaces_init(struct confd *confd)
{
	int rc;

	rc = srx_require_modules(confd->conn, ietf_if_reqs);
	if (rc)
		goto fail;

	REGISTER_CHANGE(confd->session, "ietf-interfaces", "/ietf-interfaces:interfaces", 0, ifchange, confd, &confd->sub);
	REGISTER_CHANGE(confd->cand, "ietf-interfaces", "/ietf-interfaces:interfaces",
			SR_SUBSCR_UPDATE, ifchange_cand, confd, &confd->sub);

	return SR_ERR_OK;
fail:
	ERROR("init failed: %s", sr_strerror(rc));
	return rc;
}
