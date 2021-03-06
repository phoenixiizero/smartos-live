/*
 * Copyright (c) 2012, Chris Andrews. All rights reserved.
 */

#include "usdt_internal.h"

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

char *usdt_errors[] = {
  "failed to allocate memory",
  "failed to allocate page-aligned memory",
  "no probes defined",
  "failed to load DOF: %s",
  "provider is already enabled",
  "failed to unload DOF: %s",
  "probe named %s:%s:%s:%s already exists",
  "failed to remove probe %s:%s:%s:%s"
};

usdt_provider_t *
usdt_create_provider(const char *name, const char *module)
{
        usdt_provider_t *provider;

        if ((provider = malloc(sizeof *provider)) == NULL) 
                return NULL;

        provider->name = strdup(name);
        provider->module = strdup(module);
        provider->probedefs = NULL;
        provider->enabled = 0;

        return provider;
}

usdt_probedef_t *
usdt_create_probe(const char *func, const char *name, size_t argc, const char **types)
{
        int i;
        usdt_probedef_t *p;

        if (argc > USDT_ARG_MAX)
                argc = USDT_ARG_MAX;

        if ((p = malloc(sizeof *p)) == NULL)
                return (NULL);

        p->function = strdup(func);
        p->name = strdup(name);
        p->argc = argc;
        p->probe = NULL;

        for (i = 0; i < argc; i++) {
                if (strncmp("char *", types[i], 6) == 0)
                        p->types[i] = USDT_ARGTYPE_STRING;
                if (strncmp("int", types[i], 3) == 0)
                        p->types[i] = USDT_ARGTYPE_INTEGER;
        }

        return (p);
}

int
usdt_provider_add_probe(usdt_provider_t *provider, usdt_probedef_t *probedef)
{
        usdt_probedef_t *pd;

        if (provider->probedefs != NULL) {
                for (pd = provider->probedefs; (pd != NULL); pd = pd->next) {
                if ((strcmp(pd->name, probedef->name) == 0) &&
                    (strcmp(pd->function, probedef->function) == 0)) {
                                usdt_error(provider, USDT_ERROR_DUP_PROBE,
                                           provider->name, provider->module,
                                           probedef->function, probedef->name);
                                return (-1);
                        }
                }
        }

        probedef->next = NULL;
        if (provider->probedefs == NULL)
                provider->probedefs = probedef;
        else {
                for (pd = provider->probedefs; (pd->next != NULL); pd = pd->next) ;
                pd->next = probedef;
        }

        return (0);
}

int
usdt_provider_remove_probe(usdt_provider_t *provider, usdt_probedef_t *probedef)
{
        usdt_probedef_t *pd, *prev_pd = NULL;

        if (provider->probedefs == NULL) {
                usdt_error(provider, USDT_ERROR_NOPROBES);
                return (-1);
        }

        for (pd = provider->probedefs; (pd != NULL);
             prev_pd = pd, pd = pd->next) {

                if ((strcmp(pd->name, probedef->name) == 0) &&
                    (strcmp(pd->function, probedef->function) == 0)) {

                        if (prev_pd == NULL)
                                provider->probedefs = pd->next;
                        else
                                prev_pd->next = pd->next;

                        /* free(probedef); */
                        return (0);
                }
        }

        usdt_error(provider, USDT_ERROR_REMOVE_PROBE,
                   provider->name, provider->module,
                   probedef->function, probedef->name);
        return (-1);
}

int
usdt_provider_enable(usdt_provider_t *provider)
{
        usdt_strtab_t strtab;
        usdt_dof_file_t *file;
        usdt_probedef_t *pd;
        int i;
        size_t size;
        usdt_dof_section_t sects[5];

        if (provider->enabled == 1) {
                usdt_error(provider, USDT_ERROR_ALREADYENABLED);
                return (0); /* not fatal */
        }

        if (provider->probedefs == NULL) {
                usdt_error(provider, USDT_ERROR_NOPROBES);
                return (-1);
        }

        for (pd = provider->probedefs; pd != NULL; pd = pd->next) {
                if ((pd->probe = malloc(sizeof(*pd->probe))) == NULL) {
                        usdt_error(provider, USDT_ERROR_MALLOC);
                        return (-1);
                }
        }

        if ((usdt_strtab_init(&strtab, 0)) < 0) {
                usdt_error(provider, USDT_ERROR_MALLOC);
                return (-1);
        }

        if ((usdt_strtab_add(&strtab, provider->name)) == 0) {
                usdt_error(provider, USDT_ERROR_MALLOC);
                return (-1);
        }

        if ((usdt_dof_probes_sect(&sects[0], provider, &strtab)) < 0)
                return (-1);
        if ((usdt_dof_prargs_sect(&sects[1], provider)) < 0)
                return (-1);

        size = usdt_provider_dof_size(provider, &strtab);
        if ((file = usdt_dof_file_init(provider, size)) == NULL)
                return (-1);

        if ((usdt_dof_proffs_sect(&sects[2], provider, file->dof)) < 0)
                return (-1);
        if ((usdt_dof_prenoffs_sect(&sects[3], provider, file->dof)) < 0)
                return (-1);
        if ((usdt_dof_provider_sect(&sects[4], provider)) < 0)
                return (-1);

        for (i = 0; i < 5; i++)
                usdt_dof_file_append_section(file, &sects[i]);

        usdt_dof_file_generate(file, &strtab);

        if ((usdt_dof_file_load(file, provider->module)) < 0) {
                usdt_error(provider, USDT_ERROR_LOADDOF, strerror(errno));
                return (-1);
        }

        provider->enabled = 1;
        provider->file = file;

        return (0);
}

int
usdt_provider_disable(usdt_provider_t *provider)
{
        if (provider->enabled == 0)
                return (0);

        if ((usdt_dof_file_unload((usdt_dof_file_t *)provider->file)) < 0) {
                usdt_error(provider, USDT_ERROR_UNLOADDOF, strerror(errno));
                return (-1);
        }

        /* free(file) */
        provider->file = NULL;
        provider->enabled = 0;

        return (0);
}

int
usdt_is_enabled(usdt_probe_t *probe)
{
        if (probe != NULL)
                return (*probe->isenabled_addr)();
        else
                return 0;
}

void
usdt_fire_probe(usdt_probe_t *probe, size_t argc, void **nargv)
{
        if (probe != NULL)
                usdt_probe_args(probe->probe_addr, argc, nargv);
}

static void
usdt_verror(usdt_provider_t *provider, usdt_error_t error, va_list argp)
{
        vasprintf(&provider->error, usdt_errors[error], argp);
}

void
usdt_error(usdt_provider_t *provider, usdt_error_t error, ...)
{
        va_list argp;

        va_start(argp, error);
        usdt_verror(provider, error, argp);
        va_end(argp);
}

char *
usdt_errstr(usdt_provider_t *provider)
{
        return (provider->error);
}
