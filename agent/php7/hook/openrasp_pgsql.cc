/*
 * Copyright 2017-2019 Baidu Inc.
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

#include "openrasp_sql.h"
#include "openrasp_hook.h"

HOOK_FUNCTION(pg_connect, DB_CONNECTION);
HOOK_FUNCTION(pg_pconnect, DB_CONNECTION);
PRE_HOOK_FUNCTION(pg_query, SQL);
PRE_HOOK_FUNCTION(pg_send_query, SQL);
PRE_HOOK_FUNCTION(pg_prepare, SQL_PREPARED);

void parse_connection_string(char *connstring, sql_connection_entry *sql_connection_p)
{
    char *buf = NULL;
    char *cp = NULL;
    char *cp2 = NULL;
    char *pname = NULL;
    char *pval = NULL;
    if (connstring)
    {
        buf = estrdup(connstring);
        cp = buf;
        while (*cp)
        {
            if (isspace((unsigned char)*cp))
            {
                cp++;
                continue;
            }

            pname = cp;
            while (*cp)
            {
                if (*cp == '=')
                {
                    break;
                }
                if (isspace((unsigned char)*cp))
                {
                    *cp++ = '\0';
                    while (*cp)
                    {
                        if (!isspace((unsigned char)*cp))
                        {
                            break;
                        }
                        cp++;
                    }
                    break;
                }
                cp++;
            }

            if (*cp != '=')
            {
                efree(buf);
                return;
            }
            *cp++ = '\0';

            while (*cp)
            {
                if (!isspace((unsigned char)*cp))
                {
                    break;
                }
                cp++;
            }

            pval = cp;
            if (*cp != '\'')
            {
                cp2 = pval;
                while (*cp)
                {
                    if (isspace((unsigned char)*cp))
                    {
                        *cp++ = '\0';
                        break;
                    }
                    if (*cp == '\\')
                    {
                        cp++;
                        if (*cp != '\0')
                        {
                            *cp2++ = *cp++;
                        }
                    }
                    else
                    {
                        *cp2++ = *cp++;
                    }
                }
                *cp2 = '\0';
            }
            else
            {
                cp2 = pval;
                cp++;
                for (;;)
                {
                    if (*cp == '\0')
                    {
                        efree(buf);
                        return;
                    }
                    if (*cp == '\\')
                    {
                        cp++;
                        if (*cp != '\0')
                        {
                            *cp2++ = *cp++;
                        }
                        continue;
                    }
                    if (*cp == '\'')
                    {
                        *cp2 = '\0';
                        cp++;
                        break;
                    }
                    *cp2++ = *cp++;
                }
            }

            if (strcmp(pname, "user") == 0)
            {
                sql_connection_p->set_username(pval);
            }
            else if (strcmp(pname, "host") == 0)
            {
                sql_connection_p->set_host(pval);
                struct stat sb;
                if (VCWD_STAT(pval, &sb) == 0)
                {
                    sql_connection_p->set_using_socket((sb.st_mode & S_IFDIR) != 0 || (sb.st_mode & S_IFSOCK) != 0);
                }
                else
                {
                    sql_connection_p->set_using_socket(false);
                }
            }
            else if (strcmp(pname, "port") == 0)
            {
                sql_connection_p->set_port(atoi(pval));
            }
        }
        efree(buf);
    }
}

static void init_pg_connection_entry(INTERNAL_FUNCTION_PARAMETERS, sql_connection_entry *sql_connection_p)
{
    char *host = NULL, *port = NULL, *options = NULL, *tty = NULL, *dbname = NULL, *connstring = NULL;
    zval *args;
    int connect_type = 0;
    args = (zval *)safe_emalloc(ZEND_NUM_ARGS(), sizeof(zval), 0);
    if (ZEND_NUM_ARGS() < 1 || ZEND_NUM_ARGS() > 5 ||
        zend_get_parameters_array_ex(ZEND_NUM_ARGS(), args) == FAILURE)
    {
        efree(args);
        return;
    }

    sql_connection_p->set_server("pgsql");
    if (ZEND_NUM_ARGS() == 1)
    { /* new style, using connection string */
        connstring = Z_STRVAL(args[0]);
    }
    else if (ZEND_NUM_ARGS() == 2)
    { /* Safe to add conntype_option, since 2 args was illegal */
        connstring = Z_STRVAL(args[0]);
        convert_to_long_ex(&args[1]);
        connect_type = (int)Z_LVAL(args[1]);
    }
    if (connstring)
    {
        sql_connection_p->set_connection_string(connstring);
        parse_connection_string(connstring, sql_connection_p);
    }
    efree(args);
}

/**
 * pg_connect
 */
void pre_global_pg_connect_DB_CONNECTION(OPENRASP_INTERNAL_FUNCTION_PARAMETERS)
{
    if (OPENRASP_CONFIG(security.enforce_policy) &&
        check_database_connection_username(INTERNAL_FUNCTION_PARAM_PASSTHRU, init_pg_connection_entry, 1))
    {
        handle_block();
    }
}
void post_global_pg_connect_DB_CONNECTION(OPENRASP_INTERNAL_FUNCTION_PARAMETERS)
{
    if (!OPENRASP_CONFIG(security.enforce_policy) && Z_TYPE_P(return_value) == IS_RESOURCE)
    {
        check_database_connection_username(INTERNAL_FUNCTION_PARAM_PASSTHRU, init_pg_connection_entry, 0);
    }
}

/**
 * pg_pconnect 
 */
void pre_global_pg_pconnect_DB_CONNECTION(OPENRASP_INTERNAL_FUNCTION_PARAMETERS)
{
    pre_global_pg_connect_DB_CONNECTION(OPENRASP_INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
void post_global_pg_pconnect_DB_CONNECTION(OPENRASP_INTERNAL_FUNCTION_PARAMETERS)
{
    post_global_pg_connect_DB_CONNECTION(OPENRASP_INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/**
 * pg_query
 */
void pre_global_pg_query_SQL(OPENRASP_INTERNAL_FUNCTION_PARAMETERS)
{
    zval *pgsql_link = NULL;
    char *query;
    size_t query_len, argc = ZEND_NUM_ARGS();

    if (argc == 1)
    {
        if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &query, &query_len) == FAILURE)
        {
            return;
        }
    }
    else
    {
        if (zend_parse_parameters(ZEND_NUM_ARGS(), "rs", &pgsql_link, &query, &query_len) == FAILURE)
        {
            return;
        }
    }

    plugin_sql_check(query, query_len, "pgsql");
}

/**
 * pg_send_query
 */
void pre_global_pg_send_query_SQL(OPENRASP_INTERNAL_FUNCTION_PARAMETERS)
{
    pre_global_pg_query_SQL(OPENRASP_INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

void pre_global_pg_prepare_SQL_PREPARED(OPENRASP_INTERNAL_FUNCTION_PARAMETERS)
{
    zval *pgsql_link = NULL;
    char *query, *stmtname;
    size_t query_len, stmtname_len, argc = ZEND_NUM_ARGS();

    if (argc == 2)
    {
        if (zend_parse_parameters(argc, "ss", &stmtname, &stmtname_len, &query, &query_len) == FAILURE)
        {
            return;
        }
    }
    else
    {
        if (zend_parse_parameters(argc, "rss", &pgsql_link, &stmtname, &stmtname_len, &query, &query_len) == FAILURE)
        {
            return;
        }
    }
    plugin_sql_check(query, query_len, "pgsql");
}