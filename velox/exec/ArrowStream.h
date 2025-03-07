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
#include <arrow/c/abi.h>
#include "velox/core/PlanNode.h"
#include "velox/exec/Operator.h"

namespace facebook::velox::exec {

class ArrowStream : public SourceOperator {
 public:
  ArrowStream(
      int32_t operatorId,
      DriverCtx* driverCtx,
      std::shared_ptr<const core::ArrowStreamNode> arrowStream);

  virtual ~ArrowStream();

  RowVectorPtr getOutput() override;

  BlockingReason isBlocked(ContinueFuture* /* unused */) override {
    return BlockingReason::kNotBlocked;
  }
  void noMoreInput() override {
    Operator::noMoreInput();
    close();
  }

  const char* GetError();

  bool isFinished() override;
  void close() override;

 private:
  bool closed_ = false;
  RowVectorPtr rowVector_;
  std::shared_ptr<ArrowArrayStream> arrowStream_;
  memory::MemoryPool* pool_;

  // For calls from destructor
  bool isFinished0();
  void close0();
};

} // namespace facebook::velox::exec
