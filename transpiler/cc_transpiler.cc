// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "transpiler/cc_transpiler.h"

#include <string>
#include <type_traits>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "absl/types/span.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/status_macros.h"
#include "xls/contrib/xlscc/metadata_output.pb.h"
#include "xls/ir/function.h"
#include "xls/ir/node.h"
#include "xls/ir/node_iterator.h"
#include "xls/ir/nodes.h"
#include "xls/public/value.h"

namespace fully_homomorphic_encryption {
namespace transpiler {

namespace {

using xls::Bits;
using xls::Function;
using xls::Literal;
using xls::Node;
using xls::Op;
using xls::Param;

}  // namespace

std::string CcTranspiler::NodeReference(const Node* node) {
  return absl::StrFormat("temp_nodes[%d]", node->id());
}

std::string CcTranspiler::ParamBitReference(const Node* param, int offset) {
  std::string param_name = param->GetName();
  int64_t param_bits = param->GetType()->GetFlatBitCount();
  if (param_bits == 1 &&
      (param->Is<xls::TupleIndex>() || param->Is<xls::Array>())) {
    param_name = param->operand(0)->GetName();
  }
  return absl::StrFormat("%s[%d]", param_name, offset);
}

std::string CcTranspiler::OutputBitReference(absl::string_view output_arg,
                                             int offset) {
  return absl::StrFormat("%s[%d]", output_arg, offset);
}

std::string CcTranspiler::CopyTo(std::string destination, std::string source) {
  return absl::Substitute("  $0 = $1;\n", destination, source);
}

std::string CcTranspiler::InitializeNode(const Node* node) { return ""; }

// Input: Node(id = 5, op = kNot, operands = Node(id = 2))
// Output: "  temp_nodes[5] = !temp_nodes[2];\n"
absl::StatusOr<std::string> CcTranspiler::Execute(const Node* node) {
  std::string op_result;
  if (node->Is<Literal>()) {
    const Literal* literal = node->As<Literal>();
    absl::StatusOr<Bits> bit = literal->value().GetBitsWithStatus();
    XLS_CHECK(bit.ok()) << bit.status();
    if (bit->IsOne()) {
      op_result = "true";
    } else if (bit->IsZero()) {
      op_result = "false";
    } else {
      // We allow literals strictly for pulling values out of [param] arrays.
      for (const Node* user : literal->users()) {
        if (!user->Is<xls::ArrayIndex>()) {
          return absl::InvalidArgumentError("Unsupported literal value.");
        }
      }
      return "";
    }
  } else {
    if (node->op() == Op::kNot) {
      XLS_CHECK_EQ(node->operands().size(), 1);
      op_result = absl::Substitute("!$0", NodeReference(node->operands()[0]));
    } else if (node->op() == Op::kAnd) {
      XLS_CHECK_EQ(node->operands().size(), 2);
      op_result =
          absl::Substitute("$0 && $1", NodeReference(node->operands()[0]),
                           NodeReference(node->operands()[1]));
    } else if (node->op() == Op::kOr) {
      XLS_CHECK_EQ(node->operands().size(), 2);
      op_result =
          absl::Substitute("$0 || $1", NodeReference(node->operands()[0]),
                           NodeReference(node->operands()[1]));
    } else {
      return absl::InvalidArgumentError("Unsupported Op kind.");
    }
  }
  return absl::StrCat(CopyTo(NodeReference(node), op_result), "\n");
}

absl::StatusOr<std::string> CcTranspiler::TranslateHeader(
    const xls::Function* function,
    const xlscc_metadata::MetadataOutput& metadata,
    absl::string_view header_path) {
  XLS_ASSIGN_OR_RETURN(const std::string header_guard,
                       PathToHeaderGuard(header_path));
  static constexpr absl::string_view kHeaderTemplate =
      R"(#ifndef $1
#define $1

#include "absl/status/status.h"
#include "absl/types/span.h"

$0;
#endif  // $1
)";
  XLS_ASSIGN_OR_RETURN(std::string signature,
                       FunctionSignature(function, metadata));
  return absl::Substitute(kHeaderTemplate, signature, header_guard);
}

absl::StatusOr<std::string> CcTranspiler::FunctionSignature(
    const Function* function, const xlscc_metadata::MetadataOutput& metadata) {
  std::vector<std::string> param_signatures;
  if (!metadata.top_func_proto().return_type().has_as_void()) {
    param_signatures.push_back("absl::Span<bool> result");
  }
  for (Param* param : function->params()) {
    param_signatures.push_back(
        absl::StrCat("absl::Span<bool> ", param->name()));
  }

  return absl::Substitute("absl::Status $0($1)", function->name(),
                          absl::StrJoin(param_signatures, ", "));
}

absl::StatusOr<std::string> CcTranspiler::Prelude(
    const Function* function, const xlscc_metadata::MetadataOutput& metadata) {
  // $0: function name
  static constexpr absl::string_view kPrelude =
      R"(#include <unordered_map>

#include "absl/status/status.h"
#include "absl/types/span.h"

$0 {
  std::unordered_map<int, bool> temp_nodes;

)";
  XLS_ASSIGN_OR_RETURN(std::string signature,
                       FunctionSignature(function, metadata));
  return absl::Substitute(kPrelude, signature);
}

absl::StatusOr<std::string> CcTranspiler::Conclusion() {
  return R"(
  return absl::OkStatus();
}
)";
}

}  // namespace transpiler
}  // namespace fully_homomorphic_encryption
