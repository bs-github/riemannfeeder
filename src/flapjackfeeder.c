/*****************************************************************************
 *
 * FLAPJACKFEEDER.C
 * Copyright (c) 2013-2015 Birger Schmidt (http://flapjack.io)
 *
 * Derived from NPCDMOD.C ...
 * Copyright (c) 2008-2010 PNP4Nagios Project (http://www.pnp4nagios.org)
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

#ifdef HAVE_NAEMON_H
/* we compile for the naemon core ( -DHAVE_NAEMON_H was given as compile option ) */
#include "../naemon/naemon.h"
#include "string.h"
#else
/* we compile for the legacy nagios 3 / icinga 1 core */

/* include (minimum required) event broker header files */
#include "../include/nebmodules.h"
#include "../include/nebcallbacks.h"

/* include other event broker header files that we need for our work */
#include "../include/nebstructs.h"
#include "../include/broker.h"

/* include some Nagios stuff as well */
#include "../include/config.h"
#include "../include/common.h"
#include "../include/nagios.h"
#endif

/* include some pnp stuff */
#include "../include/pnp.h"
#include "../include/npcdmod.h"

/* include redis stuff */
#include "../hiredis/hiredis.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

#include <riemann/riemann-client.h>
#include <riemann/simple.h>

/* specify event broker API version (required) */
NEB_API_VERSION(CURRENT_NEB_API_VERSION);

void *npcdmod_module_handle = NULL;
char *redis_connect_retry_interval = "15";
struct timeval timeout = { 1, 500000 }; // 1.5 seconds

int flapjack_version = 1;

/* redis target structure */
typedef struct redistarget_struct {
    char         *host;
    int          port;
    char         *redis_database;
    char         *redis_queue;
    int          redis_connection_established;
    riemann_client_t *rediscontext;
    struct redistarget_struct *next;
} redistarget;

/* here will be all our redis targets */
redistarget *redistargets = NULL;

redisReply *reply;

void redis_re_connect();
int npcdmod_handle_data(int, void *);

int npcdmod_process_config_var(char *arg);
int npcdmod_process_module_args(char *args);

char servicestate[][10] = { "OK", "WARNING", "CRITICAL", "UNKNOWN", };
char hoststate[][12] = { "OK", "CRITICAL", "CRITICAL", };

int count_escapes(const char *src);
char *expand_escapes(const char* src);

int generate_event(char *buffer, size_t buffer_size, char *host_name, char *service_name,
                   char *state, char *output, char *long_output, char *tags,
                   long initial_failure_delay, long repeat_failure_delay,
                   long initial_recovery_delay, int event_time);

/* this function gets called when the module is loaded by the event broker */
int nebmodule_init(int flags, char *args, nebmodule *handle) {
    char temp_buffer[1024];
    time_t current_time;

    /* save our handle */
    npcdmod_module_handle = handle;

    /* set some info - this is completely optional, as Nagios doesn't do anything with this data */
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_TITLE, "flapjackfeeder");
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_AUTHOR, "Birger Schmidt");
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_COPYRIGHT, "Copyright (c) 2013-2015 Birger Schmidt");
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_VERSION, VERSION);
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_LICENSE, "GPL v2");
    neb_set_module_info(npcdmod_module_handle, NEBMODULE_MODINFO_DESC, "A simple performance data / check result extractor / redis writer.");

    /* log module info to the Nagios log file */
    write_to_all_logs("flapjackfeeder: Copyright (c) 2013-2015 Birger Schmidt, derived from npcdmod", NSLOG_INFO_MESSAGE);
    write_to_all_logs("flapjackfeeder: This is version '" VERSION "' running.", NSLOG_INFO_MESSAGE);

    /* process arguments */
    if (npcdmod_process_module_args(args) == ERROR) {
        write_to_all_logs("flapjackfeeder: An error occurred while attempting to process module arguments.", NSLOG_INFO_MESSAGE);
        return -1;
    }

    /* connect to redis initially */
    redis_re_connect();

    /* register for an event every 15 seconds to check (and reconnect) the redis connections */
    time(&current_time);
    schedule_new_event(EVENT_USER_FUNCTION,TRUE, current_time + atoi(redis_connect_retry_interval), TRUE,
    atoi(redis_connect_retry_interval), NULL, TRUE, (void *) redis_re_connect, "", 0);

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
    snprintf(temp_buffer, sizeof(temp_buffer) - 1,
            "flapjackfeeder: Deinitializing flapjackfeeder nagios event broker module.\n");
    temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
    write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);

    return 0;
}

/* gets called every X seconds by an event in the scheduling queue */
void redis_re_connect() {
    char temp_buffer[1024];

    redistarget *currentredistarget = redistargets;
    while (currentredistarget != NULL) {
        /* open redis connection to push check results if needed */
        if (currentredistarget->rediscontext == NULL || currentredistarget->redis_connection_established == 0) {
            snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: redis connection (%s:%d) has to be (re)established.",
                currentredistarget->host, currentredistarget->port);
            temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
            write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);

            //currentredistarget->rediscontext = redisConnectWithTimeout(currentredistarget->host, atoi(currentredistarget->port), timeout);
            currentredistarget->rediscontext = riemann_client_create(RIEMANN_CLIENT_TCP, currentredistarget->host, currentredistarget->port
                         //,
					     //RIEMANN_CLIENT_OPTION_TLS_CA_FILE, host->tls_ca_file,
					     //RIEMANN_CLIENT_OPTION_TLS_CERT_FILE, host->tls_cert_file,
					     //RIEMANN_CLIENT_OPTION_TLS_KEY_FILE, host->tls_key_file,
					     //RIEMANN_CLIENT_OPTION_NONE
                         );
            currentredistarget->redis_connection_established = 0;
            //redisSetTimeout(currentredistarget->rediscontext, timeout);
            if (riemann_client_set_timeout(currentredistarget->rediscontext, &timeout) != 0) {
                riemann_client_free(currentredistarget->rediscontext);
                currentredistarget->rediscontext = NULL;
                snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: redis connection error (%s:%d), I'll retry to connect regulary.",
                        currentredistarget->host, currentredistarget->port);
            } else {
                currentredistarget->redis_connection_established = 1;
                snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: redis connection (%s:%d) established.",
                    currentredistarget->host, currentredistarget->port);

            }
            temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
            write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
        }
        ///*
        else {
            snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: redis connection (%s:%d) seems to be fine.",
                currentredistarget->host, currentredistarget->port);
            temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
            write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
        }
        //*/
        currentredistarget = currentredistarget->next;
    }

    return;
}

/* handle data from Nagios daemon */
int npcdmod_handle_data(int event_type, void *data) {
    nebstruct_host_check_data *hostchkdata = NULL;
    nebstruct_service_check_data *srvchkdata = NULL;

    host *host=NULL;
    service *service=NULL;

    char temp_buffer[1024];
    char push_buffer[PERFDATA_BUFFER];
    int written;


    /* what type of event/data do we have? */
    switch (event_type) {

    case NEBCALLBACK_HOST_CHECK_DATA:
        /* an aggregated status data dump just started or ended... */
        if ((hostchkdata = (nebstruct_host_check_data *) data)) {

            host = find_host(hostchkdata->host_name);

            customvariablesmember *currentcustomvar = host->custom_variables;
            long initial_failure_delay  = 0;
            long repeat_failure_delay   = 0;
            long initial_recovery_delay = 0;
            char *cur = temp_buffer, * const end = temp_buffer + sizeof temp_buffer;
            temp_buffer[0] = '\x0';
            while (currentcustomvar != NULL) {
                if (strcmp(currentcustomvar->variable_name, "TAG") == 0) {
                  cur += snprintf(cur, end - cur,
                      "\"%s\",",
                      currentcustomvar->variable_value
                      );
                }
                else if (strcmp(currentcustomvar->variable_name, "INITIAL_FAILURE_DELAY") == 0) {
                      initial_failure_delay = strtol(currentcustomvar->variable_value,NULL,10);
                }
                else if (strcmp(currentcustomvar->variable_name, "REPEAT_FAILURE_DELAY") == 0) {
                      repeat_failure_delay = strtol(currentcustomvar->variable_value,NULL,10);
                }
                else if (strcmp(currentcustomvar->variable_name, "INITIAL_RECOVERY_DELAY") == 0) {
                      initial_recovery_delay = strtol(currentcustomvar->variable_value,NULL,10);
                }
                currentcustomvar = currentcustomvar->next;
            }
            cur--;
            if (strcmp(cur, ",") == 0) {
                cur[0] = '\x0';
            }
            else {
                cur++;
            }

            my_free(currentcustomvar);

            if (hostchkdata->type == NEBTYPE_HOSTCHECK_PROCESSED) {

                int written = generate_event(push_buffer, PERFDATA_BUFFER,
                    hostchkdata->host_name,
                    "HOST",
                    hoststate[hostchkdata->state],
                    hostchkdata->output,
                    hostchkdata->long_output,
                    temp_buffer,
                    initial_failure_delay,
                    repeat_failure_delay,
                    initial_recovery_delay,
                    (int)hostchkdata->timestamp.tv_sec);

                redistarget *currentredistarget = redistargets;
                while (currentredistarget != NULL) {
                    if (written >= PERFDATA_BUFFER) {
                        snprintf(temp_buffer, sizeof(temp_buffer) - 1,
                            "flapjackfeeder: Buffer size of %d in npcdmod.h is too small, ignoring data for %s\n",
                            PERFDATA_BUFFER, hostchkdata->host_name);
                        temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
                        write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
                    } else if (currentredistarget->redis_connection_established) {
                        //reply = redisCommand(currentredistarget->rediscontext,"LPUSH %s %s", currentredistarget->redis_queue, push_buffer);

                        if (reply != NULL) {
                            //freeReplyObject(reply);

                        } else {
                            snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: redis write (%s:%d) fail, lost check result (host %s - %s).",
                                currentredistarget->host, currentredistarget->port,
                                hostchkdata->host_name, hoststate[hostchkdata->state]);
                            temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
                            write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
                            currentredistarget->redis_connection_established = 0;
                            //redisFree(currentredistarget->rediscontext);
                        }
                    } else {
                        snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: redis connection (%s:%d) fail, lost check result (host %s - %s).",
                            currentredistarget->host, currentredistarget->port,
                            hostchkdata->host_name, hoststate[hostchkdata->state]);
                        temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
                        write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
                    }
                    currentredistarget = currentredistarget->next;
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

                customvariablesmember *currentcustomvar = service->custom_variables;
                long initial_failure_delay  = 0;
                long repeat_failure_delay   = 0;
                long initial_recovery_delay = 0;
                char *cur = temp_buffer, * const end = temp_buffer + sizeof temp_buffer;
                temp_buffer[0] = '\x0';
                while (currentcustomvar != NULL) {
                    if (strcmp(currentcustomvar->variable_name, "TAG") == 0) {
                      cur += snprintf(cur, end - cur,
                          "\"%s\",",
                          currentcustomvar->variable_value
                          );
                    }
                    else if (strcmp(currentcustomvar->variable_name, "INITIAL_FAILURE_DELAY") == 0) {
                          initial_failure_delay = strtol(currentcustomvar->variable_value,NULL,10);
                    }
                    else if (strcmp(currentcustomvar->variable_name, "REPEAT_FAILURE_DELAY") == 0) {
                          repeat_failure_delay = strtol(currentcustomvar->variable_value,NULL,10);
                    }
                    else if (strcmp(currentcustomvar->variable_name, "INITIAL_RECOVERY_DELAY") == 0) {
                          initial_recovery_delay = strtol(currentcustomvar->variable_value,NULL,10);
                    }
                    currentcustomvar = currentcustomvar->next;
                }
                cur--;
                if (strcmp(cur, ",") == 0) {
                    cur[0] = '\x0';
                }
                else {
                    cur++;
                }

                my_free(currentcustomvar);

                written = generate_event(push_buffer, PERFDATA_BUFFER,
                    srvchkdata->host_name,
                    srvchkdata->service_description,
                    servicestate[srvchkdata->state],
                    srvchkdata->output,
                    srvchkdata->long_output,
                    temp_buffer,
                    initial_failure_delay,
                    repeat_failure_delay,
                    initial_recovery_delay,
                    (int)srvchkdata->timestamp.tv_sec);

                riemann_message_t *r;

                redistarget *currentredistarget = redistargets;
                while (currentredistarget != NULL) {
                    if (written >= PERFDATA_BUFFER) {
                        snprintf(temp_buffer, sizeof(temp_buffer) - 1,
                            "flapjackfeeder: Buffer size of %d in npcdmod.h is too small, ignoring data for %s / %s\n",
                            PERFDATA_BUFFER, srvchkdata->host_name, srvchkdata->service_description);
                        temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
                        write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
                    } else if (currentredistarget->redis_connection_established) {
                        //reply = redisCommand(currentredistarget->rediscontext,"LPUSH %s %s", currentredistarget->redis_queue, push_buffer);
                        r = riemann_communicate_event(currentredistarget->rediscontext,
                                RIEMANN_EVENT_FIELD_HOST, "localhost",
                                RIEMANN_EVENT_FIELD_SERVICE, "demo-client",
                                RIEMANN_EVENT_FIELD_STATE, "ok",
                                RIEMANN_EVENT_FIELD_TAGS, "demo-client", "riemann-c-client", NULL,
                                RIEMANN_EVENT_FIELD_NONE);
                        if (r->ok != 1) {
                            snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: redis write (%s:%d) fail, lost check result (%s : %s - %s).",
                                currentredistarget->host, currentredistarget->port,
                                srvchkdata->host_name, srvchkdata->service_description, servicestate[srvchkdata->state]);
                            temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
                            write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
                            currentredistarget->redis_connection_established = 0;
                            //redisFree(currentredistarget->rediscontext);
                        } else {
                            snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: redis write (%s:%d) SUCCESS, check result (%s : %s - %s).",
                                currentredistarget->host, currentredistarget->port,
                                srvchkdata->host_name, srvchkdata->service_description, servicestate[srvchkdata->state]);
                            temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
                            write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
                        }
                        //riemann_message_free (r);
                    } else {
                        snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: redis connection (%s:%d) fail, lost check result (%s : %s - %s).",
                            currentredistarget->host, currentredistarget->port,
                            srvchkdata->host_name, srvchkdata->service_description, servicestate[srvchkdata->state]);
                        temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
                        write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
                    }
                    currentredistarget = currentredistarget->next;
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
        // fill redistarget with defaults (if parameters are missing from module config)
        /* allocate memory for a new redis target */
        if ((redistargets = malloc(sizeof(redistarget))) == NULL) {
            write_to_all_logs("Error: Could not allocate memory for redis target\n", NSLOG_INFO_MESSAGE);
        }
        redistargets->host = "127.0.0.1";
        redistargets->port = 6379;
        redistargets->redis_database = "0";
        redistargets->redis_queue = "events";
        redistargets->redis_connection_established = 0;
    	redistargets->next = NULL;
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
                    my_free(arglist[argcount]);
                my_free(arglist);
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
                my_free(arglist[arg]);
            my_free(arglist);
            return ERROR;
        }
    }

    if (redistargets == NULL || redistargets->host == NULL //|| redistargets->port == NULL ||
        ) {
        write_to_all_logs("flapjackfeeder: Error: You have to configure at least one redis target tuple (i.e. redis_host=localhost,redis_port=6379,redis_database=0,redis_queue=events)", NSLOG_CONFIG_ERROR);
        return ERROR;
    }

    /* free allocated memory */
    for (arg = 0; arg < argcount; arg++)
        my_free(arglist[arg]);
    my_free(arglist);

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

	redistarget *new_redistarget = NULL;

    /* process the variable... */
    if (!strcmp(var, "redis_host")) {
        // fill redistarget structure
        if (redistargets == NULL || redistargets->host != NULL) {
            /* allocate memory for a new redis target */
            if ((new_redistarget = malloc(sizeof(redistarget))) == NULL) {
                write_to_all_logs("Error: Could not allocate memory for redis target\n", NSLOG_INFO_MESSAGE);
            }
            new_redistarget->host = NULL;
            new_redistarget->port = 0;
            new_redistarget->redis_database = NULL;
            new_redistarget->redis_queue = NULL;
            new_redistarget->rediscontext = NULL;
            new_redistarget->redis_connection_established = 0;
        }
        if (redistargets != NULL && redistargets->host == NULL) {
            redistargets->host = strdup(val);
        }
        else {
            new_redistarget->host = strdup(val);
        }
        if (new_redistarget != NULL) {
        	/* add the new redistarget to the head of the redistarget list */
        	new_redistarget->next = redistargets;
        	redistargets = new_redistarget;
        }
    }

    else if (!strcmp(var, "redis_port")) {
        // fill redistarget structure
        if (redistargets == NULL || redistargets->port != 0) {
            /* allocate memory for a new redis target */
            if ((new_redistarget = malloc(sizeof(redistarget))) == NULL) {
                //logit(NSLOG_CONFIG_ERROR, TRUE, "Error: Could not allocate memory for redis target\n");
                write_to_all_logs("Error: Could not allocate memory for redis target", NSLOG_INFO_MESSAGE);
            }
            new_redistarget->host = NULL;
            new_redistarget->port = 5555;
            new_redistarget->redis_database = NULL;
            new_redistarget->redis_queue = NULL;
            new_redistarget->rediscontext = NULL;
            new_redistarget->redis_connection_established = 0;
        }
        if (redistargets != NULL && redistargets->port == 0) {
            redistargets->port = atoi(val);
        }
        else {
            new_redistarget->port = atoi(val);
        }
        if (new_redistarget != NULL) {
        	/* add the new redistarget to the head of the redistarget list */
        	new_redistarget->next = redistargets;
        	redistargets = new_redistarget;
        }
    }

    else if (!strcmp(var, "redis_database")) {
        // fill redistarget structure
        if (redistargets == NULL || redistargets->redis_database != NULL) {
            /* allocate memory for a new redis target */
            if ((new_redistarget = malloc(sizeof(redistarget))) == NULL) {
                //logit(NSLOG_CONFIG_ERROR, TRUE, "Error: Could not allocate memory for redis target\n");
                write_to_all_logs("Error: Could not allocate memory for redis target", NSLOG_INFO_MESSAGE);
            }
            new_redistarget->host = NULL;
            new_redistarget->port = 0;
            new_redistarget->redis_database = NULL;
            new_redistarget->redis_queue = NULL;
            new_redistarget->rediscontext = NULL;
            new_redistarget->redis_connection_established = 0;
        }
        if (redistargets != NULL && redistargets->redis_database == NULL) {
            redistargets->redis_database = strdup(val);
        }
        else {
            new_redistarget->redis_database = strdup(val);
        }
        if (new_redistarget != NULL) {
            /* add the new redistarget to the head of the redistarget list */
            new_redistarget->next = redistargets;
            redistargets = new_redistarget;
        }
    }

    else if (!strcmp(var, "redis_queue")) {
        // fill redistarget structure
        if (redistargets == NULL || redistargets->redis_queue != NULL) {
            /* allocate memory for a new redis target */
            if ((new_redistarget = malloc(sizeof(redistarget))) == NULL) {
                write_to_all_logs("Error: Could not allocate memory for redis target\n", NSLOG_INFO_MESSAGE);
            }
            new_redistarget->host = NULL;
            new_redistarget->port = 0;
            new_redistarget->redis_database = NULL;
            new_redistarget->redis_queue = NULL;
            new_redistarget->rediscontext = NULL;
            new_redistarget->redis_connection_established = 0;
        }
        if (redistargets != NULL && redistargets->redis_queue == NULL) {
            redistargets->redis_queue = strdup(val);
        }
        else {
            new_redistarget->redis_queue = strdup(val);
        }
        if (new_redistarget != NULL) {
            /* add the new redistarget to the head of the redistarget list */
            new_redistarget->next = redistargets;
            redistargets = new_redistarget;
        }
    }

    else if (!strcmp(var, "redis_connect_retry_interval")) {
        redis_connect_retry_interval = strdup(val);
        snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: configure %ss as retry interval for redis reconnects.", redis_connect_retry_interval);
        temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
        write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
    }

    else if (!strcmp(var, "timeout")) {
        timeout.tv_sec = atoi(val);
        timeout.tv_usec = 0;
        snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: configure %ss as timeout for redis connects/writes.", val);
        temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
        write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
    }

    else if (!strcmp(var, "flapjack_version")) {
        flapjack_version = atoi(val);
        snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: configure %s as flapjack_version.", val);
        temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
        write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
    }

    else {
        snprintf(temp_buffer, sizeof(temp_buffer) - 1, "flapjackfeeder: I don't know what to do with '%s' as argument.", var);
        temp_buffer[sizeof(temp_buffer) - 1] = '\x0';
        write_to_all_logs(temp_buffer, NSLOG_INFO_MESSAGE);
        return ERROR;
    }

    return OK;
}

/* Counts escape sequences within a string

   Used for calculating the size of the destination string for
   expand_escapes, below.
*/
int count_escapes(const char *src) {
    int e = 0;

    char c = *(src++);

    while (c) {
        switch(c) {
            case '\\':
                e++;
                break;
            case '\"':
                e++;
                break;
        }
        c = *(src++);
    }

    return(e);
}

/* Expands escape sequences within a string
 *
 * src must be a string with a NUL terminator
 *
 * NUL characters are not expanded to \0 (otherwise how would we know when
 * the input string ends?)
 *
 * Adapted from http://stackoverflow.com/questions/3535023/convert-characters-in-a-c-string-to-their-escape-sequences
 */
char *expand_escapes(const char* src)
{
    char* dest;
    char* d;

    if ((src == NULL) || ( strlen(src) == 0)) {
        dest = malloc(sizeof(char));
        d = dest;
    } else {
        // escaped lengths must take NUL terminator into account
        int dest_len = strlen(src) + count_escapes(src) + 1;
        dest = malloc(dest_len * sizeof(char));
        d = dest;

        char c = *(src++);

        while (c) {
            switch(c) {
                case '\\':
                    *(d++) = '\\';
                    *(d++) = '\\';
                    break;
                case '\"':
                    *(d++) = '\\';
                    *(d++) = '\"';
                    break;
                default:
                    *(d++) = c;
            }
            c = *(src++);
        }
    }

    *d = '\0'; /* Ensure NUL terminator */

    return(dest);
}

int generate_event(char *buffer, size_t buffer_size, char *host_name, char *service_name,
                   char *state, char *output, char *long_output, char *tags,
                   long initial_failure_delay, long repeat_failure_delay,
                   long initial_recovery_delay, int event_time) {

    char *escaped_host_name    = expand_escapes(host_name);
    char *escaped_service_name = expand_escapes(service_name);
    char *escaped_state        = expand_escapes(state);
    char *escaped_output       = expand_escapes(output);
    char *escaped_long_output  = expand_escapes(long_output);

    int written = snprintf(buffer, buffer_size,
                            "{"
                                "\"entity\":\"%s\","                   // HOSTNAME
                                "\"check\":\"%s\","                    // SERVICENAME
                                "\"type\":\"service\","                // type
                                "\"state\":\"%s\","                    // HOSTSTATE
                                "\"summary\":\"%s\","                  // HOSTOUTPUT
                                "\"details\":\"%s\","                  // HOSTlongoutput
                                "\"tags\":[%s],"                       // tags
                                "\"initial_failure_delay\":%lu,"       // initial_failure_delay
                                "\"repeat_failure_delay\":%lu,"        // repeat_failure_delay
                                "\"initial_recovery_delay\":%lu,"      // initial_recovery_delay
                                "\"time\":%d"                          // TIMET
                            "}",
                                escaped_host_name,
                                escaped_service_name,
                                escaped_state,
                                escaped_output,
                                escaped_long_output,
                                tags,
                                initial_failure_delay,
                                repeat_failure_delay,
                                initial_recovery_delay,
                                event_time);

    my_free(escaped_host_name);
    my_free(escaped_service_name);
    my_free(escaped_state);
    my_free(escaped_output);
    my_free(escaped_long_output);

    return(written);
}
