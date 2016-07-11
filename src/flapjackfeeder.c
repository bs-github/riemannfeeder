/*****************************************************************************
 *
 * RIEMANNFEEDER.C
 * Copyright (c) 2016 Birger Schmidt
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include "naemon.h"
#include "string.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

#include <riemann/riemann-client.h>
#include <riemann/simple.h>

/* specify event broker API version (required) */
NEB_API_VERSION(CURRENT_NEB_API_VERSION);

void *npcdmod_module_handle = NULL;
char *riemann_connect_retry_interval = "15";
struct timeval timeout = { 1, 500000 }; // 1.5 seconds

/* riemann target structure */
typedef struct riemanntarget_struct {
    char         *host;
    int          port;
    int          riemann_connection_established;
    riemann_client_t *riemanncontext;
    struct riemanntarget_struct *next;
} riemanntarget;

/* here will be all our riemann targets */
riemanntarget *riemanntargets = NULL;

riemann_message_t *r;

void riemann_re_connect();
int npcdmod_handle_data(int, void *);

int npcdmod_process_config_var(char *arg);
int npcdmod_process_module_args(char *args);

char servicestate[][10] = { "OK", "WARNING", "CRITICAL", "UNKNOWN", };
char hoststate[][12] = { "OK", "CRITICAL", "CRITICAL", };

/* this function gets called when the module is loaded by the event broker */
int nebmodule_init(int flags, char *args, nebmodule *handle) {
    char temp_buffer[1024];

    /* save our handle */
    npcdmod_module_handle = handle;

    /* set some info - this is completely optional, as Nagios doesn't do anything with this data */
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_TITLE, "riemannfeeder");
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_AUTHOR, "Birger Schmidt");
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_COPYRIGHT, "Copyright (c) 2016 Birger Schmidt");
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_VERSION, VERSION);
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_LICENSE, "GPL v2");
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_DESC, "A simple check result extractor / riemann writer.");

    /* log module info to the Nagios log file */
    nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: Copyright (c) 2016 Birger Schmidt");
    nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: This is version '" VERSION "' running.");

    /* process arguments */
    if (npcdmod_process_module_args(args) == ERROR) {
        nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: An error occurred while attempting to process module arguments.");
        return -1;
    }

    /* connect to riemann initially */
    /* this will register for an event every 15 seconds to check (and reconnect) the riemann connections */
    riemann_re_connect();

    /* register to be notified of certain events... */
    neb_register_callback(NEBCALLBACK_HOST_CHECK_DATA,
            npcdmod_module_handle, 0, npcdmod_handle_data);
    neb_register_callback(NEBCALLBACK_SERVICE_CHECK_DATA,
            npcdmod_module_handle, 0, npcdmod_handle_data);
    return 0;
}

/* this function gets called when the module is unloaded by the event broker */
int nebmodule_deinit(int flags, int reason) {
    char temp_buffer[1024];

    /* deregister for all events we previously registered for... */
    neb_deregister_callback(NEBCALLBACK_HOST_CHECK_DATA,npcdmod_handle_data);
    neb_deregister_callback(NEBCALLBACK_SERVICE_CHECK_DATA,npcdmod_handle_data);

    /* log a message to the Nagios log file */
    nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: Deinitializing riemannfeeder nagios event broker module.\n");

    return 0;
}

/* gets called every X seconds by an event in the scheduling queue */
void riemann_re_connect() {
    char hostname[1024];
    time_t current_time;

    riemanntarget *currentriemanntarget = riemanntargets;
    while (currentriemanntarget != NULL) {
        /* open riemann connection to push check results if needed */
        if (currentriemanntarget->riemanncontext == NULL || currentriemanntarget->riemann_connection_established == 0) {
            nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: riemann connection (%s:%d) has to be (re)established.",
                currentriemanntarget->host, currentriemanntarget->port);

            currentriemanntarget->riemanncontext = riemann_client_create(RIEMANN_CLIENT_TCP, currentriemanntarget->host, currentriemanntarget->port
                         //,
					     //RIEMANN_CLIENT_OPTION_TLS_CA_FILE, host->tls_ca_file,
					     //RIEMANN_CLIENT_OPTION_TLS_CERT_FILE, host->tls_cert_file,
					     //RIEMANN_CLIENT_OPTION_TLS_KEY_FILE, host->tls_key_file,
					     //RIEMANN_CLIENT_OPTION_NONE
                         );
            currentriemanntarget->riemann_connection_established = 0;
            if (riemann_client_set_timeout(currentriemanntarget->riemanncontext, &timeout) != 0) {
                riemann_client_free(currentriemanntarget->riemanncontext);
                currentriemanntarget->riemanncontext = NULL;
                nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: riemann connection error (%s:%d), I'll retry to connect regulary.",
                        currentriemanntarget->host, currentriemanntarget->port);
            } else {
                currentriemanntarget->riemann_connection_established = 1;
                nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: riemann connection (%s:%d) established.",
                    currentriemanntarget->host, currentriemanntarget->port);
                time(&current_time);
                gethostname(hostname, 1024);
                r = riemann_communicate_event(currentriemanntarget->riemanncontext,
                        RIEMANN_EVENT_FIELD_HOST, hostname,
                        RIEMANN_EVENT_FIELD_SERVICE, "riemannfeeder",
                        RIEMANN_EVENT_FIELD_STATE, "ok",
                        RIEMANN_EVENT_FIELD_TAGS, "naemon-client", "riemann-c-client", "riemannfeeder", NULL,
                        RIEMANN_EVENT_FIELD_TIME, (int64_t)current_time,
                        RIEMANN_EVENT_FIELD_NONE);
                riemann_message_free (r);
            }
        }
        currentriemanntarget = currentriemanntarget->next;
    }

    /* Recurring event */
    schedule_event(atoi(riemann_connect_retry_interval), riemann_re_connect, NULL);

    return;
}

/* handle data from Nagios daemon */
int npcdmod_handle_data(int event_type, void *data) {
    nebstruct_host_check_data *hostchkdata = NULL;
    nebstruct_service_check_data *srvchkdata = NULL;

    host *host=NULL;
    service *service=NULL;

    char temp_buffer[1024];

    /* what type of event/data do we have? */
    switch (event_type) {

    case NEBCALLBACK_HOST_CHECK_DATA:
        /* an aggregated status data dump just started or ended... */
        if ((hostchkdata = (nebstruct_host_check_data *) data)) {

            host = find_host(hostchkdata->host_name);

            if (hostchkdata->type == NEBTYPE_HOSTCHECK_PROCESSED) {

                riemanntarget *currentriemanntarget = riemanntargets;
                while (currentriemanntarget != NULL) {
                    if (currentriemanntarget->riemann_connection_established) {
                        r = riemann_communicate_event(currentriemanntarget->riemanncontext,
                                RIEMANN_EVENT_FIELD_HOST, hostchkdata->host_name,
                                //RIEMANN_EVENT_FIELD_SERVICE, "HOST", // default -> nil
                                RIEMANN_EVENT_FIELD_STATE, hoststate[hostchkdata->state],
                                RIEMANN_EVENT_FIELD_TAGS, "naemon-client", "riemann-c-client", NULL,
                                RIEMANN_EVENT_FIELD_TIME, (int64_t)hostchkdata->timestamp.tv_sec,
                                RIEMANN_EVENT_FIELD_STRING_ATTRIBUTES,
                                    "output", hostchkdata->output,
                                    "long_output", hostchkdata->long_output,
                                    NULL,
                                RIEMANN_EVENT_FIELD_NONE);
                        if (!r || r->error || (r->has_ok && !r->ok)) {
                            nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: riemann write (%s:%d) fail, lost check result (%s - %s).",
                                currentriemanntarget->host, currentriemanntarget->port,
                                hostchkdata->host_name, hoststate[hostchkdata->state]);
                            currentriemanntarget->riemann_connection_established = 0;
                        }
                        riemann_message_free (r);
                    } else {
                        nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: riemann connection (%s:%d) fail, lost check result (host %s - %s).",
                            currentriemanntarget->host, currentriemanntarget->port,
                            hostchkdata->host_name, hoststate[hostchkdata->state]);
                    }
                    currentriemanntarget = currentriemanntarget->next;
                }
            }
        }
        break;

    case NEBCALLBACK_SERVICE_CHECK_DATA:
        /* an aggregated status data dump just started or ended... */
        if ((srvchkdata = (nebstruct_service_check_data *) data)) {

            if (srvchkdata->type == NEBTYPE_SERVICECHECK_PROCESSED) {

                /* find the nagios service object for this service */
                service = find_service(srvchkdata->host_name, srvchkdata->service_description);

                riemanntarget *currentriemanntarget = riemanntargets;
                while (currentriemanntarget != NULL) {
                    if (currentriemanntarget->riemann_connection_established) {
                        r = riemann_communicate_event(currentriemanntarget->riemanncontext,
                                RIEMANN_EVENT_FIELD_HOST, srvchkdata->host_name,
                                RIEMANN_EVENT_FIELD_SERVICE, srvchkdata->service_description,
                                RIEMANN_EVENT_FIELD_STATE, servicestate[srvchkdata->state],
                                RIEMANN_EVENT_FIELD_TAGS, "naemon-client", "riemann-c-client", NULL,
                                RIEMANN_EVENT_FIELD_TIME, (int64_t)srvchkdata->timestamp.tv_sec,
                                RIEMANN_EVENT_FIELD_STRING_ATTRIBUTES,
                                    "output", srvchkdata->output,
                                    "long_output", srvchkdata->long_output,
                                    NULL,
                                RIEMANN_EVENT_FIELD_NONE);
                        if (!r || r->error || (r->has_ok && !r->ok)) {
                            nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: riemann write (%s:%d) fail, lost check result (%s : %s - %s).",
                                currentriemanntarget->host, currentriemanntarget->port,
                                srvchkdata->host_name, srvchkdata->service_description, servicestate[srvchkdata->state]);
                            currentriemanntarget->riemann_connection_established = 0;
                        }
                        riemann_message_free (r);
                    } else {
                        nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: riemann connection (%s:%d) fail, lost check result (%s : %s - %s).",
                            currentriemanntarget->host, currentriemanntarget->port,
                            srvchkdata->host_name, srvchkdata->service_description, servicestate[srvchkdata->state]);
                    }
                    currentriemanntarget = currentriemanntarget->next;
                }
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

/****************************************************************************/
/* CONFIG FUNCTIONS                                                         */
/****************************************************************************/

/* process arguments that were passed to the module at startup */
int npcdmod_process_module_args(char *args) {
    char *ptr = NULL;
    char **arglist = NULL;
    char **newarglist = NULL;
    int argcount = 0;
    int memblocks = 64;
    int arg = 0;

    if (args == NULL) {
        // fill riemanntarget with defaults (if parameters are missing from module config)
        /* allocate memory for a new riemann target */
        if ((riemanntargets = malloc(sizeof(riemanntarget))) == NULL) {
            nm_log(NSLOG_INFO_MESSAGE, "Error: Could not allocate memory for riemann target\n");
        }
        riemanntargets->host = "127.0.0.1";
        riemanntargets->port = 5555;
        riemanntargets->riemann_connection_established = 0;
    	riemanntargets->next = NULL;
        return OK;
    }

    /* get all the var/val argument pairs */

    /* allocate some memory */
    if ((arglist = (char **) malloc(memblocks * sizeof(char **))) == NULL)
        return ERROR;

    /* process all args */
    ptr = strtok(args, ",");
    while (ptr) {

        /* save the argument */
        arglist[argcount++] = strdup(ptr);

        /* allocate more memory if needed */
        if (!(argcount % memblocks)) {
            if ((newarglist = (char **) realloc(arglist, (argcount + memblocks)
                    * sizeof(char **))) == NULL) {
                for (arg = 0; arg < argcount; arg++)
                    nm_free(arglist[argcount]);
                nm_free(arglist);
                return ERROR;
            } else
                arglist = newarglist;
        }

        ptr = strtok(NULL, ",");
    }

    /* terminate the arg list */
    arglist[argcount] = NULL;

    /* process each argument */
    for (arg = 0; arg < argcount; arg++) {
        if (npcdmod_process_config_var(arglist[arg]) == ERROR) {
            for (arg = 0; arg < argcount; arg++)
                nm_free(arglist[arg]);
            nm_free(arglist);
            return ERROR;
        }
    }

    if (riemanntargets == NULL || riemanntargets->host == NULL //|| riemanntargets->port == NULL ||
        ) {
        nm_log(NSLOG_CONFIG_ERROR, "riemannfeeder: Error: You have to configure at least one riemann target tuple (i.e. riemann_host=localhost,riemann_port=5555)");
        return ERROR;
    }

    /* free allocated memory */
    for (arg = 0; arg < argcount; arg++)
        nm_free(arglist[arg]);
    nm_free(arglist);

    return OK;
}

/* process a single module config variable */
int npcdmod_process_config_var(char *arg) {
    char temp_buffer[1024];
    char *var = NULL;
    char *val = NULL;

    /* split var/val */
    var = strtok(arg, "=");
    val = strtok(NULL, "\n");

    /* skip incomplete var/val pairs */
    if (var == NULL || val == NULL)
        return OK;

    /* strip var/val */
    strip(var);
    strip(val);

	riemanntarget *new_riemanntarget = NULL;

    /* process the variable... */
    if (!strcmp(var, "riemann_host")) {
        // fill riemanntarget structure
        if (riemanntargets == NULL || riemanntargets->host != NULL) {
            /* allocate memory for a new riemann target */
            if ((new_riemanntarget = malloc(sizeof(riemanntarget))) == NULL) {
                nm_log(NSLOG_INFO_MESSAGE, "Error: Could not allocate memory for riemann target\n");
            }
            new_riemanntarget->host = NULL;
            new_riemanntarget->port = 0;
            new_riemanntarget->riemanncontext = NULL;
            new_riemanntarget->riemann_connection_established = 0;
        }
        if (riemanntargets != NULL && riemanntargets->host == NULL) {
            riemanntargets->host = strdup(val);
        }
        else {
            new_riemanntarget->host = strdup(val);
        }
        if (new_riemanntarget != NULL) {
        	/* add the new riemanntarget to the head of the riemanntarget list */
        	new_riemanntarget->next = riemanntargets;
        	riemanntargets = new_riemanntarget;
        }
    }

    else if (!strcmp(var, "riemann_port")) {
        // fill riemanntarget structure
        if (riemanntargets == NULL || riemanntargets->port != 0) {
            /* allocate memory for a new riemann target */
            if ((new_riemanntarget = malloc(sizeof(riemanntarget))) == NULL) {
                nm_log(NSLOG_INFO_MESSAGE, "Error: Could not allocate memory for riemann target");
            }
            new_riemanntarget->host = NULL;
            new_riemanntarget->port = 0;
            new_riemanntarget->riemanncontext = NULL;
            new_riemanntarget->riemann_connection_established = 0;
        }
        if (riemanntargets != NULL && riemanntargets->port == 0) {
            riemanntargets->port = atoi(val);
        }
        else {
            new_riemanntarget->port = atoi(val);
        }
        if (new_riemanntarget != NULL) {
        	/* add the new riemanntarget to the head of the riemanntarget list */
        	new_riemanntarget->next = riemanntargets;
        	riemanntargets = new_riemanntarget;
        }
    }

    else if (!strcmp(var, "riemann_connect_retry_interval")) {
        riemann_connect_retry_interval = strdup(val);
        nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: configure %ss as retry interval for riemann reconnects.", riemann_connect_retry_interval);
    }

    else if (!strcmp(var, "timeout")) {
        timeout.tv_sec = atoi(val);
        timeout.tv_usec = 0;
        nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: configure %ss as timeout for riemann connects/writes.", val);
    }

    else {
        nm_log(NSLOG_INFO_MESSAGE, "riemannfeeder: I don't know what to do with '%s' as argument.", var);
        return ERROR;
    }

    return OK;
}
