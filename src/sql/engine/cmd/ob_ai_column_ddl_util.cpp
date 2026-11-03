/*
 * Copyright (c) 2025 OceanBase.
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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/cmd/ob_ai_column_ddl_util.h"

#include <stdio.h>

#include "common/mysqlclient/ob_mysql_result.h"
#include "common/rowkey/ob_rowkey_info.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/allocator/page_arena.h"
#include "lib/atomic/ob_atomic.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/ob_server_struct.h"

namespace oceanbase
{
namespace sql
{

using namespace common;
using namespace share;
using namespace share::schema;

namespace
{

int fetch_column_ids(common::ObISQLClient &client,
                     const uint64_t table_id,
                     common::ObIArray<common::ObString> &names,
                     common::ObIArray<uint64_t> &ids)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt(
        "SELECT column_id, column_name FROM oceanbase.__all_column WHERE table_id = %lu",
        table_id))) {
  } else {
    ObISQLClient::ReadResult res;
    if (OB_FAIL(client.read(res, sql.ptr()))) {
      LOG_WARN("failed to read column ids", K(ret), K(table_id));
    } else if (OB_ISNULL(res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      common::sqlclient::ObMySQLResult *result = res.get_result();
      while (OB_SUCC(ret)) {
        int64_t column_id = 0;
        ObString column_name;
        if (OB_FAIL(result->next())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
          }
        } else if (OB_FAIL(result->get_int("column_id", column_id))) {
        } else if (OB_FAIL(result->get_varchar("column_name", column_name))) {
        } else if (OB_FAIL(names.push_back(column_name))) {
        } else if (OB_FAIL(ids.push_back(static_cast<uint64_t>(column_id)))) {
        }
      }
    }
  }
  return ret;
}

uint64_t find_column_id(const common::ObIArray<common::ObString> &names,
                        const common::ObIArray<uint64_t> &ids,
                        const common::ObString &name)
{
  uint64_t column_id = 0;
  for (int64_t i = 0; i < names.count(); ++i) {
    if (names.at(i) == name) {
      column_id = ids.at(i);
      break;
    }
  }
  return column_id;
}

// Collects column names/ids straight from the in-memory table schema, so the
// DDL executor never has to issue inner SQL (which deadlocks on the request
// worker while the DDL statement is still executing).
int collect_column_ids_from_schema(const share::schema::ObTableSchema &table_schema,
                                   common::ObIArray<common::ObString> &names,
                                   common::ObIArray<uint64_t> &ids)
{
  int ret = OB_SUCCESS;
  const int64_t column_count = table_schema.get_column_count();
  for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
    const share::schema::ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column schema is null", K(ret), K(i));
    } else if (OB_FAIL(names.push_back(column->get_column_name_str()))
               || OB_FAIL(ids.push_back(column->get_column_id()))) {
    }
  }
  return ret;
}

int build_hidden_column_name(const common::ObString &column_name,
                             const char *suffix,
                             common::ObSqlString &out)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(out.append_fmt("__ai_%.*s%s",
                             column_name.length(), column_name.ptr(), suffix))) {
  }
  return ret;
}

// The worker matches a published result back to its row through this id, so it
// only has to be unique per retry, matching ObAiColumnDml's generator.
void make_retry_request_id(char *buf, const int64_t len)
{
  static uint64_t seq = 0;
  const uint64_t id = ATOMIC_AAF(&seq, 1);
  snprintf(buf, static_cast<size_t>(len), "%lx_%lx",
           static_cast<uint64_t>(ObTimeUtility::current_time()), id);
}

int append_quoted_literal(const common::ObString &value, common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int32_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if ('\'' == c || '\\' == c) {
      if (OB_FAIL(sql.append("\\", 1))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append(&c, 1))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
  }
  return ret;
}

// Renders one rowkey as the "`col` = literal" text the worker replays, the same
// shape ObAiColumnDml builds from a live row.
int build_retry_rowkey_predicate(const share::schema::ObTableSchema &table_schema,
                                 const common::ObIArray<common::ObString> &rowkey_values,
                                 common::ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  const common::ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
  if (static_cast<int64_t>(rowkey_info.get_size()) != rowkey_values.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("rowkey value count mismatch", K(ret), K(rowkey_info.get_size()),
             K(rowkey_values.count()));
  }
  for (int32_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
    uint64_t column_id = OB_INVALID_ID;
    const share::schema::ObColumnSchemaV2 *column = NULL;
    if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
      LOG_WARN("failed to get rowkey column id", K(ret), K(i));
    } else if (OB_ISNULL(column = table_schema.get_column_schema(column_id))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("rowkey column schema is missing", K(ret), K(column_id));
    } else if (0 != i && OB_FAIL(predicate.append(" and "))) {
    } else if (OB_FAIL(predicate.append_fmt("`%.*s` = ",
                                            column->get_column_name_str().length(),
                                            column->get_column_name_str().ptr()))) {
    } else if (column->get_meta_type().is_numeric_type()) {
      // CAST(.. AS CHAR) yields exactly what print_sql_literal would
      if (OB_FAIL(predicate.append_fmt("%.*s", rowkey_values.at(i).length(),
                                       rowkey_values.at(i).ptr()))) {
      }
    } else if (OB_FAIL(append_quoted_literal(rowkey_values.at(i), predicate))) {
    }
  }
  return ret;
}

} // anonymous namespace

int ObAiColumnDdlUtil::persist_ai_columns(const uint64_t table_id,
                                          const common::ObIArray<ObAiColumnDef> &defs,
                                          const share::schema::ObTableSchema *table_schema)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObSEArray<ObString, 32> column_names;
  ObSEArray<uint64_t, 32> column_ids;
  if (defs.empty()) {
    // nothing to do
  } else if (OB_INVALID_ID == table_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid table id for ai column definition", K(ret), K(table_id));
  } else if (NULL != table_schema
             && OB_FAIL(collect_column_ids_from_schema(*table_schema, column_names, column_ids))) {
  } else if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    LOG_WARN("failed to start transaction", K(ret));
  } else if (NULL == table_schema
             && OB_FAIL(fetch_column_ids(trans, table_id, column_names, column_ids))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
      const ObAiColumnDef &def = defs.at(i);
      const uint64_t result_column_id = find_column_id(column_names, column_ids, def.column_name_);
      ObSqlString state_name;
      ObSqlString request_name;
      ObSqlString spec_name;
      uint64_t state_column_id = 0;
      uint64_t request_column_id = 0;
      uint64_t spec_column_id = 0;
      if (0 == result_column_id) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column is not found in the catalog", K(ret), K(table_id), K(def.column_name_));
      } else if (OB_FAIL(build_hidden_column_name(def.column_name_, AI_COLUMN_STATE_SUFFIX, state_name))) {
      } else if (OB_FAIL(build_hidden_column_name(def.column_name_, AI_COLUMN_REQUEST_SUFFIX, request_name))) {
      } else if (OB_FAIL(build_hidden_column_name(def.column_name_, AI_COLUMN_SPEC_SUFFIX, spec_name))) {
      } else {
        state_column_id = find_column_id(column_names, column_ids, state_name.string());
        request_column_id = find_column_id(column_names, column_ids, request_name.string());
        spec_column_id = find_column_id(column_names, column_ids, spec_name.string());
      }
      if (OB_SUCC(ret)) {
        ObDMLSqlSplicer dml;
        ObSqlString buffer;
        int64_t affected_rows = 0;
        if (OB_FAIL(dml.add_pk_column("table_id", static_cast<int64_t>(table_id)))) {
        } else if (OB_FAIL(dml.add_pk_column("column_id", static_cast<int64_t>(result_column_id)))) {
        } else if (OB_FAIL(dml.add_column("column_name", ObHexEscapeSqlStr(def.column_name_)))) {
        } else if (OB_FAIL(dml.add_column("model_name", ObHexEscapeSqlStr(def.model_name_)))) {
        } else if (OB_FAIL(dml.add_column("ai_func_type", def.ai_func_type_))) {
        } else if (OB_FAIL(dml.add_column("result_type", def.result_type_))) {
        } else if (OB_FAIL(dml.add_column("normalized_expr", ObHexEscapeSqlStr(def.normalized_expr_)))) {
        } else if (OB_FAIL(dml.add_column("input_expr", ObHexEscapeSqlStr(def.input_expr_)))) {
        } else if (OB_FAIL(dml.add_column("max_retries", def.max_retries_))) {
        } else if (OB_FAIL(dml.add_column("sync_mode", def.sync_mode_))) {
        } else if (OB_FAIL(dml.add_column("definition_version", static_cast<int64_t>(def.definition_version_)))) {
        } else if (OB_FAIL(dml.add_column("state_column_id", static_cast<int64_t>(state_column_id)))) {
        } else if (OB_FAIL(dml.add_column("request_column_id", static_cast<int64_t>(request_column_id)))) {
        } else if (OB_FAIL(dml.add_column("result_spec_column_id", static_cast<int64_t>(spec_column_id)))) {
        } else if (OB_FAIL(dml.splice_insert_sql(OB_ALL_AI_COLUMN_TNAME, buffer))) {
        } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
        } else if (!is_single_row(affected_rows)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected affected rows", K(ret), K(affected_rows));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(trans.end(true))) {
      LOG_WARN("failed to commit ai column definition", K(ret), K(table_id));
    }
  } else {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(false))) {
      LOG_WARN("failed to rollback ai column definition", K(tmp_ret));
    }
  }
  return ret;
}

int ObAiColumnDdlUtil::drop_ai_columns(const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_INVALID_ID == table_id) {
    // nothing to do
  } else if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
  } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE table_id = %lu",
                                    OB_ALL_AI_COLUMN_TNAME, table_id))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
  } else if (FALSE_IT(sql.reuse())) {
  } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE table_id = %lu",
                                    OB_ALL_AI_COLUMN_TASK_TNAME, table_id))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(trans.end(true))) {
    }
  } else {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(false))) {
      LOG_WARN("failed to rollback ai column cleanup", K(tmp_ret));
    }
  }
  return ret;
}

int ObAiColumnDdlUtil::retry_ai_column(share::schema::ObSchemaGetterGuard &schema_guard,
                                       const share::schema::ObTableSchema &table_schema,
                                       const ObAiColumnDef &def,
                                       const common::ObString &predicate)
{
  int ret = OB_SUCCESS;
  const share::schema::ObDatabaseSchema *db_schema = NULL;
  ObSqlString state_name;
  ObSqlString request_name;
  ObSqlString spec_name;
  const common::ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
  const int32_t rowkey_size = rowkey_info.get_size();
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is not ready", K(ret));
  } else if (OB_INVALID_ID == def.result_column_id_ || OB_INVALID_ID == def.state_column_id_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column ids are not resolved", K(ret), K(def));
  } else if (rowkey_size <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table has no rowkey column", K(ret), K(table_schema.get_table_id()));
  } else if (OB_FAIL(schema_guard.get_database_schema(table_schema.get_database_id(), db_schema))) {
    LOG_WARN("failed to get database schema", K(ret), K(table_schema.get_database_id()));
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("database schema is null", K(ret), K(table_schema.get_database_id()));
  } else if (OB_FAIL(build_hidden_column_name(def.column_name_, AI_COLUMN_STATE_SUFFIX, state_name))
             || OB_FAIL(build_hidden_column_name(def.column_name_, AI_COLUMN_REQUEST_SUFFIX,
                                                 request_name))
             || OB_FAIL(build_hidden_column_name(def.column_name_, AI_COLUMN_SPEC_SUFFIX,
                                                 spec_name))) {
    LOG_WARN("failed to build hidden column names", K(ret), K(def.column_name_));
  } else {
    const common::ObString &db_name = db_schema->get_database_name_str();
    const common::ObString &table_name = table_schema.get_table_name_str();
    ObArenaAllocator alloc("AiRetryRowkey");
    common::ObSEArray<common::ObString, 32> rowkey_values;
    ObSqlString select_sql;

    // 1) collect the selected rowkeys first: a "status = 'READY'" predicate is
    // answered by the hidden state column, which the invalidate below overwrites.
    if (OB_FAIL(select_sql.append("SELECT "))) {
    }
    for (int32_t i = 0; OB_SUCC(ret) && i < rowkey_size; ++i) {
      uint64_t column_id = OB_INVALID_ID;
      const share::schema::ObColumnSchemaV2 *column = NULL;
      if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
        LOG_WARN("failed to get rowkey column id", K(ret), K(i));
      } else if (OB_ISNULL(column = table_schema.get_column_schema(column_id))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("rowkey column schema is missing", K(ret), K(column_id));
      } else if (OB_FAIL(select_sql.append_fmt("%sIFNULL(CAST(`%.*s` AS CHAR), '') AS k%d",
                                               0 == i ? "" : ", ",
                                               column->get_column_name_str().length(),
                                               column->get_column_name_str().ptr(), i))) {
      }
    }
    if (OB_SUCC(ret)
        && OB_FAIL(select_sql.append_fmt(" FROM `%.*s`.`%.*s`",
                                         db_name.length(), db_name.ptr(),
                                         table_name.length(), table_name.ptr()))) {
    }
    if (OB_SUCC(ret) && !predicate.empty()
        && OB_FAIL(select_sql.append_fmt(" WHERE %.*s", predicate.length(), predicate.ptr()))) {
    }

    ObMySQLTransaction trans;
    bool started = false;
    int64_t row_count = 0;
    if (OB_SUCC(ret)) {
      if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
        LOG_WARN("failed to start transaction", K(ret));
      } else if (FALSE_IT(started = true)) {
      } else {
        ObISQLClient::ReadResult read_result;
        if (OB_FAIL(trans.read(read_result, select_sql.ptr()))) {
          LOG_WARN("failed to select retry rows", K(ret), "sql", select_sql.ptr());
        } else if (OB_ISNULL(read_result.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("read result is null", K(ret), "sql", select_sql.ptr());
        } else {
          common::sqlclient::ObMySQLResult *result = read_result.get_result();
          while (OB_SUCC(ret)) {
            if (OB_FAIL(result->next())) {
              if (OB_ITER_END == ret) {
                ret = OB_SUCCESS;
                break;
              }
              LOG_WARN("failed to read retry rows", K(ret), "sql", select_sql.ptr());
            } else {
              for (int32_t i = 0; OB_SUCC(ret) && i < rowkey_size; ++i) {
                char alias[16];
                common::ObString value;
                common::ObString value_copy;
                snprintf(alias, sizeof(alias), "k%d", i);
                if (OB_FAIL(result->get_varchar(alias, value))) {
                  LOG_WARN("failed to read rowkey value", K(ret), K(i));
                } else if (OB_FAIL(common::ob_write_string(alloc, value, value_copy))) {
                  LOG_WARN("failed to copy rowkey value", K(ret));
                } else if (OB_FAIL(rowkey_values.push_back(value_copy))) {
                  LOG_WARN("failed to collect rowkey value", K(ret));
                }
              }
            }
          }
          if (OB_SUCC(ret)) {
            row_count = rowkey_values.count() / rowkey_size;
          }
        }
      }
    }

    // 2) queue one request id per row. The worker groups tasks by
    // (table_id, request_id) and replays group.at(0).rowkey_predicate_, so a
    // request id shared by several rows would collapse them into one.
    if (OB_SUCC(ret) && row_count > 0) {
      ObSqlString task_sql;
      int64_t affected_rows = 0;
      if (OB_FAIL(task_sql.append_fmt(
            "INSERT INTO %s (table_id, column_id, request_id, rowkey_predicate, "
            "eligible_ts, state, retry_count) VALUES ", OB_ALL_AI_COLUMN_TASK_TNAME))) {
        LOG_WARN("failed to build retry task sql", K(ret));
      }
      for (int64_t r = 0; OB_SUCC(ret) && r < row_count; ++r) {
        common::ObSEArray<common::ObString, 4> row_values;
        ObSqlString row_predicate;
        ObSqlString update_sql;
        char request_id[64];
        for (int32_t i = 0; OB_SUCC(ret) && i < rowkey_size; ++i) {
          if (OB_FAIL(row_values.push_back(rowkey_values.at(r * rowkey_size + i)))) {
            LOG_WARN("failed to collect rowkey value", K(ret));
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(build_retry_rowkey_predicate(table_schema, row_values, row_predicate))) {
          LOG_WARN("failed to build rowkey predicate", K(ret), K(r));
        } else {
          make_retry_request_id(request_id, sizeof(request_id));
          const int32_t request_id_len = static_cast<int32_t>(strlen(request_id));
          if (OB_FAIL(task_sql.append_fmt("%s(%lu, %lu, ", 0 == r ? "" : ", ",
                                          table_schema.get_table_id(),
                                          def.result_column_id_))) {
          } else if (OB_FAIL(append_quoted_literal(
                       common::ObString(request_id_len, request_id), task_sql))) {
          } else if (OB_FAIL(task_sql.append(", "))) {
          } else if (OB_FAIL(append_quoted_literal(row_predicate.string(), task_sql))) {
          } else if (OB_FAIL(task_sql.append(", now(6), 1, 0)"))) {
          } else if (OB_FAIL(update_sql.append_fmt(
                       "UPDATE `%.*s`.`%.*s` SET `%.*s` = NULL, `%.*s` = '%s', "
                       "`%.*s` = '%.*s', `%.*s` = '%lu' WHERE %.*s",
                       db_name.length(), db_name.ptr(),
                       table_name.length(), table_name.ptr(),
                       def.column_name_.length(), def.column_name_.ptr(),
                       state_name.length(), state_name.string().ptr(), "PENDING",
                       request_name.length(), request_name.string().ptr(),
                       request_id_len, request_id,
                       spec_name.length(), spec_name.string().ptr(),
                       def.definition_version_,
                       row_predicate.length(), row_predicate.string().ptr()))) {
          } else if (OB_FAIL(trans.write(update_sql.ptr(), affected_rows))) {
            LOG_WARN("failed to invalidate ai column result", K(ret), "sql", update_sql.ptr());
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(trans.write(task_sql.ptr(), affected_rows))) {
        LOG_WARN("failed to queue ai column retry tasks", K(ret), "sql", task_sql.ptr());
      }
    }

    if (started) {
      if (OB_SUCC(ret)) {
        if (OB_FAIL(trans.end(true))) {
          LOG_WARN("failed to commit ai column retry", K(ret));
        }
      } else {
        int tmp_ret = trans.end(false);
        if (OB_SUCCESS != tmp_ret) {
          LOG_WARN("failed to rollback ai column retry", K(tmp_ret));
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
