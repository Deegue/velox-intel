/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include "velox/substrait/SubstraitToVeloxPlan.h"

#include "velox/type/Type.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::substrait {

namespace {
template <TypeKind KIND>
VectorPtr setVectorFromVariantsByKind(
    const std::vector<velox::variant>& value,
    memory::MemoryPool* pool) {
  using T = typename TypeTraits<KIND>::NativeType;

  auto flatVector = std::dynamic_pointer_cast<FlatVector<T>>(
      BaseVector::create(CppToType<T>::create(), value.size(), pool));

  for (vector_size_t i = 0; i < value.size(); i++) {
    if (value[i].isNull()) {
      flatVector->setNull(i, true);
    } else {
      flatVector->set(i, value[i].value<T>());
    }
  }
  return flatVector;
}

template <>
VectorPtr setVectorFromVariantsByKind<TypeKind::VARBINARY>(
    const std::vector<velox::variant>& value,
    memory::MemoryPool* pool) {
  throw std::invalid_argument("Return of VARBINARY data is not supported");
}

template <>
VectorPtr setVectorFromVariantsByKind<TypeKind::VARCHAR>(
    const std::vector<velox::variant>& value,
    memory::MemoryPool* pool) {
  auto flatVector = std::dynamic_pointer_cast<FlatVector<StringView>>(
      BaseVector::create(VARCHAR(), value.size(), pool));

  for (vector_size_t i = 0; i < value.size(); i++) {
    if (value[i].isNull()) {
      flatVector->setNull(i, true);
    } else {
      flatVector->set(i, StringView(value[i].value<Varchar>()));
    }
  }
  return flatVector;
}

VectorPtr setVectorFromVariants(
    const TypePtr& type,
    const std::vector<velox::variant>& value,
    velox::memory::MemoryPool* pool) {
  return VELOX_DYNAMIC_SCALAR_TYPE_DISPATCH(
      setVectorFromVariantsByKind, type->kind(), value, pool);
}

template <typename T>
// Get the lowest value for numeric type.
T getLowest() {
  return std::numeric_limits<T>::lowest();
};

// Get the lowest value for string.
template <>
std::string getLowest<std::string>() {
  return "";
};

// Get the max value for numeric type.
template <typename T>
T getMax() {
  return std::numeric_limits<T>::max();
};

// The max value will be used in BytesRange. Return empty string here instead.
template <>
std::string getMax<std::string>() {
  return "";
};

// Substrait function names.
const std::string sIsNotNull = "is_not_null";
const std::string sGte = "gte";
const std::string sGt = "gt";
const std::string sLte = "lte";
const std::string sLt = "lt";
const std::string sEqual = "equal";
const std::string sIn = "in";
const std::string sOr = "or";
const std::string sNot = "not";

// Substrait types.
const std::string sI32 = "i32";
const std::string sI64 = "i64";
} // namespace

std::shared_ptr<const core::PlanNode> SubstraitVeloxPlanConverter::toVeloxPlan(
    const ::substrait::JoinRel& sJoin) {
  if (!sJoin.has_left()) {
    VELOX_FAIL("Left Rel is expected in JoinRel.");
  }
  if (!sJoin.has_right()) {
    VELOX_FAIL("Right Rel is expected in JoinRel.");
  }

  auto leftNode = toVeloxPlan(sJoin.left());
  auto rightNode = toVeloxPlan(sJoin.right());

  auto outputSize =
      leftNode->outputType()->size() + rightNode->outputType()->size();
  std::vector<std::string> outputNames;
  std::vector<std::shared_ptr<const Type>> outputTypes;
  outputNames.reserve(outputSize);
  outputTypes.reserve(outputSize);
  for (const auto& node : {leftNode, rightNode}) {
    const auto& names = node->outputType()->names();
    outputNames.insert(outputNames.end(), names.begin(), names.end());
    const auto& types = node->outputType()->children();
    outputTypes.insert(outputTypes.end(), types.begin(), types.end());
  }
  auto outputRowType = std::make_shared<const RowType>(
      std::move(outputNames), std::move(outputTypes));

  // extract join keys from join expression
  std::vector<const ::substrait::Expression::FieldReference*> leftExprs,
      rightExprs;
  extractJoinKeys(sJoin.expression(), leftExprs, rightExprs);
  VELOX_CHECK_EQ(leftExprs.size(), rightExprs.size());
  size_t numKeys = leftExprs.size();

  std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>> leftKeys,
      rightKeys;
  leftKeys.reserve(numKeys);
  rightKeys.reserve(numKeys);
  for (size_t i = 0; i < numKeys; ++i) {
    leftKeys.emplace_back(
        exprConverter_->toVeloxExpr(*leftExprs[i], outputRowType));
    rightKeys.emplace_back(
        exprConverter_->toVeloxExpr(*rightExprs[i], outputRowType));
  }

  std::shared_ptr<const core::ITypedExpr> filter;
  if (sJoin.has_post_join_filter()) {
    filter =
        exprConverter_->toVeloxExpr(sJoin.post_join_filter(), outputRowType);
  }

  // Map join type
  core::JoinType joinType;
  switch (sJoin.type()) {
    case ::substrait::JoinRel_JoinType::JoinRel_JoinType_JOIN_TYPE_INNER:
      joinType = core::JoinType::kInner;
      break;
    case ::substrait::JoinRel_JoinType::JoinRel_JoinType_JOIN_TYPE_OUTER:
      joinType = core::JoinType::kFull;
      break;
    case ::substrait::JoinRel_JoinType::JoinRel_JoinType_JOIN_TYPE_LEFT:
      joinType = core::JoinType::kLeft;
      break;
    case ::substrait::JoinRel_JoinType::JoinRel_JoinType_JOIN_TYPE_RIGHT:
      joinType = core::JoinType::kRight;
      break;
    case ::substrait::JoinRel_JoinType::JoinRel_JoinType_JOIN_TYPE_SEMI:
      joinType = core::JoinType::kLeftSemi;
      break;
    case ::substrait::JoinRel_JoinType::JoinRel_JoinType_JOIN_TYPE_ANTI:
      joinType = core::JoinType::kAnti;
      break;
    default:
      VELOX_NYI("Unsupported Join type: {}", sJoin.type());
  }

  // Create join node
  return std::make_shared<core::HashJoinNode>(
      nextPlanNodeId(),
      joinType,
      leftKeys,
      rightKeys,
      filter,
      leftNode,
      rightNode,
      outputRowType);
}

std::shared_ptr<const core::PlanNode> SubstraitVeloxPlanConverter::toVeloxPlan(
    const ::substrait::AggregateRel& sAgg) {
  std::shared_ptr<const core::PlanNode> childNode;
  if (sAgg.has_input()) {
    childNode = toVeloxPlan(sAgg.input());
  } else {
    VELOX_FAIL("Child Rel is expected in AggregateRel.");
  }

  core::AggregationNode::Step aggStep;
  setPhase(sAgg, aggStep);
  return toVeloxAgg(sAgg, childNode, aggStep);
}

std::shared_ptr<const core::PlanNode> SubstraitVeloxPlanConverter::toVeloxAgg(
    const ::substrait::AggregateRel& sAgg,
    const std::shared_ptr<const core::PlanNode>& childNode,
    const core::AggregationNode::Step& aggStep) {
  const auto& inputType = childNode->outputType();
  std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>
      veloxGroupingExprs;

  // Get the grouping expressions.
  uint32_t groupingOutIdx = 0;
  for (const auto& grouping : sAgg.groupings()) {
    for (const auto& groupingExpr : grouping.grouping_expressions()) {
      // Velox's groupings are limited to be Field.
      veloxGroupingExprs.emplace_back(
          exprConverter_->toVeloxExpr(groupingExpr.selection(), inputType));
      groupingOutIdx += 1;
    }
  }

  // Parse measures and get the aggregate expressions.
  // Each measure represents one aggregate expression.
  std::vector<std::shared_ptr<const core::CallTypedExpr>> aggExprs;
  aggExprs.reserve(sAgg.measures().size());
  for (const auto& smea : sAgg.measures()) {
    const auto& aggFunction = smea.measure();
    std::string funcName = subParser_->findVeloxFunction(
        functionMap_, aggFunction.function_reference());
    std::vector<std::shared_ptr<const core::ITypedExpr>> aggParams;
    aggParams.reserve(aggFunction.args().size());
    for (const auto& arg : aggFunction.args()) {
      aggParams.emplace_back(exprConverter_->toVeloxExpr(arg, inputType));
    }
    auto aggVeloxType =
        toVeloxType(subParser_->parseType(aggFunction.output_type())->type);
    auto aggExpr = std::make_shared<const core::CallTypedExpr>(
        aggVeloxType, std::move(aggParams), funcName);
    aggExprs.emplace_back(aggExpr);
  }

  bool ignoreNullKeys = false;
  std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>> aggregateMasks(
      sAgg.measures().size());
  std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>
      preGroupingExprs = {};

  // Get the output names of Aggregation.
  std::vector<std::string> aggOutNames;
  aggOutNames.reserve(sAgg.measures().size());
  for (int idx = groupingOutIdx; idx < groupingOutIdx + sAgg.measures().size();
       idx++) {
    aggOutNames.emplace_back(subParser_->makeNodeName(planNodeId_, idx));
  }

  // Create Aggregate node.
  auto aggNode = std::make_shared<core::AggregationNode>(
      nextPlanNodeId(),
      aggStep,
      veloxGroupingExprs,
      preGroupingExprs,
      aggOutNames,
      aggExprs,
      aggregateMasks,
      ignoreNullKeys,
      childNode);
  return aggNode;
}

core::PlanNodePtr SubstraitVeloxPlanConverter::toVeloxPlan(
    const ::substrait::ProjectRel& sProject) {
  core::PlanNodePtr childNode;
  if (sProject.has_input()) {
    childNode = toVeloxPlan(sProject.input());
  } else {
    VELOX_FAIL("Child Rel is expected in ProjectRel.");
  }

  // Construct Velox Expressions.
  const auto& projectExprs = sProject.expressions();
  std::vector<std::string> projectNames;
  std::vector<core::TypedExprPtr> expressions;
  projectNames.reserve(projectExprs.size());
  expressions.reserve(projectExprs.size());

  const auto& inputType = childNode->outputType();
  int colIdx = 0;
  for (const auto& expr : projectExprs) {
    expressions.emplace_back(exprConverter_->toVeloxExpr(expr, inputType));
    projectNames.emplace_back(subParser_->makeNodeName(planNodeId_, colIdx));
    colIdx += 1;
  }

  return std::make_shared<core::ProjectNode>(
      nextPlanNodeId(),
      std::move(projectNames),
      std::move(expressions),
      childNode);
}

core::PlanNodePtr SubstraitVeloxPlanConverter::toVeloxPlan(
    const ::substrait::FilterRel& filterRel) {
  core::PlanNodePtr childNode;
  if (filterRel.has_input()) {
    childNode = toVeloxPlan(filterRel.input());
  } else {
    VELOX_FAIL("Child Rel is expected in FilterRel.");
  }

  const auto& inputType = childNode->outputType();
  const auto& sExpr = filterRel.condition();

  return std::make_shared<core::FilterNode>(
      nextPlanNodeId(),
      exprConverter_->toVeloxExpr(sExpr, inputType),
      childNode);
}

bool isPushDownSupportedByFormat(
    const dwio::common::FileFormat& format,
    connector::hive::SubfieldFilters& subfieldFilters) {
  switch (format) {
    case dwio::common::FileFormat::PARQUET: {
      for (const auto& filter : subfieldFilters) {
        switch (filter.second->kind()) {
          // see ParquetReader.cpp:175

          // supported
          case common::FilterKind::kBigintRange:
          case common::FilterKind::kDoubleRange:
          case common::FilterKind::kBytesValues:
          case common::FilterKind::kBytesRange:
          case common::FilterKind::kBigintValuesUsingBitmask:
          case common::FilterKind::kBigintValuesUsingHashTable:
            break;

          // not supported
          case common::FilterKind::kAlwaysFalse:
          case common::FilterKind::kAlwaysTrue:
          case common::FilterKind::kIsNull:
          case common::FilterKind::kIsNotNull:
          case common::FilterKind::kBoolValue:
          case common::FilterKind::kFloatRange:
          case common::FilterKind::kBigintMultiRange:
          case common::FilterKind::kMultiRange:
          default:
            return false;
        }
      }
      break;
    }
    case dwio::common::FileFormat::ORC:
    case dwio::common::FileFormat::DWRF:
    case dwio::common::FileFormat::RC:
    case dwio::common::FileFormat::RC_TEXT:
    case dwio::common::FileFormat::RC_BINARY:
    case dwio::common::FileFormat::TEXT:
    case dwio::common::FileFormat::JSON:
    case dwio::common::FileFormat::ALPHA:
    case dwio::common::FileFormat::UNKNOWN:
    default:
      break;
  }
  return true;
}

std::shared_ptr<const core::PlanNode> SubstraitVeloxPlanConverter::toVeloxPlan(
    const ::substrait::ReadRel& sRead) {
  // Check if the ReadRel specifies an input of stream. If yes, the pre-built
  // input node will be used as the data source.
  auto splitInfo = std::make_shared<SplitInfo>();
  auto streamIdx = streamIsInput(sRead);
  if (streamIdx >= 0) {
    if (inputNodesMap_.find(streamIdx) == inputNodesMap_.end()) {
      VELOX_FAIL(
          "Could not find source index {} in input nodes map.", streamIdx);
    }
    auto streamNode = inputNodesMap_[streamIdx];
    splitInfo->isStream = true;
    splitInfoMap_[streamNode->id()] = splitInfo;
    return streamNode;
  }

  // Otherwise, will create TableScan node for ReadRel.
  // Get output names and types.
  std::vector<std::string> colNameList;
  std::vector<TypePtr> veloxTypeList;
  if (sRead.has_base_schema()) {
    const auto& baseSchema = sRead.base_schema();
    colNameList.reserve(baseSchema.names().size());
    for (const auto& name : baseSchema.names()) {
      colNameList.emplace_back(name);
    }
    auto substraitTypeList = subParser_->parseNamedStruct(baseSchema);
    veloxTypeList.reserve(substraitTypeList.size());
    for (const auto& substraitType : substraitTypeList) {
      veloxTypeList.emplace_back(toVeloxType(substraitType->type));
    }
  }

  // Parse local files and construct split info.
  if (sRead.has_local_files()) {
    const auto& fileList = sRead.local_files().items();
    splitInfo->paths.reserve(fileList.size());
    splitInfo->starts.reserve(fileList.size());
    splitInfo->lengths.reserve(fileList.size());
    for (const auto& file : fileList) {
      // Expect all Partitions share the same index.
      splitInfo->partitionIndex = file.partition_index();
      splitInfo->paths.emplace_back(file.uri_file());
      splitInfo->starts.emplace_back(file.start());
      splitInfo->lengths.emplace_back(file.length());
      auto format = file.format();
      if (format == 2 || format == 3) {
        splitInfo->format = dwio::common::FileFormat::DWRF;
      } else if (format == 1) {
        splitInfo->format = dwio::common::FileFormat::PARQUET;
      } else {
        splitInfo->format = dwio::common::FileFormat::UNKNOWN;
      }
    }
  }
  // Do not hard-code connector ID and allow for connectors other than Hive.
  static const std::string kHiveConnectorId = "test-hive";

  // Velox requires Filter Pushdown must being enabled.
  bool filterPushdownEnabled = true;
  std::shared_ptr<connector::hive::HiveTableHandle> tableHandle;
  if (!sRead.has_filter()) {
    tableHandle = std::make_shared<connector::hive::HiveTableHandle>(
        kHiveConnectorId,
        "hive_table",
        filterPushdownEnabled,
        connector::hive::SubfieldFilters{},
        nullptr);
  } else {
    // Flatten the conditions connected with 'and'.
    std::vector<::substrait::Expression_ScalarFunction> scalarFunctions;
    flattenConditions(sRead.filter(), scalarFunctions);

    // Separate the filters to be two parts. The first part can be
    // pushed down.
    std::vector<::substrait::Expression_ScalarFunction> subfieldFunctions;
    std::vector<::substrait::Expression_ScalarFunction> remainingFunctions;
    separateFilters(scalarFunctions, subfieldFunctions, remainingFunctions);

    // Create the filters to be pushed down.
    connector::hive::SubfieldFilters subfieldFilters =
        toSubfieldFilters(colNameList, veloxTypeList, subfieldFunctions);

    // Connect the remaining filters with 'and'.
    std::shared_ptr<const core::ITypedExpr> remainingFilter;

    if (!isPushDownSupportedByFormat(splitInfo->format, subfieldFilters)) {
      // A subfieldFilter is not supported by the format,
      // mark all filter as remaining filters.
      subfieldFilters.clear();
      remainingFilter =
          connectWithAnd(colNameList, veloxTypeList, scalarFunctions);
    } else {
      remainingFilter =
          connectWithAnd(colNameList, veloxTypeList, remainingFunctions);
    }

    tableHandle = std::make_shared<connector::hive::HiveTableHandle>(
        kHiveConnectorId,
        "hive_table",
        filterPushdownEnabled,
        std::move(subfieldFilters),
        remainingFilter);
  }

  // Get assignments and out names.
  std::vector<std::string> outNames;
  outNames.reserve(colNameList.size());
  std::unordered_map<std::string, std::shared_ptr<connector::ColumnHandle>>
      assignments;
  for (int idx = 0; idx < colNameList.size(); idx++) {
    auto outName = subParser_->makeNodeName(planNodeId_, idx);
    assignments[outName] = std::make_shared<connector::hive::HiveColumnHandle>(
        colNameList[idx],
        connector::hive::HiveColumnHandle::ColumnType::kRegular,
        veloxTypeList[idx]);
    outNames.emplace_back(outName);
  }
  auto outputType = ROW(std::move(outNames), std::move(veloxTypeList));

  if (sRead.has_virtual_table()) {
    return toVeloxPlan(sRead, outputType);
  } else {
    auto tableScanNode = std::make_shared<core::TableScanNode>(
        nextPlanNodeId(), outputType, tableHandle, assignments);
    // Set split info map.
    splitInfoMap_[tableScanNode->id()] = splitInfo;
    return tableScanNode;
  }
}

core::PlanNodePtr SubstraitVeloxPlanConverter::toVeloxPlan(
    const ::substrait::ReadRel& readRel,
    const RowTypePtr& type) {
  ::substrait::ReadRel_VirtualTable readVirtualTable = readRel.virtual_table();
  int64_t numVectors = readVirtualTable.values_size();
  int64_t numColumns = type->size();
  int64_t valueFieldNums =
      readVirtualTable.values(numVectors - 1).fields_size();
  std::vector<RowVectorPtr> vectors;
  vectors.reserve(numVectors);

  int64_t batchSize = valueFieldNums / numColumns;

  for (int64_t index = 0; index < numVectors; ++index) {
    std::vector<VectorPtr> children;
    ::substrait::Expression_Literal_Struct rowValue =
        readRel.virtual_table().values(index);
    auto fieldSize = rowValue.fields_size();
    VELOX_CHECK_EQ(fieldSize, batchSize * numColumns);

    for (int64_t col = 0; col < numColumns; ++col) {
      const TypePtr& outputChildType = type->childAt(col);
      std::vector<variant> batchChild;
      batchChild.reserve(batchSize);
      for (int64_t batchId = 0; batchId < batchSize; batchId++) {
        // each value in the batch
        auto fieldIdx = col * batchSize + batchId;
        ::substrait::Expression_Literal field = rowValue.fields(fieldIdx);

        auto expr = exprConverter_->toVeloxExpr(field);
        if (auto constantExpr =
                std::dynamic_pointer_cast<const core::ConstantTypedExpr>(
                    expr)) {
          if (!constantExpr->hasValueVector()) {
            batchChild.emplace_back(constantExpr->value());
          } else {
            VELOX_UNSUPPORTED(
                "Values node with complex type values is not supported yet");
          }
        } else {
          VELOX_FAIL("Expected constant expression");
        }
      }
      children.emplace_back(
          setVectorFromVariants(outputChildType, batchChild, pool_));
    }

    vectors.emplace_back(
        std::make_shared<RowVector>(pool_, type, nullptr, batchSize, children));
  }
  return std::make_shared<core::ValuesNode>(nextPlanNodeId(), vectors);
}

core::PlanNodePtr SubstraitVeloxPlanConverter::toVeloxPlan(
    const ::substrait::Rel& sRel) {
  if (sRel.has_aggregate()) {
    return toVeloxPlan(sRel.aggregate());
  }
  if (sRel.has_project()) {
    return toVeloxPlan(sRel.project());
  }
  if (sRel.has_filter()) {
    return toVeloxPlan(sRel.filter());
  }
  if (sRel.has_join()) {
    return toVeloxPlan(sRel.join());
  }
  if (sRel.has_read()) {
    return toVeloxPlan(sRel.read());
  }
  VELOX_NYI("Substrait conversion not supported for Rel.");
}

core::PlanNodePtr SubstraitVeloxPlanConverter::toVeloxPlan(
    const ::substrait::RelRoot& sRoot) {
  // TODO: Use the names as the output names for the whole computing.
  const auto& sNames = sRoot.names();
  if (sRoot.has_input()) {
    const auto& sRel = sRoot.input();
    return toVeloxPlan(sRel);
  }
  VELOX_FAIL("Input is expected in RelRoot.");
}

std::shared_ptr<const core::PlanNode> SubstraitVeloxPlanConverter::toVeloxPlan(
    const ::substrait::Plan& sPlan) {
  // Construct the function map based on the Substrait representation,
  // and initialize the expression converter with it.
  constructFuncMap(sPlan);

  // In fact, only one RelRoot or Rel is expected here.
  for (const auto& sRel : sPlan.relations()) {
    if (sRel.has_root()) {
      return toVeloxPlan(sRel.root());
    }
    if (sRel.has_rel()) {
      return toVeloxPlan(sRel.rel());
    }
  }
  VELOX_FAIL("RelRoot or Rel is expected in Plan.");
}

void SubstraitVeloxPlanConverter::constructFuncMap(
    const ::substrait::Plan& sPlan) {
  // Construct the function map based on the Substrait representation.
  for (const auto& sExtension : sPlan.extensions()) {
    if (!sExtension.has_extension_function()) {
      continue;
    }
    const auto& sFmap = sExtension.extension_function();
    auto id = sFmap.function_anchor();
    auto name = sFmap.name();
    functionMap_[id] = name;
  }
  exprConverter_ =
      std::make_shared<SubstraitVeloxExprConverter>(pool_, functionMap_);
}

std::string SubstraitVeloxPlanConverter::nextPlanNodeId() {
  auto id = fmt::format("{}", planNodeId_);
  planNodeId_++;
  return id;
}

void SubstraitVeloxPlanConverter::flattenConditions(
    const ::substrait::Expression& substraitFilter,
    std::vector<::substrait::Expression_ScalarFunction>& scalarFunctions) {
  auto typeCase = substraitFilter.rex_type_case();
  switch (typeCase) {
    case ::substrait::Expression::RexTypeCase::kScalarFunction: {
      auto sFunc = substraitFilter.scalar_function();
      auto filterNameSpec = subParser_->findSubstraitFuncSpec(
          functionMap_, sFunc.function_reference());
      // TODO: Only and relation is supported here.
      if (subParser_->getSubFunctionName(filterNameSpec) == "and") {
        for (const auto& sCondition : sFunc.args()) {
          flattenConditions(sCondition, scalarFunctions);
        }
      } else {
        scalarFunctions.emplace_back(sFunc);
      }
      break;
    }
    default:
      VELOX_NYI("GetFlatConditions not supported for type '{}'", typeCase);
  }
}

std::string SubstraitVeloxPlanConverter::findFuncSpec(uint64_t id) {
  return subParser_->findSubstraitFuncSpec(functionMap_, id);
}

void SubstraitVeloxPlanConverter::setPhase(
    const ::substrait::AggregateRel& sAgg,
    core::AggregationNode::Step& aggStep) {
  if (sAgg.measures().size() == 0) {
    // When only groupings exist, set the phase to be Single.
    aggStep = core::AggregationNode::Step::kSingle;
    return;
  }

  // Use the first measure to set aggregation phase.
  const auto& smea = sAgg.measures()[0];
  const auto& aggFunction = smea.measure();
  switch (aggFunction.phase()) {
    case ::substrait::AGGREGATION_PHASE_INITIAL_TO_INTERMEDIATE:
      aggStep = core::AggregationNode::Step::kPartial;
      break;
    case ::substrait::AGGREGATION_PHASE_INTERMEDIATE_TO_INTERMEDIATE:
      aggStep = core::AggregationNode::Step::kIntermediate;
      break;
    case ::substrait::AGGREGATION_PHASE_INTERMEDIATE_TO_RESULT:
      aggStep = core::AggregationNode::Step::kFinal;
      break;
    case ::substrait::AGGREGATION_PHASE_INITIAL_TO_RESULT:
      aggStep = core::AggregationNode::Step::kSingle;
      break;
    default:
      VELOX_FAIL("Aggregate phase is not supported.");
  }
}

int32_t SubstraitVeloxPlanConverter::streamIsInput(
    const ::substrait::ReadRel& sRead) {
  if (sRead.has_local_files()) {
    const auto& fileList = sRead.local_files().items();
    if (fileList.size() == 0) {
      VELOX_FAIL("At least one file path is expected.");
    }

    // The stream input will be specified with the format of
    // "iterator:${index}".
    std::string filePath = fileList[0].uri_file();
    std::string prefix = "iterator:";
    std::size_t pos = filePath.find(prefix);
    if (pos == std::string::npos) {
      return -1;
    }

    // Get the index.
    std::string idxStr = filePath.substr(pos + prefix.size(), filePath.size());
    try {
      return stoi(idxStr);
    } catch (const std::exception& err) {
      VELOX_FAIL(err.what());
    }
  }
  if (validationMode_) {
    return -1;
  }
  VELOX_FAIL("Local file is expected.");
}

void SubstraitVeloxPlanConverter::extractJoinKeys(
    const ::substrait::Expression& joinExpression,
    std::vector<const ::substrait::Expression::FieldReference*>& leftExprs,
    std::vector<const ::substrait::Expression::FieldReference*>& rightExprs) {
  std::vector<const ::substrait::Expression*> expressions;
  expressions.push_back(&joinExpression);
  while (!expressions.empty()) {
    auto visited = expressions.back();
    expressions.pop_back();
    if (visited->rex_type_case() ==
        ::substrait::Expression::RexTypeCase::kScalarFunction) {
      const auto& funcName =
          subParser_->getSubFunctionName(subParser_->findVeloxFunction(
              functionMap_, visited->scalar_function().function_reference()));
      const auto& args = visited->scalar_function().args();
      if (funcName == "and") {
        expressions.push_back(&args[0]);
        expressions.push_back(&args[1]);
      } else if (funcName == "eq") {
        VELOX_CHECK(std::all_of(
            args.cbegin(), args.cend(), [](const ::substrait::Expression& arg) {
              return arg.has_selection();
            }));
        leftExprs.push_back(&args[0].selection());
        rightExprs.push_back(&args[1].selection());
      } else {
        VELOX_NYI("Join condition {} not supported.", funcName);
      }
    } else {
      VELOX_FAIL(
          "Unable to parse from join expression: {}",
          joinExpression.DebugString());
    }
  }
}

connector::hive::SubfieldFilters SubstraitVeloxPlanConverter::toSubfieldFilters(
    const std::vector<std::string>& inputNameList,
    const std::vector<TypePtr>& inputTypeList,
    const std::vector<::substrait::Expression_ScalarFunction>&
        scalarFunctions) {
  // A map between the column index and the FilterInfo.
  std::unordered_map<uint32_t, std::shared_ptr<FilterInfo>> colInfoMap;
  for (uint32_t idx = 0; idx < inputTypeList.size(); idx++) {
    colInfoMap[idx] = std::make_shared<FilterInfo>();
  }

  // Construct the FilterInfo for the related column.
  for (const auto& scalarFunction : scalarFunctions) {
    auto filterNameSpec = subParser_->findSubstraitFuncSpec(
        functionMap_, scalarFunction.function_reference());
    auto filterName = subParser_->getSubFunctionName(filterNameSpec);
    if (filterName == sNot) {
      VELOX_CHECK(scalarFunction.args().size() == 1);
      VELOX_CHECK(
          scalarFunction.args()[0].has_scalar_function(),
          "Scalar function expected.");
      // Set its chid to filter info with reverse enabled.
      setFilterMap(
          scalarFunction.args()[0].scalar_function(),
          inputTypeList,
          colInfoMap,
          true);
      continue;
    }

    if (filterName == sOr) {
      VELOX_CHECK(scalarFunction.args().size() == 2);
      VELOX_CHECK(std::all_of(
          scalarFunction.args().cbegin(),
          scalarFunction.args().cend(),
          [](const ::substrait::Expression& arg) {
            return arg.has_scalar_function();
          }));
      // Set the chidren functions to filter info. They should be
      // effective to the same field.
      for (const auto& arg : scalarFunction.args()) {
        setFilterMap(arg.scalar_function(), inputTypeList, colInfoMap);
      }
      continue;
    }

    setFilterMap(scalarFunction, inputTypeList, colInfoMap);
  }

  // Create subfield filters based on the constructed filter info map.
  return mapToFilters(inputNameList, inputTypeList, colInfoMap);
}

bool SubstraitVeloxPlanConverter::fieldOrWithLiteral(
    const ::substrait::Expression_ScalarFunction& function,
    uint32_t& fieldIndex) {
  if (function.args().size() == 1) {
    if (function.args()[0].has_selection()) {
      // Only field exists.
      fieldIndex = subParser_->parseReferenceSegment(
          function.args()[0].selection().direct_reference());
      return true;
    } else {
      return false;
    }
  }

  if (function.args().size() != 2) {
    return false;
  }
  bool fieldExists = false;
  bool literalExists = false;
  for (const auto& param : function.args()) {
    auto typeCase = param.rex_type_case();
    switch (typeCase) {
      case ::substrait::Expression::RexTypeCase::kSelection:
        fieldIndex = subParser_->parseReferenceSegment(
            param.selection().direct_reference());
        fieldExists = true;
        break;
      case ::substrait::Expression::RexTypeCase::kLiteral:
        literalExists = true;
        break;
      default:
        break;
    }
  }
  // Whether the field and literal both exist.
  return fieldExists && literalExists;
}

bool SubstraitVeloxPlanConverter::chidrenFunctionsOnSameField(
    const ::substrait::Expression_ScalarFunction& function) {
  // Get the column indices of the chidren functions.
  std::vector<int32_t> colIndices;
  for (const auto& arg : function.args()) {
    if (!arg.has_scalar_function()) {
      return false;
    }
    auto scalarFunction = arg.scalar_function();
    for (const auto& param : scalarFunction.args()) {
      if (param.has_selection()) {
        auto field = param.selection();
        VELOX_CHECK(field.has_direct_reference());
        int32_t colIdx =
            subParser_->parseReferenceSegment(field.direct_reference());
        colIndices.emplace_back(colIdx);
      }
    }
  }

  if (std::all_of(colIndices.begin(), colIndices.end(), [&](uint32_t idx) {
        return idx == colIndices[0];
      })) {
    // All indices are the same.
    return true;
  }
  return false;
}

std::unordered_set<uint32_t> SubstraitVeloxPlanConverter::getInColIndices(
    const std::vector<::substrait::Expression_ScalarFunction>&
        scalarFunctions) {
  std::unordered_set<uint32_t> inCols;
  for (const auto& scalarFunction : scalarFunctions) {
    auto filterName =
        subParser_->getSubFunctionName(subParser_->findSubstraitFuncSpec(
            functionMap_, scalarFunction.function_reference()));

    if (filterName == sIn) {
      VELOX_CHECK(
          scalarFunction.args().size() > 0, "Arg is expected for IN function.");
      if (scalarFunction.args()[0].has_selection()) {
        // If the arg is other types, eg., function, it cannot be pushed down.
        uint32_t colIdx = getColumnIndexFromIn(scalarFunction);
        if (inCols.find(colIdx) == inCols.end()) {
          inCols.insert(colIdx);
        }
      }
    }
  }
  return inCols;
}

bool SubstraitVeloxPlanConverter::canPushdownCommonFunction(
    const ::substrait::Expression_ScalarFunction& scalarFunction,
    const std::unordered_set<uint32_t>& inCols,
    const std::string& filterName) {
  // Condtions can be pushed down.
  std::unordered_set<std::string> supportedCommonFunctions = {
      sIsNotNull, sGte, sGt, sLte, sLt, sEqual, sIn};
  uint32_t fieldIdx;

  if (supportedCommonFunctions.find(filterName) ==
          supportedCommonFunctions.end() ||
      !fieldOrWithLiteral(scalarFunction, fieldIdx)) {
    // The arg should be field or field with literal.
    return false;
  }

  if (inCols.find(fieldIdx) == inCols.end()) {
    return true;
  } else if (filterName == sIsNotNull || filterName == sIn) {
    // IN can only coexist with isNotNull in pushdown.
    return true;
  } else {
    return false;
  }
}

bool SubstraitVeloxPlanConverter::canPushdownNot(
    const ::substrait::Expression_ScalarFunction& scalarFunction,
    const std::unordered_set<uint32_t>& inCols,
    std::unordered_set<uint32_t>& notEqualCols) {
  VELOX_CHECK(
      scalarFunction.args().size() == 1, "Only one arg is expected for Not.");
  auto notArg = scalarFunction.args()[0];
  if (!notArg.has_scalar_function()) {
    // Not with a Boolean Literal is not supported curretly.
    // It can be pushed down with an AlwaysTrue or AlwaysFalse Range.
    return false;
  }

  auto nameSpec = subParser_->findSubstraitFuncSpec(
      functionMap_, notArg.scalar_function().function_reference());
  auto functionName = subParser_->getSubFunctionName(nameSpec);

  std::unordered_set<std::string> supportedNotFunctions = {
      sGte, sGt, sLte, sLt, sEqual};

  uint32_t fieldIdx;
  bool isFieldOrWithLiteral =
      fieldOrWithLiteral(notArg.scalar_function(), fieldIdx);
  if (supportedNotFunctions.find(functionName) == supportedNotFunctions.end() ||
      !isFieldOrWithLiteral || inCols.find(fieldIdx) != inCols.end()) {
    // If there is already a IN filter for this column, Not condtion
    // cannot be pushed down.
    return false;
  }

  // Mutiple not(equal) conditons cannot be pushed down because
  // the multiple range is in OR relation while AND relation is
  // actually needed.
  if (functionName == sEqual) {
    for (const auto& eqArg : notArg.scalar_function().args()) {
      if (!eqArg.has_selection()) {
        continue;
      }
      uint32_t colIdx = subParser_->parseReferenceSegment(
          eqArg.selection().direct_reference());
      // If one not(equal) condition for this column already exists,
      // this function cannot be pushed down then.
      if (notEqualCols.find(colIdx) == notEqualCols.end()) {
        notEqualCols.insert(colIdx);
      } else {
        return false;
      }
    }
  }
  return true;
}

bool SubstraitVeloxPlanConverter::canPushdownOr(
    const ::substrait::Expression_ScalarFunction& scalarFunction,
    const std::unordered_set<uint32_t>& inCols) {
  // OR Conditon whose chidren functions are on different columns is not
  // supported to be pushed down.
  if (!chidrenFunctionsOnSameField(scalarFunction)) {
    return false;
  }

  std::unordered_set<std::string> supportedOrFunctions = {
      sIsNotNull, sGte, sGt, sLte, sLt, sEqual, sIn};

  bool inExists = false;
  for (const auto& arg : scalarFunction.args()) {
    if (!arg.has_scalar_function()) {
      // Or relation betweeen literals is not supported to be pushded down
      // currently.
      return false;
    }

    auto nameSpec = subParser_->findSubstraitFuncSpec(
        functionMap_, arg.scalar_function().function_reference());
    auto functionName = subParser_->getSubFunctionName(nameSpec);

    uint32_t fieldIdx;
    bool isFieldOrWithLiteral =
        fieldOrWithLiteral(arg.scalar_function(), fieldIdx);
    if (supportedOrFunctions.find(functionName) == supportedOrFunctions.end() ||
        !isFieldOrWithLiteral || inCols.find(fieldIdx) != inCols.end()) {
      // The arg should be field or field with literal.
      // If there is already a IN filter for this column, OR condtion
      // cannot be pushed down.
      return false;
    }

    if (functionName == sIn || functionName == sIsNotNull) {
      std::vector<std::string> types;
      subParser_->getSubFunctionTypes(nameSpec, types);
      if (std::find(types.begin(), types.end(), sI32) != types.end() ||
          std::find(types.begin(), types.end(), sI64) != types.end()) {
        // BigintMultiRange can only accept vector of BigintRange.
        return false;
      }
      if (functionName == sIn) {
        if (inExists) {
          // OR relation of several IN functions is not supported to be pushed
          // down currently.
          return false;
        }
        inExists = true;
      }
    }
  }
  return true;
}

void SubstraitVeloxPlanConverter::separateFilters(
    const std::vector<::substrait::Expression_ScalarFunction>& scalarFunctions,
    std::vector<::substrait::Expression_ScalarFunction>& subfieldFunctions,
    std::vector<::substrait::Expression_ScalarFunction>& remainingFunctions) {
  // Used to record the columns indices for not(equal) conditions.
  std::unordered_set<uint32_t> notEqualCols;

  // Get the unique columns indices for IN condition.
  std::unordered_set<uint32_t> inCols = getInColIndices(scalarFunctions);

  for (const auto& scalarFunction : scalarFunctions) {
    auto filterNameSpec = subParser_->findSubstraitFuncSpec(
        functionMap_, scalarFunction.function_reference());
    auto filterName = subParser_->getSubFunctionName(filterNameSpec);
    if (filterName != sNot && filterName != sOr) {
      // Check if the condition is supported to be pushed down.
      if (canPushdownCommonFunction(scalarFunction, inCols, filterName)) {
        subfieldFunctions.emplace_back(scalarFunction);
      } else {
        remainingFunctions.emplace_back(scalarFunction);
      }
      continue;
    }

    // Check whether NOT and OR functions can be pushed down.
    // If yes, the scalar function will be added into the subfield functions.
    bool supported = false;
    if (filterName == sNot) {
      supported = canPushdownNot(scalarFunction, inCols, notEqualCols);
    } else if (filterName == sOr) {
      supported = canPushdownOr(scalarFunction, inCols);
    }

    if (supported) {
      subfieldFunctions.emplace_back(scalarFunction);
    } else {
      remainingFunctions.emplace_back(scalarFunction);
    }
  }
}

uint32_t SubstraitVeloxPlanConverter::getColumnIndexFromIn(
    const ::substrait::Expression_ScalarFunction& scalarFunction) {
  VELOX_CHECK(
      scalarFunction.args().size() == 2, "Two args expected in In expression.");
  VELOX_CHECK(scalarFunction.args()[0].has_selection(), "Field expected.");

  // Get the column index.
  uint32_t colIdx = subParser_->parseReferenceSegment(
      scalarFunction.args()[0].selection().direct_reference());
  VELOX_CHECK(scalarFunction.args()[1].has_literal(), "Literal expected.");
  VELOX_CHECK(scalarFunction.args()[1].literal().has_list(), "List expected.");
  return colIdx;
}

void SubstraitVeloxPlanConverter::setInValues(
    const ::substrait::Expression_ScalarFunction& scalarFunction,
    std::unordered_map<uint32_t, std::shared_ptr<FilterInfo>>& colInfoMap) {
  // Get the column index.
  uint32_t colIdx = getColumnIndexFromIn(scalarFunction);

  // Get the value list.
  std::vector<variant> variants;
  auto valueList = scalarFunction.args()[1].literal().list();
  variants.reserve(valueList.values().size());
  for (const auto& literal : valueList.values()) {
    variants.emplace_back(
        exprConverter_->toTypedVariant(literal)->veloxVariant);
  }

  // Set the value list to filter info.
  colInfoMap[colIdx]->setValues(variants);
}

template <typename T>
void SubstraitVeloxPlanConverter::setColInfoMap(
    const std::string& filterName,
    uint32_t colIdx,
    std::optional<variant> literalVariant,
    bool reverse,
    std::unordered_map<uint32_t, std::shared_ptr<FilterInfo>>& colInfoMap) {
  if (filterName == sIsNotNull) {
    if (reverse) {
      VELOX_NYI("Reverse not supported for filter name '{}'", filterName);
    }
    colInfoMap[colIdx]->forbidsNull();
    return;
  }

  if (filterName == sGte) {
    if (reverse) {
      colInfoMap[colIdx]->setUpper(literalVariant, true);
    } else {
      colInfoMap[colIdx]->setLower(literalVariant, false);
    }
    return;
  }

  if (filterName == sGt) {
    if (reverse) {
      colInfoMap[colIdx]->setUpper(literalVariant, false);
    } else {
      colInfoMap[colIdx]->setLower(literalVariant, true);
    }
    return;
  }

  if (filterName == sLte) {
    if (reverse) {
      colInfoMap[colIdx]->setLower(literalVariant, true);
    } else {
      colInfoMap[colIdx]->setUpper(literalVariant, false);
    }
    return;
  }

  if (filterName == sLt) {
    if (reverse) {
      colInfoMap[colIdx]->setLower(literalVariant, false);
    } else {
      colInfoMap[colIdx]->setUpper(literalVariant, true);
    }
    return;
  }

  if (filterName == sEqual) {
    if (reverse) {
      colInfoMap[colIdx]->setNotValue(literalVariant);
    } else {
      colInfoMap[colIdx]->setLower(literalVariant, false);
      colInfoMap[colIdx]->setUpper(literalVariant, false);
    }
    return;
  }
  VELOX_NYI("SetColInfoMap not supported for filter name '{}'", filterName);
}

void SubstraitVeloxPlanConverter::setFilterMap(
    const ::substrait::Expression_ScalarFunction& scalarFunction,
    const std::vector<TypePtr>& inputTypeList,
    std::unordered_map<uint32_t, std::shared_ptr<FilterInfo>>& colInfoMap,
    bool reverse) {
  auto nameSpec = subParser_->findSubstraitFuncSpec(
      functionMap_, scalarFunction.function_reference());
  auto functionName = subParser_->getSubFunctionName(nameSpec);

  // Extract the in args and set to filter info.
  if (functionName == sIn) {
    setInValues(scalarFunction, colInfoMap);
    return;
  }

  // Extract the column index and column bound from the scalar function.
  std::optional<uint32_t> colIdx;
  std::optional<::substrait::Expression_Literal> substraitLit;
  for (const auto& param : scalarFunction.args()) {
    auto typeCase = param.rex_type_case();
    switch (typeCase) {
      case ::substrait::Expression::RexTypeCase::kSelection:
        colIdx = subParser_->parseReferenceSegment(
            param.selection().direct_reference());
        break;
      case ::substrait::Expression::RexTypeCase::kLiteral:
        substraitLit = param.literal();
        break;
      default:
        VELOX_NYI(
            "Substrait conversion not supported for arg type '{}'", typeCase);
    }
  }
  if (!colIdx.has_value()) {
    VELOX_NYI("Column index is expected in subfield filters creation.");
  }

  // Set the extracted bound to the specific column.
  uint32_t colIdxVal = colIdx.value();
  auto inputType = inputTypeList[colIdxVal];
  std::optional<variant> val;
  switch (inputType->kind()) {
    case TypeKind::INTEGER:
      if (substraitLit) {
        val = variant(substraitLit.value().i32());
      }
      setColInfoMap<int>(functionName, colIdxVal, val, reverse, colInfoMap);
      break;
    case TypeKind::BIGINT:
      if (substraitLit) {
        val = variant(substraitLit.value().i64());
      }
      setColInfoMap<int64_t>(functionName, colIdxVal, val, reverse, colInfoMap);
      break;
    case TypeKind::DOUBLE:
      if (substraitLit) {
        val = variant(substraitLit.value().fp64());
      }
      setColInfoMap<double>(functionName, colIdxVal, val, reverse, colInfoMap);
      break;
    case TypeKind::VARCHAR:
      if (substraitLit) {
        val = variant(substraitLit.value().string());
      }
      setColInfoMap<std::string>(
          functionName, colIdxVal, val, reverse, colInfoMap);
      break;
    default:
      VELOX_NYI(
          "Subfield filters creation not supported for input type '{}'",
          inputType);
  }
}

template <TypeKind KIND, typename FilterType>
void SubstraitVeloxPlanConverter::createNotEqualFilter(
    variant notVariant,
    bool nullAllowed,
    std::vector<std::unique_ptr<FilterType>>& colFilters) {
  using NativeType = typename RangeTraits<KIND>::NativeType;
  using RangeType = typename RangeTraits<KIND>::RangeType;

  // Value > lower
  std::unique_ptr<FilterType> lowerFilter = std::make_unique<RangeType>(
      notVariant.value<NativeType>(), /*lower*/
      false, /*lowerUnbounded*/
      true, /*lowerExclusive*/
      getMax<NativeType>(), /*upper*/
      true, /*upperUnbounded*/
      false, /*upperExclusive*/
      nullAllowed); /*nullAllowed*/
  colFilters.emplace_back(std::move(lowerFilter));

  // Value < upper
  std::unique_ptr<FilterType> upperFilter = std::make_unique<RangeType>(
      getLowest<NativeType>(), /*lower*/
      true, /*lowerUnbounded*/
      false, /*lowerExclusive*/
      notVariant.value<NativeType>(), /*upper*/
      false, /*upperUnbounded*/
      true, /*upperExclusive*/
      nullAllowed); /*nullAllowed*/
  colFilters.emplace_back(std::move(upperFilter));
}

template <TypeKind KIND>
void SubstraitVeloxPlanConverter::setInFilter(
    const std::vector<variant>& variants,
    bool nullAllowed,
    const std::string& inputName,
    connector::hive::SubfieldFilters& filters) {}

template <>
void SubstraitVeloxPlanConverter::setInFilter<TypeKind::DOUBLE>(
    const std::vector<variant>& variants,
    bool nullAllowed,
    const std::string& inputName,
    connector::hive::SubfieldFilters& filters) {
  std::vector<double> values;
  values.reserve(variants.size());
  for (const auto& variant : variants) {
    double value = variant.value<double>();
    values.emplace_back(value);
  }
  filters[common::Subfield(inputName)] =
      common::createDoubleValues(values, nullAllowed);
}

template <>
void SubstraitVeloxPlanConverter::setInFilter<TypeKind::BIGINT>(
    const std::vector<variant>& variants,
    bool nullAllowed,
    const std::string& inputName,
    connector::hive::SubfieldFilters& filters) {
  std::vector<int64_t> values;
  values.reserve(variants.size());
  for (const auto& variant : variants) {
    int64_t value = variant.value<int64_t>();
    values.emplace_back(value);
  }
  filters[common::Subfield(inputName)] =
      common::createBigintValues(values, nullAllowed);
}

template <>
void SubstraitVeloxPlanConverter::setInFilter<TypeKind::INTEGER>(
    const std::vector<variant>& variants,
    bool nullAllowed,
    const std::string& inputName,
    connector::hive::SubfieldFilters& filters) {
  // Use bigint values for int type.
  std::vector<int64_t> values;
  values.reserve(variants.size());
  for (const auto& variant : variants) {
    // Use the matched type to get value from variant.
    int64_t value = variant.value<int32_t>();
    values.emplace_back(value);
  }
  filters[common::Subfield(inputName)] =
      common::createBigintValues(values, nullAllowed);
}

template <>
void SubstraitVeloxPlanConverter::setInFilter<TypeKind::VARCHAR>(
    const std::vector<variant>& variants,
    bool nullAllowed,
    const std::string& inputName,
    connector::hive::SubfieldFilters& filters) {
  std::vector<std::string> values;
  values.reserve(variants.size());
  for (const auto& variant : variants) {
    std::string value = variant.value<std::string>();
    values.emplace_back(value);
  }
  filters[common::Subfield(inputName)] =
      std::make_unique<common::BytesValues>(values, nullAllowed);
}

template <TypeKind KIND, typename FilterType>
void SubstraitVeloxPlanConverter::setSubfieldFilter(
    std::vector<std::unique_ptr<FilterType>> colFilters,
    const std::string& inputName,
    bool nullAllowed,
    connector::hive::SubfieldFilters& filters) {
  using MultiRangeType = typename RangeTraits<KIND>::MultiRangeType;

  if (colFilters.size() == 1) {
    filters[common::Subfield(inputName)] = std::move(colFilters[0]);
  } else if (colFilters.size() > 1) {
    filters[common::Subfield(inputName)] =
        std::make_unique<MultiRangeType>(std::move(colFilters), nullAllowed);
  }
}

template <TypeKind KIND, typename FilterType>
void SubstraitVeloxPlanConverter::constructSubfieldFilters(
    uint32_t colIdx,
    const std::string& inputName,
    const std::shared_ptr<FilterInfo>& filterInfo,
    connector::hive::SubfieldFilters& filters) {
  using NativeType = typename RangeTraits<KIND>::NativeType;
  using RangeType = typename RangeTraits<KIND>::RangeType;
  using MultiRangeType = typename RangeTraits<KIND>::MultiRangeType;

  if (!filterInfo->isInitialized()) {
    return;
  }

  uint32_t rangeSize = std::max(
      filterInfo->lowerBounds_.size(), filterInfo->upperBounds_.size());
  bool nullAllowed = filterInfo->nullAllowed_;

  // Handle 'in' filter.
  if (filterInfo->valuesVector_.size() > 0) {
    setInFilter<KIND>(
        filterInfo->valuesVector_, nullAllowed, inputName, filters);
    // Currently, In cannot coexist with other filter conditions
    // due to multirange is in 'OR' relation but 'AND' is needed.
    VELOX_CHECK(
        rangeSize == 0,
        "LowerBounds or upperBounds conditons cannot be supported after IN filter.");
    VELOX_CHECK(
        !filterInfo->notValue_.has_value(),
        "Not equal cannot be supported after IN filter.");
    return;
  }

  // Construct the Filters.
  std::vector<std::unique_ptr<FilterType>> colFilters;

  // Handle not(equal) filter.
  if (filterInfo->notValue_) {
    variant notVariant = filterInfo->notValue_.value();
    createNotEqualFilter<KIND, FilterType>(
        notVariant, filterInfo->nullAllowed_, colFilters);
    // Currently, Not-equal cannot coexist with other filter conditions
    // due to multirange is in 'OR' relation but 'AND' is needed.
    VELOX_CHECK(
        rangeSize == 0,
        "LowerBounds or upperBounds conditons cannot be supported after not-equal filter.");
    filters[common::Subfield(inputName)] =
        std::make_unique<MultiRangeType>(std::move(colFilters), nullAllowed);
    return;
  }

  // Handle null filtering.
  if (rangeSize == 0 && !nullAllowed) {
    std::unique_ptr<common::IsNotNull> filter =
        std::make_unique<common::IsNotNull>();
    filters[common::Subfield(inputName)] = std::move(filter);
    return;
  }

  // Handle other filter ranges.
  NativeType lowerBound = getLowest<NativeType>();
  NativeType upperBound = getMax<NativeType>();
  bool lowerUnbounded = true;
  bool upperUnbounded = true;
  bool lowerExclusive = false;
  bool upperExclusive = false;

  for (uint32_t idx = 0; idx < rangeSize; idx++) {
    if (idx < filterInfo->lowerBounds_.size() &&
        filterInfo->lowerBounds_[idx]) {
      lowerUnbounded = false;
      variant lowerVariant = filterInfo->lowerBounds_[idx].value();
      lowerBound = lowerVariant.value<NativeType>();
      lowerExclusive = filterInfo->lowerExclusives_[idx];
    }
    if (idx < filterInfo->upperBounds_.size() &&
        filterInfo->upperBounds_[idx]) {
      upperUnbounded = false;
      variant upperVariant = filterInfo->upperBounds_[idx].value();
      upperBound = upperVariant.value<NativeType>();
      upperExclusive = filterInfo->upperExclusives_[idx];
    }
    std::unique_ptr<FilterType> filter = std::make_unique<RangeType>(
        lowerBound,
        lowerUnbounded,
        lowerExclusive,
        upperBound,
        upperUnbounded,
        upperExclusive,
        nullAllowed);
    colFilters.emplace_back(std::move(filter));
  }

  // Set the SubfieldFilter.
  setSubfieldFilter<KIND, FilterType>(
      std::move(colFilters), inputName, filterInfo->nullAllowed_, filters);
}

connector::hive::SubfieldFilters SubstraitVeloxPlanConverter::mapToFilters(
    const std::vector<std::string>& inputNameList,
    const std::vector<TypePtr>& inputTypeList,
    std::unordered_map<uint32_t, std::shared_ptr<FilterInfo>> colInfoMap) {
  // Construct the subfield filters based on the filter info map.
  connector::hive::SubfieldFilters filters;
  for (uint32_t colIdx = 0; colIdx < inputNameList.size(); colIdx++) {
    auto inputType = inputTypeList[colIdx];
    switch (inputType->kind()) {
      case TypeKind::INTEGER:
        constructSubfieldFilters<TypeKind::INTEGER, common::BigintRange>(
            colIdx, inputNameList[colIdx], colInfoMap[colIdx], filters);
        break;
      case TypeKind::BIGINT:
        constructSubfieldFilters<TypeKind::BIGINT, common::BigintRange>(
            colIdx, inputNameList[colIdx], colInfoMap[colIdx], filters);
        break;
      case TypeKind::DOUBLE:
        constructSubfieldFilters<TypeKind::DOUBLE, common::Filter>(
            colIdx, inputNameList[colIdx], colInfoMap[colIdx], filters);
        break;
      case TypeKind::VARCHAR:
        constructSubfieldFilters<TypeKind::VARCHAR, common::Filter>(
            colIdx, inputNameList[colIdx], colInfoMap[colIdx], filters);
        break;
      default:
        VELOX_NYI(
            "Subfield filters creation not supported for input type '{}'",
            inputType);
    }
  }
  return filters;
}

std::shared_ptr<const core::ITypedExpr>
SubstraitVeloxPlanConverter::connectWithAnd(
    std::vector<std::string> inputNameList,
    std::vector<TypePtr> inputTypeList,
    const std::vector<::substrait::Expression_ScalarFunction>&
        remainingFunctions) {
  if (remainingFunctions.size() == 0) {
    return nullptr;
  }
  auto inputType = ROW(std::move(inputNameList), std::move(inputTypeList));
  std::shared_ptr<const core::ITypedExpr> remainingFilter =
      exprConverter_->toVeloxExpr(remainingFunctions[0], inputType);
  if (remainingFunctions.size() == 1) {
    return remainingFilter;
  }
  // Will connect multiple functions with AND.
  uint32_t idx = 1;
  while (idx < remainingFunctions.size()) {
    std::vector<std::shared_ptr<const core::ITypedExpr>> params;
    params.reserve(2);
    params.emplace_back(remainingFilter);
    params.emplace_back(
        exprConverter_->toVeloxExpr(remainingFunctions[idx], inputType));
    remainingFilter = std::make_shared<const core::CallTypedExpr>(
        BOOLEAN(), std::move(params), "and");
    idx += 1;
  }
  return remainingFilter;
}

} // namespace facebook::velox::substrait
