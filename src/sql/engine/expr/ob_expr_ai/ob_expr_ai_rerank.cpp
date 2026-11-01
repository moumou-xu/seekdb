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
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_type(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_type(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query is null");
    res.set_null();
  } else if (arg_documents->is_null()) {
    // a null document list propagates to a null result
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObArray<ObString> header_array;
    ObString model_id;
    ObString query;
    ObString doc_key;
    bool has_doc_key = (OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null());
    ObJsonArray *document_array = nullptr;
    ObArray<ObJsonNode *> document_nodes;
    ObArray<ObRerankResultItem> result_items;
    int64_t sequence = 0;
    ObIJsonBase *j_base = nullptr;
    bool is_null_result = false;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                   ctx.exec_ctx_, temp_allocator, *arg_model_id, expr.args_[MODEL_IDX]->datum_meta_,
                   expr.args_[MODEL_IDX]->obj_meta_.has_lob_header(), model_id))
        || OB_FAIL(ObTextStringHelper::read_real_string_data(
                   ctx.exec_ctx_, temp_allocator, *arg_query, expr.args_[QUERY_IDX]->datum_meta_,
                   expr.args_[QUERY_IDX]->obj_meta_.has_lob_header(), query))) {
    } else if (has_doc_key && OB_FAIL(ObTextStringHelper::read_real_string_data(
                   ctx.exec_ctx_, temp_allocator, *arg_doc_key, expr.args_[DOC_KEY_IDX]->datum_meta_,
                   expr.args_[DOC_KEY_IDX]->obj_meta_.has_lob_header(), doc_key))) {
    } else if (has_doc_key && doc_key.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key is empty");
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
                                                      j_base, is_null_result))) {
    } else if (is_null_result || OB_ISNULL(j_base)) {
      res.set_null();
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else if (OB_FAIL(get_document_texts(temp_allocator, document_array, has_doc_key,
                                          doc_key, document_nodes))) {
    } else if (model_id.empty() || query.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query is empty");
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, resolved_endpoint, header_array))) {
    } else {
      const int64_t count = document_nodes.count();
      for (int64_t start_idx = 0; OB_SUCC(ret) && start_idx < count; start_idx += MAX_BATCH_SIZE) {
        const int64_t end_idx = (start_idx + MAX_BATCH_SIZE) < count
                                ? (start_idx + MAX_BATCH_SIZE) : count;
        if (OB_FAIL(eval_ai_rerank_batch(temp_allocator, *info, resolved_endpoint, header_array,
                                         query, document_nodes, start_idx, end_idx,
                                         result_items, sequence))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (result_items.count() != document_nodes.count()) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank, the model did not return every document", K(ret));
      } else {
        sort_result_items(result_items);
        ObJsonArray *result_array = nullptr;
        ObString raw_str;
        if (OB_FAIL(build_result_array(temp_allocator, document_array, has_doc_key,
                                       result_items, result_array))) {
        } else if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::get_document_texts(ObIAllocator &allocator,
                                       ObJsonArray *document_array,
                                       const bool has_doc_key,
                                       ObString &doc_key,
                                       ObArray<ObJsonNode *> &document_nodes)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const int64_t count = document_array->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *doc_node = document_array->get_value(i);
      ObJsonNode *text_node = nullptr;
      if (OB_ISNULL(doc_node)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
      } else if (!has_doc_key) {
        if (doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        } else {
          text_node = doc_node;
        }
      } else {
        if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          ObJsonObject *doc_obj = static_cast<ObJsonObject *>(doc_node);
          text_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(text_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (text_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(document_nodes.push_back(text_node))) {
      }
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank_batch(ObIAllocator &allocator,
                                         const ObAIFuncExprInfo &info,
                                         const share::ObAiModelEndpointInfo &endpoint_info,
                                         ObArray<ObString> &header_array,
                                         ObString &query,
                                         ObArray<ObJsonNode *> &document_nodes,
                                         const int64_t start_idx,
                                         const int64_t end_idx,
                                         ObArray<ObRerankResultItem> &result_items,
                                         int64_t &sequence)
{
  INIT_SUCC(ret);
  ObJsonArray *batch_document_array = nullptr;
  ObJsonArray *batch_result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, batch_document_array))) {
  } else {
    for (int64_t i = start_idx; OB_SUCC(ret) && i < end_idx; i++) {
      if (OB_FAIL(batch_document_array->append(document_nodes.at(i)))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(inner_eval_ai_rerank(allocator, info, endpoint_info, header_array,
                                          query, batch_document_array, batch_result_array))) {
  } else if (OB_ISNULL(batch_result_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (static_cast<int64_t>(batch_result_array->element_count()) != (end_idx - start_idx)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("ai_rerank, the model returned an unexpected result count", K(ret));
  } else {
    const int64_t batch_count = end_idx - start_idx;
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_count; i++) {
      ObJsonNode *result_node = batch_result_array->get_value(i);
      ObJsonObject *result_obj = nullptr;
      ObRerankResultItem item;
      int64_t local_index = i;
      float score = 0.0;
      ObJsonNode *score_node = nullptr;
      if (OB_ISNULL(result_node) || result_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank, the model result is not an object", K(ret));
      } else if (OB_FALSE_IT(result_obj = static_cast<ObJsonObject *>(result_node))) {
      } else {
        ObJsonNode *index_node = result_obj->get_value(INDEX_KEY);
        if (OB_NOT_NULL(index_node)) {
          if (index_node->json_type() != ObJsonNodeType::J_INT) {
            ret = OB_INVALID_DATA;
            LOG_WARN("ai_rerank, the model returned an invalid index", K(ret));
          } else {
            local_index = static_cast<ObJsonInt *>(index_node)->value();
          }
        }
      }
      if (OB_SUCC(ret) && (local_index < 0 || local_index >= batch_count)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank, the model returned an out of range index", K(ret));
      }
      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(score_node = result_obj->get_value(SCORE_KEY))) {
        if (OB_ISNULL(score_node = result_obj->get_value(SCORE_KEY_ALIAS))) {
          ret = OB_INVALID_DATA;
          LOG_WARN("ai_rerank, the model returned no score", K(ret));
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(extract_score(allocator, score_node, score))) {
      } else {
        item.global_index_ = start_idx + local_index;
        item.sequence_ = sequence++;
        item.score_ = score;
        item.score_node_ = score_node;
        item.result_node_ = result_node;
        if (OB_FAIL(result_items.push_back(item))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::extract_score(ObIAllocator &allocator, ObJsonNode *score_node, float &score)
{
  INIT_SUCC(ret);
  share::ObJsonReaderHelper json_reader(allocator);
  if (OB_ISNULL(score_node) || !share::ObJsonHelper::is_number_type(score_node)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("ai_rerank, the model returned an invalid score", K(ret));
  } else if (OB_FAIL(json_reader.get_float_value(score_node, score))) {
  }
  return ret;
}

void ObExprAIRerank::sort_result_items(ObArray<ObRerankResultItem> &result_items)
{
  // Insertion sort keeps the compare result stable: items with the same score
  // stay in the order the model returned them.
  for (int64_t i = 1; i < result_items.count(); i++) {
    ObRerankResultItem item = result_items.at(i);
    int64_t j = i - 1;
    while (j >= 0 && result_items.at(j).score_ < item.score_) {
      result_items.at(j + 1) = result_items.at(j);
      --j;
    }
    result_items.at(j + 1) = item;
  }
}

int ObExprAIRerank::build_result_array(ObIAllocator &allocator,
                                       ObJsonArray *document_array,
                                       const bool has_doc_key,
                                       ObArray<ObRerankResultItem> &result_items,
                                       ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array_obj = nullptr;
  if (OB_ISNULL(document_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array_obj))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < result_items.count(); i++) {
      ObRerankResultItem &item = result_items.at(i);
      if (has_doc_key) {
        ObJsonNode *document_node = document_array->get_value(item.global_index_);
        ObJsonObject *sorted_obj = nullptr;
        if (OB_ISNULL(document_node) || document_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ISNULL(sorted_obj = static_cast<ObJsonObject *>(
                       document_node->clone(&allocator, true)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(sorted_obj->add(MODEL_SCORE_KEY, item.score_node_))) {
        } else if (OB_FAIL(result_array_obj->append(sorted_obj))) {
        }
      } else {
        // Keep the original document index so the caller can map the result
        // back to its input row, even when the model answers out of order.
        ObJsonObject *result_obj = static_cast<ObJsonObject *>(item.result_node_);
        ObJsonNode *index_node = result_obj->get_value(INDEX_KEY);
        if (OB_ISNULL(index_node)) {
          ObJsonInt *global_index_node = nullptr;
          if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, item.global_index_,
                                                      global_index_node))) {
          } else if (OB_FAIL(result_obj->add(INDEX_KEY, global_index_node))) {
          }
        } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
          static_cast<ObJsonInt *>(index_node)->set_value(item.global_index_);
        }
        if (OB_SUCC(ret) && OB_FAIL(result_array_obj->append(item.result_node_))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    result_array = result_array_obj;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator, 
                                          const ObAIFuncExprInfo &info, 
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array, 
                                          ObString &query, 
                                          ObJsonArray *document_array, 
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (OB_ISNULL(response) || response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
    LOG_WARN("ai_rerank, the model result is not an array", K(ret));
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
