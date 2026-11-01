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

#ifndef OCEANBASE_SQL_OB_EXPR_AI_RERANK_H_
#define OCEANBASE_SQL_OB_EXPR_AI_RERANK_H_
 
#include "sql/engine/expr/ob_expr_operator.h"
#include "sql/engine/ob_exec_context.h"
#include "ob_ai_func_client.h"
#include "ob_ai_func.h"
#include "ob_ai_func_utils.h"
 
namespace oceanbase 
{
namespace sql 
{

// One rerank result, restored to the position of the original document.
struct ObRerankResultItem
{
  int64_t global_index_;   // index of the document in the input array
  int64_t sequence_;       // appearance order, used to keep the sort stable
  float score_;            // score returned by the model
  common::ObJsonNode *score_node_;   // score json value returned by the model
  common::ObJsonNode *result_node_;  // whole result object returned by the model
};

class ObExprAIRerank : public ObFuncExprOperator 
{
public:
  explicit ObExprAIRerank(common::ObIAllocator &alloc);
  virtual ~ObExprAIRerank();
  virtual int calc_result_typeN(ObExprResType &type, ObExprResType *types_array,
                                int64_t param_num,
                                common::ObExprTypeCtx &type_ctx) const override;
  static int eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res);

  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
  virtual bool need_rt_ctx() const override { return true; }
 
private:
  static int inner_eval_ai_rerank(common::ObIAllocator &allocator, 
                                  const common::ObAIFuncExprInfo &info, 
                                  const share::ObAiModelEndpointInfo &endpoint_info,
                                  common::ObArray<common::ObString> &header_array, 
                                  common::ObString &query, 
                                  common::ObJsonArray *document_array, 
                                  common::ObJsonArray *&result_array);
  // Send one request that carries [start_idx, end_idx) of the documents and
  // append every returned item with its global index.
  static int eval_ai_rerank_batch(common::ObIAllocator &allocator,
                                  const common::ObAIFuncExprInfo &info,
                                  const share::ObAiModelEndpointInfo &endpoint_info,
                                  common::ObArray<common::ObString> &header_array,
                                  common::ObString &query,
                                  common::ObArray<common::ObJsonNode *> &document_nodes,
                                  const int64_t start_idx,
                                  const int64_t end_idx,
                                  common::ObArray<ObRerankResultItem> &result_items,
                                  int64_t &sequence);
  static int get_document_texts(common::ObIAllocator &allocator,
                                common::ObJsonArray *document_array,
                                const bool has_doc_key,
                                common::ObString &doc_key,
                                common::ObArray<common::ObJsonNode *> &document_nodes);
  static int extract_score(common::ObIAllocator &allocator,
                           common::ObJsonNode *score_node,
                           float &score);
  static int build_result_array(common::ObIAllocator &allocator,
                                common::ObJsonArray *document_array,
                                const bool has_doc_key,
                                common::ObArray<ObRerankResultItem> &result_items,
                                common::ObJsonArray *&result_array);
  static void sort_result_items(common::ObArray<ObRerankResultItem> &result_items);
  static constexpr int MODEL_IDX = 0;
  static constexpr int QUERY_IDX = 1;
  static constexpr int DOCUMENTS_IDX = 2;
  static constexpr int DOC_KEY_IDX = 3;
  static constexpr int64_t MAX_BATCH_SIZE = 20;
  static constexpr char MODEL_SCORE_KEY[20] = "_model_score";
  static constexpr char INDEX_KEY[20] = "index";
  static constexpr char SCORE_KEY[20] = "relevance_score";
  static constexpr char SCORE_KEY_ALIAS[10] = "score";
  DISALLOW_COPY_AND_ASSIGN(ObExprAIRerank);
};

} // namespace sql
} // namespace oceanbase
#endif // OCEANBASE_SQL_OB_EXPR_AI_RERANK_H_
