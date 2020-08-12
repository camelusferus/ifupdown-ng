/*
 * cmd/ifquery.c
 * Purpose: look up information in /etc/network/interfaces
 *
 * Copyright (c) 2020 Ariadne Conill <ariadne@dereferenced.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * This software is provided 'as is' and without any warranty, express or
 * implied.  In no event shall the authors be liable for any damages arising
 * from the use of this software.
 */

#define _GNU_SOURCE
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include "libifupdown/libifupdown.h"
#include "cmd/multicall.h"

void
print_interface(struct lif_interface *iface)
{
	if (!lif_lifecycle_query_dependents(&exec_opts, iface, iface->ifname))
		return;

	if (iface->is_auto)
		printf("auto %s\n", iface->ifname);

	printf("iface %s\n", iface->ifname);

	struct lif_node *iter;
	LIF_DICT_FOREACH(iter, &iface->vars)
	{
		struct lif_dict_entry *entry = iter->data;

		if (!strcmp(entry->key, "address"))
		{
			struct lif_address *addr = entry->data;
			char addr_buf[512];

			if (!lif_address_unparse(addr, addr_buf, sizeof addr_buf, true))
			{
				printf("  # warning: failed to unparse address\n");
				continue;
			}

			printf("  %s %s\n", entry->key, addr_buf);
		}
		else
			printf("  %s %s\n", entry->key, (const char *) entry->data);
	}

	printf("\n");
}

void
print_interface_dot(struct lif_dict *collection, struct lif_interface *iface, struct lif_interface *parent)
{
	if (!lif_lifecycle_query_dependents(&exec_opts, iface, iface->ifname))
		return;

	if (iface->is_up)
		return;

	if (parent != NULL)
		printf("\"%s\" -> ", parent->ifname);

	printf("\"%s\"", iface->ifname);

	printf("\n");

	struct lif_dict_entry *entry = lif_dict_find(&iface->vars, "requires");

	if (entry == NULL)
		return;

	char require_ifs[4096] = {};
	strlcpy(require_ifs, entry->data, sizeof require_ifs);
	char *reqp = require_ifs;

	for (char *tokenp = lif_next_token(&reqp); *tokenp; tokenp = lif_next_token(&reqp))
	{
		struct lif_interface *child_if = lif_interface_collection_find(collection, tokenp);

		print_interface_dot(collection, child_if, iface);
		child_if->is_up = true;
	}
}

static inline size_t
count_set_bits(const char *netmask)
{
	/* netmask set to CIDR length */
	if (strchr(netmask, '.') == NULL)
		return strtol(netmask, NULL, 10);

	size_t r = 0;
	struct in_addr in;

	if (inet_pton(AF_INET, netmask, &in) == 0)
		return r;

	/* take the IP, put it in host endian order, and
	 * flip it so that all the set bits are set to the right.
	 * then we can simply count down from 32 and right-shift
	 * until the bit field is all zero.
	 */
	unsigned int bits = htonl(in.s_addr);
	for (bits = ~bits, r = 32; bits; bits >>= 1, r--)
		;

	return r;
}

void
print_interface_property(struct lif_interface *iface, const char *property)
{
	struct lif_node *iter;
	bool printing_address = !strcmp(property, "address");

	LIF_DICT_FOREACH(iter, &iface->vars)
	{
		struct lif_dict_entry *entry = iter->data;

		if (strcmp(entry->key, property))
			continue;

		if (printing_address)
		{
			struct lif_address *addr = entry->data;
			size_t orig_netmask = addr->netmask;

			if (!addr->netmask)
			{
				/* if fallback netmask is not set, default to 255.255.255.0 */
				addr->netmask = 24;

				struct lif_dict_entry *entry = lif_dict_find(&iface->vars, "netmask");
				if (entry != NULL)
					addr->netmask = count_set_bits(entry->data);
			}

			char addr_buf[512];

			if (!lif_address_unparse(addr, addr_buf, sizeof addr_buf, true))
				continue;

			addr->netmask = orig_netmask;

			printf("%s\n", addr_buf);
		}
		else
			printf("%s\n", (const char *) entry->data);
	}
}

void
list_interfaces(struct lif_dict *collection, struct match_options *opts)
{
	struct lif_node *iter;

	if (opts->dot)
	{
		printf("digraph interfaces {\n");
		printf("edge [color=blue fontname=Sans fontsize=10]\n");
		printf("node [fontname=Sans fontsize=10]\n");
	}

	LIF_DICT_FOREACH(iter, collection)
	{
		struct lif_dict_entry *entry = iter->data;
		struct lif_interface *iface = entry->data;

		if (opts->is_auto && !iface->is_auto)
			continue;

		if (opts->exclude_pattern != NULL &&
		    !fnmatch(opts->exclude_pattern, iface->ifname, 0))
			continue;

		if (opts->include_pattern != NULL &&
		    fnmatch(opts->include_pattern, iface->ifname, 0))
			continue;

		if (opts->pretty_print)
			print_interface(iface);
		else if (opts->dot)
			print_interface_dot(collection, iface, NULL);
		else
			printf("%s\n", iface->ifname);
	}

	if (opts->dot)
		printf("}\n");
}

void
list_state(struct lif_dict *state, struct match_options *opts)
{
	struct lif_node *iter;

	LIF_DICT_FOREACH(iter, state)
	{
		struct lif_dict_entry *entry = iter->data;

		if (opts->exclude_pattern != NULL &&
		    !fnmatch(opts->exclude_pattern, entry->key, 0))
			continue;

		if (opts->include_pattern != NULL &&
		    fnmatch(opts->include_pattern, entry->key, 0))
			continue;

		printf("%s=%s\n", entry->key, (const char *) entry->data);
	}
}

static bool listing = false, listing_stat = false;

static void
handle_local(int short_opt, const struct if_option *opt, const char *opt_arg, const struct if_applet *applet)
{
	(void) opt;
	(void) applet;

	switch (short_opt) {
	case 'L':
		listing = true;
		break;
	case 'P':
		match_opts.pretty_print = true;
		break;
	case 's':
		listing_stat = true;
		break;
	case 'D':
		match_opts.dot = true;
		break;
	case 'p':
		match_opts.property = opt_arg;
		break;
	}
}

static struct if_option local_options[] = {
	{'s', "state", NULL, "show configured state", false, handle_local},
	{'p', "property", "property PROPERTY", "print values of properties matching PROPERTY", true, handle_local},
	{'D', "dot", NULL, "generate a dependency graph", false, handle_local},
	{'L', "list", NULL, "list matching interfaces", false, handle_local},
	{'P', "pretty-print", NULL, "pretty print the interfaces instead of just listing", false, handle_local},
};

static struct if_option_group local_option_group = {
	.desc = "Program-specific options",
	.group_size = ARRAY_SIZE(local_options),
	.group = local_options
};

int
ifquery_main(int argc, char *argv[])
{
	struct lif_dict state = {};
	struct lif_dict collection = {};

	if (!lif_state_read_path(&state, exec_opts.state_file))
	{
		fprintf(stderr, "%s: could not parse %s\n", argv0, exec_opts.state_file);
		return EXIT_FAILURE;
	}

	if (!lif_interface_file_parse(&collection, exec_opts.interfaces_file))
	{
		fprintf(stderr, "%s: could not parse %s\n", argv0, exec_opts.interfaces_file);
		return EXIT_FAILURE;
	}

	/* --list --state is not allowed */
	if (listing && listing_stat)
		generic_usage(self_applet, EXIT_FAILURE);

	if (listing)
	{
		list_interfaces(&collection, &match_opts);
		return EXIT_SUCCESS;
	}
	else if (listing_stat)
	{
		list_state(&state, &match_opts);
		return EXIT_SUCCESS;
	}

	if (optind >= argc)
		generic_usage(self_applet, EXIT_FAILURE);

	int idx = optind;
	for (; idx < argc; idx++)
	{
		struct lif_interface *iface = lif_state_lookup(&state, &collection, argv[idx]);

		if (iface == NULL)
		{
			struct lif_dict_entry *entry = lif_dict_find(&collection, argv[idx]);

			if (entry != NULL)
				iface = entry->data;
		}

		if (iface == NULL)
		{
			fprintf(stderr, "%s: unknown interface %s\n", argv0, argv[idx]);
			return EXIT_FAILURE;
		}

		if (match_opts.property != NULL)
			print_interface_property(iface, match_opts.property);
		else
			print_interface(iface);
	}

	return EXIT_SUCCESS;
}

struct if_applet ifquery_applet = {
	.name = "ifquery",
	.desc = "query interface configuration",
	.main = ifquery_main,
	.usage = "ifquery [options] <interfaces>\n  ifquery [options] --list",
	.groups = { &global_option_group, &match_option_group, &exec_option_group, &local_option_group },
};
